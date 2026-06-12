/* -------------------------------------------------------------------------
 *
 * iceberg_fdw.h
 *      Iceberg 只读 FDW 公共声明（第一阶段：插件骨架）。
 *
 * 本头文件在模块内部共享：OPTIONS 白名单校验入口由 option.cpp 提供，
 * 回调注册与桩实现由 iceberg_fdw.cpp 提供。
 *
 * -------------------------------------------------------------------------
 */
#ifndef ICEBERG_FDW_H
#define ICEBERG_FDW_H

#include "postgres.h"
#include "foreign/foreign.h"

/*
 * 单个合法 OPTION 的描述：名字 + 允许出现的目录上下文（server / table /
 * user mapping / fdw）。校验逻辑见 option.cpp。
 */
typedef struct IcebergFdwOption {
    const char* optname;
    Oid         optcontext; /* 该 option 允许出现的 catalog Oid */
} IcebergFdwOption;

/* OPTIONS 白名单校验：供 validator 调用。非法或缺失必需项时 ereport(ERROR)。 */
extern void IcebergValidateOptions(List* options_list, Oid catalog);

/* ---------------------------------------------------------------------------
 * 规划期：fdw_private 契约与 catalog 解析（第二阶段）
 * --------------------------------------------------------------------------- */

/*
 * ForeignScan.fdw_private 的固定枚举位（规划期编码、执行期解码），顺序见
 * 开发计划 §4.1。整个 fdw_private 是可序列化 List：
 *   [ScanEntry]      子 List：{metadata_location(String), snapshot_id(Integer),
 *                              schema_id(Integer)}
 *   [RetrievedAttrs] IntList：投影列 attno
 *   [PushedFilter]   String：下推谓词序列化串（仅供 Reader 做 I/O 裁剪）
 */
typedef enum IcebergPrivIndex {
    IcebergPrivScanEntry = 0,
    IcebergPrivRetrievedAttrs,
    IcebergPrivPushedFilter,
    IcebergPrivNum
} IcebergPrivIndex;

/*
 * 规划期临时状态，挂在 baserel->fdw_private（C 指针，仅规划期存活，不进入执行
 * 计划——执行期私有数据走上面的可序列化 fdw_private）。
 */
typedef struct IcebergFdwPlanState {
    /* 表身份：来自外表 OPTIONS（validator 已保证两项必填） */
    char* namespace_name;
    char* table_name;

    /* Catalog 解析结果（第二阶段 mock，后续切系统表 tables_internal 点查） */
    char* metadata_location;
    int64 snapshot_id;
    int   schema_id;

    double rows; /* 基数估计（本期固定兜底） */
} IcebergFdwPlanState;

/* catalog.cpp：表身份与元信息解析（第二阶段 mock）。 */
extern IcebergFdwPlanState* IcebergGetTableIdentity(Oid foreigntableid);
extern void IcebergCatalogResolveTable(IcebergFdwPlanState* st);
extern double IcebergGetTableCardinality(const IcebergFdwPlanState* st);

#endif /* ICEBERG_FDW_H */
