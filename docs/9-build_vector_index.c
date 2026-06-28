/*
 * 向量 IVF 索引供数器（build-only，fs）。
 *
 * 在已落地的向量表（file_scan_vector 写入 fs）上经 bridge C-ABI 建 IVF_FLAT 索引
 * + apply update_statistics，打印带索引的新 metadata_location。证明向量索引同样可在
 * file:// 上 build（by_metadata 路径与存储无关，无需 MinIO/REST）。
 *
 * 用法：
 *   build_vector_index <metadata_location> <namespace> <table> <vector_column> <index_name>
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
    if (argc < 6) {
        fprintf(stderr, "Usage: %s <metadata_location> <namespace> <table> <vector_column> <index_name>\n", argv[0]);
        return 1;
    }
    const char* meta_in  = argv[1];
    const char* ns       = argv[2];
    const char* table    = argv[3];
    const char* column   = argv[4];
    const char* idx_name = argv[5];

    char ns_json[256], cols_json[256];
    snprintf(ns_json, sizeof(ns_json), "[\"%s\"]", ns);
    snprintf(cols_json, sizeof(cols_json), "[\"%s\"]", column);

    IcebergBridgeError* E = NULL;
    IcebergBridgeStorage* S = NULL;
    check(iceberg_bridge_storage_open("{\"storage_scheme\":\"fs\"}", &S, &E), &E, "storage_open");

    /* IVF_FLAT on vector column。 */
    IcebergIndexBuildByMetadataRequest req;
    memset(&req, 0, sizeof(req));
    req.table_namespace_json  = ns_json;
    req.table_name            = table;
    req.metadata_location     = meta_in;
    req.index_name            = idx_name;
    req.index_type            = ICEBERG_INDEX_IVF_FLAT;
    req.column_names_json     = cols_json;
    req.implementation        = "ivf";
    req.build_parameters_json = "{\"num_clusters\":1}";
    req.build_mode_json       = "{\"mode\":\"build_all_uncovered\"}";

    IcebergBridgeString* out = NULL;
    check(iceberg_index_rs_build_index_by_metadata(S, &req, &out, &E), &E, "build_index");
    char* stats = take_str(out);

    IcebergBridgeUpdateStatisticsRequest sreq;
    memset(&sreq, 0, sizeof(sreq));
    sreq.table_namespace_json   = ns_json;
    sreq.table_name             = table;
    sreq.base_metadata_location = meta_in;
    sreq.statistics_file_json    = stats;
    IcebergBridgeString* sout = NULL;
    check(iceberg_bridge_update_statistics(S, &sreq, &sout, &E), &E, "update_statistics");
    char* sraw = take_str(sout);
    free(stats);

    char* new_meta = extract_new_meta(sraw);
    free(sraw);
    if (!new_meta) die("failed to extract new_metadata_location");

    printf("INDEXED_METADATA=%s\n", new_meta);
    free(new_meta);

    iceberg_bridge_storage_release(S);
    return 0;
}
