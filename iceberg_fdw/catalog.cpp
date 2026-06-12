/* -------------------------------------------------------------------------
 *
 * catalog.cpp
 *      Iceberg 外表的表身份与元信息解析（第二阶段：mock）。
 *
 * 本期不访问对象存储、不解析 metadata.json。表身份来自外表 OPTIONS；元信息
 * （metadata_location / 当前 snapshot / schema）暂以 mock 值承接，待 catalog
 * 系统表 iceberg_catalog.tables_internal 落地后，把 ResolveTable / Cardinality
 * 换成对该表的点查（开发计划 §3 阶段二）。
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include "commands/defrem.h"
#include "foreign/foreign.h"
#include "lib/stringinfo.h"
#include "nodes/pg_list.h"

#include "iceberg_fdw.h"

/*
 * 从外表 OPTIONS 取 (namespace, table_name)，构造规划期状态。
 *
 * validator 已在 DDL 期强制外表必带这两项；此处再次缺失即属内部不一致，响亮
 * 报错而非静默放过。
 */
IcebergFdwPlanState* IcebergGetTableIdentity(Oid foreigntableid)
{
    ForeignTable* ft = GetForeignTable(foreigntableid);
    IcebergFdwPlanState* st = (IcebergFdwPlanState*)palloc0(sizeof(IcebergFdwPlanState));
    ListCell* lc = NULL;

    foreach (lc, ft->options) {
        DefElem* def = (DefElem*)lfirst(lc);

        if (strcmp(def->defname, "namespace") == 0) {
            st->namespace_name = defGetString(def);
        } else if (strcmp(def->defname, "table_name") == 0) {
            st->table_name = defGetString(def);
        }
    }

    if (st->namespace_name == NULL || st->table_name == NULL) {
        ereport(ERROR,
            (errcode(ERRCODE_FDW_OPTION_NAME_NOT_FOUND),
                errmsg("iceberg_fdw: foreign table is missing \"namespace\" or \"table_name\" option")));
    }

    return st;
}

/*
 * 元信息解析（第二阶段 mock）。
 *
 * 真实实现将点查 iceberg_catalog.tables_internal，取 metadata_location、
 * current_snapshot_id、current_schema_id 等（开发计划 §3 阶段二、catalog 必需
 * 字段参考 ref-fdw-catalog-required-fields.md）。本期以 namespace/table_name
 * 拼出占位 metadata_location，snapshot/schema 取固定值，使规划期闭环可独立验证。
 */
void IcebergCatalogResolveTable(IcebergFdwPlanState* st)
{
    st->metadata_location =
        psprintf("mock://%s/%s/metadata/v1.metadata.json", st->namespace_name, st->table_name);
    st->snapshot_id = 1; /* mock 当前快照 */
    st->schema_id = 0;   /* mock 当前 schema */
}

/*
 * 基数估计（第二阶段 mock）。
 *
 * 真实实现取 snapshots.total_records；无该统计时回退固定默认行数（开发计划
 * §3 阶段二、ref-fdw-catalog-required-fields.md）。本期统一返回固定兜底。
 */
double IcebergGetTableCardinality(const IcebergFdwPlanState* st)
{
    return 1000.0; /* 固定兜底行数 */
}
