/*
 * 标量 BTree 索引供数器（build-only）。
 *
 * provision_local 仅写表数据，不建索引；catalog.create_index 不支持 btree。
 * 本工具经 bridge C-ABI 建 idx_id（key_column=id）+ apply update_statistics，
 * 把带索引的新 metadata_location 写回 <warehouse>/.metadata_location 并打印。
 *
 * 用法：
 *   1. cargo run --release --example provision_local -- <warehouse>
 *   2. cc -std=c11 -x c -I<bridge>/include build_scalar_index.c \
 *        -L<bridge>/target/release -liceberg_rust_bridge \
 *        -Wl,-rpath,<bridge>/target/release -o build_scalar_index
 *   3. ./build_scalar_index <warehouse>
 */
#include "iceberg_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die(const char* m) { fprintf(stderr, "FATAL: %s\n", m); exit(1); }

static void check(IcebergBridgeStatus s, IcebergBridgeError** ep, const char* label) {
    if (s != ICEBERG_BRIDGE_OK) {
        const char* msg = *ep ? iceberg_bridge_error_message(*ep) : "(no err)";
        fprintf(stderr, "%s FAILED: status=%d msg=%s\n", label, s, msg ? msg : "");
        if (*ep) iceberg_bridge_error_free(*ep);
        exit(1);
    }
    if (ep && *ep) { iceberg_bridge_error_free(*ep); *ep = NULL; }
}

static char* take_str(IcebergBridgeString* v) {
    if (!v) die("NULL string result");
    const char* d = iceberg_bridge_string_data(v);
    char* r = strcpy((char*)malloc(d ? strlen(d) + 1 : 1), d ? d : "");
    iceberg_bridge_string_free(v);
    return r;
}

/* 从 update_statistics 返回的 JSON 抽取 new_metadata_location。 */
static char* extract_new_meta(const char* raw) {
    char* key = strstr(raw, "\"new_metadata_location\"");
    if (!key) return NULL;
    char* v0 = strchr(key + strlen("\"new_metadata_location\""), '"');
    if (!v0) return NULL;
    char* v1 = strchr(v0 + 1, '"');
    if (!v1) return NULL;
    size_t n = v1 - v0 - 1;
    char* r = (char*)malloc(n + 1);
    memcpy(r, v0 + 1, n);
    r[n] = '\0';
    return r;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <warehouse>\n", argv[0]); return 1; }
    const char* wh = argv[1];

    char mp[1024], meta[1024];
    snprintf(mp, sizeof(mp), "%s/.metadata_location", wh);
    FILE* f = fopen(mp, "r");
    if (!f) die("open .metadata_location");
    if (!fgets(meta, sizeof(meta), f)) die("read .metadata_location");
    fclose(f);
    size_t l = strlen(meta);
    while (l > 0 && (meta[l - 1] == '\n' || meta[l - 1] == '\r')) meta[--l] = '\0';

    IcebergBridgeError* E = NULL;
    IcebergBridgeStorage* S = NULL;
    check(iceberg_bridge_storage_open("{\"storage_scheme\":\"fs\"}", &S, &E), &E, "storage_open");

    /* 在 "id"（field_id 1）上建 BTree。 */
    IcebergIndexBuildByMetadataRequest req;
    memset(&req, 0, sizeof(req));
    req.table_namespace_json  = "[\"default\"]";
    req.table_name            = "t";
    req.metadata_location     = meta;
    req.index_name            = "idx_id";
    req.index_type            = ICEBERG_INDEX_BTREE_FLAT;
    req.column_names_json     = "[\"id\"]";
    req.implementation        = "btree";
    req.build_parameters_json = "{\"key_column\":\"id\"}";
    req.build_mode_json       = "{\"mode\":\"build_all_uncovered\"}";

    IcebergBridgeString* out = NULL;
    check(iceberg_index_rs_build_index_by_metadata(S, &req, &out, &E), &E, "build_index");
    char* stats = take_str(out);

    /* apply StatisticsFile → 新 metadata_location。 */
    IcebergBridgeUpdateStatisticsRequest sreq;
    memset(&sreq, 0, sizeof(sreq));
    sreq.table_namespace_json   = "[\"default\"]";
    sreq.table_name             = "t";
    sreq.base_metadata_location = meta;
    sreq.statistics_file_json    = stats;
    IcebergBridgeString* sout = NULL;
    check(iceberg_bridge_update_statistics(S, &sreq, &sout, &E), &E, "update_statistics");
    char* sraw = take_str(sout);
    free(stats);

    char* new_meta = extract_new_meta(sraw);
    free(sraw);
    if (!new_meta) die("failed to extract new_metadata_location");

    /* 回写 .metadata_location 并打印（供 FDW 接线引用）。 */
    f = fopen(mp, "w");
    if (!f) die("rewrite .metadata_location");
    fprintf(f, "%s\n", new_meta);
    fclose(f);

    printf("INDEXED_METADATA=%s\n", new_meta);
    free(new_meta);

    iceberg_bridge_storage_release(S);
    return 0;
}
