/* -------------------------------------------------------------------------
 *
 * iceberg_fdw.cpp
 *      Iceberg 只读 FDW —— 第一阶段：插件骨架与回调注册。
 *
 * 本阶段只交付"可编译、可注册"的最小插件：
 *   - handler 注册六个核心扫描回调 + ReScan/Explain 占位；
 *   - validator 经 OPTIONS 白名单校验 DDL（实现于 option.cpp）。
 *
 * 六个核心回调与 ReScan/Explain 此处均为显式桩：被调用即 ereport(ERROR)
 * 报"尚未实现"。CREATE EXTENSION / SERVER / FOREIGN TABLE 等 DDL 不触发
 * 这些回调；只有对外表执行 SELECT 才会进入规划/执行回调。桩保证未实现的
 * 路径响亮失败，而非静默返回空结果或脏数据（后续阶段逐个落地）。
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include "access/reloptions.h"
#include "access/sysattr.h"
#include "commands/explain.h"
#include "executor/executor.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "lib/stringinfo.h"
#include "nodes/bitmapset.h"
#include "nodes/makefuncs.h"
#include "nodes/nodes.h"
#include "nodes/value.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/var.h"

#include "iceberg_fdw.h"

PG_MODULE_MAGIC;

/* SQL 层入口 */
extern "C" Datum iceberg_fdw_handler(PG_FUNCTION_ARGS);
extern "C" Datum iceberg_fdw_validator(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(iceberg_fdw_handler);
PG_FUNCTION_INFO_V1(iceberg_fdw_validator);

/* 扫描回调（第一阶段桩，签名对齐 fdwapi.h） */
static void icebergGetForeignRelSize(PlannerInfo* root, RelOptInfo* baserel, Oid foreigntableid);
static void icebergGetForeignPaths(PlannerInfo* root, RelOptInfo* baserel, Oid foreigntableid);
static ForeignScan* icebergGetForeignPlan(PlannerInfo* root, RelOptInfo* baserel, Oid foreigntableid,
    ForeignPath* best_path, List* tlist, List* scan_clauses, Plan* outer_plan);
static void icebergBeginForeignScan(ForeignScanState* node, int eflags);
static TupleTableSlot* icebergIterateForeignScan(ForeignScanState* node);
static void icebergReScanForeignScan(ForeignScanState* node);
static void icebergEndForeignScan(ForeignScanState* node);
static void icebergExplainForeignScan(ForeignScanState* node, ExplainState* es);

/*
 * Handler：返回填好回调指针的 FdwRoutine。
 *
 * 用 makeNode(FdwRoutine) 创建，未显式赋值的字段自动为 NULL（含本期不支持
 * 的写入、JOIN 下推、向量化等可选回调）。
 */
Datum iceberg_fdw_handler(PG_FUNCTION_ARGS)
{
    FdwRoutine* routine = makeNode(FdwRoutine);

    /* 六个核心扫描回调 */
    routine->GetForeignRelSize = icebergGetForeignRelSize;
    routine->GetForeignPaths = icebergGetForeignPaths;
    routine->GetForeignPlan = icebergGetForeignPlan;
    routine->BeginForeignScan = icebergBeginForeignScan;
    routine->IterateForeignScan = icebergIterateForeignScan;
    routine->EndForeignScan = icebergEndForeignScan;

    /* 占位：FdwRoutine 要求注册，本期不实现 */
    routine->ReScanForeignScan = icebergReScanForeignScan;
    routine->ExplainForeignScan = icebergExplainForeignScan;

    PG_RETURN_POINTER(routine);
}

/*
 * Validator：DDL 期逐个 catalog 上下文校验 OPTIONS。核心白名单逻辑在
 * option.cpp。
 */
Datum iceberg_fdw_validator(PG_FUNCTION_ARGS)
{
    List* options_list = untransformRelOptions(PG_GETARG_DATUM(0));
    Oid   catalog = PG_GETARG_OID(1);

    IcebergValidateOptions(options_list, catalog);

    PG_RETURN_VOID();
}

/* ---------------------------------------------------------------------------
 * 规划期三回调（第二阶段）：产出 ForeignScan 节点与可序列化 fdw_private，
 * 不访问对象存储。Catalog 解析为 mock（见 catalog.cpp）。
 * --------------------------------------------------------------------------- */

/*
 * 计算投影列 attno：取目标列与谓词引用列的并集。
 *
 * Reader 必须返回目标列；同时谓词全部保留为节点 qual 由 ExecQual 逐行复核
 * （见开发计划 §1.3），故谓词引用的列也必须读出，否则复核取不到值。
 */
static List* IcebergBuildRetrievedAttrs(RelOptInfo* baserel)
{
    Bitmapset* attrs = NULL;
    ListCell*  lc = NULL;
    List*      result = NIL;
    int        x = -1;

    /* 目标列 */
    pull_varattnos((Node*)baserel->reltarget->exprs, baserel->relid, &attrs);

    /* 谓词引用列（复核所需） */
    foreach (lc, baserel->baserestrictinfo) {
        RestrictInfo* ri = (RestrictInfo*)lfirst(lc);
        pull_varattnos((Node*)ri->clause, baserel->relid, &attrs);
    }

    /* 还原偏移、滤掉系统列与 whole-row（attno <= 0），按 attno 升序产出 IntList */
    while ((x = bms_next_member(attrs, x)) >= 0) {
        AttrNumber attno = x + FirstLowInvalidHeapAttributeNumber;
        if (attno > 0) {
            result = lappend_int(result, attno);
        }
    }
    return result;
}

/*
 * 序列化下推谓词。
 *
 * 本期尚无 Iceberg 表达式 deparser，用 nodeToString 保留原始表达式（可经
 * stringToNode 还原），作为 pushedFilter 的占位载体；真实 Reader 接入后替换为
 * Iceberg filter 串。注意：pushedFilter 仅供 Reader 做 I/O 裁剪，不替代节点
 * qual 的行级复核（开发计划 §1.3）。
 */
static char* IcebergSerializeFilter(List* exprs)
{
    if (exprs == NIL) {
        return pstrdup("");
    }
    return nodeToString(exprs);
}

/*
 * 第二阶段验证辅助：从刚装配的 fdw_private 解码并打印三槽字段，确认编码/解码
 * 往返正确（开发计划 §3 阶段二验证）。走 LOG：始终写入服务器日志、可 grep，
 * 不污染客户端输出。后续阶段由 EXPLAIN 自定义输出取代。
 */
static void IcebergDebugDumpPrivate(List* fdw_private)
{
    List*          scanEntry = (List*)list_nth(fdw_private, IcebergPrivScanEntry);
    List*          retrievedAttrs = (List*)list_nth(fdw_private, IcebergPrivRetrievedAttrs);
    char*          pushedFilter = strVal((Value*)list_nth(fdw_private, IcebergPrivPushedFilter));
    char*          meta = strVal((Value*)linitial(scanEntry));
    int64          snap = intVal((Value*)lsecond(scanEntry));
    int            sch = intVal((Value*)lthird(scanEntry));
    StringInfoData buf;
    ListCell*      lc = NULL;

    initStringInfo(&buf);
    foreach (lc, retrievedAttrs) {
        appendStringInfo(&buf, "%d ", lfirst_int(lc));
    }

    elog(LOG,
        "iceberg_fdw plan: metadata_location=%s snapshot_id=%ld schema_id=%d retrieved_attrs=[%s] pushed_filter=%s",
        meta, snap, sch, buf.data, pushedFilter);
}

static void icebergGetForeignRelSize(PlannerInfo* root, RelOptInfo* baserel, Oid foreigntableid)
{
    /* 取表身份，估基数，挂规划期临时状态。 */
    IcebergFdwPlanState* st = IcebergGetTableIdentity(foreigntableid);

    st->rows = IcebergGetTableCardinality(st);
    baserel->rows = st->rows;
    baserel->fdw_private = (void*)st;
}

static void icebergGetForeignPaths(PlannerInfo* root, RelOptInfo* baserel, Oid foreigntableid)
{
    /*
     * 登记单条全表扫描路径。成本为占位估计（startup 0，total = 行数）；写成
     * add_path 形态，为后续索引/多路径预留（开发计划 §3 阶段二、§6）。
     */
    Cost startup_cost = 0.0;
    Cost total_cost = baserel->rows;

    add_path(root, baserel,
        (Path*)create_foreignscan_path(root, baserel, startup_cost, total_cost,
            NIL,   /* pathkeys */
            NULL,  /* required_outer */
            NULL,  /* fdw_outerpath */
            NIL)); /* fdw_private（路径级，不用） */
}

static ForeignScan* icebergGetForeignPlan(PlannerInfo* root, RelOptInfo* baserel, Oid foreigntableid,
    ForeignPath* best_path, List* tlist, List* scan_clauses, Plan* outer_plan)
{
    IcebergFdwPlanState* st = (IcebergFdwPlanState*)baserel->fdw_private;

    /* 元信息解析（mock；后续系统表点查 tables_internal）。 */
    IcebergCatalogResolveTable(st);

    /* ScanEntry：{metadata_location, snapshot_id, schema_id}。 */
    List* scanEntry = list_make3(makeString(st->metadata_location),
        makeInteger(st->snapshot_id), makeInteger(st->schema_id));

    /* 投影列（目标列 ∪ 谓词引用列）。 */
    List* retrievedAttrs = IcebergBuildRetrievedAttrs(baserel);

    /*
     * 谓词处理（开发计划 §1.3、§3 阶段二）：
     *  - local_exprs = 全部原始谓词，作节点 qual 由 ExecQual 逐行复核兜底；
     *  - 同一组谓词序列化进 pushedFilter，仅供 Reader 做 I/O 裁剪。
     * 本期可下推 = 全部，故二者同源；下推与本地 qual 并存而非互斥。
     */
    List*  local_exprs = extract_actual_clauses(scan_clauses, false);
    Value* pushedFilter = makeString(IcebergSerializeFilter(local_exprs));

    /* 按枚举顺序装配可序列化 fdw_private。 */
    List* fdw_private = list_make3(scanEntry, retrievedAttrs, pushedFilter);

    IcebergDebugDumpPrivate(fdw_private);

    return make_foreignscan(tlist,
        local_exprs,    /* qpqual = 全部谓词（复核兜底） */
        baserel->relid, /* scanrelid */
        NIL,            /* fdw_exprs */
        fdw_private,
        NIL,            /* fdw_scan_tlist */
        NIL,            /* fdw_recheck_quals */
        outer_plan);
}

/* ---------------------------------------------------------------------------
 * 执行期回调桩：被调用即报错（第四阶段替换实现）。
 * --------------------------------------------------------------------------- */

#define ICEBERG_FDW_NOT_IMPLEMENTED(cb)                                  \
    ereport(ERROR,                                                       \
        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),                         \
            errmsg("iceberg_fdw: %s is not implemented yet", (cb))))

static void icebergBeginForeignScan(ForeignScanState* node, int eflags)
{
    /*
     * EXPLAIN（无 ANALYZE）只规划不取数：早退，fdw_state 保持 NULL，使 EXPLAIN
     * 能渲染 ForeignScan 节点而不触发未实现的取数路径（开发计划 §4）。真实执行
     * 期初始化留第四阶段。
     */
    if (eflags & EXEC_FLAG_EXPLAIN_ONLY) {
        return;
    }
    ICEBERG_FDW_NOT_IMPLEMENTED("BeginForeignScan");
}

static TupleTableSlot* icebergIterateForeignScan(ForeignScanState* node)
{
    ICEBERG_FDW_NOT_IMPLEMENTED("IterateForeignScan");
    return NULL; /* 不可达 */
}

static void icebergReScanForeignScan(ForeignScanState* node)
{
    ICEBERG_FDW_NOT_IMPLEMENTED("ReScanForeignScan");
}

static void icebergEndForeignScan(ForeignScanState* node)
{
    /*
     * explain-only 路径下 Begin 早退、未建状态：无资源可释，直接返回（EXPLAIN
     * 结束时执行器仍会调用 End）。真实资源释放留第四阶段。
     */
    if (node->fdw_state == NULL) {
        return;
    }
    ICEBERG_FDW_NOT_IMPLEMENTED("EndForeignScan");
}

static void icebergExplainForeignScan(ForeignScanState* node, ExplainState* es)
{
    /*
     * 占位：本期不产出自定义 EXPLAIN 输出（Iceberg 表/快照/下推摘要留后续）。
     * 但不可报错——EXPLAIN 会调用本回调，桩报错会打断对外表的 EXPLAIN。
     * 故此处为良性空操作，通用 EXPLAIN 仍会显示 Foreign Scan 节点。
     */
}
