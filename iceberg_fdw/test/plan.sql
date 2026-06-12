-- iceberg_fdw 第二阶段验证：规划期三回调产出 ForeignScan 计划与 fdw_private。
-- 期望：每个 EXPLAIN 均为 Foreign Scan 节点；谓词保留为 Filter（节点 qual 复核，
-- 见开发计划 §1.3）；Output 反映投影裁剪。解码后的 fdw_private 三槽经服务器
-- 日志 "iceberg_fdw plan:" 打印（grep 服务器日志核对 metadata_location/
-- snapshot_id/schema_id/retrieved_attrs/pushed_filter）。
\set ON_ERROR_STOP off

DROP FOREIGN TABLE IF EXISTS ice_t;
DROP SERVER IF EXISTS ice_srv;
CREATE EXTENSION IF NOT EXISTS iceberg_fdw;

CREATE SERVER ice_srv FOREIGN DATA WRAPPER iceberg_fdw
  OPTIONS (catalog_kind 'pg_native', catalog_schema 'iceberg_catalog');
CREATE FOREIGN TABLE ice_t (id int, name text)
  SERVER ice_srv OPTIONS (namespace 'ns1', table_name 'tbl1');

-- 投影两列 + 等值谓词：期望 Output: id, name；Filter: (id = 5)
EXPLAIN (VERBOSE, COSTS off) SELECT id, name FROM ice_t WHERE id = 5;

-- 仅投影一列 + 范围谓词：期望 Output: id；Filter: (id > 10)；投影裁剪生效
EXPLAIN (VERBOSE, COSTS off) SELECT id FROM ice_t WHERE id > 10;

-- 无谓词：期望无 Filter 行
EXPLAIN (VERBOSE, COSTS off) SELECT id, name FROM ice_t;

DROP FOREIGN TABLE IF EXISTS ice_t;
DROP SERVER IF EXISTS ice_srv;
