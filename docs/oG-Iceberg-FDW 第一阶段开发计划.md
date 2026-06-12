# openGauss Iceberg FDW 当期开发计划（6.18）

本文给出 Iceberg FDW core 当期的开发任务分解，按阶段（phase）分段开发、局部验证、断点续作。范围与口径以本仓《5. Iceberg_FDW 详细设计》《Iceberg-SDK-能力核实（临时）》《oG-Iceberg-FDW SDK 接口与下推参数》为准。

下列为**参考（非遵从）**来源，仅借鉴实现思路，不构成本期范围或接口约束：

- `ref-iceberg-fdw-managed-fullscan-implementation-design.md`（andy123552）：managed Iceberg 表的全表扫描方案。其谓词复核（SDK filter 仅剪枝、全部 qual 本地 recheck）、`fdw_private` 序列化分槽、type/operator adapter 分层值得借鉴；但其 managed DDL/事务 hook、建表即写 Iceberg metadata 的范围**本期不采纳**——本期为只读扫描，元信息经系统表查询获得。
- `ref-fdw-catalog-required-fields.md`（andy123552）：catalog 必需字段清单（`tables_internal`/`table_schemas`/`snapshots`），与本期系统表取数字段对齐参考。
- `Iceberg_Java_SDK_Data_Reading_Guide.md`（本仓）：Java SDK 读取路径指引。
- `ref-FallMoo-d2a67c3.patch`（FallMoo）：`NativeIcebergReader` 向量化/行式回退读取实证代码，佐证《能力核实》关于 Record→Arrow 须自写的结论。

## 1. 目标与范围

### 1.1 本期目标

实现 Iceberg 外表只读全表扫描闭环：规划期解析表身份与当前版本指针、编码私有数据；执行期打开 Reader 取 Arrow 列式批，转为 HeapTuple 行式返回执行器。覆盖列裁剪与基础谓词下推。

### 1.2 范围取舍

下表为本期明确取舍，细化或覆盖概要设计中的待定项。

| 项 | 本期处理 | 说明 |
| --- | --- | --- |
| 输出形态 | 行式 `Arrow → HeapTuple` | 不实现向量化输出 `VecIterateForeignScan`，仅留接口形态 |
| Reader | Java SDK + C/C++ 桥接 | 按《fdw-iceberg-sdk-arrow_design.md》路径；Arrow C Data Interface 为 FDW↔Reader 边界 |
| 列裁剪 | 实现 | `retrievedAttrs` 译为 Reader 投影列名 |
| 谓词下推 | 实现，全部下推 | 暂定输入无复杂表达式，谓词拆分全归 remote、local 为空 |
| delete file | 不读取 | Java 向量化快路径不支持 delete；行式回退路径后续阶段启用 |
| delta 合并 | 仅预留挂接点 | 不实现合并逻辑 |
| `ReScanForeignScan` | 占位桩 | 注册但不实现真实重绕，不影响六个核心回调 |
| `ExplainForeignScan` | 占位桩 | 注册但不实现，不影响六个核心回调 |
| 向量索引 / 回表 / topk | 预留 | 不实现 |
| openGauss 内核改动 | 按需评估 | 开发中识别哪些能力无法在 extension 内承载、须侵入内核，再决定避免/mock/侵入 |

六个核心回调：`GetForeignRelSize`、`GetForeignPaths`、`GetForeignPlan`、`BeginForeignScan`、`IterateForeignScan`、`EndForeignScan`。

### 1.3 谓词下推与复核

本期假设输入谓词均为简单谓词（AND 合取的 `col OP const`、`col IS [NOT] NULL`、`col IN (...)`）。`classifyConditions` 中 `is_foreign_expr` 恒为真，全部谓词序列化进 `IcebergPrivPushedFilter` 下推给 Reader。

但向量化路径的下推**仅用于 I/O 裁剪**（manifest / 文件 / row-group 三级跳过），不做行级精确过滤（依据《Iceberg-SDK-能力核实》§2.4、§5.1）。因此全部原始谓词**同时保留为 `ForeignScan` 节点 qual**，由执行器 `ExecQual` 逐行复核兜底——下推与本地 qual **并存而非互斥**，否则会多返回存活 row-group 内不匹配的行。后续若引入无法下推的谓词，则该谓词只保留本地 qual、不进 `pushedFilter`。

## 2. 开发组织与代码落位

### 2.1 集成形态

FDW core 以 extension 插件形式链接 openGauss，经 `handler` 返回 `FdwRoutine` 注册回调，经 `validator` 校验 OPTIONS。

### 2.2 代码落位策略

- FDW 独立代码置于 myidea 仓库开发分支，独立于 openGauss 主体演进。
- Reader 取数后端按《fdw-iceberg-sdk-arrow_design.md》路径确定为 Java SDK + C/C++ 桥接：JNI 封装 + 内嵌 JVM 单例 + Arrow C Data 导入，FDW 经统一三接口驱动。
- 与 openGauss 内核耦合的部分（`fdwapi.h`、`RelOptInfo`、`ForeignScanState`、`TupleTableSlot` 等结构与 `make_foreignscan`/`ExecStoreHeapTuple` 等内核函数）不预设完全 mock：开发中逐项确认哪些能力可在 extension 内承载、哪些须侵入内核修改，对前者直接对接，对后者再决定避免、mock 或提出内核改动。
- 桥接层与转换层可先以构造的 Arrow 批做单元测试，使 FDW core 逻辑在脱离实例的环境独立验证；真实 SDK 联调与实例联调随集成推进。

### 2.3 模块文件构成

```
iceberg_fdw/
  iceberg_fdw.control          扩展元数据
  iceberg_fdw--1.0.sql         注册 handler / validator / FDW
  Makefile                     MODULE_big / EXTENSION / REGRESS
  iceberg_fdw.h                公共声明
  iceberg_fdw.cpp              handler + 六核心回调 + rescan/explain 桩
  option.cpp                   validator 与 OPTIONS 白名单
  catalog.cpp                  IcebergGetTableIdentity / ResolveTable / Cardinality
  convert.cpp                  Arrow → HeapTuple 转换与 converters
  bridge/iceberg_scan.cpp      C 桥接：open/next/close + JVM 生命周期 + JNI 封装
  bridge/IcebergJniReader.java Java SDK 入口：openTable/readNextBatch/releaseBatch/close
  test/                        converter 与闭环单元测试（可用构造 Arrow 批）
```

## 3. 分阶段 TODO

阶段依赖：一 → {二、三 可并行} → 四 → 五 →（六 集成，后续）。每阶段含独立验证，可断点续作。

### 阶段一：插件骨架与回调注册

目标：可编译、可注册的最小插件，回调先空实现。

- `iceberg_fdw_handler` 用 `makeNode(FdwRoutine)` 注册六核心回调 + `ReScanForeignScan`/`ExplainForeignScan` 桩函数。
- `iceberg_fdw_validator` 与 OPTIONS 白名单：server 端 `catalog_kind`/`catalog_schema` 与存储凭证；table 端 `namespace`/`table_name`。
- `control`/`--1.0.sql`/`Makefile` 齐备。

验证：`CREATE EXTENSION`、`CREATE SERVER`、`CREATE FOREIGN TABLE` 成功；validator 对非法 OPTION 报错。

### 阶段二：规划期三回调

目标：产出正确的 `ForeignScan` 计划节点与 `fdw_private`，不访问对象存储。

- `GetForeignRelSize`：`IcebergGetTableIdentity(foreigntableid)` 取表身份，暂存 `baserel->fdw_private`；`IcebergGetTableCardinality` 取基数（来源待定，本期固定选择率兜底），写 `baserel->rows`。
- `GetForeignPaths`：登记单条全表扫描 `ForeignPath`；写成可追加多路径的形态，为后续索引路径预留。
- `GetForeignPlan`：
  - `IcebergCatalogResolveTable(ident)` 点查 `tables_internal`，取 `metadata_location`/`current_snapshot_id`/`current_schema_id`/`table_uuid`/`table_location`；
  - `BuildRetrievedAttrs(baserel)` 由 `reltarget` 算投影列 attno；
  - `classifyConditions(scan_clauses)` 本期可下推谓词全归 remote，`SerializeFilter` 序列化进 `IcebergPrivPushedFilter`；同时 `local_exprs = extract_actual_clauses(scan_clauses, false)` 取**全部**原始谓词作节点 qual 供执行器复核（下推 ≠ 移除本地 qual，见 §1.3）；
  - 装配 `fdw_private = list_make3(scanEntry, retrievedAttrs, pushedFilter)`；
  - `make_foreignscan(tlist, local_exprs, scanrelid, NIL, fdw_private, …, outer_plan)`（第二参 `local_exprs` 即节点 qual，本期含全部谓词；`outer_plan` 为 openGauss 特有末参）。
- Catalog 访问本期以 mock 元信息或测试表承接，待元信息表落地后切换。

验证：`EXPLAIN VERBOSE` 计划节点为 ForeignScan；以日志打印解码后的 `scanEntry`、`retrievedAttrs`、`pushedFilter` 字段正确。

### 阶段三：SDK / 桥接层与 Arrow C Data 边界

目标：按《fdw-iceberg-sdk-arrow_design.md》路径打通 Java SDK + C/C++ 桥接，固定 Reader 边界接口。

- 按《oG-Iceberg-FDW SDK 接口与下推参数》签名实现三接口：
  - `iceberg_scan_open(cxt, metadata_location, storage_config, columns, n_columns, filter, out_schema)`
  - `iceberg_scan_next(scan, out_array, out_schema)` 返回 `int nrows`（0=结束）
  - `iceberg_scan_close(scan)`
- 桥接层：JNI 封装四个 Java 方法（`openTable`/`readNextBatch`/`releaseBatch`/`close`）；内嵌 JVM 单例（`pthread_once` 初始化，classpath 经 GUC 配置）；`JNIEnv*` 按线程缓存于 TLS；每次 JNI 调用后 `ExceptionCheck` 转 `ereport`。
- Java 侧：`TableMetadataParser.read` 加载 metadata → `newScan` → 向量化读 Parquet → `Data.exportVector` 导出 Arrow C Data；含 delete file（MOR 表）本期不支持（行式回退路径后续阶段启用）。
- 可先用构造 Arrow 批的桩替身验证桥接调用与转换层，再接入真实 SDK。

验证：`open → next* → close` 读出 Arrow 批；JVM 仅初始化一次；异常经 `ExceptionCheck` 转 PG ERROR。

### 阶段四：执行期回调与 Arrow → HeapTuple

目标：打通取批、类型转换、逐行返回。

- `BeginForeignScan`：解码 `fdw_private`；建 `IcebergScanState`（`rel`/`tupdesc`/`retrievedAttrs`/`reader`/批缓冲字段/`batch_cxt`/`temp_cxt`）；`AttnosToColumnNames` 译列名；`BuildStorageConfig` 组装凭证；`iceberg_scan_open` 取 `out_schema`；`BuildConverters` 建转换函数表。`EXEC_FLAG_EXPLAIN_ONLY` 早退，`fdw_state` 保持 NULL。
- `IterateForeignScan` + `fetch_more_data`：批状态机（批耗尽再取）；`ArrowBatchToHeapTuples` 逐行转换并写入 `batch_cxt`；`ExecStoreHeapTuple` 逐行返回；预留 delta union 挂接点（本期空实现）。
- `EndForeignScan`：`iceberg_scan_close` + 删内存上下文。
- `ReScanForeignScan`/`ExplainForeignScan`：占位桩，互不影响核心回调。

转换说明：Arrow 单元格经 converter 转为 `Datum`（单值容器，定长按值、变长存指针），一行的 `Datum` 数组经 `heap_form_tuple` 打包为 `HeapTuple`（整行元组，含元组头与 null bitmap），再入 slot。变长列与 decimal 须在批回收前拷入 `batch_cxt`。

验证：converter 单元测试覆盖 NULL bitmap、变长列拷贝、decimal128 展开、date/timestamp 纪元偏移；每个测试编码"为何此转换正确"的语义断言。

### 阶段五：闭环联调（全 mock 链路）

目标：`Begin → Iterate* → End` 全链对账。

- 以构造 Arrow 批的桩 Reader 跑通全表扫描，逐行结果与输入一致。
- 列裁剪：`retrievedAttrs` 正确译为 Reader `columns`，仅返回投影列。
- 谓词下推：传入 Reader 的 `filter` 串内容正确，过滤后行集正确。

验证：单元测试或小程序逐行对账行数与列值。

### 阶段六：真实集成（后续，不在本期主体）

目标：从 mock 切换到真实环境。

- 在实例环境编译安装 extension，跑通真实 `SELECT`；接入选定 Reader；接入真实 `iceberg_catalog.*` 元信息表。
- 接入回归测试 `test/sql` 与 `expected`，挂 `Makefile` 的 `REGRESS`。

## 4. 关键契约

### 4.1 规划 → 执行私有数据 `fdw_private`

按固定枚举顺序装配的可序列化 `List`，规划期编码、执行期解码：

| 枚举位 | 内容 | 本期 |
| --- | --- | --- |
| `IcebergPrivScanEntry` | `{ metadata_location, snapshot_id, schema_id }` | 实现 |
| `IcebergPrivRetrievedAttrs` | 投影列 `List<attno>` | 实现 |
| `IcebergPrivPushedFilter` | 下推谓词序列化串 | 实现（全部下推） |
| `IcebergPrivIndexSpec` / `IcebergPrivLocator` | 索引与回表 | 预留，不编码 |

约束：`fdw_private` 不携带 Reader 句柄、已打开文件或内存上下文。

### 4.2 执行态 `IcebergScanState`

`BeginForeignScan` 建立、`node->fdw_state` 持有。本期字段：`rel`/`tupdesc`/`retrievedAttrs`/`reader`（`IcebergReaderState*` = scan 句柄 + converters）/`tuples`/`num_tuples`/`next_tuple`/`eof_reached`/`batch_cxt`/`temp_cxt`。`locator_cursor` 等预留字段不启用。

## 5. 类型映射本期范围

Iceberg 经 Arrow 映射到 openGauss 类型，本期覆盖标量与常用类型：

| Iceberg | openGauss | 转换 |
| --- | --- | --- |
| int / long | INTEGER / BIGINT | 直读 buffer |
| boolean | BOOLEAN | bitmap 取位 |
| float / double | FLOAT4 / FLOAT8 | 直读 buffer |
| string / binary | TEXT/VARCHAR / BYTEA | offset+length，`palloc` 拷入 `batch_cxt` |
| uuid | UUID | 16 字节定长 memcpy |
| date | DATE | 距 epoch 天数，注意纪元偏移 |
| time | TIME | 微秒 |
| timestamp | TIMESTAMP | epoch 微秒，注意纪元偏移 |
| decimal(p,s) | NUMERIC | decimal128 逐值展开为 Numeric |

`list<float> → vector`（`access/datavec`）最小通路为阶段二、三数据基础，本期可选打通，不作硬性要求。嵌套 list/struct/map 不在本期范围。

## 6. 后续阶段预留接口（仅形态）

下列接口本期仅保留形态，不实现逻辑，确保后续接入无须重构：

- 多路径登记：`GetForeignPaths` 可追加 ForeignPath / CustomScan。
- 向量化输出：`VecIterateForeignScan` / `ArrowBatchToVectorBatch`。
- 位置回表：`iceberg_read_by_locator`。
- 索引私有数据：`IcebergPrivIndexSpec` / `IcebergPrivLocator`。
- delta 合并：`IterateForeignScan` 内的 union 挂接点。

## 7. 验证方法

| 层 | 手段 | 环境 |
| --- | --- | --- |
| 规划期（阶段二） | `EXPLAIN VERBOSE` + 日志打印解码字段 | 实例环境 |
| Reader 边界（阶段三） | mock open/next/close 单元测试 | 脱离实例 |
| 类型转换（阶段四） | converter 单元测试（边界 + 语义断言） | 脱离实例 |
| 全链路（阶段五） | mock 全链逐行对账 | 脱离实例 |
| 真实集成（阶段六） | `SELECT` / `count(*)` / `make check` 对账 | 实例环境 |

数据库相关操作以 omm 用户执行；纯逻辑与转换层测试以构造 Arrow 批在脱离实例的环境完成。
