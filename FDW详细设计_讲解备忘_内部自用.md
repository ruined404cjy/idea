# FDW 详细设计（5.md）讲解备忘 · 内部自用

> 配合《5. Iceberg_FDW 详细设计》会议讲解用，非交付文档。内容：①5.md 中各符号的来源、位置与含义；②最终被 5.md 省略、但讲解时值得说明的细节；③最新 commit 与上下游文档（`GaussVector_Iceberg_Catalog_Design.md`、`6. fdw-iceberg-sdk-arrow-design.md`）的接口对齐与当前选择。

---

## 1. 符号速查（按 5.md 出现顺序）

来源标记：**oG**＝openGauss/PG 内置 API/类型，按既有签名用；**自**＝本模块自定义；**借**＝沿用 pg_lake/postgres_fdw 的模式或命名；**上**＝上游 catalog 表；**规**＝Iceberg 规范；**存**＝对象存储。

| 符号 | 来源 | 位置 | 作用（在该处做什么） |
| --- | --- | --- | --- |
| `makeNode` / `FdwRoutine` / `PG_FUNCTION_INFO_V1` | oG | §1.3 | handler 用 `makeNode(FdwRoutine)` 建结构并逐字段赋回调；`PG_FUNCTION_INFO_V1` 导出 C 函数 |
| `iceberg_fdw_handler` / `iceberg_fdw_validator` | 自 | §1.3 | handler 返回装好的 `FdwRoutine`；validator 校验 OPTIONS |
| OPTION 键 `catalog_kind`/`catalog_schema`/`namespace`/`table_name` | 自 | §1.4 | server/table 上声明的配置键，定位 Iceberg 表与后端（OPTIONS 机制本身为 oG） |
| `untransformRelOptions` / `defGetString` | oG | §1.4（validator） | 解析 OPTIONS 键值 |
| `ereport` / `ERRCODE_*` | oG | §1.4 / §4.1 | 错误上报（非法 OPTION、NoSuchTable 等） |
| `enum IcebergScanPrivateIndex`(`IcebergPrivFileScanTasks`/`IcebergPrivRetrievedAttrs`) | 自 | §2.1 | `fdw_private` 的固定编码下标 |
| 「枚举编码→Begin 解码」/「局部谓词交节点 qual」模式 | 借 | §2.1 / §3.3 | 同 postgres_fdw `FdwScanPrivateIndex`、file_fdw（不自行过滤，`scan_clauses` 入 `make_foreignscan`） |
| `IcebergScanState` | 自 | §2.2 | 执行态（框架层）结构 |
| `Relation` / `TupleDesc` / `RelationGetDescr` | oG | §2.2 / §3.4 | 关系与元组描述符（`tupdesc` 取自扫描关系） |
| `reader`（`void*`） | 自 | §2.2 | 不透明 reader 句柄，内部实现属下半层 |
| 批取数缓冲 `tuples`/`num_tuples`/`next_tuple`/`eof_reached` | 借 | §2.2 / §3.5 | reader 一次返一批、FDW 内逐行吐；同 postgres_fdw `PgFdwScanState` |
| `MemoryContext` / `AllocSetContextCreate` / `MemoryContextReset`·`Delete`（`batch_cxt`/`temp_cxt`） | oG | §2.2 / §3.4–3.6 | 每批 reset 回收、End 释放 |
| `FileScanTask` | 自 | §2.3 | 单数据文件扫描描述（路径 + 格式） |
| `PlanFiles` | 自 | §2.3 / §3.3 | 读 manifest 展开产出 `FileScanTask`（等价 SDK `planFiles`） |
| `PlannerInfo` / `RelOptInfo`(`rows`/`baserestrictinfo`/`fdw_private`) | oG | §3.1–3.3 | 规划上下文与关系优化信息 |
| `IcebergGetTableIdentity` | 自 | §3.1 / §4.1 | 从 OPTIONS 取表身份 + catalog 配置 |
| `GetForeignTable()->options` / `GetForeignServer()->options` | oG | §3.1 / §4.1 | 读外表/server OPTIONS（`ftoptions`/`srvoptions` 的 `DefElem`）；"取外表 OPTIONS"即此 |
| `IcebergGetTableCardinality` | 自 | §3.1 | 取当前 snapshot 记录数作表基数（来源待定，§5.2） |
| `clauselist_selectivity` | oG | §3.1 | 优化器内置，估谓词联合选择率，折算 `baserel->rows` |
| `create_foreignscan_path` / `add_path` / `ForeignPath` | oG | §3.2 | 构造并登记全表扫描路径 |
| `extract_actual_clauses` / `RestrictInfo` | oG | §3.3 | 从 `RestrictInfo` 取裸表达式作节点 qual |
| `IcebergCatalogResolveTable` | 自 | §3.3 / §4.1 | `(ns,table)`→`metadata_location`+指针，Iceberg `loadTable` 只读部分 |
| `BuildRetrievedAttrs` | 自 | §3.3 | 由 `tupdesc`/targetlist 算投影列 attno |
| `IcebergBuildScanPrivate` | 自 | §3.3 | 按枚举顺序装配 `fdw_private` |
| `make_foreignscan` / `ForeignScan` / `outer_plan` | oG | §3.3 | 构造 `ForeignScan` 节点（`fdw_private` 第 5 参；`outer_plan` 为 openGauss 特有） |
| `ForeignScanState`(`fdw_state`/`ss.ps.plan`/`ss.ss_currentRelation`) / `eflags` / `EXEC_FLAG_EXPLAIN_ONLY` | oG | §3.4–3.7 | 执行态节点与标志（EXPLAIN-only 不建态） |
| `palloc0` / `list_nth` | oG | §3.4 | 分配执行态、按下标解码 `fdw_private` |
| `ReaderOpen` / `ReaderNextBatch` / `ReaderClose` | 自 | §3.4–3.6 / §4.3 | 下层 reader 接口（示意；对应 6.md `iceberg_scan_open`/`_next`/`_close`） |
| `fetch_more_data` | 自 | §3.5 | 取批静态函数（批缓存状态机） |
| `TupleTableSlot` / `ExecClearTuple` / `ExecStoreTuple` / `heap_form_tuple` | oG | §3.5 | 槽操作与建元组 |
| `ExplainState` / `ExplainPropertyText` | oG | §3.7 | EXPLAIN 输出 |
| `DescribeSource` | 自 | §3.7 | 描述扫描来源（文件数 / `manifest_list` / `snapshot_id`） |
| `SPI_*` / `quote_identifier` / `pstrdup` | oG | §4.1 | 在 C 内执行 catalog SQL、标识符转义、跨上下文复制结果 |
| `iceberg_catalog.tables`(`metadata_location`/`current_schema_id`/`current_snapshot_id`/`table_uuid`) | 上 | §3.3 / §4.1 / §4.2 | 普通列，SPI 一次取；表身份与当前指针 |
| `iceberg_catalog.snapshots`(`manifest_list`/`operation`) | 上 | §3.3 / §4.2 | 普通列；`manifest_list` 为 PlanFiles 入口；记录数未缓存 |
| `iceberg_catalog.table_schemas.schema_json` | 上 | §4.3 | JSONB，reader 解析做 field_id 对应（FDW 不读） |
| `field_id` | 规 | §4.3 | 列稳定标识；reader 用于文件列↔表列对应（schema evolution） |
| `total-records` | 规 | §3.1 / §4.2 | snapshot summary 中的表基数；未缓存（§5.2） |
| `metadata.json` / manifest / data / delete file | 存 | §2.3 / §4.3 | `PlanFiles` 与 reader 读取的对象 |

---

## 2. 被 5.md 省略、但讲解时值得提的细节

| # | 主题 | 5.md 现状 | 讲解补充 |
| --- | --- | --- | --- |
| 1 | `field_id` / schema evolution | 仅一句"不在 FDW 层" | Iceberg 用稳定 `field_id` 标识列，列增删/改名/重排不变；按 field_id 把文件列对应到表列由 reader 依据 `schema_json` 完成（6.md §4.2）。FDW 只用 attno 表达投影 |
| 2 | `IcebergPrivResidualQuals` | 已删 | 早期版本有此项；删除原因：本模块不向 reader 下推谓词，局部谓词由执行器对节点 qual 自动重过滤，无需在 `fdw_private` 另存。将来做 reader 端裁剪再加 |
| 3 | `IcebergPrivNum` 哨兵 | 已删 | 曾作枚举末尾计数哨兵（`Assert(list_length==Num)`）；C 习惯，非必需，已去 |
| 4 | 向量化 / `VecIterateForeignScan` | 出范围 | 本模块仅行式。GV 自身有向量化读取（6.md §4：Formation Java-SDK 路径 / 直接 Parquet C++ 路径，由 `enable_istore_cache` 切换） |
| 5 | 并行 / SMP | 未提 | openGauss `create_foreignscan_path` 有 `dop` 入参；并行需把 FileScanTask 切分给多线程，本期不做（参见文档 4） |
| 6 | `catalog_kind` 取值 | 默认 `pg_native` | 实际现仅 `pg_native` 单值（rest 已从文档移除）；保留该 OPTION 是为后端选择留口，可讨论是否精简 |
| 7 | `ValidateTableDef` | 已删 | 曾作 DDL 期（CREATE/ALTER FOREIGN TABLE）校验外表定义的 oG 钩子占位（区别于校验 OPTIONS 的 validator）；按"不为未来设计"已移除 |
| 8 | 批大小 | 未写 | reader 一次返一批，6.md 默认 64K 行；FDW 只按"批耗尽再取"驱动，与批大小无关 |
| 9 | `batch_cxt` vs `temp_cxt` | 结构里有，正文少提 | `batch_cxt` 每批 reset 一次性回收变长列拷贝；`temp_cxt` 留作单行/临时分配。变长列（string/binary）必须拷进 `batch_cxt`，因 Arrow buffer 在释放批后失效（6.md §3.3.4） |
| 10 | EXPLAIN-only 下 `fdw_state==NULL` | 各回调有判 | `Begin` 在 `EXPLAIN_ONLY` 不建态，故 `Iterate`/`End` 均先判 NULL；`Explain` 读 `plan->fdw_private` 不依赖执行态，因此安全 |
| 11 | `relid` 备选解析路径 | §4.1 待定一句 | `tables` 有 `UNIQUE(relid)`；若建表时把外表 OID 写入，可用 `WHERE relid=foreigntableid` 解析，免依赖 OPTIONS 正确性。纯外部表 relid 可空，需 Catalog 注册侧配合，故暂以 `(ns,table)` 为主 |

---

## 3. 上下游接口对齐与当前选择

最新 commit 依据上游 `GaussVector_Iceberg_Catalog_Design.md` 与下游 `6. fdw-iceberg-sdk-arrow-design.md` 对齐了两处关键接口。

### 3.1 上游：规划期如何读 snapshot

**链路**：`tables.current_snapshot_id` →（按 `table_uuid + snapshot_id`）`snapshots` 行 → `snapshots.manifest_list`。三者皆为 Catalog **普通列**，规划期一次 SPI 取得，**不读对象存储**。

**对齐点**（catalog 设计 §6.3.4）：
- `snapshots.manifest_list`（TEXT，可空）："Iceberg V2 快速路径直接使用该字段；旧格式缺失时回退解析 `metadata.json`"——5.md §2.3 入口取法与此一致。
- `snapshots` 只缓存 `operation`，**不缓存完整 summary**（catalog 设计 §6.3.4 说明 5）。故**表基数 `total-records` 不可直接取**。

**当前选择（估行，5.md §3.1）**：
- 主选：扩 `snapshots` 增 `total_records BIGINT`，commit 时从 summary 写入；估行直接 SPI 取。与 catalog 设计"后续需成本分析时再引入 summary 统计"的扩展路径一致，且规避规划期对象存储 IO。
- 落地前回退：规划期读 `metadata.json` 当前 snapshot summary，或先给固定/粗略估算。
- **待上游确认**：是否接受在 `snapshots` 增列并在 commit 路径维护。

### 3.2 下游：FileScanTask（文件清单）确定的时机

**背景**：Catalog 明确不缓存 manifest/data file 明细，故文件清单**必须运行期读对象存储**产出。唯一可决策的是**产出时机**。5.md §2.3 给出 A（规划期产出完整列表）/ B（执行期产出，`fdw_private` 只带入口指针）。

**下游已定（6.md）**：SDK reader 走**方案 B**——`BeginForeignScan` 从 `fdw_private` 取 `metadata_location`/`snapshot_id`，在 `openTable` 内由 SDK `newScan().planFiles()` 内部完成文件规划（6.md §3.3.2）。

**当前选择：方案 B（执行期产出）为默认**，理由：
1. 与 6.md SDK 路径一致——SDK 的 `TableMetadataParser.read(path)` + `planFiles()` 本就在 reader 内部，FDW 再传文件列表会重复规划，并绕过 SDK 的 MOR / pruning 语义；
2. 规划期**零对象存储 IO**；
3. 缓存/预备计划下 snapshot 在执行期解析，一致性更好；
4. 首期规划期代价用 `snapshots` 行数摘要即可，不需要文件级信息。

**方案 A 的保留场景**：自有 C++ reader 且需要规划期文件级代价或并行（SMP）切分时；此时可把缓存的 `manifest_list` 下推给 reader 跳过 `metadata.json`。

**入口参数的小差异（待与 6.md 作者对齐）**：方案 B 下 FDW 传给 reader 的入口，6.md SDK 用 `metadata_location`（SDK API 取 metadata 路径），5.md 另提 `manifest_list`（自有 reader 走缓存快路径，跳过 metadata.json）。二者都来自 `tables`/`snapshots` 普通列、规划期 SPI 即得；建议统一为"FDW 同时可下传 `metadata_location` 与（命中时）`manifest_list`，由 reader 择用"。

### 3.3 框架层与 reader 层的结构边界

| 层 | 结构 | 持有字段 | 出处 |
| --- | --- | --- | --- |
| FDW 框架层 | `IcebergScanState` | `rel`/`tupdesc`、`fileScanTasks`/`retrievedAttrs`、批缓冲四字段、`batch_cxt`/`temp_cxt`、`void *reader` | 5.md §2.2 |
| reader 层 | `IcebergReaderState`（挂在 `void *reader` 后） | `scan` 句柄、`converters`（类型转换函数表） | 6.md §3.3.2 |

reader 不感知框架层批缓冲字段；框架层不感知 reader 内部。这条边界使"行式 tuple 调度"（FDW 层）与"Arrow 取数/转换"（reader 层，6.md）解耦。

---

## 4. 可能的提问预案

| 问题 | 回答要点 |
| --- | --- |
| 为什么 `fdw_private` 不存 field_id 映射？ | reader 用 `schema_json` 按 field_id 对应文件列，FDW 只传 attno（§1 表、细节 1） |
| SPI 会保留在实际实现里吗？ | pg_native 下 catalog 是库内普通表，从 C 读它最通用是 SPI，大概率保留；备选 syscache/直连内核表扫描更快但更重 |
| 只做行式，向量化呢？ | 出范围；GV 自身有向量化（6.md §4 Formation/直接 Parquet） |
| 并行/SMP？ | 出范围；`create_foreignscan_path` 有 `dop` 接口待用，需 FileScanTask 切分（文档 4） |
| 文件清单 A/B 怎么定？ | 当前 B（执行期，reader 内部 planFiles），与 6.md 一致；A 留给"需规划期文件级代价/并行"场景（§3.2） |
| 估行准不准？ | 首期用当前 snapshot 记录数 × 选择率；记录数来源待上游在 `snapshots` 增列或回退读 metadata.json（§3.1） |
| ReScan 怎么实现？ | reader 接口仅 open/next/close，无 rewind，重扫经 close+reopen（与 6.md 一致，5.md §3.6） |
| 一次规划会多次 Begin 吗？ | 会——见 §5.1 澄清：prepared/缓存计划跨执行多次、一条计划多个外表 scan 节点多次；ReScan 不算（不重调 Begin，但会重开 reader） |

---

## 5. 元数据访问频率与元表结构建议（FDW 视角）

上游元表设计尚未定稿，本节从 FDW 的访问模式出发，提出降低开销（尤其对象存储访问）的结构建议，供与 Catalog 侧对齐。

### 5.1 FDW 对元数据的访问频率与开销分布

| FDW 操作 | 频率 | 读 catalog | 读对象存储 |
| --- | --- | --- | --- |
| 规划 `GetForeignRelSize` | **低**（每查询一次，计划缓存后摊薄） | `tables`（snapshot 指针）+ `snapshots`（基数，若已缓存） | 仅当读 `metadata.json` 取 `total-records` 时 |
| 规划 `GetForeignPlan` | **低**（同上） | `tables`（4 列）[+ `snapshots.manifest_list`，方案 A] | 方案 A：`PlanFiles` 展开 manifest |
| 执行 `BeginForeignScan` | **中**（每个外表 scan 节点一次；详见下方澄清） | 方案 B：reader 读 `schema_json` | 方案 B：`PlanFiles` 展开 manifest |
| 执行 `IterateForeignScan` | **高**（每行） | 无 | reader 读 data file |
| 执行 `ReScan` | 中 | 无 | 方案 B 下重开 reader 会再次 `PlanFiles`（见澄清） |

**「一次规划多次 Begin」澄清**（回答常见疑问）：

- **跨执行（存在）**：prepared / 缓存的通用计划被多次 `EXECUTE`，每次执行重新初始化扫描 → `Begin` 被调多次。这是与本模块特性无关的通用情形。
- **一次执行内**：一个 `ForeignScan` 节点只 `Begin` 一次；仅当一条计划含多个外表 scan 节点（自连接、同表多次引用）时，一次执行才有多个 `Begin`。
- **ReScan ≠ Begin**：相关子查询 / nested-loop 内表会多次 `ReScanForeignScan`，**不重调 `Begin`**；但本设计 ReScan = close+reopen reader（5.md §3.6），故 **reader open（方案 B 含 `PlanFiles`）随 ReScan 次数翻倍**——这是单次查询内对象存储读放大的主因。
- **范围外**：并行（SMP）/ 分区下的多 `Begin` 不在当前范围。

三点结论：

1. **catalog 访问应尽量前移到规划期**（低频、可随计划缓存），让 `Begin` 尽量只用 `fdw_private` 已编码结果。
2. catalog 普通列的 SPI 读（索引命中）**廉价**；真正贵的是**对象存储访问**。
3. 对象存储里 data file 读不可免（数据本身）；**可优化的是 manifest 展开（获取文件清单）**——它在每次 `PlanFiles` 发生：方案 A 规划期一次、方案 B 每个 `Begin`、且每次 `ReScan` 重开都再来一次。

### 5.2 元表结构建议

| # | 建议 | 目标开销 | 代价 / 注意 | 评级 |
| --- | --- | --- | --- | --- |
| 1 | `snapshots` 增 `total_records BIGINT`（commit 时从 snapshot summary 写入） | 规划期估行免读 `metadata.json` | 一列；commit 路径维护 | **推荐** |
| 2 | 按 snapshot 缓存**文件清单**（新增 `data_files(table_uuid, snapshot_id, file_path, format, …)`） | 把 manifest 展开的对象存储读降到「每 snapshot 首次」 | 与现"不缓存 manifest/data 明细"原则相悖；存储膨胀 | **高价值，待 Catalog 权衡** |
| 3 | `table_schemas.schema_json` 顶层列展开为 `table_columns(table_uuid, schema_id, field_id, name, ordinal, type, required)` | reader/FDW 取列免 JSONB 解析，可按需投影 | 嵌套类型仍须留 `schema_json`；冗余两份 | 可选（收益小） |
| 4 | `tables` + `snapshots` 合并为一次 JOIN 查询 | 减少规划期 SPI 往返 | 实现细节，几乎无副作用 | 小优化 |

**建议 2（关键）的论据**：某一 snapshot 的数据文件集合**不可变**（snapshot 一旦生成，其引用的文件清单固定）。因此「按 `(table_uuid, snapshot_id)` 缓存文件清单」**天然安全**——表 commit 新 snapshot 时 `current_snapshot_id` 前移，FDW 自然去读新 snapshot（首次 miss、之后命中），旧行可按容量/时间裁剪，**无需写时失效逻辑**。这正面回应了 Catalog 设计排除 data_files 缓存的主要顾虑（一致性/失效成本）。对重复查询同一表的场景，这是单项最大的对象存储节省。

**建议 3（JSON 展开）的判断**：用户提示的"复合 JSON 字段展开"对 FDW 层收益有限——FDW 层本身不读 `schema_json`（5.md），它由 reader 在每次 open 解析以做 field_id/类型对应。展开为列可省 JSONB 解析、并让 FDW 侧直接以 SQL 解析 attno→field_id 后下传（省去 reader 解析）；但：① 解析成本相对对象存储读可忽略；② Iceberg 嵌套类型（struct/list/map 的子 field_id）无法平铺，仍须保留 `schema_json` 作权威。故定位为「顶层列的派生缓存」，可选、非首期重点。

### 5.3 对「方案 A/B（FileScanTask 产出时机）」的影响

文件清单缓存（建议 2）会**弱化 A/B 之争**：无论规划期（A）还是执行期（B）产出，对象存储的 manifest 展开都被缓存吸收到「每 snapshot 首次」，之后均为廉价 SPI 读缓存。

- 推荐组合：**方案 B + 建议 2 缓存**——既保 snapshot 新鲜度（执行期解析当前 snapshot），又把重复扫描/ReScan 的对象存储读降到首次；
- 若无缓存，方案 B 的每 `Begin`/每 `ReScan` 都读对象存储，重复查询代价显著，此时方案 A + 计划缓存反而更省（但有 snapshot 漂移风险）。

即：**是否上文件清单缓存，直接决定 A/B 的取舍**。建议把建议 2 作为前置项与 Catalog 侧确认。
