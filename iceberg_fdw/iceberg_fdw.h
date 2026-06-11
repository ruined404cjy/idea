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

#endif /* ICEBERG_FDW_H */
