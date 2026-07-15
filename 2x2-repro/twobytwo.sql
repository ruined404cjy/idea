\set ON_ERROR_STOP on
-- 单一 fs 环境、单一 server（iceberg_catalog_server, warehouse=file://），连续跑 2×2 四条路径。
-- 索引由 catalog create_index 真建；索引选路由 FDW 规划期真实发现。
-- :sc_base  = 外部供数的标量表基线 metadata（带数据/快照，建索引前）
-- :vec_base = 外部供数的向量表基线 metadata（带数据/快照，建索引前）

CREATE EXTENSION IF NOT EXISTS iceberg_catalog;
CREATE EXTENSION IF NOT EXISTS iceberg_fdw;

SELECT iceberg_catalog.create_namespace('demo', '{}'::jsonb) IS NOT NULL AS ns;

-- 注册 4 张表（schema = 供数表真实 schema）。create_table 自动建外表并登记 relid。
-- namespace/table 对 bridge 是 label，真实定位靠 metadata_location。
SELECT jsonb_typeof(iceberg_catalog.create_table('demo','sc_full',
  '{"type":"struct","fields":[{"id":1,"name":"id","type":"int","required":true},{"id":2,"name":"name","type":"string","required":false},{"id":3,"name":"score","type":"double","required":false}]}'::jsonb)) AS t1;
SELECT jsonb_typeof(iceberg_catalog.create_table('demo','sc_idx',
  '{"type":"struct","fields":[{"id":1,"name":"id","type":"int","required":true},{"id":2,"name":"name","type":"string","required":false},{"id":3,"name":"score","type":"double","required":false}]}'::jsonb)) AS t2;
-- 向量列 c_vector 携 vector_dim:3 → catalog 自动外表直接映射为 vector(3)（PR#73），无须手工重建。
SELECT jsonb_typeof(iceberg_catalog.create_table('demo','vec_full',
  '{"type":"struct","fields":[{"id":1,"name":"c_integer","type":"int","required":true},{"id":2,"name":"c_vector","type":{"type":"list","element-id":3,"element":"float","element-required":true},"required":false,"vector_dim":3}]}'::jsonb)) AS t3;
SELECT jsonb_typeof(iceberg_catalog.create_table('demo','vec_idx',
  '{"type":"struct","fields":[{"id":1,"name":"c_integer","type":"int","required":true},{"id":2,"name":"c_vector","type":{"type":"list","element-id":3,"element":"float","element-required":true},"required":false,"vector_dim":3}]}'::jsonb)) AS t4;

-- 接线：把 4 张表的 metadata_location 指向外部供数的基线（带数据/快照）。
-- 供数在 catalog 之外完成，create_table 注册的是空表，须改指供数数据。
UPDATE iceberg_catalog.tables_internal SET metadata_location=:sc_base  WHERE namespace='demo' AND table_name IN ('sc_full','sc_idx');
UPDATE iceberg_catalog.tables_internal SET metadata_location=:vec_base WHERE namespace='demo' AND table_name IN ('vec_full','vec_idx');

\echo '========== 经 catalog 建索引（标量 btree / 向量 ivf_flat），CAS 回写带索引 metadata + 注册 table_indexes =========='
SELECT iceberg_catalog.create_index('demo','sc_idx','idx_id','["id"]'::jsonb,'btree','btree','{"key_column":"id"}'::jsonb) IS NOT NULL AS sc_indexed;
SELECT iceberg_catalog.create_index('demo','vec_idx','idx_vec','["c_vector"]'::jsonb,'ivf_flat','ivf','{"num_clusters":1}'::jsonb) IS NOT NULL AS vec_indexed;

-- 四张外表均由 create_table 自动建、relid 已登记，直接可用：
--   标量 id/name/score → int/text/double；向量 c_vector → vector(3)（vector_dim）。
-- 不再手工重建外表或回填 relid。

\echo '========== ① 标量 × 全表（谓词下推 id>3）=========='
EXPLAIN (VERBOSE, COSTS OFF) SELECT id, name FROM demo.sc_full WHERE id > 3;
SELECT id, name FROM demo.sc_full WHERE id > 3 ORDER BY id;

\echo '========== ② 标量 × 索引（id>3，带 btree 元数据 → 真实发现 scalar index scan）=========='
EXPLAIN (VERBOSE, COSTS OFF) SELECT id, name FROM demo.sc_idx WHERE id > 3;
SELECT id, name FROM demo.sc_idx WHERE id > 3 ORDER BY id;

SET enable_vectorsearch = on;
\echo '========== ③ 向量 × 全表（基线元数据，真实发现退回全表）=========='
EXPLAIN (VERBOSE, COSTS OFF) SELECT c_integer FROM demo.vec_full ORDER BY c_vector <-> '[1,0,0]' LIMIT 3;
SELECT c_integer FROM demo.vec_full ORDER BY c_vector <-> '[1,0,0]' LIMIT 3;

\echo '========== ④ 向量 × 索引（带 IVF 元数据，真实发现命中 idx_vec）=========='
EXPLAIN (VERBOSE, COSTS OFF) SELECT c_integer FROM demo.vec_idx ORDER BY c_vector <-> '[1,0,0]' LIMIT 3;
SELECT c_integer FROM demo.vec_idx ORDER BY c_vector <-> '[1,0,0]' LIMIT 3;
