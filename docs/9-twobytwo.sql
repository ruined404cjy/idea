\set ON_ERROR_STOP on
-- 单一 fs 环境，单一 server（iceberg_catalog_server, warehouse=file://），连续跑 2×2 四条路径。
-- 占位符：:sc_meta = 带 btree 的标量表元数据；:vec_meta = 带 IVF 的向量表元数据。

CREATE EXTENSION IF NOT EXISTS iceberg_catalog;
CREATE EXTENSION IF NOT EXISTS iceberg_fdw;

SELECT iceberg_catalog.create_namespace('demo', '{}'::jsonb) IS NOT NULL AS ns;

-- 注册 4 张表（占位 schema = 真实 schema）。namespace/table 对 bridge 是 label，真实定位靠 metadata_location。
SELECT jsonb_typeof(iceberg_catalog.create_table('demo','sc_full',
  '{"type":"struct","fields":[{"id":1,"name":"id","type":"int","required":true},{"id":2,"name":"name","type":"string","required":false},{"id":3,"name":"score","type":"double","required":false}]}'::jsonb)) AS t1;
SELECT jsonb_typeof(iceberg_catalog.create_table('demo','sc_idx',
  '{"type":"struct","fields":[{"id":1,"name":"id","type":"int","required":true},{"id":2,"name":"name","type":"string","required":false},{"id":3,"name":"score","type":"double","required":false}]}'::jsonb)) AS t2;
SELECT jsonb_typeof(iceberg_catalog.create_table('demo','vec_full',
  '{"type":"struct","fields":[{"id":1,"name":"c_integer","type":"int","required":true},{"id":2,"name":"c_vector","type":{"type":"list","element-id":3,"element":"float","element-required":true},"required":false}]}'::jsonb)) AS t3;
SELECT jsonb_typeof(iceberg_catalog.create_table('demo','vec_idx',
  '{"type":"struct","fields":[{"id":1,"name":"c_integer","type":"int","required":true},{"id":2,"name":"c_vector","type":{"type":"list","element-id":3,"element":"float","element-required":true},"required":false}]}'::jsonb)) AS t4;

-- 重建外表为正确列类型 + 各自的 mock 选项
DROP FOREIGN TABLE demo.sc_full;
CREATE FOREIGN TABLE demo.sc_full (id integer, name text)
  SERVER iceberg_catalog_server OPTIONS (namespace 'demo', table_name 'sc_full');

DROP FOREIGN TABLE demo.sc_idx;
CREATE FOREIGN TABLE demo.sc_idx (id integer, name text)
  SERVER iceberg_catalog_server OPTIONS (namespace 'demo', table_name 'sc_idx',
    mock_index_scan 'scalar', mock_index_name 'idx_id', mock_scalar_column 'id',
    mock_scalar_expr '{"field_id":1,"op":"gt","value":{"type":"int64","value":3}}');

DROP FOREIGN TABLE demo.vec_full;
CREATE FOREIGN TABLE demo.vec_full (c_integer integer, c_vector vector(3))
  SERVER iceberg_catalog_server OPTIONS (namespace 'demo', table_name 'vec_full');

DROP FOREIGN TABLE demo.vec_idx;
CREATE FOREIGN TABLE demo.vec_idx (c_integer integer, c_vector vector(3))
  SERVER iceberg_catalog_server OPTIONS (namespace 'demo', table_name 'vec_idx',
    mock_index_scan 'vector', mock_index_name 'idx_vec', mock_vector_column 'c_vector',
    mock_query_vector '1,0,0', mock_topk '3');

-- relid + 元数据指针
UPDATE iceberg_catalog.tables_internal SET relid='demo.sc_full'::regclass  WHERE namespace='demo' AND table_name='sc_full';
UPDATE iceberg_catalog.tables_internal SET relid='demo.sc_idx'::regclass   WHERE namespace='demo' AND table_name='sc_idx';
UPDATE iceberg_catalog.tables_internal SET relid='demo.vec_full'::regclass WHERE namespace='demo' AND table_name='vec_full';
UPDATE iceberg_catalog.tables_internal SET relid='demo.vec_idx'::regclass  WHERE namespace='demo' AND table_name='vec_idx';
UPDATE iceberg_catalog.tables_internal SET metadata_location=:sc_meta  WHERE namespace='demo' AND table_name IN ('sc_full','sc_idx');
UPDATE iceberg_catalog.tables_internal SET metadata_location=:vec_meta WHERE namespace='demo' AND table_name IN ('vec_full','vec_idx');

\echo '========== ① 标量 × 全表（谓词下推 id>3）=========='
EXPLAIN (VERBOSE, COSTS OFF) SELECT id, name FROM demo.sc_full WHERE id > 3;
SELECT id, name FROM demo.sc_full WHERE id > 3 ORDER BY id;

\echo '========== ② 标量 × 索引（btree Gt(3)）=========='
EXPLAIN (VERBOSE, COSTS OFF) SELECT id, name FROM demo.sc_idx;
SELECT id, name FROM demo.sc_idx ORDER BY id;

\echo '========== ③ 向量 × 全表（VectorSearch TopK over full scan）=========='
SET enable_vectorsearch = on;
EXPLAIN (VERBOSE, COSTS OFF) SELECT c_integer FROM demo.vec_full ORDER BY c_vector <-> '[1,0,0]' LIMIT 3;
SELECT c_integer FROM demo.vec_full ORDER BY c_vector <-> '[1,0,0]' LIMIT 3;

\echo '========== ④ 向量 × 索引（IVF index scan, q=[1,0,0] topk3）=========='
EXPLAIN (VERBOSE, COSTS OFF) SELECT c_integer FROM demo.vec_idx;
SELECT c_integer, c_vector::text FROM demo.vec_idx;
