-- FDW 执行器向量索引扫描 e2e 接线脚本。
-- 配套文档:8. FDW 执行期索引扫描-流程与mock.md
-- 配套补丁:8-fdw-mock-index-shim.patch(打到 iceberg_fdw 仓后重建 FDW)
--
-- 前置:
--   1. MinIO + REST catalog 已起(iceberg-rust-bridge/tests/e2e_infra/docker-compose.yml)。
--   2. 已运行 bridge `cargo run --example e2e_provision`,取得其打印的 metadata_location。
--   3. gaussdb 以 s3 环境重启:ICEBERG_WAREHOUSE=s3://warehouse/ 及
--      ICEBERG_S3_ENDPOINT/ACCESS_KEY/SECRET_KEY/REGION(catalog.create_index 经 env 定位存储)。
--   4. 在全新 database 内执行本脚本。
--
-- 占位符(按你的环境替换):
--   :base_metadata     e2e_provision 打印的 metadata_location
--   :s3_endpoint       MinIO 端点,compose 默认 http://127.0.0.1:9000
--   :s3_key/:s3_secret MinIO 凭证,compose 默认 admin / password
-- 用法示例:
--   gsql -d <db> -p <port> \
--     -v base_metadata="'s3://warehouse/e2e_cpp/vectors/metadata/xxx.metadata.json'" \
--     -v s3_endpoint="'http://127.0.0.1:9000'" \
--     -v s3_key="'admin'" -v s3_secret="'password'" \
--     -f 8-idx-e2e-wiring.sql

CREATE EXTENSION IF NOT EXISTS iceberg_catalog;
CREATE EXTENSION IF NOT EXISTS iceberg_fdw;

-- 1. namespace + 表(schema 须与 e2e_provision 一致:id field1 int, embedding field2 list<float> elem field3)
SELECT iceberg_catalog.create_namespace('e2e_cpp', '{}'::jsonb) IS NOT NULL AS ns_created;

SELECT jsonb_typeof(iceberg_catalog.create_table(
    'e2e_cpp', 'vectors',
    '{"type":"struct","fields":['
    '{"id":1,"name":"id","type":"int","required":true},'
    '{"id":2,"name":"embedding","type":{"type":"list","element-id":3,"element":"float","element-required":true},"required":false}'
    ']}'::jsonb
)) AS create_table_result_type;

-- 2. 给自动建的 server 补 MinIO s3 凭证(自动建只带 warehouse)
ALTER SERVER iceberg_catalog_server OPTIONS (
    ADD s3_endpoint :s3_endpoint,
    ADD s3_region 'us-east-1',
    ADD s3_access_key_id :s3_key,
    ADD s3_secret_access_key :s3_secret,
    ADD s3_path_style_access 'true'
);

-- 3. 重建外表为 vector(2) + mock 选项(触发向量索引执行器)
DROP FOREIGN TABLE e2e_cpp.vectors;
CREATE FOREIGN TABLE e2e_cpp.vectors (
    id integer,
    embedding vector(2)
)
SERVER iceberg_catalog_server
OPTIONS (
    namespace 'e2e_cpp',
    table_name 'vectors',
    mock_index_scan 'vector',
    mock_index_name 'idx_embedding',
    mock_vector_column 'embedding',
    mock_query_vector '1,0',
    mock_topk '4'
);

UPDATE iceberg_catalog.tables_internal
SET relid = 'e2e_cpp.vectors'::regclass
WHERE namespace = 'e2e_cpp' AND table_name = 'vectors';

-- 4. 指向 e2e_provision 的数据
UPDATE iceberg_catalog.tables_internal
SET metadata_location = :base_metadata
WHERE namespace = 'e2e_cpp' AND table_name = 'vectors';

-- 5. 经 catalog 建索引(build_index→update_statistics→CAS 改 metadata_location→INSERT table_indexes)
SELECT iceberg_catalog.create_index(
    'e2e_cpp', 'vectors', 'idx_embedding',
    '["embedding"]'::jsonb, 'ivf_flat', 'ivf', '{"num_clusters":1}'::jsonb
) AS create_index_result;

SELECT namespace, table_name, index_name, field_ids, index_status
FROM iceberg_catalog.table_indexes;

-- 6. EXPLAIN:应显示 bridge vector index scan
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, embedding::text FROM e2e_cpp.vectors;

-- 7. 实扫:期望近邻序 id=2(d0), {1,4}(d1), 3(d2)
SELECT id, embedding::text FROM e2e_cpp.vectors;
