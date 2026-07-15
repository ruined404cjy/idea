# 并行向量索引扫描（路径④）开发与测试现状

日期：2026-07-15
运行环境：openGauss 单机，端口 26000，测试库 `r2x2_0704`

## 1. 范围

本文汇总并行向量索引扫描（路径④，hybrid partition parallel）的本地改动、机制、测试与遗留。路径④指对 `iceberg_fdw` 外表执行 `ORDER BY vec <-> const LIMIT k`、且该表带向量索引时的并行执行：内核 `VectorSearch` 算子在 `Streaming(LOCAL GATHER)` 之上做全局 top-K，各 worker 按分区分片经 bridge 取候选。栈由内核、FDW、bridge、index、catalog、delta 六组件构成，本轮改动集中在前四者。

## 2. 本地改动

各仓分支与改动范围：

| 仓 | 分支 | 改动 | 提交状态 |
|----|------|------|---------|
| 内核 | `feat/parallel-scan-kernel-support` | 门2 stream 泄漏修复、VectorSearch 补 `exec_nodes`、重扫复用、gather 守卫 | 已 commit（未合入 datainfra_dev） |
| FDW | PR 分支 `feat/parallel-vector-index-hybrid` | 并行④ hybrid 决策 + 部分覆盖 is_partial 路由 | 见 §5，PR #19 |
| bridge | PR 分支 `feat/parallel-vector-index-hybrid-scan` | hybrid 混合扫描数据面 | 见 §5，PR #68 |
| index | `winter-parallel` | Interface 3/4（分支自带）+ 本地单分区守卫解除、vacuum stub | 工作区改动 |

### 2.1 内核

三文件，实现并行执行的内核前置：

- `plancache.cpp`：`ResetStream` 回复条件加 `|| IS_SINGLE_NODE`，修单机下 `is_stream` 经嵌套 SPI 泄漏为 0 导致跳过 `create_local_gather` 而串行。
- `planner.cpp`：`VectorSearch` 构造后 `inherit_plan_locator_info` 补 `exec_nodes`（缺失则并行向量执行期 SIGSEGV）；`Limit→Sort→VectorSearch` 重写在 SMP 下命中同一 gather 放宽。
- `nodeVectorSearch.cpp`：`ExecReScanVectorSearch` 无参数变化时复用已物化 top-k、不重扫 Stream 子节点。

门1（`exec_type`）在 FDW 侧化解（`make_foreignscan` 显式传 `EXEC_ON_DATANODES`）。

### 2.2 FDW

**并行④ hybrid 决策**（`iceberg_fdw.cpp`/`plan_scan_adapter.cpp`/`sdk_scan_adapter.cpp`/两 header）：分区表 + `query_dop>1` 时下发 `scan_method=FULL` + `task_group` + `index_profile`（`wire_version:1`、`mock_all_index:false`、`index_binding.index_name`），`query_vector` 经 `fdw_private[4]` 带外传执行期；否则走串行直连 `VECTOR_INDEX`。`path.dop=query_dop`，并行区 dop 一致。

**部分覆盖 is_partial 路由**（`index_scan_adapter.cpp`/`iceberg_fdw.cpp`/`iceberg_fdw.h`）：向量发现 `icebergIdxFindIndexNameByKind` 加 `require_full`+`out_is_partial`，`require_full=false`（向量）接受部分覆盖并透传 `is_partial`，标量②/通用 probe 仍只接受全覆盖。`GetForeignPlan` 按路径分级：

```
向量索引路径
 ├─ 并行(dop>1) ────────────► hybrid（bridge 处理 uncovered→FALLBACK）
 ├─ 非并行 & is_partial=true ─► 退全表 VectorSearch③（直连无 FALLBACK 会漏 uncovered 行）
 └─ 非并行 & is_partial=false ► 原直连 VECTOR_INDEX
```

### 2.3 bridge

**hybrid 混合扫描数据面**（`arrow_stream.rs`/`scan.rs`/`iceberg_bridge.h`）：`build_hybrid_arrow_stream` 把一个 task group 的 task 按 `execution_source` 拆 INDEX / FALLBACK 两相，`INDEX_THEN_FALLBACK` 链接。真实路径 `hybrid_real_index_streams`（`mock_all_index:false`）：

- Interface 3 `plan_index_files_by_metadata` 派生覆盖，取每个 covered 文件的 segment（按 `segment_id` 去重）；
- covered 段经 Interface 4 做 ANN，返回 ≤ `fetch_k` 候选（含尾 `_distance`），`strip_distance_column` 剥距离列；
- uncovered 文件 → FALLBACK 全扫；
- `over_fetch_k = fetch_k`，每 worker 独立过取完整 `fetch_k`（不按 dop 均分）；全局 top-K 由内核精确重排保证。

`segment ⊆ group` 由 `segment⊆partition`（index build 保证）+ `partition⊆group`（bridge 分区原子分桶）传递保证。`HybridScanRequest` 顶层 `wire_version:1` 必填。

### 2.4 index

`winter-parallel` 分支自带 Interface 3/4（`plan_index_files_by_metadata`/`index_scan_by_metadata`）+ RowAddress 直连。本地工作区改动：解除单分区 PoC 守卫（多分区 e2e 需要）、`vacuum_index_by_metadata` 返回 `Unsupported` 的极简 stub（satisfy bridge 编译）。

### 2.5 改动与设计文档对应

| 改动 | 对应文档 |
|------|---------|
| 并行④ hybrid 数据面 | `iceberg_fdw/design/iceberg-fdw-parallel-scan-design.md`、`/sda/parallel-scan-implementation-milestones.md`、`iceberg-index/docs/index-parallel-scan-design.md` |
| 部分覆盖 is_partial 路由 | 本文 §2.2、§3 |

## 3. registry 定位与部分覆盖机制

### 3.1 registry 与 statistics 的结构

索引的覆盖信息（各索引的 segment、`covered_data_files`）组成 `SnapshotIndexRegistry`，序列化为 JSON 存入一个 Puffin blob（类型 `REGISTRY_BLOB_TYPE = "huawei.gauss-infra.index-meta-v1"`）。该 puffin 由 Iceberg metadata 的一条 statistics-file 引用：

```
metadata.statistics[]  (按 snapshot_id 为键的 map)
  └─ StatisticsFile { snapshot_id, statistics_path → puffin, blob_metadata:[{type=REGISTRY_BLOB_TYPE, snapshot_id}] }
        └─ puffin 内 blob = SnapshotIndexRegistry
```

`StatisticsFile.snapshot_id` 与 registry `BlobMetadata.snapshot_id` 由 `write_registry_statistics_file(table, snapshot_id, payload)` 用**同一参数**设置，二者恒相等，均等于**构建索引时的 current 快照**，不是表当前 current 快照。二者相等当且仅当构建之后未再写数据。

一张表可有多条 statistics（多次构建各留一条，键为各自构建时快照，`set_statistics` 只替换同键项、不移除其他）。

### 3.2 registry 定位

`find_registry_puffin(snapshot_id)` 遍历 statistics，按 `stat.snapshot_id == snapshot_id`（严格相等）取 registry，调用方传入表 current 快照。该定位依赖 registry 挂在 current 快照上。

### 3.3 覆盖计算（per-segment）

`plan_coverage(entry, plan, snapshot)` 用 current 快照的活文件集，按 segment 原子重算覆盖：`effective_files = covered_data_files ∩ live_files`，仅当 segment 下辖文件**全部仍存活**才复用该 segment（`effective_files.len() == covered_data_files.len()`），任一被删/改则整段失效。未被任何有效 segment 覆盖的文件落 uncovered。语义：

- segment 外追加文件 → 原 segment 不变（仍 indexed），追加文件 uncovered；
- segment 内文件增删改 → 该 segment 失效（下辖文件全落 uncovered）。

### 3.4 部分覆盖对 registry 提交时机的要求

部分覆盖要求 registry 挂在表 current 快照上（§3.2 严格定位）。数据写入产生新 current 快照，故构建索引后再写数据会使 registry 停在旧（祖先）快照，current 快照上无 registry，索引对发现不可见。解法：**bridge 在数据 commit 时原子更新 statistics（含 snapshot_id），使 registry 始终随当前快照**。在此提交语义下，`covered_data_files` 覆盖旧文件、新写文件 uncovered，形成部分覆盖。

被 uncovered 的文件的向量**不在任何 segment 索引内**（segment 索引按构建时文件子集建立），故 Interface 4 只搜 covered 段、FALLBACK 全扫 uncovered 文件，二者不重复。

## 4. 测试

图例：✅ 通过 · ⚠️ 暴露问题 · 📐 已设计未执行

### 4.1 fixture

| fixture | 构造 | 说明 |
|---------|------|------|
| `demo.vpart` | 2 分区各 1 文件、6 行、IVF num_clusters=1 | 全覆盖 |
| `demo.vpart_pc` | 建索引覆盖 {f0,f1}→追加 f0b→registry 重挂 current | registry@current 部分覆盖（= 提交时原子更新后的形态），covered 2/3、uncovered 1；`c_integer=7 vec=[0.5,0,0]`，7 行 |

### 4.2 测试矩阵与结果

| 场景 | 观测 | 结果 |
|------|------|------|
| 基线④ dop=2/4 全覆盖 | EXPLAIN + 结果 | ✅ `VectorSearch(TopK3,FetchK50)→Streaming(LOCAL GATHER)→Foreign Scan`，`[1,2,3]` |
| 投影变体（`SELECT *`/子集/含向量列/含分区列） | 列映射 | ✅ `c_vector` 自动纳入投影供距离计算 |
| 谓词+④（`c_shard=0`/`c_integer>3`） | 结果 | ✅ `[1,3,5]`/`[4,5,6]`（识别过取-过滤欠返规模风险） |
| topK/查询向量/tie（LIMIT 1/5/6，`[4,0,0]`/`[3.5,0,0]`） | 结果 | ✅ top-K 集合正确；tie-break 顺序随 dop 变（预期） |
| **部分覆盖发现**（`demo.vpart_pc`，registry@current） | `match_index` | ✅ `is_partial:true, covered 2/3, segment 2` |
| **部分覆盖 hybrid** dop=4 | EXPLAIN + 结果 | ✅ `Streaming(LOCAL GATHER)→bridge full scan`；`[1(0),7(0.5),2(1)]`——追加行 7 经 uncovered→FALLBACK 纳入，**FALLBACK 分支携真实行跑通** |
| **部分覆盖 dop=1** | EXPLAIN + 结果 | ✅ 无 Streaming、退全表③；`[1,7,2]` 正确，不触发 Arrow bug |
| 部分覆盖全量 dop=4 LIMIT 7 | 结果 | ✅ `[1,7,2,3,4,5,6]` 距离全序、7 行全参与 |
| 回归 全覆盖 dop=4 | 结果 | ✅ `[1,2,3]` 不变（门只重路由 partial） |
| 回归 全覆盖 dop=1 | 结果 | ⚠️ 仍报 `invalid Arrow list array`（预存 bug，见 §6） |

判据：dop=4 部分覆盖若 FALLBACK 断裂则追加行 7 缺失、退化为 `[1,2,3]`；实测含 7，证 uncovered→FALLBACK 真实执行。

### 4.3 未覆盖场景

| 场景 | 方法 |
|------|------|
| 多簇 IVF + nprobe + recall | 现 fixture num_clusters=1 退化为暴力；多簇 + 更大数据，变 nprobe 与暴力 top-K 比对 recall，验剪枝与 `over_fetch_k` 补偿 |
| candidate = dop × fetchK | bridge/FDW 加临时计数或 tracing，断言候选量 ≈ group_count × fetch_k |
| segment⊄partition 不变式 | 逐文件校验已移除，构造畸形 registry 验行为 |
| 错误/边界 | wire_version 缺失/错值、index_name 不存在、CancelCheck 取消、segment 内文件删改后失效 |
| 规模/吞吐 | 更大行数、分区数 > dop、dop∈{8}（受 bridge 单线程 runtime 限制，见 §6） |

## 5. PR 与合入状态

| 仓 | PR | base | git 可合入 | 编译 |
|----|----|------|:---------:|------|
| FDW | [#19](https://github.com/DataInfraLab/iceberg_fdw/pull/19) | main(82d9656) | ✅ MERGEABLE/CLEAN | 依赖 bridge hybrid + index Interface 3/4 |
| bridge | [#68](https://github.com/DataInfraLab/iceberg-rust-bridge/pull/68) | main(851cef5) rebase 后 | ✅ MERGEABLE/CLEAN | 待 index ABI 定稿（见下） |

**编译依赖（index ABI 定稿）**：bridge main 上，`#67`（parallel index build）与 `#66`（maintenance）已引入对 index/storage ABI 的调用，且 index 的 Interface 4 扫描接口在演进（`index_scan_by_metadata` 一版与 `ScanRequest`/`ScanCursor`/`SCAN_WIRE_VERSION` 一版并存）。bridge main + hybrid 编译需要：

1. index ABI 收敛到一个分支，同时含 `#67` 的 parallel-build 接口与 Interface 3/4，命名与 bridge main 期望一致；
2. hybrid `arrow_stream.rs` 与 `#67` 的 `parallel_build_abi.rs` 适配到最终 Interface 3/4 ABI；
3. storage 侧提供 `iceberg_storage_opendal::maintenance` 的 ABI（`#66` 依赖）。

三者对齐前 bridge main 不能整体编译。PR #68 已在 git 层可合入，编译对齐待 index 定稿后进行。

## 6. 遗留问题

| 级别 | 问题 | 说明 |
|------|------|------|
| Bug（正交） | 全覆盖 dop=1 直连 `invalid Arrow list array` | 直连 `VECTOR_INDEX`（OLD `search_vector_by_metadata`）报错，`sdk_scan_adapter.cpp` `icebergArrowVectorDatum`。引擎层返回合法 `List<Float32>`，故障在 C-Data 导出/FDW 列映射边界，由 vpart 3 数据列触发。is_partial 门顺带绕开**部分覆盖**的该路径，**全覆盖** dop=1 仍触发 |
| 编译依赖 | index ABI 定稿 | 见 §5 |
| 性能未解 | bridge 单线程 runtime | 全局 `BLOCK_ON_LOCK` 使 N worker 的 batch 生产串行；本轮只验计划形态与数据面正确性，吞吐 scaling 待 bridge 改多线程/per-scan runtime |
| 调优点 | per-worker over-fetch | 每 worker 过取完整 `fetch_k`（不按 dop 均分）；正确性无需改，可选调优 |
| 临时改动 | 单分区 PoC 守卫解除、vacuum stub | 跑通本地 e2e 的临时措施 |

## 7. 附录

### 7.1 复现

```sql
SET enable_vectorsearch = on;
-- 全覆盖 dop=4
SET query_dop = 4;
SELECT c_integer FROM demo.vpart ORDER BY c_vector <-> '[1,0,0]' LIMIT 3;      -- [1,2,3]
-- 部分覆盖 hybrid（registry@current）
SELECT c_integer FROM demo.vpart_pc ORDER BY c_vector <-> '[1,0,0]' LIMIT 3;   -- [1,7,2]
-- 部分覆盖 dop=1 退全表③
SET query_dop = 1;
SELECT c_integer FROM demo.vpart_pc ORDER BY c_vector <-> '[1,0,0]' LIMIT 3;   -- [1,7,2]
```

诊断工具（bridge examples）：`probe_vpart_match`（列索引，镜像 FDW 发现）、`probe_vpart_search`（引擎级 ANN，带 nprobe）、`provision_partial_coverage`（部分覆盖 fixture）。

### 7.2 手工注册 fixture 要点

- `tables_internal` 主键 `(namespace, table_name)`、`table_uuid` 唯一，复用会冲突。
- **`current_snapshot_id` 必须填真实值**，否则 `icebergResolveSnapshotId` 返 NULL → 并行规划早退 → 误判串行。
- bridge 按 `metadata_location` 定位、按列名解析 projection，故 iceberg 字段名须 = 外表列名。

### 7.3 数据面流程（并行④ hybrid）

<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 900 520" font-family="sans-serif" font-size="13">
  <defs>
    <marker id="a" markerWidth="10" markerHeight="10" refX="8" refY="3" orient="auto"><path d="M0,0 L8,3 L0,6 Z" fill="#555"/></marker>
  </defs>
  <rect x="0" y="0" width="900" height="520" fill="#ffffff"/>
  <rect x="290" y="12" width="320" height="34" rx="4" fill="#eef3fb" stroke="#4a72b0"/>
  <text x="450" y="33" text-anchor="middle">SQL: ORDER BY vec &lt;-&gt; const LIMIT k</text>
  <line x1="450" y1="46" x2="450" y2="66" stroke="#555" marker-end="url(#a)"/>
  <rect x="260" y="66" width="380" height="52" rx="4" fill="#f4f0fb" stroke="#7a5cb0"/>
  <text x="450" y="86" text-anchor="middle">FDW 发现 match_index（registry@current 严格定位）</text>
  <text x="450" y="106" text-anchor="middle" font-size="11" fill="#555">透传 is_partial</text>
  <line x1="450" y1="118" x2="450" y2="140" stroke="#555" marker-end="url(#a)"/>
  <polygon points="450,142 640,182 450,222 260,182" fill="#fbf3e8" stroke="#c08a3a"/>
  <text x="450" y="178" text-anchor="middle">路径分级</text>
  <text x="450" y="196" text-anchor="middle" font-size="11" fill="#555">is_partial? 并行?</text>
  <line x1="640" y1="182" x2="720" y2="182" stroke="#555" marker-end="url(#a)"/>
  <text x="685" y="174" text-anchor="middle" font-size="11">并行</text>
  <rect x="720" y="160" width="170" height="44" rx="4" fill="#e8f5ec" stroke="#3a9a5c"/>
  <text x="805" y="180" text-anchor="middle">hybrid</text>
  <text x="805" y="196" text-anchor="middle" font-size="11">FULL+index_profile</text>
  <line x1="450" y1="222" x2="450" y2="250" stroke="#555" marker-end="url(#a)"/>
  <text x="470" y="240" font-size="11">非并行 &amp; 全覆盖</text>
  <rect x="300" y="250" width="300" height="40" rx="4" fill="#eef3fb" stroke="#4a72b0"/>
  <text x="450" y="275" text-anchor="middle">直连 VECTOR_INDEX（无 FALLBACK）</text>
  <line x1="260" y1="182" x2="120" y2="182" stroke="#555" marker-end="url(#a)"/>
  <text x="190" y="174" text-anchor="middle" font-size="11">非并行 &amp; 部分覆盖</text>
  <rect x="20" y="160" width="200" height="44" rx="4" fill="#fdeeee" stroke="#c0504a"/>
  <text x="120" y="180" text-anchor="middle">退全表 VectorSearch③</text>
  <text x="120" y="196" text-anchor="middle" font-size="11">避免漏 uncovered 行</text>
  <line x1="805" y1="204" x2="805" y2="230" stroke="#555" marker-end="url(#a)"/>
  <rect x="620" y="230" width="270" height="128" rx="4" fill="#f7f7f7" stroke="#888"/>
  <text x="755" y="250" text-anchor="middle" font-weight="bold">bridge hybrid_real_index_streams</text>
  <rect x="632" y="266" width="115" height="34" rx="3" fill="#e8f5ec" stroke="#3a9a5c"/>
  <text x="689" y="287" text-anchor="middle" font-size="11">covered→IF4 ANN</text>
  <rect x="762" y="266" width="115" height="34" rx="3" fill="#fdeeee" stroke="#c0504a"/>
  <text x="819" y="287" text-anchor="middle" font-size="11">uncovered→FALLBACK</text>
  <text x="755" y="322" text-anchor="middle" font-size="11">INDEX_THEN_FALLBACK 链接</text>
  <text x="755" y="342" text-anchor="middle" font-size="11" fill="#555">剥 _distance，over_fetch_k=fetch_k</text>
  <line x1="755" y1="358" x2="755" y2="392" stroke="#555" marker-end="url(#a)"/>
  <line x1="450" y1="290" x2="450" y2="392" stroke="#555" marker-end="url(#a)"/>
  <line x1="120" y1="204" x2="120" y2="392" stroke="#555" marker-end="url(#a)"/>
  <rect x="250" y="392" width="400" height="46" rx="4" fill="#eef3fb" stroke="#4a72b0"/>
  <text x="450" y="412" text-anchor="middle" font-weight="bold">内核 VectorSearch（容量=topK 最大堆）</text>
  <text x="450" y="429" text-anchor="middle" font-size="11">gather 上全局精确重排 top-K</text>
  <line x1="450" y1="438" x2="450" y2="460" stroke="#555" marker-end="url(#a)"/>
  <rect x="330" y="460" width="240" height="34" rx="4" fill="#eef7ef" stroke="#3a9a5c"/>
  <text x="450" y="481" text-anchor="middle">Limit k → 结果</text>
</svg>

三条数据面（hybrid / 直连 / 退③）殊途同归于内核全局 top-K。
