# Iceberg Java SDK 能力核实（临时）

日期：2026-06-11。本文核实并统一 FDW 与 SDK 设计文档所依赖的四项能力——分区剪枝、列裁剪、列统计剪枝、谓词下推、MOR——的含义、阶段与边界，辨析"向量化快路径"等同名概念，作为后续设计对齐依据。

## 1. 核实方法与源码版本

| 来源 | 内容 |
| --- | --- |
| 本地源码 | `catalog-service-research/src_ref/iceberg`，main 分支 commit `f825d09`（2026-05-20）。已将 sparse-checkout 扩展至完整模块：`api`/`core`/`arrow`/`parquet`/`data`/`spark` |
| 对照设计 | `new-catalog/design/fdw-iceberg-sdk-arrow_design.md`；`myidea/docs/oG-Iceberg-FDW core 接口清单.md` §1.4/§1.5；`oG-Iceberg-FDW 概要设计.md` §3.4 |

下文结论均给出源码锚点（`文件:行`）。

## 2. 术语统一：四项能力的阶段 / 输入 / 粒度 / 结果

各文档对这些能力的称呼不一（"分区摘要/列统计/行组"三层、"下推 filter"、"MOR 回退"等）。统一定义如下：

| 能力 | 阶段 | 针对什么 | 输入 | 粒度（结果） | SDK 机制（实现类） |
| --- | --- | --- | --- | --- | --- |
| 分区剪枝 | 规划期 `planFiles()` | 分区列 | `scan.filter(expr)` + manifest 分区摘要 | 跳过整个 manifest / 整个数据文件 | `ManifestEvaluator`、`ResidualEvaluator` |
| 列统计剪枝 | 规划期 `planFiles()` | 任意列 | `scan.filter(expr)` + 文件列统计（min/max/null) | 跳过整个数据文件 | `InclusiveMetricsEvaluator` |
| 行组剪枝 | 执行期 Reader | 任意列 | `scan.filter(expr)` + Parquet footer 行组统计 | 跳过整个 row group | `ReadConf` 的 `ParquetMetricsRowGroupFilter` / `ParquetDictionaryRowGroupFilter` / `ParquetBloomRowGroupFilter` |
| 列裁剪（投影下推） | 执行期 Reader | 选中列 | `scan.select(cols)` / `project(schema)` | 只读选中列的 column chunk | `Scan.select`，投影 schema = 选中列 ∪ filter 用到的列 |
| 行级精确过滤 | 执行期 Reader | 任意列 | `scan.filter(expr)` 的残差 | 逐行剔除不匹配行 | **仅行式 `ParquetReader`（`filterRecords=true`）**，向量化路径不做 |
| MOR（delete 应用） | 执行期 Reader | 行 | `FileScanTask.deletes()` | 逐行删除/标记 | **仅 `iceberg-data` 的 `DeleteFilter`**，向量化路径不做 |

要点：

- 前三项（分区/列统计/行组剪枝）+ 列裁剪是**自动且两条读取路径都生效**的裁剪能力，FDW 只需正确传 `filter` 与 `select`。
- 概要设计 §3.4 的"分区摘要 / 列统计 / 行组"三层，分别对应上表前三行。
- 后两项（行级精确过滤、MOR）**只在特定读取路径生效**，是下文辨析的核心。

### 2.1 分区剪枝

`scan.filter(expr)` 后，`planFiles()` 自动执行两级分区裁剪：manifest 级 `ManifestEvaluator` 用 manifest list 的分区列 lower/upper 跳过整片 manifest；文件级 `ResidualEvaluator` 用每个文件的分区值对 `expr` 偏求值，残差恒 false 则跳过该文件。仅作用于分区列（或其 transform 派生），要求表已分区且谓词引用分区源列。结果是整文件保留/丢弃，不触及行。

### 2.2 列统计剪枝

数据文件携带 `value_count`/`null_count`/`lower_bounds`/`upper_bounds`。`InclusiveMetricsEvaluator` 在 `planFiles()` 内用这些统计判断整个文件是否可能命中，不命中则跳过；作用于任意列，是分区裁剪之外的又一层**文件级**裁剪。`includeColumnStats()` 可把统计暴露给调用方。

### 2.3 列裁剪

`scan.select(Collection<String>)` 或 `project(Schema)`。投影 schema = 选中列 ∪ 过滤表达式用到的列；Reader 只读这些列的 Parquet column chunk。

### 2.4 谓词下推（关键边界）

`scan.filter(Expression)` 在向量化路径只走到"跳过"，不剔除行。逐层为证：

1. `ArrowReader` 调 `builder.filter(task.residual())`（`arrow/.../ArrowReader.java:342`）。
2. `Parquet.ReadBuilder`：`filterRecords` 默认 `true`（`parquet/.../Parquet.java:1205`），但向量化分支构造 `VectorizedParquetReader`（`:1503`）**不传 filterRecords**；`useRecordFilter(filterRecords)` 只挂在行式 `ParquetReader`（`:1566`）。
3. `VectorizedParquetReader` 用 `ReadConf.shouldSkip[]` 跳过整个 row group（`VectorizedParquetReader.java:155-157` `while (shouldSkip[...]) reader.skipNextRowGroup()`）；`shouldSkip` 由三个 row-group 级 filter 计算（`ReadConf.java:94-113`），存活 row group 内的行整批返回、不逐行判定。

结论：**向量化路径的谓词下推是 I/O 裁剪（manifest/文件/row group 三级跳过），不是行级精确过滤。**

### 2.5 MOR（delete file）

- 规划：`planFiles()` 产出 `FileScanTask`，`task.deletes()` 返回 `List<DeleteFile>`，`FileContent` 区分 `POSITION_DELETES`/`EQUALITY_DELETES`。core 自动完成"哪些 delete 作用于哪个数据文件"。
- 应用：`iceberg-data` 的 `DeleteFilter`（`data/.../DeleteFilter.java:51`）输入 `FileScanTask`、拆出 `posDeletes`/`eqDeletes`，`filter(CloseableIterable<T> records)`（`:188`）逐行剔除或标记；`GenericDeleteFilter`（`:31`，输入 `FileIO, FileScanTask, tableSchema, requestedSchema`）以 `Record` 为单位应用。粒度为行级。

## 3. 同名概念辨析："向量化快路径" 是什么

设计文档 §3.1.2 出现的"向量化快路径"/"行式回退路径"是 **SDK 内部两种读 Parquet 的实现**，**两者最终都通过 Arrow C Data 输出 Arrow**——区别在内部读取方式，不在输出格式。

| 维度 | 向量化快路径 | 行式回退路径 |
| --- | --- | --- |
| SDK 入口 | `VectorizedTableScanIterable` → `ArrowReader` → `VectorizedParquetReader` | `planFiles()` → `GenericReader` 读为 `Record` |
| 内部读取 | 列式 `ColumnarBatch`（FieldVector，堆外） | 行式 `Record`（逐行对象） |
| 谓词残差 | 仅 row group 级跳过（§2.4） | 行级精确过滤（`filterRecords=true`） |
| MOR | **不支持**：含 delete 直接抛 `UnsupportedOperationException`（`ArrowReader.java:240-242`，注释 `:98`"Delete files are not supported"） | **支持**：`DeleteFilter` 逐行应用 pos/eq delete |
| 输出格式 | `Data.exportVector` 导出 Arrow C Data | `Record` → `convertToArrowTable` 转列式 → 导出 Arrow C Data |
| 依赖模块 | `iceberg-arrow` + `iceberg-parquet` | `iceberg-data` + `iceberg-arrow`（仅导出） |

> 补充：`iceberg-arrow` 内已有位置删除的列式表示机件（`VectorizedArrowReader.DeletedVectorReader`、`VectorHolder.DeletedVectorHolder`），Spark 的列式读取用它把 position delete 表达为删除掩码。但 `ArrowReader`/`VectorizedTableScanIterable` 这一便捷类**未接线**，遇 delete 直接抛异常。即"列式 + MOR"在 Iceberg 中可行，只是现成的独立 iceberg-arrow 入口不提供。

## 4. 关键问题解答

**Q1：'向量化快路径' 指的是 SDK 中读 Parquet 返回 Arrow 的路径？**

是，但需精确：它指 `iceberg-arrow` 的**列式**读取入口（`VectorizedTableScanIterable`）。行式回退路径同样返回 Arrow，只是内部先读成 `Record` 再转列。两条都"返回 Arrow"。

**Q2：需要 SDK 返回 Arrow，就无法 MOR 和（行级）谓词过滤？**

这是把两个不同的轴耦合了，需拆开：

- 轴 A：**输出格式**是否 Arrow——两条路径都输出 Arrow，输出 Arrow 本身**不**妨碍 MOR。
- 轴 B：**内部读取**是列式还是行式——MOR 与行级精确过滤目前只在**行式**内部读取（`DeleteFilter` / `filterRecords`）实现；现成的列式入口 `ArrowReader` 不做这两件事。

所以正确表述是：**用现成的 iceberg-arrow 向量化入口，就拿不到 MOR 和行级精确过滤；但改走行式回退（仍输出 Arrow）即可同时获得两者。** 二者并非"要 Arrow 就不能 MOR"，而是"要 MOR/行级过滤就得行式内部读取再转 Arrow"。第三条路（列式 + 删除掩码）机件存在但需自行接线，不在本期。

**Q3：回退为行式返回，性能差距有多大？**

差距显著（数倍量级），成因有三：① 每行 `Record` 对象物化与字段装箱，丧失列式批量读取；② 逐行 delete 谓词判定（equality delete 还需对 delete set 做逐行查找）；③ `Record → convertToArrowTable` 多一次行转列。Iceberg 引入 arrow 向量化读取的目的正是相对行式 generic 读取取得数倍吞吐提升，故含 delete 的表走行式回退，扫描吞吐通常有数倍差距。

但该差距**仅作用于确有 delete 文件的表**：干净表（无 delete）恒走向量化快路径，无此损失；copy-on-write 表也无 delete。差距范围因此局限于 V2 MOR 且存在未压实 delete 的表。

## 5. 对 FDW 设计的影响与统一口径

1. **谓词下推 = 裁剪，非精确过滤（向量化路径）。** FDW 把谓词下推进 `pushedFilter` 后，**仍须把同一谓词保留为 `ForeignScan` 节点 qual**，由执行器 `ExecQual` 逐行兜底，否则会多返回存活 row group 内不匹配的行。下推收益是跳过 manifest/文件/row group，不替代过滤。
   - 与 `core 接口清单 §1.5`「残差留节点 qual 由 ExecQual 施加」一致。
   - 与 `第一阶段开发计划 §1.3`「谓词全下推、节点 qual 为空、Reader 精确过滤」**冲突**，该处须修正为「下推用于裁剪 + 保留本地 qual 兜底」。
2. **delete 探测分路。** `openTable` 须先探测当前快照是否含 delete file：无则向量化快路径，有则本期报清晰错误（行式回退留后续）。与设计文档 §3.1.2 一致。
3. **裁剪与列裁剪为 SDK 自动能力**，FDW 只需正确传 `filter`/`select`，无需自行实现剪枝。
