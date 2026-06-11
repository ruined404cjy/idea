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
#include "commands/explain.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "nodes/makefuncs.h"
#include "nodes/nodes.h"

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
 * 回调桩：被调用即报错。后续阶段（规划期=阶段二、执行期=阶段四）替换实现。
 * --------------------------------------------------------------------------- */

#define ICEBERG_FDW_NOT_IMPLEMENTED(cb)                                  \
    ereport(ERROR,                                                       \
        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),                         \
            errmsg("iceberg_fdw: %s is not implemented yet", (cb))))

static void icebergGetForeignRelSize(PlannerInfo* root, RelOptInfo* baserel, Oid foreigntableid)
{
    ICEBERG_FDW_NOT_IMPLEMENTED("GetForeignRelSize");
}

static void icebergGetForeignPaths(PlannerInfo* root, RelOptInfo* baserel, Oid foreigntableid)
{
    ICEBERG_FDW_NOT_IMPLEMENTED("GetForeignPaths");
}

static ForeignScan* icebergGetForeignPlan(PlannerInfo* root, RelOptInfo* baserel, Oid foreigntableid,
    ForeignPath* best_path, List* tlist, List* scan_clauses, Plan* outer_plan)
{
    ICEBERG_FDW_NOT_IMPLEMENTED("GetForeignPlan");
    return NULL; /* 不可达：ereport(ERROR) 不返回，此处仅消除编译告警 */
}

static void icebergBeginForeignScan(ForeignScanState* node, int eflags)
{
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
    ICEBERG_FDW_NOT_IMPLEMENTED("EndForeignScan");
}

static void icebergExplainForeignScan(ForeignScanState* node, ExplainState* es)
{
    ICEBERG_FDW_NOT_IMPLEMENTED("ExplainForeignScan");
}
