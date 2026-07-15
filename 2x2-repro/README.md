# 2×2 索引扫描验证：当前功能、代码现状与复现

## 1. 目标与范围

2×2 指 `{标量, 向量} × {全表, 索引}` 四条 FDW 扫描路径。本文给出在单一 fs 环境（无需 MinIO/REST）连续验证四条路径的可复现流程，并说明当前功能、各仓代码现状与流程中的非用户操作。

验证对象是三条链路：catalog 建索引、FDW 规划期索引发现、FDW 从 `BeginForeignScan` 起的执行链。四条扫描路径全部为真实链路：索引由 `iceberg_catalog.create_index` 真建，标量与向量的索引选路由 FDW 按 `metadata_location` 探 bridge 自动完成。

## 2. 当前功能

扫描模型是正交两轴：`{标量, 向量}` 是列属性（分发键），`{全表, 索引}` 是扫描方法。两个"全表"格合一，落地为 3 条执行器→bridge 路径，由 `ForeignScan.fdw_private` 顶层 `scan_method` 分发（`IcebergFdwScanMethod`：FULL=0、VECTOR_INDEX=1、SCALAR_INDEX=2）。

| 路径 | 索引创建 | 规划期发现 | bridge 数据面 |
|---|---|---|---|
| ① 标量 × 全表 | 无 | `baserestrictinfo` → `sdk_filter` | `iceberg_bridge_scan_open` + `filter_json` 谓词下推 |
| ② 标量 × 索引 | `create_index ... 'btree'` | 谓词翻译 + 按列名探 btree | `iceberg_index_rs_search_scalar_by_metadata`（BTree，type=200） |
| ③ 向量 × 全表 | 无 | 内核 PR#3 topK；未命中索引退回全表 | `iceberg_bridge_scan_open` + 内核 VectorSearch 做 TopK |
| ④ 向量 × 索引 | `create_index ... 'ivf_flat'` | 内核 PR#3 + bridge 探 IVF | `iceberg_index_rs_search_vector_by_metadata`（IVF） |

两索引路径共享 `iceberg_index_rs_metadata_scan_{schema,next_batch,rescan,close}` 排空 + `iceberg_index_rs_abi_version`（==3）。

### 2.1 向量索引发现

- 内核 PR#3 把 `ORDER BY vec <-> const LIMIT k` 解析为 `VectorSearch` 算子，并将 `VectorSearchTopKInfo`（`relid`/`vector_attnum`/`query_vector`/`topK`/`fetchK`/`metric`）挂到 `root->vectorsearch_topk_infos`。
- FDW `icebergGetForeignRelSize` 调 `icebergBuildVectorPathInfo`：从 `vectorsearch_topk_infos` 取本表 topK 信息，按 `metadata_location` 调 `iceberg_vector_index_match_available` 探 bridge 是否存在覆盖该向量列的索引。
- 命中索引 → 合成 `VECTOR_INDEX` path（路径④）；未命中 → 返回 NULL 退回全表（路径③）。

路径③④ 查询写法相同（`ORDER BY <-> LIMIT`），区别只在 `metadata_location` 是否带索引。`VectorSearch` 算子始终在 `Foreign Scan` 之上：索引路径按 `FetchK` 过取候选（默认 50），再由 `VectorSearch` 重排到 `TopK`。

### 2.2 标量索引发现

- FDW `icebergGetForeignPlan` 在无向量 topK（`plan_state->path_info == NULL`）时调 `icebergBuildScalarPathInfo`，对已分类的下推谓词（`quals.sdk_filter`）做标量索引发现。
- `iceberg_scalar_index_match_available` 逐谓词：①把谓词翻译为 bridge 标量表达式 JSON（`{"field_id","op","value"}`，算子 eq/lt/le/gt/ge，类型 int64/utf8/float64/bool）；②经 `iceberg_index_probe_index_name(...,"Scalar")` 按 `metadata_location` 调 `match_index` 并按**列名**筛选覆盖该列、非 partial 的 btree 索引。首个「可翻译 + 命中索引」的谓词合成 `SCALAR_INDEX` path，否则退回全表。
- 列由索引名选定，不靠 field_id：btree 单列、搜索只用 `op`+`value`，`expression.field_id` 不被读取（仅信息性，取列映射位置序值）。
- 正确性兜底：下推谓词在执行计划中保留为 local recheck（`Filter Mode: recheck` + 保留的 `Filter:` qual），过返由 recheck 滤除，`is_partial==false` 门控防欠返。

路径①② 查询写法相同（`WHERE id > 3`），区别只在 `metadata_location` 是否带索引（基线→全表①，带 btree→索引②）。

## 3. 流程中的非用户操作

四条扫描路径与索引发现都是真实链路；索引创建（`create_index`）也是用户 SQL。流程中仍有两类操作非用户 SQL，源于栈当前能力边界：

1. **直接供数**：`provision_local`（标量）+ `file_scan_vector`（向量）。栈无 SQL 写路径（FDW 只读、无 `INSERT`），故经 iceberg-rust 直接写 parquet + 快照 + metadata，产出带数据的基线 `metadata_location`。
2. **元数据指针接线**：`UPDATE tables_internal SET metadata_location = <基线>`。供数在 catalog 之外完成，`create_table` 注册的是空表，须把指针改指供数数据。`create_index` 以此为 base 建索引并 CAS 回写带索引版本。

向量外表不再需要手工重建 relid：向量列的 `create_table` schema 携 `vector_dim:N` 属性，catalog 自动外表直接映射为 `vector(N)`（PR#73），`create_table` 已登记 relid，四张外表均直接可用。

## 4. 代码现状（各仓 HEAD）

| 组件 | 仓库 | 分支 / HEAD | 与 2×2 相关现状 |
|---|---|---|---|
| 内核 | `DataInfraLab/openGauss-server-datainfra` | `datainfra_dev` b40e2a2b8 | PR#3 已并：`VectorSearchTopKInfo` + `vectorsearch_topk_infos` 贯穿规划期 |
| bridge | `DataInfraLab/iceberg-rust-bridge` | `main` ef28da5 | `search_{scalar,vector}_by_metadata` + `build_index_by_metadata` + `metadata_scan_*` + `abi_version==3`，四路数据面齐备 |
| index | `DataInfraLab/iceberg-index` | `main` f5a4030 | BTree + IVF_FLAT + IVF_PQ build 齐备 |
| fdw | `DataInfraLab/iceberg_fdw` | `main` ad99a92（功能基线 50515c5） | 执行器四路齐备；向量与标量索引发现均真实接入；支持 `double` 列（声明/投影/解码/下推） |
| catalog | `HardingHang/Catalog` | `main` 8a769e5 | `create_index` 索引类型无关、透传 bridge；`list<float>+vector_dim:N → vector(N)` 自动外表映射（PR#73） |
| delta | `DataInfraLab/iceberg_delta` | `master` dd121c0 | 写/MOR hook，与扫描正交 |

catalog 已知问题（非 2×2 阻塞）：无 `vector_dim` 的 `list<float>`（及 `map`/`struct` 等其他嵌套类型）在 `create_table` 自动外表 DDL 生成时因 `parse_schema_fields` 未初始化 `sql_type` 而产出非法类型，报 "Create foreign table failed: function returned NULL"。本套件所有向量列均带 `vector_dim`，不触发此路径。

## 5. 复现步骤

### 5.1 占位符（按本机取值，下文命令统一引用）

| 占位符 | 含义 |
|---|---|
| `$GAUSSHOME` | openGauss 安装目录（含 `bin/pg_config`、`lib`） |
| `$PGDATA` / `$PGPORT` | 实例数据目录 / 端口 |
| `<kernel>` | 内核源码树根；`<kernel>/src/include` 供 FDW 编译 |
| `<bridge>` | iceberg-rust-bridge 仓根；`.so` 为 `<bridge>/target/release/libiceberg_rust_bridge.so` |
| `<iceberg_fdw>` | iceberg_fdw 仓根 |
| `<catalog>` | openGauss-Catalog 仓根 |
| `<wh_scalar>` / `<wh_vec>` | 标量 / 向量本地仓库目录（任意可写空目录） |
| `<suite>` | 本套件所在目录 |

构建/cargo 一律前置 `env -u LD_LIBRARY_PATH`（避开 openGauss 旧 libstdc++ 断系统 cc1）；gaussdb 运行须 `LD_LIBRARY_PATH=$GAUSSHOME/lib`。

### 5.2 构建并安装 FDW 与 catalog

```
cd <iceberg_fdw>
git checkout main && git pull
PGC=$GAUSSHOME/bin/pg_config ; SRC=<kernel>/src/include
env -u LD_LIBRARY_PATH make PG_CONFIG=$PGC OPENGAUSS_SRC_INCLUDE=$SRC -j4
env -u LD_LIBRARY_PATH make PG_CONFIG=$PGC OPENGAUSS_SRC_INCLUDE=$SRC install

cd <catalog>
git checkout feat/create-index-btree     # PR #71 合并后用 main
env -u LD_LIBRARY_PATH make PG_CONFIG=$PGC install
```

FDW 引用内核 PR#3 的 `vectorsearch_topk_infos`，须在已含 PR#3 的内核（`datainfra_dev`）上构建。catalog 须含 `create_index` 的 btree 字符串映射（`feat/create-index-btree` 分支 / PR #71）。

### 5.3 以 fs 环境重启 gaussdb

```
export GAUSSHOME=<...> PGDATA=<...>
export PATH=$GAUSSHOME/bin:$PATH LD_LIBRARY_PATH=$GAUSSHOME/lib
export ICEBERG_RUST_BRIDGE_SO=<bridge>/target/release/libiceberg_rust_bridge.so
gs_ctl restart -D $PGDATA -Z single_node
```

FDW 扫描的存储配置取自外表 server 选项（warehouse=`file://` → fs），与 env 无关。

### 5.4 供数（fs）

标量表（上游 example）：

```
cd <bridge>
env -u LD_LIBRARY_PATH PATH=$HOME/.cargo/bin:$PATH \
  cargo run --release --example provision_local -- <wh_scalar>
```

`provision_local` 建 `default.t`（`id int`、`name string`、`score double`，5 行 id=1..5），打印的 `metadata_location` 记为 `SC_BASE`。

向量表（本套件自带的 Rust 集成测试，须在 bridge 仓内编译后运行）：

```
cp <suite>/file_scan_vector.rs <bridge>/tests/
cd <bridge>
env -u LD_LIBRARY_PATH PATH=$HOME/.cargo/bin:$PATH \
  ICEBERG_FILE_SCAN_WAREHOUSE=file://<wh_vec> \
  ICEBERG_FILE_SCAN_NAMESPACE=vscan_ns ICEBERG_FILE_SCAN_TABLE=scan_table \
  cargo test --release --test file_scan_vector file_scan_vector -- --nocapture
```

`file_scan_vector` 建 `vscan_ns.scan_table`（`c_integer int`、`c_vector list<float>` dim3，5 行 `[1,0,0]/[2,0,0]/[1,2,0]/[1,0,3]/[5,0,0]`），打印的 `METADATA_LOCATION` 记为 `VEC_BASE`。

索引不在此步建：`twobytwo.sql` 内由 `create_index` 真建 btree / ivf_flat 并 CAS 回写带索引 metadata。

### 5.5 接线并跑 2×2

```
gsql -d postgres -p $PGPORT -c "CREATE DATABASE twobytwo;"
gsql -d twobytwo -p $PGPORT \
  -v sc_base="'$SC_BASE'" -v vec_base="'$VEC_BASE'" \
  -f <suite>/twobytwo.sql
```

脚本在单一 `iceberg_catalog_server`（fs）下注册 4 张表（向量列携 `vector_dim:3`，自动外表即 `vector(3)`），把元数据指针改指供数基线，经 `create_index` 在 `sc_idx`/`vec_idx` 上真建 btree/IVF，再逐条 EXPLAIN + 实扫。四张外表均直接用 `create_table` 自动建的，不再重建。namespace/table 对 bridge 是未校验 label，真实定位靠 `metadata_location`（`StaticTable::from_metadata_file`）。

## 6. 验证结果

| 路径 | EXPLAIN 关键行 | 行结果 |
|---|---|---|
| ① 标量×全表 | `bridge full scan` · `SDK Filter: present` · `Filter Mode: exact` · `SDK Predicate Count: 1` | `4 bob` / `5 eve` |
| ② 标量×索引 | `bridge scalar index scan` · `Index Name: idx_id` · `Predicate Columns: id` · `Filter Mode: recheck` | `4 bob` / `5 eve` |
| ③ 向量×全表 | `Vector Search(TopK 3, FetchK 50, L2)` → `bridge full scan` | `c_integer` = 1,2,3 |
| ④ 向量×索引 | `Vector Search(TopK 3, FetchK 50, L2)` → `bridge vector index scan` · `Index Name: idx_vec` · `FetchK 50` | `c_integer` = 1,2,3 |

路径② EXPLAIN：

```
 Foreign Scan on demo.sc_idx
   Filter: (sc_idx.id > 3)
   Iceberg FDW: bridge scalar index scan
   Index Name: idx_id
   Predicate Columns: id
   Filter Mode: recheck
```

路径④ EXPLAIN：

```
 Limit
   ->  Vector Search
         Algorithm: TopK   Metric: L2   TopK: 3   FetchK: 50
         ->  Foreign Scan on demo.vec_idx
               Iceberg FDW: bridge vector index scan
               Index Name: idx_vec
               Vector Column: c_vector
               Metric: L2   TopK: 3   FetchK: 50   Query Vector Dim: 3
```

`create_index` 在 `iceberg_catalog.table_indexes` 注册 `idx_id`（sc_idx）与 `idx_vec`（vec_idx）。①② 同解（4,5）表明全表谓词下推与 btree 索引一致；③④ 同解（1,2,3）表明 VectorSearch-over-full-scan 与 IVF 索引路径等价。①②、③④ 的差异均仅在 `metadata_location` 是否带索引，证明标量/向量发现都按索引存在性自动选路。

## 7. 说明

- **存储无关**：bridge by_metadata 索引（向量 IVF、标量 btree）的 build/search 均经 `metadata_location` + `FileIO` 定位，fs/s3 皆可；本流程全程 `file://`。
- **类型白名单**：FDW 外表列支持 `int2/int4/int8`、`double precision`、`text/varchar/bpchar`、`vector(n)`；标量表的 `score double` 列经自动外表可直接声明/投影/下推。
- **snapshot**：by-metadata 搜索由 `metadata_location` 固定快照，不消费 `snapshot_id`。
- **多谓词**：标量发现取单个可下推谓词合成索引路径，其余谓词靠 local recheck 兜底。

## 8. 文件清单

本套件为自包含目录，文件同级：

| 文件 | 说明 |
|---|---|
| `README.md` | 本文 |
| `twobytwo.sql` | 单环境 2×2 接线 + 建索引 + 实扫脚本（`-v sc_base`/`vec_base`） |
| `file_scan_vector.rs` | 向量表供数器（bridge 集成测试，运行前复制进 `<bridge>/tests/`） |
| `twobytwo-flow.svg` | 2×2 规划期→执行器→bridge 分发概览 |

执行器实装见 FDW `src/scalar_index_scan_adapter.cpp`、`src/index_scan_adapter.cpp` 与 `src/iceberg_fdw.cpp` 的标量/向量分流；建索引见 catalog `src/index.cpp`。
