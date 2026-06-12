# Iceberg-Rust SDK 能力核实（临时）

日期：2026-06-12。项目改用 Iceberg-rust SDK。本文以与下篇《Iceberg Java SDK 能力核实》一致的关注点（分区剪枝、列统计剪枝、行组剪枝、列裁剪、行级精确过滤、MOR），核实 iceberg-rust 的取数能力与接口，并核对 FDW 桥接层所需三项输入（行谓词、列投影、`metadata_location`）是否与 Java 一致。结论仅在与 Java 存在实质差异处对照说明。

## 1. 核实方法与源码版本

| 来源 | 内容 |
| --- | --- |
| 本地源码 | `catalog-service-research/src_ref/iceberg-rust`，main 分支 commit `427670d5`（2026-06-12）。核心读取能力集中于 crate `crates/iceberg`，本文锚点均相对 `crates/iceberg/src/` |
| 对照 | 本文档第二部分《Iceberg Java SDK 能力核实》；`oG-Iceberg-FDW SDK 接口与下推参数.md` |

下文结论均给出源码锚点（`文件:行`）。

## 2. 能力的阶段 / 输入 / 粒度 / 结果

各能力的称呼沿用第二部分的统一定义。iceberg-rust 的对应实现如下：

| 能力 | 阶段 | 输入 | 粒度（结果） | SDK 机制（实现） |
| --- | --- | --- | --- | --- |
| 分区剪枝 | 规划期 `plan_files()` | `with_filter` + manifest 分区摘要 / 文件分区值 | 跳过整片 manifest / 整个数据文件 | manifest 级 `ManifestEvaluator`（`scan/context.rs:225`）+ 文件级 `ExpressionEvaluator`（残差偏求值，`scan/mod.rs:524`） |
| 列统计剪枝 | 规划期 `plan_files()` | `with_filter` + 文件列统计（min/max/null) | 跳过整个数据文件 | `InclusiveMetricsEvaluator::eval`（`scan/mod.rs:529`） |
| 行组剪枝 | 执行期 `to_arrow()` | `with_filter` + Parquet 行组统计 | 跳过整个 row group | `RowGroupMetricsEvaluator`（`get_selected_row_group_indices`，`reader/pipeline.rs:326`）；**默认开启**（`row_group_filtering_enabled=true`，`scan/mod.rs:80`） |
| 页级行选择 | 执行期 `to_arrow()` | `with_filter` + Parquet page index | 跳过 row group 内的行区间 | `PageIndexEvaluator`（`get_row_selection_for_filter_predicate`，`reader/pipeline.rs:349`）；**默认关闭**（`row_selection_enabled=false`，`scan/mod.rs:81`） |
| 列裁剪（投影下推） | 执行期 `to_arrow()` | `select(cols)` | 只读选中列的 column chunk | `TableScanBuilder::select`（`scan/mod.rs:119`）→ `ProjectionMask`（`reader/pipeline.rs:227`） |
| 行级精确过滤 | 执行期 `to_arrow()` | `with_filter` 残差 | 逐行剔除不匹配行 | Arrow `RowFilter` / `ArrowPredicateFn`（`get_row_filter`，`reader/row_filter.rs:41`；挂载 `reader/pipeline.rs:323`） |
| MOR（delete 应用） | 执行期 `to_arrow()` | `FileScanTask.deletes` | 逐行删除 | 位置删除 → `RowSelection`（`build_deletes_row_selection`，`reader/pipeline.rs:359`）；等值删除 → 转 `Predicate` 与 filter 取 AND（`build_equality_delete_predicate`，`reader/pipeline.rs:265`） |

要点：

- 取数链单一：`StaticTable::from_metadata_file` → `scan()` → `select`/`with_filter` → `build()` → `to_arrow()`，返回 `ArrowRecordBatchStream`（异步流，逐批 `RecordBatch`）。前五项剪裁与列裁剪全部由 `filter`/`select` 驱动、在 `plan_files()`/`to_arrow()` 内部自动施加，调用方只需正确传入二者。
- 规划期两级分区裁剪 + 文件级列统计裁剪，与 Java 同构（`ManifestEvaluator`/`ExpressionEvaluator`/`InclusiveMetricsEvaluator` 同名）。
- **与 Java 的关键差异（详见 §4）：行级精确过滤与 MOR 在 rust 唯一的 Arrow 路径 `to_arrow()` 中默认实现**，无须像 Java 那样为获得二者而改走行式回退、再自写 `Record`→Arrow 转换。

### 2.1 行级精确过滤

`with_filter(Predicate)` 经 `plan_files()` 绑定为 `FileScanTask.predicate` 后，在 `to_arrow()` 内转为 Arrow `RowFilter`（`ArrowPredicateFn`，逐行求值产出布尔掩码，`reader/row_filter.rs:41-67`）挂到 Parquet 读取器（`with_row_filter`，`reader/pipeline.rs:323`）。即下推谓词在 rust 中**逐行精确生效**，而非仅 I/O 裁剪。

边界：谓词转换器目前仅支持顶层标量列（`project_column` 遇 struct 列报错，`reader/predicate_visitor.rs:263-274`）；引用文件中缺失列时按列值为 null 的语义退化为恒真/恒假（如 `is_null` 缺列恒真、`not_null` 缺列恒假，`reader/predicate_visitor.rs:362/379`），属正确的 schema 演进处理，非近似放行。

### 2.2 MOR（delete file）

- 规划：`plan_files()` 将 delete 文件按数据文件归并入 `FileScanTask.deletes`（`scan/task.rs:87`）；data manifest 与 delete manifest 分流处理，互窜会报 `FeatureUnsupported`（`process_data_manifest_entry`/`process_delete_manifest_entry`，`scan/mod.rs:501`/`:558`）。
- 应用：位置删除经 `RowSelection` 剔除（`reader/pipeline.rs:359-385`）；等值删除构造为 `Predicate` 与扫描 filter 取 AND 后一并交给 `RowFilter`（`reader/pipeline.rs:265-278`）。两类 delete 在 `to_arrow()` 中默认应用。
- 限制：等值删除列不支持 list/map 类型（`arrow/caching_delete_file_loader.rs:583-598`）。

## 3. 接口与 FDW 三项输入核对

FDW 桥接层驱动 SDK 取数所需的三项输入，rust 与 Java 一一对应、语义一致：

| FDW 所需输入 | Java | Rust | 是否一致 |
| --- | --- | --- | --- |
| `metadata_location` | `StaticTable` 从 metadata 路径打开 | `StaticTable::from_metadata_file(metadata_location, ident, file_io)`（`table.rs:364`） | 一致 |
| 列投影 | `scan.select(Collection<String>)` | `TableScanBuilder::select(cols)`（`scan/mod.rs:119`） | 一致 |
| 行谓词 | `scan.filter(Expression)` | `TableScanBuilder::with_filter(Predicate)`（`scan/mod.rs:99`） | 一致 |

补充：

- **谓词为表达式对象，非字符串**。rust 核心无 SQL 串解析器，桥接层须自行构造 `Predicate`（`Reference::new(col).equal_to(Datum)` 等，`expr/term.rs:44/142`；`and`/`or` 见 `expr/predicate.rs:563/591`）。此点与 Java 一致——Java 侧同样须把 `filter` 串反解析为 `Expression`。
- **凭证 / 存储配置**经 `FileIOBuilder::with_props` 注入（`io/file_io.rs:206`），对应 Java `open` 的 `storage_config`。
- **输出直接为 Arrow**：`to_arrow()` 内部完成 Parquet→Arrow，返回 `ArrowRecordBatchStream`，无须自写 `Record`→`VectorSchemaRoot`（与 Java 行式回退路径的关键差异，见 §4）。`next` 语义即推进该流取一批 `RecordBatch`。

## 4. 与 Java 的实质差异及对 FDW 设计的影响

| 维度 | Java（现成 iceberg-arrow 入口） | Rust（唯一 `to_arrow()` 路径） |
| --- | --- | --- |
| Arrow 路径数 | 两条：列式向量化 `ArrowReader` 与行式 generic 回退 | 一条：`to_arrow()` |
| 行级精确过滤 | 向量化路径不做（仅 row group 跳过）；行级过滤仅行式路径 | **默认做**（`RowFilter` 逐行） |
| MOR | 向量化路径遇 delete 抛 `UnsupportedOperationException`；MOR 仅行式路径 | **默认做**（位置 + 等值删除） |
| `Record`→Arrow 转换 | 行式路径须自写逐字段填 `VectorSchemaRoot` | 无须，库内直接产出 `RecordBatch` |

对 FDW 设计的影响：

1. **下推谓词在 rust 中是精确过滤，非仅裁剪。** 因此 Java 设计中"下推谓词须同时保留为 `ForeignScan` 节点 qual 由 `ExecQual` 兜底"的要求，对 rust 已成功下推（可转为 `Predicate`）的顶层标量谓词，就正确性而言不再必需；桥接层无法翻译为 `Predicate` 的谓词本就留作 `local_exprs` 兜底。是否保留 recheck 作为防御层属设计取舍，但已非 Java 那样的必选项。
2. **MOR 在 rust 为现成能力。** 含 delete 的表无须 Java 的"delete 探测分路 / 行式回退"，`to_arrow()` 默认处理位置与等值删除。若本期范围限定为无 delete 的表，可不依赖此能力；但 rust 不存在 Java"要 Arrow 就拿不到 MOR"的耦合。
3. **裁剪与列裁剪同为 SDK 自动能力**，FDW 只需正确传 `filter`/`select`，与 Java 一致。

---

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
| 输出格式 | `Data.exportVector` 导出 Arrow C Data | `Record` → **自写转换填 `VectorSchemaRoot`** → `Data.exportVectorSchemaRoot` 导出 Arrow C Data |
| 依赖模块 | `iceberg-arrow` + `iceberg-parquet` | `iceberg-data` + `iceberg-arrow`（仅导出） |

> 补充：`iceberg-arrow` 内已有位置删除的列式表示机件（`VectorizedArrowReader.DeletedVectorReader`、`VectorHolder.DeletedVectorHolder`），Spark 的列式读取用它把 position delete 表达为删除掩码。但 `ArrowReader`/`VectorizedTableScanIterable` 这一便捷类**未接线**，遇 delete 直接抛异常。即"列式 + MOR"在 Iceberg 中可行，只是现成的独立 iceberg-arrow 入口不提供。

> 更正：上表行式回退路径原写的 `convertToArrowTable` 并非 Iceberg 社区 API——源码核实 iceberg-arrow 仅提供 Parquet→Arrow 的列式读取，**无任何 `Record`→Arrow 现成转换**。该步须**自写**：逐字段建 `FieldVector`、按 Iceberg 类型映射 Arrow 类型、逐行填值组成 `VectorSchemaRoot`，再由 Arrow 库 `Data.exportVectorSchemaRoot` 导出 C Data（`exportVectorSchemaRoot` 是 Arrow 库现成，与自写的 `Record`→`VectorSchemaRoot` 衔接）。FallMoo fork（`org.apache.iceberg.arrow.bridge.NativeIcebergReader`）已实证此路径：`recordsToArrow` / `toArrowType` / `setVectorValue` 即手写转换，delete 应用仍用现成 `GenericDeleteFilter`。

## 4. 关键问题解答

**Q1：'向量化快路径' 指的是 SDK 中读 Parquet 返回 Arrow 的路径？**

是，但需精确：它指 `iceberg-arrow` 的**列式**读取入口（`VectorizedTableScanIterable`）。行式回退路径同样返回 Arrow，只是内部先读成 `Record` 再转列。两条都"返回 Arrow"。

**Q2：需要 SDK 返回 Arrow，就无法 MOR 和（行级）谓词过滤？**

这是把两个不同的轴耦合了，需拆开：

- 轴 A：**输出格式**是否 Arrow——两条路径都输出 Arrow，输出 Arrow 本身**不**妨碍 MOR。
- 轴 B：**内部读取**是列式还是行式——MOR 与行级精确过滤目前只在**行式**内部读取（`DeleteFilter` / `filterRecords`）实现；现成的列式入口 `ArrowReader` 不做这两件事。

所以正确表述是：**用现成的 iceberg-arrow 向量化入口，就拿不到 MOR 和行级精确过滤；但改走行式回退（仍输出 Arrow）即可同时获得两者。** 二者并非"要 Arrow 就不能 MOR"，而是"要 MOR/行级过滤就得行式内部读取再转 Arrow"。第三条路（列式 + 删除掩码）机件存在但需自行接线，不在本期。

**Q3：回退为行式返回，性能差距有多大？**

差距显著（数倍量级），成因有三：① 每行 `Record` 对象物化与字段装箱，丧失列式批量读取；② 逐行 delete 谓词判定（equality delete 还需对 delete set 做逐行查找）；③ `Record →（自写）VectorSchemaRoot` 多一次行转列。Iceberg 引入 arrow 向量化读取的目的正是相对行式 generic 读取取得数倍吞吐提升，故含 delete 的表走行式回退，扫描吞吐通常有数倍差距。

但该差距**仅作用于确有 delete 文件的表**：干净表（无 delete）恒走向量化快路径，无此损失；copy-on-write 表也无 delete。差距范围因此局限于 V2 MOR 且存在未压实 delete 的表。

## 5. 对 FDW 设计的影响与统一口径

1. **谓词下推 = 裁剪，非精确过滤（向量化路径）。** FDW 把谓词下推进 `pushedFilter` 后，**仍须把同一谓词保留为 `ForeignScan` 节点 qual**，由执行器 `ExecQual` 逐行兜底，否则会多返回存活 row group 内不匹配的行。下推收益是跳过 manifest/文件/row group，不替代过滤。
   - 与《5. Iceberg_FDW 详细设计》§3.3、§3.5「残差留节点 qual 由 ExecQual 施加」一致。
   - 已与《当期开发计划》§1.3、§3 阶段二「下推用于裁剪 + `local_exprs` 保留全部谓词兜底」对齐。
2. **delete 探测分路。** `openTable` 须先探测当前快照是否含 delete file：无则向量化快路径，有则本期不支持（行式回退留后续）。与设计文档 §3.1.2 一致。
3. **裁剪与列裁剪为 SDK 自动能力**，FDW 只需正确传 `filter`/`select`，无需自行实现剪枝。
