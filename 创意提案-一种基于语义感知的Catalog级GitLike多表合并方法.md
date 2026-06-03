# 一种基于语义感知的 Catalog 级 Git-Like 多表合并方法

> 创新创意提案 ｜ 方向：主流 Catalog Service 的 Git-Like 数据管理
> 副题：将冲突判定粒度由表级细化至子结构级，实现多表分支的可证明安全自动合并

---

## 摘要

Lakehouse 已普遍具备单表版本能力（如 Apache Iceberg、Apache Paimon 的 branch/tag），主流 Catalog Service（Apache Polaris、Unity Catalog 等）也在逐步补齐多表事务协调能力。在以分支方式开展数据并行开发与协作的过程中，分支合并是难度最高的环节。

当两个分支修改同一张表时，目前最接近 Git-Like 的 Catalog 方案（以 Project Nessie 为代表）只能在整张表的粒度上判定冲突：它将一张表的 schema、properties、partition spec、snapshot 视为一个不可分割的字节序列，因此"一个分支新增列、另一个分支修改表属性"这类相互独立、本可安全合并的变更，也会被判定为冲突并退回人工处理。这既延长数据发布周期，也使 Data PR、自动化分支协作等更进一步的协作模式难以建立。

本提案提出一种语义感知的 Catalog 级多表合并方法：将冲突判定的地址空间由表级细化至表的子结构级，并以操作差分记录变更意图，从而对相互独立的变更实现可证明安全的自动合并，对无法证明安全的变更则保守地退回人工。该方法以一个独立的语义合并服务，叠加于主流 Catalog Service 的 Git-Like 控制面之上，对版本存储内核保持格式无关，可分阶段实施。

---

## 1. 场景问题

### 1.1 应用场景

Catalog 级 Git-Like 管理面向以下几类典型应用形态：

- **多表联合变更与发布**：一次业务演进往往同时涉及多张表（如事实表、维表与派生视图）的结构或数据变更，需要在隔离分支中完成修改与验证，再作为一个整体合并发布。
- **隔离的开发与实验环境**：开发、测试与数据实验需要在不影响主线的隔离环境中进行，完成后再将有效结果合并回主线。
- **数据资产的协作治理**：多个团队在各自分支上并行修改同一批数据资产，依赖合并与评审机制保证主线的一致性与正确性。

上述形态的共同前提是：分支可低成本创建、独立演进，并最终通过合并回归主线。

### 1.2 问题所在

在这些应用形态中，分支的创建与隔离已不是主要障碍，合并则是主要的能力缺口。当前 Catalog 级合并以整张表为冲突判定单位，由此产生两类问题，构成本提案的出发点：

- **相互独立的变更被误判为冲突**：两个分支即使修改的是同一张表的不同子结构（例如一方调整 schema、另一方调整表属性），也会因"同一张表被双方修改"而被判定为冲突。当一次变更横跨多张表时，此类误判会显著增加人工处理量并延长发布周期。
- **自动合并可能产生不被察觉的错误结果**：表的不同变更具有不同的合并语义（例如向同一分区追加数据与覆盖该分区数据）。若以统一方式自动合并而不区分操作语义，可能在合并结果中丢失或破坏数据，且此类错误通常在下游环节才被发现。

两类问题的共同根因是：合并器只能识别"某张表发生了变更"，无法识别"该表内部发生了何种变更"。

| 问题 | 根因 | 影响 |
|---|---|---|
| 相互独立的变更被误判为冲突 | 冲突判定以表为单位，无法区分子结构 | 合并自动化程度低，发布周期延长 |
| 自动合并产生不被察觉的错误结果 | 不区分追加、覆盖、回滚等操作语义 | 数据正确性风险，问题暴露滞后 |
| 合并与评审缺乏可解释依据 | 仅有字节级差异，无语义级差异 | 难以建立 Data PR 与自动化协作机制 |

---

## 2. 现状

### 2.1 能力分层：合并是 Git-Like 的关键环节

| 层级 | 代表能力 | 多表合并 |
|---|---|---|
| 表格式层 | Iceberg/Paimon 单表 branch、Delta/Hudi timeline | 仅单表，无跨表合并 |
| Catalog 事务协调层 | Iceberg REST `transactions/commit`、Delta CMT | 保证一次提交 all-or-nothing，但无长期 branch/merge |
| Catalog Git-Like 层 | Nessie（refs/commits/merge/transplant） | 有 merge，但冲突粒度停在 ContentKey（表）级 |
| 主流 Catalog（Polaris/Unity） | 治理控制面成熟 | 尚无 catalog-wide branch/merge |

综合来看，行业已具备多表原子提交与表级 branch/merge 能力，但尚无方案将多表合并细化到子结构语义级别，这是本提案的着力点。

### 2.2 现有合并停留在表级的原因

- **Nessie 的设计取舍**：merge/transplant 围绕 `ContentKey`、`CommitOp`、`MergeBehavior` 运行；version store core 保持格式无关，不解析 Iceberg metadata JSON，也不判断两次 schema 变更是否兼容。它提供了 `resolvedContent` 这一外部解析入口，但没有内置表格式语义合并引擎，其官方文档亦将锁与冲突描述为表级粒度。
- **主流 Catalog 尚未到达该阶段**：Polaris、Unity Catalog 当前以"当前 catalog tree"为中心，尚未提供 catalog-wide branch，更未涉及语义合并。
- **不能沿用代码合并方式**：代码可按行或抽象语法树进行三向合并，而湖仓表的正确合并需要同时理解 schema id、field id、snapshot lineage、partition spec、sort order、delete files 等信息，行级差异在此不具备意义。
- **实证结论**：ASE 2024 对大规模真实合并场景的研究给出两条工程原则。其一，结果不确定时应报告冲突而非强行合并，因为不被察觉的错误合并代价最高；其二，针对单一子结构的专用合并规则，其正确率接近通用的全结构合并，而错误率明显更低。

| 方案 | 冲突粒度 | 是否理解表格式语义 | 安全自动合并 |
|---|---|---|---|
| Iceberg 表级 branch | 单表 snapshot | 否 | 仅 fast-forward |
| Nessie merge | 表（ContentKey） | 否（仅提供外部 resolver 入口） | 否（相互独立的变更亦判冲突） |
| 普通 Git / AST merge | 行 / 语法树 | 不适用于表 | 不适用 |
| **本提案** | **表的子结构 + 操作语义** | **是（可按表格式插件化）** | **是（分级、可证明安全）** |

---

## 3. 价值

- **提升合并自动化程度，缩短发布周期**：将原本需人工逐表确认的误判冲突转为自动合并，缩短多表联合发布的周期。
- **避免不被察觉的错误合并**：以操作优先级与"可证明安全则自动、否则退回人工"的保守策略，将数据正确性置于自动化之上。
- **支撑 Data PR 机制**：子结构级语义差异（如"新增 email 列、修改 owner 属性"）是 PR 评审与质量门禁的前提，字节级差异无法提供。
- **支持自动化代理的自助分支**：清晰的"可自动合并 / 需人工确认"边界，使自动化代理的实验结果能按风险分流，人工仅需处理需要确认的部分。
- **不绑定单一 Catalog，渐进增强**：作为叠加于主流 Catalog Git-Like 控制面之上的服务，复用既有权限、凭证、存储与提交链路，不要求改造表格式文件。

---

## 4. 创意方案

### 4.1 总体定位：在 Git-Like 控制面之上叠加语义合并服务

本方案不重建 Catalog，而是在已有的 Git-Like 控制面（由 refs、commits、content values、index 构成）之上，增加一个语义合并服务。三层职责严格分离，以避免将表格式语义硬编码进版本存储内核：

```
                 引擎 / Data PR / 自动化代理 / CLI·UI
                              │ merge / diff / review
                              ▼
   ┌───────────────────────────────────────────────────────────┐
   │  Policy & Workflow 层                                       │
   │  决定哪些表/分支允许自动合并，记录审批与审计，承载 Data PR    │
   └───────────────────────────┬───────────────────────────────┘
                               ▼
   ┌───────────────────────────────────────────────────────────┐
   │  Semantic Merge Service（本提案核心，可按表格式插件化）       │
   │  ① 子结构地址空间细化   ② 操作差分与分级安全自动合并          │
   │  读取 base/source/target 三版本 metadata，产出 resolvedContent│
   └───────────────────────────┬───────────────────────────────┘
                               ▼
   ┌───────────────────────────────────────────────────────────┐
   │  VersionStore Core（格式无关，沿用现有 Git-Like 控制面）      │
   │  refs / commits / content values / index / merge primitive  │
   │  仅提供冲突定位、expectedTargetContent、resolvedContent 校验  │
   └───────────────────────────┬───────────────────────────────┘
                               ▼
            主流 Catalog（Polaris/Unity，main 物化与治理基线） + 对象存储
```

核心设计原则：**内核仅负责定位冲突，语义服务负责判断能否安全合并并生成结果，策略层负责决定是否自动应用。** 当语义服务失败或无法判定时，一律退回内核的人工冲突路径，从而保证引入语义能力不会降低系统的安全下限。

### 4.2 创新核心一：子结构地址空间细化

本方案对"冲突"给出更精确的定义。依据分离逻辑对版本控制的形式化（Swierstra & Löh, *The Semantics of Version Control*）：

> 两个变更相互独立、可安全合并，当且仅当其修改域不相交。

现有 Catalog 合并将一张表视为一个原子地址，只要双方都修改了该表，修改域即视为相交并判定为冲突。本方案将一张表的地址空间细化为可独立寻址的子结构：

```
StoreKey("sales.orders")
  ├── .schema          （列集合，按 Iceberg field id 寻址）
  ├── .properties      （键值对，按 key 寻址）
  ├── .partitionSpec   （分区字段）
  ├── .sortOrder       （排序字段）
  └── .snapshots       （数据快照序列）
```

细化后，"一方新增列、另一方修改表属性"的情形为：

```
mod(source) = { .schema }
mod(target) = { .properties }
mod 相交 = ∅   →  相互独立，可安全自动合并
```

冲突判定由表级统一判定转为子结构级精确判断。下表给出子结构维度的简化判定矩阵（✓ 可自动合并；✗ 需人工确认；⚠ 取决于具体值）：

| A↓ ＼ B→ | 改 schema | 改 properties | 改 partitionSpec | 改 snapshots |
|---|---|---|---|---|
| 改 schema | ⚠（是否同一列） | ✓ | ⚠ | ✓ |
| 改 properties | ✓ | ⚠（是否同一 key） | ✓ | ✓ |
| 改 partitionSpec | ⚠ | ✓ | ✗ | ⚠ |
| 改 snapshots | ✓ | ✓ | ⚠ | ⚠（取决于操作类型） |

即使在同一子结构内（如 schema × schema），亦可进一步细分：双方新增不同列（按 field id 不重叠）可安全合并；双方对同一列赋予不同类型则判定为冲突。由此，将"凡修改 schema 即冲突"细化为"仅当修改同一列且结果矛盾时才冲突"。

### 4.3 创新核心二：操作差分与分级安全自动合并

仅有子结构细化尚不充分。在 `.snapshots` 子结构内部，"向分区追加数据"与"覆盖分区数据"的合并语义存在本质差异。因此本方案不仅记录最终状态，还记录变更意图（操作），借鉴 operation-based 版本模型（Baseline）。

将通用的 `Put`/`Delete` 提交记录扩展为语义操作，并按其对数据安全的影响划分优先级：

| 优先级 | 操作举例 | 合并策略 |
|---|---|---|
| P0 存在性 | CreateTable / DropTable / RenameTable | 永远人工确认 |
| P1 破坏性 | OverwriteFiles / RollbackToSnapshot / ReplacePartitions | 默认拒绝自动合并 |
| P2 结构替换 | ReplacePartitionSpec / ReplaceSortOrder | 仅当对侧未修改同一子结构时自动合并 |
| P3 增量 | AppendFiles / AddColumn / SetProperty | 修改域不相交即自动合并 |
| P4 透明/清理 | RewriteFiles（compaction）/ ExpireSnapshots | 总可自动，冲突时让位 |

在此基础上，给出分级安全自动合并机制。该机制是将语义能力转化为可控系统行为的关键：

| 档位 | 行为 | 适用 |
|---|---|---|
| `AUTO_SAFE` | 仅自动合并表格式规范可证明无冲突的变更 | 新增不同列、修改不同 property key、不重叠 append |
| `AUTO_WITH_POLICY` | 需数据契约 / owner / branch policy 授权 | 类型拓宽、嵌套字段新增、分区演进 |
| `MANUAL` | 必须由人工或业务系统给出结果 | 同列改不同类型、覆盖/回滚、破坏性 schema 变更、P0 操作 |

**关于大模型的边界**：大模型可用于解释冲突、生成评审摘要、建议合并策略，但进入 commit 的 `resolvedContent` 必须由规则引擎或表格式库校验通过。Catalog 不应将非确定性推断直接写入表 metadata，该边界是保证数据正确性的基本要求。

### 4.4 合并执行流程

以"两个分支均对同一张 Iceberg 表执行 add-only schema 变更"为例（最窄的安全场景），resolver 流程如下：

```
1. 取 base / source / target 三个版本上该表的 content value
2. 用表格式库加载三份 metadata（如 Iceberg TableMetadataParser）
3. 校验 table uuid / location / format version 一致
4. 计算 source、target 各自相对 base 的子结构变更集
5. 仅当两侧均为 add-only schema 变更（无 snapshot/partition/property/rename/drop）时继续
6. 校验新增列名、类型、field id；必要时为一侧新增列重新分配合法 field id
7. 生成新的合法 metadata JSON（包含双方新增列）
8. 构造新的 content value，携带 metadata location、schema id 等字段
9. 调用内核 merge，对该 key 设置 expectedTargetContent 与 resolvedContent
10. 若 target 在计算期间前进，expected 校验失败，则重新读取并重新判定
```

整个流程遵循"可证明安全则提交，否则退回人工"。不满足任一安全条件时，立即降级为 `MANUAL`。

### 4.5 渐进式落地路径

| 阶段 | 目标 | 说明 |
|---|---|---|
| 基座 | branch / commit / fast-forward publish | 复用主流 Catalog Service 上的 Git-Like 控制面，提供分支、提交与快进发布能力 |
| 阶段一 | 子结构级语义 diff | 先支持在 Data PR 中展示子结构级变更，不改变合并逻辑，风险低、价值明确 |
| 阶段二 | `AUTO_SAFE` 自动合并 | 仅覆盖新增列、不同 property key、不重叠 append 三类可证明安全的变更 |
| 阶段三 | `AUTO_WITH_POLICY` + 策略门禁 | 接入数据契约、owner policy、branch protection |
| 阶段四 | 表格式插件化 + 大模型辅助评审 | 按 Iceberg/Delta/Paimon 插件扩展；大模型仅用于解释与建议 |

落地不追求一次性实现全自动合并，而是从语义 diff 起步，遵循"范围窄、结果稳"的原则，按可证明的安全边界逐档放开自动化。

### 4.6 与现有方案的差异

| 维度 | Iceberg 表级 branch | Nessie merge | 本提案 |
|---|---|---|---|
| 跨表合并 | 否 | 是 | 是 |
| 冲突粒度 | 单表 snapshot | 表（ContentKey） | **表的子结构 + 操作语义** |
| 理解表格式语义 | 否 | 否（仅外部 resolver 入口） | **是，可插件化** |
| 相互独立变更的处理 | 不适用 | 判冲突 | **自动合并** |
| 错误合并防护 | — | 退回人工 | **操作优先级 + 分级保守策略** |
| 对底座侵入 | — | 自带 Git-Like 内核 | **叠加于主流 Catalog 之上，内核保持格式无关** |

### 4.7 能力边界与非目标

为明确本方法的能力边界，划定以下非目标：

- **不做行级数据语义合并**：Catalog 层不读取数据文件内容，无法判断两个 `RowDelta` 是否修改了同一行；此类场景标记为有条件冲突，按 partition/file 范围保守处理。
- **不承诺破坏性操作的自动合并**：覆盖、回滚、删表、重命名表等 P0/P1 操作永远需要人工确认。
- **大模型不参与提交内容的最终决定**：模型仅用于解释与建议，提交结果由规则引擎或表格式库校验。
- **不替代底座的事务与权限**：原子提交、权限、凭证仍由主流 Catalog 与既有控制面负责，本方案只增强合并环节。

---

## 5. 结语

分支协作的价值最终通过合并环节实现。现有 Catalog 级方案以表为冲突判定单位，无法区分表内部的变更，导致相互独立的变更被频繁误判为冲突，也使自动合并难以在保证正确性的前提下放开。本提案将冲突判定细化至子结构与操作语义，并采用"可证明安全则自动合并、否则退回人工"的保守策略，使多表合并在提升自动化程度的同时保持数据正确性。该方法不重建 Catalog，而是叠加于主流 Catalog Service 的 Git-Like 控制面之上，对版本存储内核保持格式无关，可分阶段实施。
