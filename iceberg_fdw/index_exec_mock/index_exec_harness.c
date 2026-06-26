/*
 * index_exec_harness.c —— 复现 FDW index_scan_adapter 的 v3 调用序列。
 *
 * 用与适配器相同的 iceberg_index_abi.h / iceberg_bridge_abi.h，dlopen 指定 bridge .so，
 * dlsym 适配器所用的 9 个符号，按 BeginForeignScan→iceberg_index_scan_open→Iterate→ReScan→close
 * 同样的顺序调用：storage_open → search_vector_by_metadata → metadata_scan_next_batch 循环
 * → 解码 Arrow 子数组 → rescan 重放 → close → storage_release。
 *
 * 用法：index_exec_harness <bridge.so> <metadata_location>
 *   - 指向 mock bridge：跑通数据面，打印固定结果集行。
 *   - 指向真实 bridge：校验 abi_version==3、storage_open，search 对无效 metadata 返回干净错误。
 *
 * 编译：cc -I<fdw>/include index_exec_harness.c -ldl -o index_exec_harness
 */
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iceberg_bridge_abi.h"
#include "iceberg_index_abi.h"

/* 适配器 dlsym 的 9 个符号 */
typedef struct {
    iceberg_idx_abi_version_fn abi_version;
    iceberg_idx_search_vector_by_metadata_fn search_vector;
    iceberg_idx_metadata_scan_next_batch_fn scan_next_batch;
    iceberg_idx_metadata_scan_rescan_fn scan_rescan;
    iceberg_idx_metadata_scan_close_fn scan_close;
    iceberg_bridge_storage_open_fn storage_open;
    iceberg_bridge_storage_release_fn storage_release;
    iceberg_bridge_error_message_fn error_message;
    iceberg_bridge_error_free_fn error_free;
} Api;

static void *
must_sym(void *h, const char *name)
{
    void *p = dlsym(h, name);

    if (p == NULL) {
        fprintf(stderr, "  [FAIL] dlsym %s: %s\n", name, dlerror());
        exit(2);
    }
    return p;
}

static void
report_error(const Api *api, IcebergBridgeError *err, const char *op)
{
    const char *msg = (err != NULL) ? api->error_message(err) : NULL;

    printf("  [err] %s -> %s\n", op, msg != NULL ? msg : "(no message)");
    if (err != NULL) {
        api->error_free(err);
    }
}

/* 解码并打印一个 batch：children = [id int32, label int32, _distance float64]。 */
static void
print_batch(const ArrowArray *batch)
{
    if (batch->n_children < 3) {
        printf("  [warn] batch has %lld children (<3)\n", (long long)batch->n_children);
        return;
    }
    const int32_t *id = (const int32_t *)batch->children[0]->buffers[1];
    const int32_t *label = (const int32_t *)batch->children[1]->buffers[1];
    const double *dist = (const double *)batch->children[2]->buffers[1];
    int64_t off = batch->offset;

    for (int64_t r = 0; r < batch->length; r++) {
        printf("    row %lld: id=%d label=%d _distance=%.3f\n", (long long)r, id[off + r], label[off + r],
            dist[off + r]);
    }
}

/* 复现 Iterate：循环 next_batch 直到 is_last，累计并打印行数。返回总行数，-1 表示错误。 */
static long
drain_scan(const Api *api, IcebergIdxScanHandle *scan)
{
    long total = 0;
    bool is_last = false;

    do {
        ArrowArray batch;
        IcebergBridgeError *err = NULL;
        IcebergBridgeStatus st = api->scan_next_batch(scan, &batch, &is_last, &err);

        if (st != ICEBERG_BRIDGE_STATUS_OK) {
            report_error(api, err, "metadata_scan_next_batch");
            return -1;
        }
        if (batch.release != NULL && batch.length > 0) {
            print_batch(&batch);
            total += batch.length;
        }
        if (batch.release != NULL) {
            batch.release(&batch);
        }
    } while (!is_last);

    return total;
}

int
main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bridge.so> <metadata_location>\n", argv[0]);
        return 1;
    }
    const char *so_path = argv[1];
    const char *metadata_location = argv[2];

    printf("== dlopen %s ==\n", so_path);
    void *h = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (h == NULL) {
        fprintf(stderr, "  [FAIL] dlopen: %s\n", dlerror());
        return 2;
    }

    Api api;
    api.abi_version = (iceberg_idx_abi_version_fn)must_sym(h, "iceberg_index_rs_abi_version");
    api.search_vector =
        (iceberg_idx_search_vector_by_metadata_fn)must_sym(h, "iceberg_index_rs_search_vector_by_metadata");
    api.scan_next_batch =
        (iceberg_idx_metadata_scan_next_batch_fn)must_sym(h, "iceberg_index_rs_metadata_scan_next_batch");
    api.scan_rescan = (iceberg_idx_metadata_scan_rescan_fn)must_sym(h, "iceberg_index_rs_metadata_scan_rescan");
    api.scan_close = (iceberg_idx_metadata_scan_close_fn)must_sym(h, "iceberg_index_rs_metadata_scan_close");
    api.storage_open = (iceberg_bridge_storage_open_fn)must_sym(h, "iceberg_bridge_storage_open");
    api.storage_release = (iceberg_bridge_storage_release_fn)must_sym(h, "iceberg_bridge_storage_release");
    api.error_message = (iceberg_bridge_error_message_fn)must_sym(h, "iceberg_bridge_error_message");
    api.error_free = (iceberg_bridge_error_free_fn)must_sym(h, "iceberg_bridge_error_free");
    printf("  [ok] dlsym 9/9 符号命中\n");

    uint32_t ver = api.abi_version();
    printf("== abi_version = %u (expect %u) -> %s ==\n", ver, ICEBERG_IDX_ABI_VERSION_EXPECTED,
        ver == ICEBERG_IDX_ABI_VERSION_EXPECTED ? "OK" : "MISMATCH");

    /* storage_open（与 full scan 同一份 storage_config_json） */
    IcebergBridgeStorage *storage = NULL;
    IcebergBridgeError *err = NULL;
    IcebergBridgeStatus st =
        api.storage_open("{\"storage_scheme\":\"file\"}", &storage, &err);
    printf("== storage_open -> %s ==\n", st == ICEBERG_BRIDGE_STATUS_OK ? "OK" : "ERR");
    if (st != ICEBERG_BRIDGE_STATUS_OK) {
        report_error(&api, err, "storage_open");
        dlclose(h);
        return 0;
    }

    /* 构造 v3 search 请求（与适配器一致）：namespace JSON 数组、L2、k=4。 */
    float qv[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    IcebergIdxSearchByMetadataRequest req;
    memset(&req, 0, sizeof(req));
    req.table_namespace_json = "[\"mock_ns\"]";
    req.table_name = "mock_table";
    req.metadata_location = metadata_location;
    req.index_name = "idx_embedding";
    req.query_vector = qv;
    req.query_dim = 4;
    req.k = 4;
    req.distance_type = ICEBERG_IDX_DIST_L2;
    req.params_json = NULL;

    IcebergIdxScanHandle *scan = NULL;
    err = NULL;
    st = api.search_vector(storage, &req, &scan, &err);
    printf("== search_vector_by_metadata -> %s ==\n", st == ICEBERG_BRIDGE_STATUS_OK ? "OK" : "ERR");
    if (st != ICEBERG_BRIDGE_STATUS_OK) {
        report_error(&api, err, "search_vector_by_metadata");
        api.storage_release(storage);
        dlclose(h);
        return 0; /* 真实 bridge 对无效 metadata 在此返回干净错误，属预期 */
    }

    printf("== Iterate（首次） ==\n");
    long n1 = drain_scan(&api, scan);
    printf("  -> 共 %ld 行\n", n1);

    printf("== ReScan 后重放 ==\n");
    err = NULL;
    st = api.scan_rescan(scan, &err);
    if (st != ICEBERG_BRIDGE_STATUS_OK) {
        report_error(&api, err, "metadata_scan_rescan");
    } else {
        long n2 = drain_scan(&api, scan);
        printf("  -> 共 %ld 行（应与首次一致）\n", n2);
    }

    api.scan_close(scan);
    api.storage_release(storage);
    dlclose(h);
    printf("== done ==\n");
    return 0;
}
