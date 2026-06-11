/* -------------------------------------------------------------------------
 *
 * option.cpp
 *      iceberg_fdw 的 OPTIONS 白名单与校验。
 *
 * 校验在 DDL 期（CREATE/ALTER SERVER / FOREIGN TABLE / USER MAPPING）由
 * validator 触发，逐个 catalog 上下文分别调用。本文件只负责"合法性"：
 * 拒绝未知 option，并在外表级强制要求表身份（namespace / table_name）。
 * option 取值的语义解析留待规划期回调（第二阶段）。
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_user_mapping.h"
#include "commands/defrem.h"
#include "lib/stringinfo.h"

#include "iceberg_fdw.h"

/*
 * iceberg_fdw 合法 OPTIONS 白名单。
 *
 * 分层依据（详见第一阶段开发计划 §1.4）：catalog 后端配置为一个 server 下
 * 所有外表共享，置于 server；表身份逐表声明，置于 foreign table；对象存储
 * 访问凭证敏感，置于 user mapping。
 */
static const IcebergFdwOption g_valid_options[] = {
    /* server：Catalog 后端定位与对象存储端点 */
    {"catalog_kind",   ForeignServerRelationId},
    {"catalog_schema", ForeignServerRelationId},
    {"endpoint",       ForeignServerRelationId},
    {"region",         ForeignServerRelationId},

    /* foreign table：Iceberg 表身份（二级命名 namespace.table_name） */
    {"namespace",  ForeignTableRelationId},
    {"table_name", ForeignTableRelationId},

    /* user mapping：对象存储访问凭证 */
    {"access_key", UserMappingRelationId},
    {"secret_key", UserMappingRelationId},

    /* 哨兵 */
    {NULL, InvalidOid}};

/* 判断 option 在给定 catalog 上下文下是否合法。 */
static bool is_valid_option(const char* optname, Oid context)
{
    const IcebergFdwOption* opt = NULL;

    for (opt = g_valid_options; opt->optname != NULL; opt++) {
        if (context == opt->optcontext && strcmp(opt->optname, optname) == 0) {
            return true;
        }
    }
    return false;
}

/* 拼出某 catalog 上下文下所有合法 option 名，用于错误提示。 */
static void append_valid_options_hint(StringInfo buf, Oid context)
{
    const IcebergFdwOption* opt = NULL;

    for (opt = g_valid_options; opt->optname != NULL; opt++) {
        if (context == opt->optcontext) {
            appendStringInfo(buf, "%s%s", (buf->len > 0) ? ", " : "", opt->optname);
        }
    }
}

/*
 * 校验一组 OPTIONS。catalog 为当前被校验对象的目录 Oid。
 *
 * 失败即 ereport(ERROR)：未知 option 给出该上下文的合法项提示；外表级缺失
 * namespace 或 table_name 时直接报错——表身份是后续所有解析的前提，必须在
 * DDL 期堵住，不能留到查询期才暴露。
 */
void IcebergValidateOptions(List* options_list, Oid catalog)
{
    ListCell* cell = NULL;
    bool      has_namespace = false;
    bool      has_table_name = false;

    foreach (cell, options_list) {
        DefElem* def = (DefElem*)lfirst(cell);

        if (!is_valid_option(def->defname, catalog)) {
            StringInfoData buf;

            initStringInfo(&buf);
            append_valid_options_hint(&buf, catalog);

            ereport(ERROR,
                (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
                    errmsg("invalid option \"%s\"", def->defname),
                    buf.len > 0 ? errhint("Valid options in this context are: %s", buf.data)
                                : errhint("There are no valid options in this context.")));
        }

        if (strcmp(def->defname, "namespace") == 0) {
            has_namespace = true;
        } else if (strcmp(def->defname, "table_name") == 0) {
            has_table_name = true;
        }
    }

    /* 外表级：表身份两项均为必需。 */
    if (catalog == ForeignTableRelationId) {
        if (!has_namespace || !has_table_name) {
            ereport(ERROR,
                (errcode(ERRCODE_FDW_OPTION_NAME_NOT_FOUND),
                    errmsg("iceberg_fdw foreign table requires both \"namespace\" and \"table_name\" options")));
        }
    }
}
