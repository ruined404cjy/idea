# openGauss Iceberg FDW：SDK 接口与下推参数（Iceberg-rust 版）

本文列出 FDW 取数路径上的两层接口——**FDW → rust 桥接层（C ABI）**与**桥接层 → iceberg-rust SDK（rust API）**——的函数签名、返回值与对应关系，以及下推参数。结论：行列下推所需参数随 open/scan 一次传入、SDK 内部自动施加，无需额外接口或单独传参。

源码版本：`catalog-service-research/src_ref/iceberg-rust`，commit `427670d5`，下文锚点相对 `crates/iceberg/src/`。各能力的语义与边界另见《Iceberg-SDK-能力核实（临时）》。

## 1. 两层接口与对应关系

### 1.1 FDW → 桥接层（C ABI）

桥接层对 FDW 暴露三函数。C ABI 形态由 openGauss FDW 需要决定（`MemoryContext`、Arrow C Data 出参），不随 SDK 由 Java 改 rust 而变；完整定义见《5. Iceberg_FDW 详细设计》§4.3。

```c
IcebergScan *iceberg_scan_open(MemoryContext cxt,
                               const char  *metadata_path,
                               const char  *storage_config,
                               const char **columns, int n_columns,
                               const char  *filter,        // 下推谓词编码，可空
                               ArrowSchema **out_schema);  // 投影后 schema

int  iceberg_scan_next (IcebergScan *scan,
                        ArrowArray  **out_array,           // 本批列数据
                        ArrowSchema **out_schema);         // 返回行数，0 = EOF

void iceberg_scan_close(IcebergScan *scan);
```

### 1.2 桥接层 → rust SDK，及两层对应

| FDW→桥接层 | 桥接层→rust SDK（签名与返回） | 对应关系 |
| --- | --- | --- |
| `iceberg_scan_open(...)` | ① `FileIOBuilder::new(factory: Arc<dyn StorageFactory>) -> FileIOBuilder`（io/file_io.rs:198）`.with_props(props) -> Self`（:212）`.build() -> FileIO`（:228）<br>② `StaticTable::from_metadata_file(metadata_location: &str, table_ident: TableIdent, file_io: FileIO) -> Result<StaticTable>` `async`（table.rs:364）<br>③ `StaticTable::scan(&self) -> TableScanBuilder`（table.rs:384）`.select(column_names: impl IntoIterator<Item = impl ToString>) -> Self`（scan/mod.rs:119）`.with_filter(predicate: Predicate) -> Self`（:99）`.with_batch_size(Option<usize>) -> Self`（:87）`.build() -> Result<TableScan>`（:189）<br>④ `TableScan::to_arrow(&self) -> Result<ArrowRecordBatchStream>` `async`（scan/mod.rs:464） | **一对多**：单个 `open` 展开为 ①–④ 调用链（外加 `filter`→`Predicate` 构造、runtime 驱动）。`IcebergScan*` 句柄内封装 ④ 的流（及其依赖的 `StaticTable`/`FileIO`/runtime） |
| `iceberg_scan_next(...)` | `<ArrowRecordBatchStream as TryStreamExt>::try_next(&mut self) -> Result<Option<RecordBatch>>`（`ArrowRecordBatchStream = BoxStream<'static, Result<RecordBatch>>`，scan/mod.rs:48） | **基本一对一**：`next` ↔ `try_next` |
| `iceberg_scan_close(...)` | 无显式 SDK 函数——`drop` 流 / `StaticTable` / `FileIO` | **一对一（语义）**：iceberg-rust 不设 close，资源随 rust 对象 `drop` 释放 |

### 1.3 参数 / 返回是否直传

经桥接层传递的参数**多为直传**，但有三处非直传（桥接层须转换），返回值统一经 `arrow::ffi` 转换：

| C ABI 参数 / 返回 | rust SDK 侧 | 直传？ |
| --- | --- | --- |
| `metadata_path` | ② `metadata_location: &str` | 是 |
| `columns` / `n_columns` | ③ `select` 的列名集合 | 是（仅列名字符串） |
| `storage_config` | ① `with_props` 的键值 | 否：单串 → 键值对 |
| `filter` | ③ `with_filter` 的 `Predicate` | **否：编码串 → `Predicate` 对象**（桥接层构造，见 §3） |
| `cxt` | — | 无 SDK 对应（openGauss 内存域，桥接层自用） |
| `table_ident`（无 C 入参） | ② `TableIdent` | 桥接层由 `(namespace, table)` 合成，静态表仅名义用途 |
| `factory`（无 C 入参） | ① `Arc<dyn StorageFactory>` | 桥接层内部选定存储后端（fs/s3 等） |
| `out_schema`（open） | `schema_to_arrow_schema(投影 schema)`（arrow/schema.rs:698） | 否：由投影 schema 导出，不读数据 |
| `out_array` + `out_schema`（next） | `RecordBatch`（自带 `schema()`）经 `arrow::ffi` 导出 | 否：`RecordBatch` → `FFI_ArrowArray`/`FFI_ArrowSchema` |
| `int`（next 返回） | `Option<RecordBatch>` | 否：行数 / `None`=0=EOF |

两点 rust 约束（Java/JNI 版不涉及）：

- **须在 tokio runtime 内调用**：②③④ 中的 `async` 调用，`from_metadata_file` 内部 `Runtime::try_current()?`（table.rs:376）脱离 tokio 上下文即报错。桥接层须自持 tokio runtime 并在其中驱动（如 `block_on`），地位相当于 Java 版的 JVM/JNI 生命周期。
- **C Data 导出由桥接层完成**：iceberg-rust 只交付原生 `RecordBatch`，无 FFI 导出；`out_array`/`out_schema` 由桥接层用 arrow crate 的 `arrow::ffi` 产生。

## 2. 下推参数与 SDK 自动能力

列裁剪、谓词裁剪、行级精确过滤、MOR 均在 `select`/`with_filter` 之后由 SDK 自动施加，无额外接口。参数全部来自上表的 `columns`、`filter` 或规划期归并的 `deletes`，运行期无需再传。

| 能力 | SDK 内部调用 | 参数来源 |
| --- | --- | --- |
| 列裁剪 | `select` 在 `build()` 内按名解析为 field_id（scan/mod.rs:256），下游 `ProjectionMask` 按 field_id 投影 | `columns` |
| 谓词绑定 | `with_filter(Predicate)`（scan/mod.rs:99） | `filter` |
| 分区剪枝 | `plan_files()` 内 `ManifestEvaluator`（scan/context.rs:225）/ 文件级 `ExpressionEvaluator`（scan/mod.rs:524） | 自动（`filter` 已绑定） |
| 列统计剪枝 | `plan_files()` 内 `InclusiveMetricsEvaluator::eval`（scan/mod.rs:529） | 自动（`filter` 已绑定） |
| 行组剪枝 | `to_arrow()` 内 `RowGroupMetricsEvaluator`（arrow/reader/pipeline.rs:326），默认开启（scan/mod.rs:80） | 自动（`filter` 已绑定） |
| 行级精确过滤 | `to_arrow()` 内 Arrow `RowFilter`（arrow/reader/row_filter.rs:41，挂载 pipeline.rs:323） | 自动（`filter` 残差，逐行求值） |
| MOR（delete 应用） | `to_arrow()` 内：位置删除 → `RowSelection`（pipeline.rs:359）；等值删除 → 转 `Predicate` 与 filter 取 AND（pipeline.rs:265） | 自动（`FileScanTask.deletes`，规划期归并） |
| 列式批读 | `to_arrow()` → `ParquetRecordBatchStreamBuilder`（arrow/reader/pipeline.rs:211） | `batch_size`（默认或 `with_batch_size`，决定 `next` 单批行数） |

分区 / 列统计剪枝在 `plan_files()` 内，行组剪枝 / 行级过滤 / MOR 在 `to_arrow()` 的读取器内，均随 `filter`（及 `deletes`）自动生效。

## 3. 投影与谓词按列名而非 field_id

`select` 与 `Predicate` 的字段引用均为**列名字符串**，SDK 在 `build()`/`bind()` 时才按名解析为 field_id（`field_id_by_name`，scan/mod.rs:256）；field_id 仅为 SDK 内部表示，公开接口不接收 field_id。由此桥接层须注意：

- 即便上层 FDW 以 field_id 维系列身份（规避列改名），传入 SDK 前仍须先经 schema 把 field_id 映射为**当前快照列名**；无法直接下传 field_id。
- 仅支持**顶层标量列**：引用嵌套字段（struct 子列）在 `build()` 报错（scan/mod.rs:266）；投影列名不存在亦报错（scan/mod.rs:230）。
- 谓词字段同理，须为当前顶层列名（`Reference::new(name)`，expr/term.rs）；`filter` 由桥接层按列名构造 `Predicate`（非 SQL 串）。

## 4. 与 Java 版的实质差异

rust 取 Arrow 仅 `to_arrow()` 一条列式路径，但较 Java 的向量化快路径多做两事，影响 FDW 设计：

1. **下推谓词为行级精确过滤，非仅 I/O 裁剪。** `filter` 经 `RowFilter` 逐行生效（pipeline.rs:323）。故对已成功转为 `Predicate` 并下推的顶层标量谓词，把同一谓词保留为 `ForeignScan` 节点 qual 由 `ExecQual` 复核，非正确性所必需（可作防御层保留）。桥接层无法翻译为 `Predicate` 的谓词仍按 `local_exprs` 由执行器兜底。
2. **MOR 默认支持。** 位置与等值删除在 `to_arrow()` 中默认应用（pipeline.rs:359/265），无须 delete 探测分路或行式回退。本期如限定无 delete 表，可不依赖此能力，但不存在 Java「要 Arrow 就拿不到 MOR」的耦合。
