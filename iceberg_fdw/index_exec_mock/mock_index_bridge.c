/*
 * mock_index_bridge.c —— FDW 执行期索引扫描的「下游 mock」。
 *
 * 编译成 libmock_index_bridge.so，导出 FDW index_scan_adapter dlsym 的全部 9 个符号
 * （v3 索引 search/scan + bridge storage/error）。search_vector_by_metadata 返回一份
 * 固定的 Arrow record batch：列布局为「表全部列 + 尾部 _distance」，与 v3 真实 bridge 输出
 * 同形，用于在无 MinIO/REST catalog 时驱动执行器数据面。
 *
 * 返回数据：3 列 [id int32, label int32, _distance float64]，4 行，按距离升序（k-NN 语义）。
 * 对应外表 (id int4, label int4)：materialize 取前 2 列、丢弃尾部 _distance。
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "iceberg_bridge_abi.h" /* IcebergBridgeStorage/Error/Status、ArrowArray/Schema、storage/error 原型 */
#include "iceberg_index_abi.h"  /* IcebergIdxSearchByMetadataRequest、IcebergIdxScanHandle、ICEBERG_IDX_* */

/* ---- 固定结果集 ---- */
enum { MOCK_ROWS = 4, MOCK_COLS = 3 };
static const int32_t MOCK_ID[MOCK_ROWS] = {10, 11, 12, 13};
static const int32_t MOCK_LABEL[MOCK_ROWS] = {100, 101, 102, 103};
static const double MOCK_DIST[MOCK_ROWS] = {0.5, 1.5, 2.5, 3.5};

/* ---- 错误对象 ---- */
struct IcebergBridgeError {
    char *message;
};

static IcebergBridgeError *
mock_make_error(const char *msg)
{
    IcebergBridgeError *e = (IcebergBridgeError *)malloc(sizeof(IcebergBridgeError));

    e->message = strdup(msg != NULL ? msg : "mock error");
    return e;
}

const char *
iceberg_bridge_error_message(const IcebergBridgeError *err)
{
    return (err != NULL) ? err->message : NULL;
}

void
iceberg_bridge_error_free(IcebergBridgeError *err)
{
    if (err != NULL) {
        free(err->message);
        free(err);
    }
}

/* ---- 存储句柄（不透明，mock 仅返回非空哨兵） ---- */
static int mock_storage_sentinel;

IcebergBridgeStatus
iceberg_bridge_storage_open(const char *storage_config_json, IcebergBridgeStorage **out, IcebergBridgeError **err)
{
    (void)storage_config_json;
    if (out == NULL) {
        if (err != NULL) {
            *err = mock_make_error("mock storage_open: out is NULL");
        }
        return ICEBERG_BRIDGE_STATUS_INVALID_ARGUMENT;
    }
    *out = (IcebergBridgeStorage *)&mock_storage_sentinel;
    if (err != NULL) {
        *err = NULL;
    }
    return ICEBERG_BRIDGE_STATUS_OK;
}

void
iceberg_bridge_storage_release(IcebergBridgeStorage *storage)
{
    (void)storage; /* 哨兵无需释放 */
}

/* ---- ABI 版本 ---- */
uint32_t
iceberg_index_rs_abi_version(void)
{
    return ICEBERG_IDX_ABI_VERSION_EXPECTED; /* 3 */
}

/* ---- Arrow C Data Interface 构造 ---- */

/* 子数组 release：释放数据缓冲与 buffers 数组；不释放子结构体自身（由父持有）。 */
static void
mock_child_release(ArrowArray *array)
{
    if (array == NULL || array->release == NULL) {
        return;
    }
    if (array->buffers != NULL) {
        free((void *)array->buffers[1]); /* data buffer */
        free((void *)array->buffers);
    }
    array->release = NULL;
}

/* 顶层 batch release：逐列 release 并释放子结构体、children 与 buffers 数组；不释放 batch 自身（调用方持有）。 */
static void
mock_batch_release(ArrowArray *array)
{
    if (array == NULL || array->release == NULL) {
        return;
    }
    for (int64_t i = 0; i < array->n_children; i++) {
        ArrowArray *child = array->children[i];

        if (child != NULL) {
            if (child->release != NULL) {
                child->release(child);
            }
            free(child);
        }
    }
    free(array->children);
    if (array->buffers != NULL) {
        free((void *)array->buffers);
    }
    array->release = NULL;
}

/* 构造一个原始类型子数组：n_buffers=2（validity=NULL + data），validity 省略表示全非空。 */
static ArrowArray *
mock_make_primitive_child(const void *src, size_t elem_size)
{
    ArrowArray *child = (ArrowArray *)calloc(1, sizeof(ArrowArray));
    void *data = malloc(elem_size * MOCK_ROWS);
    const void **buffers = (const void **)malloc(sizeof(void *) * 2);

    memcpy(data, src, elem_size * MOCK_ROWS);
    buffers[0] = NULL; /* validity bitmap：NULL = 全非空 */
    buffers[1] = data;

    child->length = MOCK_ROWS;
    child->null_count = 0;
    child->offset = 0;
    child->n_buffers = 2;
    child->n_children = 0;
    child->buffers = buffers;
    child->children = NULL;
    child->dictionary = NULL;
    child->release = mock_child_release;
    child->private_data = NULL;
    return child;
}

/* 把固定结果集填进调用方提供的 out_batch（顶层 struct 数组）。 */
static void
mock_fill_batch(ArrowArray *out_batch)
{
    const void **buffers = (const void **)malloc(sizeof(void *) * 1);
    ArrowArray **children = (ArrowArray **)malloc(sizeof(ArrowArray *) * MOCK_COLS);

    buffers[0] = NULL; /* struct validity：NULL = 全非空 */
    children[0] = mock_make_primitive_child(MOCK_ID, sizeof(int32_t));
    children[1] = mock_make_primitive_child(MOCK_LABEL, sizeof(int32_t));
    children[2] = mock_make_primitive_child(MOCK_DIST, sizeof(double));

    out_batch->length = MOCK_ROWS;
    out_batch->null_count = 0;
    out_batch->offset = 0;
    out_batch->n_buffers = 1;
    out_batch->n_children = MOCK_COLS;
    out_batch->buffers = buffers;
    out_batch->children = children;
    out_batch->dictionary = NULL;
    out_batch->release = mock_batch_release;
    out_batch->private_data = NULL;
}

/* ---- 扫描游标 ---- */
struct IcebergIdxScanHandle {
    bool emitted; /* 已发过数据批：再次 next 返回空批 + is_last */
};

IcebergBridgeStatus
iceberg_index_rs_search_vector_by_metadata(IcebergBridgeStorage *storage,
    const IcebergIdxSearchByMetadataRequest *request, IcebergIdxScanHandle **out_scan, IcebergBridgeError **err)
{
    if (err != NULL) {
        *err = NULL;
    }
    if (storage == NULL || request == NULL || out_scan == NULL) {
        if (err != NULL) {
            *err = mock_make_error("mock search: NULL storage/request/out_scan");
        }
        return ICEBERG_BRIDGE_STATUS_INVALID_ARGUMENT;
    }
    if (request->index_name == NULL || request->index_name[0] == '\0') {
        if (err != NULL) {
            *err = mock_make_error("mock search: empty index_name");
        }
        return ICEBERG_BRIDGE_STATUS_INVALID_ARGUMENT;
    }
    if (request->query_vector == NULL || request->query_dim == 0 || request->k == 0) {
        if (err != NULL) {
            *err = mock_make_error("mock search: empty query vector or k==0");
        }
        return ICEBERG_BRIDGE_STATUS_INVALID_ARGUMENT;
    }

    IcebergIdxScanHandle *scan = (IcebergIdxScanHandle *)calloc(1, sizeof(IcebergIdxScanHandle));
    scan->emitted = false;
    *out_scan = scan;
    return ICEBERG_BRIDGE_STATUS_OK;
}

IcebergBridgeStatus
iceberg_index_rs_metadata_scan_next_batch(IcebergIdxScanHandle *scan, ArrowArray *out_batch, bool *out_is_last,
    IcebergBridgeError **err)
{
    if (err != NULL) {
        *err = NULL;
    }
    if (scan == NULL || out_batch == NULL || out_is_last == NULL) {
        if (err != NULL) {
            *err = mock_make_error("mock next_batch: NULL argument");
        }
        return ICEBERG_BRIDGE_STATUS_INVALID_ARGUMENT;
    }

    memset(out_batch, 0, sizeof(*out_batch));
    if (scan->emitted) {
        /* 终止空批：length==0 且 is_last（与 bridge 零命中契约一致）。 */
        *out_is_last = true;
        return ICEBERG_BRIDGE_STATUS_OK;
    }

    mock_fill_batch(out_batch);
    scan->emitted = true;
    *out_is_last = true; /* 一次发完，下一次直接结束 */
    return ICEBERG_BRIDGE_STATUS_OK;
}

IcebergBridgeStatus
iceberg_index_rs_metadata_scan_rescan(IcebergIdxScanHandle *scan, IcebergBridgeError **err)
{
    if (err != NULL) {
        *err = NULL;
    }
    if (scan == NULL) {
        if (err != NULL) {
            *err = mock_make_error("mock rescan: NULL scan");
        }
        return ICEBERG_BRIDGE_STATUS_INVALID_ARGUMENT;
    }
    scan->emitted = false; /* 重放 */
    return ICEBERG_BRIDGE_STATUS_OK;
}

void
iceberg_index_rs_metadata_scan_close(IcebergIdxScanHandle *scan)
{
    free(scan); /* NULL 安全 */
}
