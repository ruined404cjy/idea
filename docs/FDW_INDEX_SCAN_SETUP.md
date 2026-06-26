# FDW 执行期索引扫描接入 — 临时 SETUP（续作引导）

本文供 Claude 在新工作区接续「FDW 执行期接入最新 index、完成索引扫描」任务，免去通读全部仓库。仅含背景、引导与 FDW 接入相关事实。

## 1. 任务目标

在 openGauss Iceberg FDW 栈中，让 FDW 执行器（从 `BeginForeignScan` 起）经 iceberg-rust-bridge 的 v3 metadata-location 索引 ABI 接入 index SDK，跑通**向量索引扫描**数据面；标量索引扫描分析其不可达根因。分工：FDW 执行器份内自己做并推 `DataInfraLab/iceberg_fdw` main；规划期（fdw_private 注入）由同事 andy 在其 fork 联通。

## 2. 本地仓库地址

| 角色 | 本地路径 | origin | 当前分支/HEAD |
|---|---|---|---|
| **FDW（主战场）** | `/sda/iceberg_fdw` | `git@github.com:DataInfraLab/iceberg_fdw.git` | 检出 `feat/index-scan-executor-v3` = `edcffea`；`edcffea` 已是 `origin/main` HEAD |
| **bridge（数据面 .so）** | `/sda/iceberg-rust-bridge` | `git@github.com:DataInfraLab/iceberg-rust-bridge.git` | `main` `107bf7c` |
| **index SDK** | `/sda/iceberg-index` | `git@github.com:DataInfraLab/iceberg-index.git` | `main` `3a6830c` |
| **我方文档/mock 仓** | `/sda/myidea` | `git@github.com:ruined404cjy/idea.git` | `main` `dc7af8a` |
| **andy 的 FDW fork（规划期）** | `/sda/opengauss_iceberg_fdw` | `git@github.com:andy123552/opengauss_iceberg_fdw.git` | `main` `d5cc11a` |
| **内核（VectorSearch 算子）** | `/sda/openGauss-server-datainfra` | `git@github.com:DataInfraLab/openGauss-server-datainfra.git` | `datainfra_dev` |
| **catalog（index 注册表，设计态）** | `/sda/new-catalog` | `git@github.com:HardingHang/Catalog.git` | `main` `1991dfb` |

新设备克隆命令（核心仓，置于同一父目录如 `/sda`，保持目录名一致以便交叉引用）：

```bash
git clone git@github.com:DataInfraLab/iceberg_fdw.git            # FDW（main 即含 v3 执行器 edcffea）
git clone git@github.com:DataInfraLab/iceberg-rust-bridge.git    # bridge 数据面
git clone git@github.com:DataInfraLab/iceberg-index.git          # index SDK
git clone git@github.com:ruined404cjy/idea.git myidea            # 我方文档/mock（注意改名 myidea）
git clone git@github.com:andy123552/opengauss_iceberg_fdw.git    # andy 规划期 fork
git clone -b datainfra_dev git@github.com:DataInfraLab/openGauss-server-datainfra.git  # 内核
git clone git@github.com:HardingHang/Catalog.git new-catalog     # catalog（index 注册表设计态）
```

除内核固定 `datainfra_dev` 外，其余仓 `main` 即为本任务工作分支。动手前对协作仓先 `git fetch` 比对远端。`gh` 可用。

## 3. 背景：正交 2×2 扫描模型（必读）

FDW 扫描模型正交：**{标量, 向量}** 是**列属性**，**{全表, 索引}** 是**扫描方法**。两个「全表」格合一，落地为 3 条执行器→bridge 路径：

| 路径（`IcebergFdwScanMethod`） | bridge 接口 | 投影列 | 现状 |
|---|---|---|---|
| 全表扫描 FULL=0 | `iceberg_bridge_scan_open` | **传列名投影**，bridge 按名+序返回 | 已通（端到端） |
| 向量索引扫描 VECTOR_INDEX=1 | `iceberg_index_rs_search_vector_by_metadata` + `metadata_scan_{schema,next_batch,rescan,close}` | **不传列名**，返回「表全部列 + 尾部 `_distance`」，列序=表 schema 序 | 执行器已接 v3，待真实索引端到端 |
| 标量索引扫描 SCALAR_INDEX=2 | **无 bridge 数据面**（仅设想 `match_index_by_metadata` 做发现） | — | 诚实占位，规划期发现恒 false，永不可执行 |

投影是**扫描方法**属性，与列标量/向量无关。索引路径无法投影：执行器把 `projected_columns` 喂成整表全列、按 attnum/schema 列序，复用共享 `iceberg_arrow_materialize_projection_row` 按列序物化、丢弃尾部 `_distance`。此对齐要求外表列序与 Iceberg schema 列序一致（Catalog 建表保证）。

## 4. 进展：FDW 执行器已接 v3（已提交推送）

执行器份内已完成并推 `origin/main`（commit `edcffea`，「feat(executor): 索引扫描执行器对接 bridge v3 metadata-location 索引 ABI」）：

- `include/iceberg_index_abi.h`：重写为 v3 metadata-location 契约。`ICEBERG_IDX_ABI_VERSION_EXPECTED=3`；函数指针 typedef 族 `iceberg_idx_abi_version_fn`、`iceberg_idx_search_vector_by_metadata_fn`、`iceberg_idx_metadata_scan_{schema,next_batch,rescan,close}_fn`；请求体 `IcebergIdxSearchByMetadataRequest{table_namespace_json, table_name, metadata_location, index_name, query_vector, query_dim, k, distance_type, params_json}`。include `iceberg_bridge_abi.h`。
- `src/index_scan_adapter.cpp`：dlsym 9 个符号（5 index 符号 + 4 复用的 bridge 符号 storage_open/release、error_message/free），校验 `abi_version()==3`；`iceberg_index_scan_open` 校验 index_name/query_vector/k/namespace/table/metadata_location → storage_open → 组 `["ns"]` JSON → `search_vector_by_metadata`；next/rescan/close 数据面；任一非 OK → `ereport(ERROR)` 携带 bridge 错误消息，不静默回退。
- `src/iceberg_fdw.cpp`：`BeginForeignScan` 索引分支用 `state->table_info.metadata_location`（删去旧 warehouse/catalog_uri/snapshot_id）。
- `include/iceberg_fdw.h`：`IcebergIndexScanRequest` 瘦身为 v3（删 warehouse/catalog_uri/snapshot_id，加 metadata_location）。

下游 mock + 文档已推 `myidea/main`（commit `dc7af8a`）：见第 6 节。

## 5. v3 ABI 契约要点（FDW↔bridge）

- **定位键 = (metadata_location, index_name)**，非 field_id、非 column_name。`metadata_location` 已 pin 当前 snapshot，执行器不再单独传 snapshot。
- bridge 头 `iceberg-rust-bridge/include/iceberg_bridge.h:426` `#define ICEBERG_INDEX_ABI_VERSION 3u`；by_metadata 符号在 `:483 build / :498 drop / :514 match / :536 search_vector`。
- FDW 与 bridge 版本号都是 3 且符号名/结构体已对齐（edcffea 完成对齐）。早前历史曾出现「FDW v3 列名版 vs bridge v3 by_metadata 版」符号不符，已消除。
- search 请求**无 projection 字段**；输出固定「表全部列 + 尾部 `_distance`」。
- 与全表扫描共用同一份 `storage_config_json`（server 选项）和同一 `libiceberg_rust_bridge.so`。

## 6. 关键文件清单

FDW（`/sda/iceberg_fdw`）：

| 文件 | 作用 |
|---|---|
| `include/iceberg_index_abi.h` | v3 ABI 头（符号 typedef、请求体、版本宏） |
| `src/index_scan_adapter.cpp` | 向量索引扫描适配器（dlsym/open/next/rescan/close） |
| `src/scalar_index_scan_adapter.cpp` | 标量占位：`match_available` 恒 false、`scan_open` 直接 ereport |
| `src/sdk_scan_adapter.cpp` | 全表扫描 + 共享 `iceberg_arrow_materialize_projection_row` |
| `src/iceberg_fdw.cpp` | Begin/Iterate/ReScan/End/Explain 按 scan_method 分流 |
| `include/iceberg_fdw.h` | `IcebergIndexScanRequest`、`IcebergFdwScanMethod` 枚举 |

myidea（`/sda/myidea`，mock + 文档）：

| 文件 | 作用 |
|---|---|
| `docs/8. FDW 执行期索引扫描-流程与mock.md` | 流程与 mock 主文档 |
| `docs/fdw_index_scan_exec_flow.svg` | 执行期流程图（含上下游两个 mock 点） |
| `iceberg_fdw/index_exec_mock/mock_index_bridge.c` | 下游 mock bridge .so，导出全 9 符号，search 返回固定 Arrow batch `[id,label,_distance]×4` |
| `iceberg_fdw/index_exec_mock/index_exec_harness.c` | 复现 adapter v3 调用序列（共用同套头文件） |
| `iceberg_fdw/index_exec_mock/run.sh` | 一键构建+跑 A(mock)/B(真 bridge) |

bridge（`/sda/iceberg-rust-bridge`）：`include/iceberg_bridge.h`（ABI 真值）、`tests/e2e_infra/docker-compose.yml`（MinIO+REST，真实数据所需）。

## 7. 走通程度（docker-free 实证矩阵）

本机无 docker（`sudo dnf` 被环境权限层拦截；静态二进制路因 WSL2 无 iptables 无法做容器网络），故 MinIO+REST 真实后端未起。改用 docker-free 分层实证：

| 层 | 用例 | 真实性 | 结果 |
|---|---|---|---|
| index SDK 向量 | `index_ivf_e2e`（真表 build→search→scan） | 真引擎+真表 | 3 passed |
| index SDK 标量 | `index_btree_e2e` + `exact_fake_scalar_e2e` | 真引擎+真表 | 2+1 passed |
| ABI 边界 | `demo_abi --features test-seam` | 真引擎 1:1 映射 C-ABI | OK |
| bridge C-ABI | `index_abi_smoke`（真 .so，metadata-location 错误契约） | 真 .so | passed |
| FDW↔bridge 接缝 | harness B（真 bridge：abi=3 / storage_open / search 解析整请求，停在文件 IO） | 真 .so | passed |
| FDW 执行器数据面 | harness A + mock bridge（search→next_batch→4 行→rescan 重放） | mock | passed |

SDK e2e 用 `tempfile::TempDir`+`memory://` 内存存储，不需 docker。编译/cargo 须 `env -u LD_LIBRARY_PATH`（避开 openGauss 旧 libstdc++）；SDK 测试须 `-p iceberg-index-sdk`。

**唯一未用真实数据覆盖的一跳**：`bridge search_vector_by_metadata → 真实已发布(puffin)索引的表 → 出行`。它需先用 bridge `build_index_by_metadata` 在 REST+MinIO 后端建并发布索引得到带索引的 `metadata_location`。两端已被真实覆盖（bridge C 符号 + 同引擎 SDK build/search），中间是 1:1 薄封装。

## 8. 标量索引为何不支持（分层根因）

标量索引「引擎有、接口未暴露」，非根本性缺失：

| 层 | 标量状态 | 证据 |
|---|---|---|
| index SDK 内核 | 已实现 | `iceberg-index-core` `IndexKind::{Scalar,Vector,FullText}`；plugins 有 BTree、ExactFakeScalar 两个 `IndexKind::Scalar`；e2e 实跑通过 |
| index ABI 层 | 门控未暴露 | `iceberg-index-abi` `IndexType` 仅向量判别值(Flat=100..Hnsw=110)；`engine.rs` scan 仅 `AccessMethod::AnnIndex` 且显式拒 scalar prefilter；`scalar_selectivity` deferred |
| bridge C-ABI | 无符号 | 仅 `search_vector_by_metadata`，无 `*_scalar_*` 对应物 |
| FDW | 诚实占位 | `scalar_index_scan_adapter.cpp` `match_available` 恒 false、`scan_open` ereport |

支持顺序：① ABI 给 `IndexType` 加标量判别值 + `AccessMethod::ScalarIndex` + `scan_scalar`（接入已拒的 prefilter）→ ② bridge 加 `iceberg_index_rs_{build,match,search}_scalar_by_metadata` 符号 → ③ FDW 把占位换真实调用、规划期 `match_available` 接 bridge 标量发现。FDW 分发/序列化骨架已就绪。

## 9. 下一步方向

1. **向量索引真实端到端（当前最高价值）**：起 `iceberg-rust-bridge/tests/e2e_infra/docker-compose.yml`（MinIO+REST）→ bridge `build_index_by_metadata`+发布得带索引 `metadata_location` → FDW 走 search_vector_by_metadata→出行。补全第 7 节唯一缺口。本机 docker 受限，需可跑 docker 的环境。
2. **规划期注入对接 andy**：执行器只消费 `fdw_private`（布局见下）。向量真实链路靠内核 VectorSearch 把 query_vector+索引选择写入 `baserel->fdw_private`；当前内核 VectorSearch 仅架在 FDW 全扫描之上、**不注入 path info、不下推 index scan**。联调期可用 debug GUC mock 注入触发向量索引路径。
3. **标量索引**：按第 8 节三层顺序推进（ABI→bridge→FDW），属跨仓较大改动。

`fdw_private` 布局（规划期↔执行器稳定契约，本次未改）：
```
fdw_private = [ Integer(1=VECTOR_INDEX), [index_name, vector_column_name, metric, k, fetch_k, query_vector(List<Float>)] ]
```

## 10. 环境与构建坑

- **libstdc++**：`LD_LIBRARY_PATH` 指向 openGauss 旧 libstdc++ 会断系统 gcc12 cc1。FDW 编译、mock/harness 编译、cargo 一律 `env -u LD_LIBRARY_PATH`。
- **WSL2 openEuler 24.03**：无 systemd、无 iptables → docker 容器网络不可用，多容器 compose 起不来。
- **openGauss 实例**：FDW .so 被 gaussdb 进程级持有，改后须 `gs_ctl restart`（保留 `ICEBERG_WAREHOUSE`/`ICEBERG_RUST_BRIDGE_SO` 等 env）。运行期 dlopen 真 .so 路径来自 env `ICEBERG_RUST_BRIDGE_SO`/`ICEBERG_BRIDGE_LIB`。本任务 docker-free 路径不经 openGauss，无须重启。
- **bridge index 引擎只支持 `memory://` 与 `http(s)://` REST catalog，不支持 `file://`**；FDW file warehouse 无法 index 端到端，真实数据须 REST+MinIO。
- **构建全量细节**见 memory `iceberg-fdw-extension-build`、`opengauss-wsl-build-env`。

## 11. 相关 memory 指针

memory 目录 `/home/omm/.claude/projects/-sda/memory/`，相关条目：

| 文件 | 关键点 |
|---|---|
| `scan-orthogonal-2x2-model.md` | 正交 2×2 模型 + 标量根因修正（必读） |
| `fdw-bridge-data-plane-connect.md` | 全表数据面 ABI、Arrow→Datum 解码、约束、验证边界 |
| `fdw-index-scan-columnname-abi-agreement.md` | index 执行器开发演进史（列名版→v3 metadata-location 的全过程与冲突厘清） |
| `opengauss-server-datainfra-vectorsearch.md` | 内核 VectorSearch 算子、index 接入设计文档(14 任务)、不下推现状 |
| `catalog-owns-ddl-fdw-scan-only.md` | DDL/元数据归 Catalog、FDW 只读 scan |
| `iceberg-rust-sdk-switch.md` | bridge 依赖 iceberg crate 0.9.1、to_arrow MOR |
| `fdw-private-node-vs-json.md` | fdw_private 经序列化 typed Node（非裸指针） |
| `iceberg-fdw-extension-build.md` / `opengauss-wsl-build-env.md` | 五仓布局、构建装载、构建坑 |
| `check-remote-before-three-repos.md` | 多仓并行开发，动手前 fetch 比对 |
