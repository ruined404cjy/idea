# openGauss Iceberg FDW 评审答问备忘（内部自用）

> 配合《openGauss Iceberg FDW 概要设计与决策点（讨论待定版）》（下称"概要"）评审讲解用，非交付文档。内容：①一分钟讲清本次工作；②术语速查（应对不熟悉背景的提问）；③各决策点倾向与依据速查；④预期质疑与回应；⑤可引用的事实弹药；⑥诚实的薄弱点与应对；⑦与既有 FDW 文档（5/6/7.md）的关系。章节号对应"概要"正文。

---

## 1. 一分钟讲清

- **平台**：在开源 openGauss 上实现 Iceberg 外表只读接入的 FDW 读取路径，可直接参考 openGauss 源码（FDW、向量化执行器、`access/datavec` 向量栈）。
- **三阶段**：① 本组做 FDW 完整读取路径（元数据 → 回表）；② 其他成员做 Iceberg 分区向量索引；③ 本组再做带索引的 ForeignScan。
- **本文性质**：阶段一的决策点讨论稿，不是定稿。重点是把影响性能、向量化适配、可扩展性的取舍摊开。
- **主张**：以 Arrow 列式接口把数据读取后端与 FDW 框架解耦，先跑通行式全表扫描最小闭环；同时在四处（多路径、私有数据编码、回表寻址、文件清单缓存）为向量索引预留形态。

---

## 2. 术语速查

| 术语 | 一句话 |
| --- | --- |
| FDW / FdwRoutine | openGauss 外部表框架；FDW 实现一组回调（估行、生成计划、扫描）把外部数据当表读 |
| ForeignScan / fdw_private | 计划节点；私有数据在规划期编码、执行期解码，内容自定义 |
| 回表 | 拿到行的定位后回数据文件取整行。全表扫描时即物化为元组；索引扫描时是按候选位置取行 |
| metadata.json / manifest | Iceberg 三层元数据：metadata.json（表顶层）→ manifest list → manifest → data file，权威源在对象存储 |
| FileScanTask | 单数据文件的扫描描述（路径、格式、残差谓词、delete 引用） |
| MOR / delete file | Merge-on-Read：v2 表用 position / equality delete 文件标记删除行，读时合并 |
| Arrow C Data Interface | 两个 C 结构体传列式数据，零拷贝、免链接 Arrow 动态库、跨语言，作为读取后端与 FDW 的统一边界 |
| VectorBatch / ScalarVector | openGauss 向量化列批（默认 1000 行）；按列存 ScalarVector，值为 8 字节 Datum 数组加逐行空标记 |
| cdylib / C FFI | 把 Rust 编为 C 接口动态库，openGauss 侧 dlopen 或链接调用 |
| snapshot | 表的一个版本，其引用的文件集合不可变 |
| ANN / IVFFlat / HNSW / DiskANN | 近似最近邻检索及三种索引；openGauss `access/datavec` 已内置 |
| 向量检索扫描路径 | openGauss 的向量检索扫描通路，阶段三的 ForeignScan 参考它 |

---

## 3. 决策倾向与依据速查

便于被问"这块怎么想"时即时回应。

| 决策点 | 倾向 | 一句话依据 |
| --- | --- | --- |
| 4.3 Reader 选型 | Rust 与自研 C 专题对比，不当场拍 | 性能与工具链需实测，避免无依据决策 |
| 4.4 交换格式 | Arrow C Data Interface | 零拷贝、换引擎不改 FDW、向量化复用同一入口 |
| 4.1 元数据解析 | 选 SDK 则交 SDK 解析 | 自带 schema evolution / 剪枝 / MOR，降低实现成本 |
| 4.2 文件规划时机 | 执行期 + 文件清单缓存 | 规划期零对象存储 I/O，缓存吸收 ReScan 读放大 |
| 5.2 输出形态 | 首期行式，尽早做向量化原型 | 行式契约稳；向量化是适配 openGauss 的性能关键 |
| 2.2 文件清单缓存 | 建议采纳 | snapshot 文件集不可变，缓存天然安全、无失效逻辑 |
| 4.5 MOR | 纳入能力，启用可后置 | 索引可能建在 MOR 表上，回表须正确 |
| 3.2/3.3/5.4 扩展位 | 首期即留形态 | 决定阶段三能否不重构接入 |
| 2.4/3.1 估行 | snapshot 记录数 × 选择率，记录数增列 | 规划期免读对象存储 |
| 3.4 Catalog 读取 | 先 SPI，瓶颈再下沉 systable | 规划期低频，可随计划缓存摊薄 |

---

## 4. 预期质疑与回应

| 质疑 | 回应要点（含弹药） |
| --- | --- |
| 决策点这么多，会不会拖慢进度？ | 概要 §8 给了首期最小闭环，其余都是为阶段二、三铺路的并行增强项，按优先级推进。讨论是为了不在错误默认上返工。 |
| 为什么不直接用 DuckDB？ | DuckDB 是独立进程，走进程间协议的行式文本路径（列→文本→行，两次转换加跨进程），且为黑盒、运维较重。我们要进程内嵌、Arrow 零拷贝、可控回表，方向不同。可作对照基线，不作主线。 |
| 为什么不用 Java SDK？ | Java 路线代价具体：内嵌 JVM、约 814MB 依赖包、GC 不可控、JNI 边界；向量化快路径不支持 MOR。开源 openGauss 下无须背 JVM，原生路线更省。Java 原型的价值是验证了 Arrow 列式边界可行，这点继承。 |
| Rust 进 openGauss 靠谱吗（语言、版本冲突）？ | 经 C ABI 加 Arrow 列式接口解耦后，Rust 侧与内核版本无强耦合。挂载用 dlopen（已有 SDK 原型以 dlopen 加载动态库验证此模式）。风险点都有成熟规避：panic 用 catch_unwind 收敛、不混用分配器（只经 Arrow release 回调移交所有权）、current-thread 运行时加 block_on。openGauss `fdwapi.h` 回调集已核实兼容。 |
| 为什么不用官方 C++ SDK？ | 它要求 C++23 / GCC 14+，与现有编译工具链冲突，门槛是硬伤。能力虽好（原生 Arrow C Data、免 libarrow），列为高风险候选，不主推。 |
| 文件清单缓存不违反"不缓存明细"原则吗，一致性怎么办？ | 不冲突。依据是 snapshot 文件集不可变：提交新 snapshot 时指针前移，FDW 自然读新 snapshot（首次未命中、之后命中），旧行按容量裁剪，无写时失效逻辑。这正面回应了当初排除明细缓存的一致性顾虑，对重复查询与 ReScan 是单项最大的对象存储节省。 |
| VectorBatch 能 Arrow 零拷贝直转吗？ | 不能。其值为 8 字节 Datum 数组、空值逐行字节、变长走伴随缓冲；Arrow 是紧凑 buffer 加位图，布局不同，必须逐值转换。 |
| 那向量化还有什么意义？ | 收益不在零拷贝，在省装配：免逐行构造元组、免行式结果二次向量化。对大批量列式消费与后续向量检索结果，省的是这部分。逐值转换成本行式列式本来相同。 |
| 先做行式会不会返工？ | 不会。读取后端输出契约是 Arrow，对行式与向量化是同一入口；换的只是下游消费段（转元组还是转 VectorBatch），上游解析、读取、下推、类型全复用。 |
| 外表没有 ctid，怎么回表？ | 外表无本地 heap 与 ctid。回表寻址须自定义：索引返回 (file_path, position)，Reader 按位置读该文件的行。所以阶段一要为 Reader 预留位置读取入口并配文件清单缓存。 |
| 估行不准会怎样？ | 影响连接顺序与路径选择。首期用 snapshot 记录数 × 选择率（记录数建议在 snapshots 增列），后续接 Iceberg 列统计提精度。向量 topk 查询代价另算。 |
| 阶段一与阶段二、三会互相阻塞吗？ | 边界清楚：阶段一交付"能读 + 能按位置回表 + 向量类型映射"，阶段二交付分区索引，阶段三把两者接起来。阶段一只要在四处（路径 3.2、私有数据 3.3、回表 5.4、文件清单 2.2）留好形态即不阻塞阶段三。回表接口需与做索引的成员联合确定。 |
| 为什么要引 Arrow，不直接读 Parquet 填元组？ | Arrow 列式接口让"用哪个引擎读"和"FDW 怎么转"解耦，换 Rust、C++、自研都不动 FDW 转换层；且零拷贝、免链 libarrow；向量化路径共用。是降低长期返工风险的关键边界。 |

---

## 5. 事实弹药（可引用，含源码位置）

| 事实 | 出处 / 数据 |
| --- | --- |
| openGauss 有向量化 FDW 路径 | `src/include/foreign/fdwapi.h`：`VecIterateForeignScan → VectorBatch*` |
| `GetForeignPlan` 有 openGauss 特有 `outer_plan` 第 7 参 | 同上 |
| VectorBatch 结构 | `src/include/vecexecutor/vectorbatch.h`：`ScalarValue = uintptr_t`（8 字节）、`BatchMaxSize = 1000`、`m_vals` / `m_flag` / `m_buf` |
| openGauss 内置向量栈 | `src/include/access/datavec/`：`vector` / `halfvec` / `sparsevec` 类型，`hnsw.h` / `ivfflat.h` / `diskann.h` 三种索引 handler，`ogai_*` 嵌入，`rabitq` / `scalarquantizer` 量化 |
| 扩展构建 | `contrib/postgres_fdw/Makefile`：`MODULE_big` + `Makefile.global`，`exclude_option = -fPIE` |
| Java 路线代价 | 内嵌 JVM、约 814MB 依赖包、向量化快路径不支持 MOR |
| iceberg-cpp 门槛 | C++23 / GCC 14+；0.2 版，写入能力规划中 |
| iceberg-rust 能力 | `StaticTable::from_metadata_file` 无 catalog 直读；0.8 起原生 position + equality delete；arrow-rs `to_ffi` 产出 Arrow C Data |
| dlopen 挂载已验证 | 既有 SDK 原型以运行时 dlopen + dlsym 加载动态库 |
| snapshot 文件集不可变 | 文件清单缓存天然安全、无失效逻辑的依据 |
| Arrow C Data Interface | 两个 C 结构体、零拷贝、免链接 libarrow、跨 C / C++ / Rust |

---

## 6. 诚实的薄弱点与应对

| 薄弱点 | 诚实表述 |
| --- | --- |
| Reader 选型未定 | Rust 与自研 C 还没定，需专题对比加小规模验证工具链与性能才拍板，这是负责任的留白 |
| 向量化收益未实测 | Arrow 到 VectorBatch 还没原型，收益是分析推断（省装配、省二次向量化），需原型量化 |
| 回表寻址依赖所选 Reader | 位置读取要确认所选 Reader 是否支持按位置取行，不支持则走自研或补接口 |
| 与阶段二索引接口未对齐 | 回表接口（候选格式、按 file + position 取行）需与做索引的成员联合确定，现仅留形态 |
| 估行首期粗 | 首期估行粗，先保证能跑、计划不离谱，精度后续接列统计 |

原则：能用源码或数据支撑的给弹药；定不了的明说需验证或联合设计，不假装已定。

---

## 7. 与既有 FDW 文档的关系

- 5.md＝FDW 详细设计，6.md＝Java SDK + Arrow 读取，7.md＝iceberg-rust 深化。三者均为闭源 GaussVector 假设下的确定方案。
- "概要"＝开源 openGauss 假设下、把决策点重新打开并为向量索引预留的讨论稿。
- 关系：5/6/7.md 是输入与素材（接口契约、Arrow 边界、Reader 选型对比可直接复用），"概要"是在新平台、新目标下的重定位与决策框架，不替代它们；详细设计待决策点收敛后按章补齐。
- 若被问"是否与既有文档矛盾"：不矛盾，是重定位——平台与目标变了，把当时收敛掉的取舍重新评估，并为向量索引留位。
