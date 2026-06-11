-- iceberg_fdw 第一阶段冒烟测试：验证插件可注册、OPTIONS 校验生效。
-- 期望：正向用例成功；负向用例（标 EXPECT ERROR）被 validator 拒绝。
\set ON_ERROR_STOP off

-- 清理上一轮残留（逆依赖序；openGauss 不支持 DROP EXTENSION ... CASCADE）
DROP FOREIGN TABLE IF EXISTS ice_t;
DROP FOREIGN TABLE IF EXISTS bad_t1;
DROP FOREIGN TABLE IF EXISTS bad_t2;
DROP SERVER IF EXISTS ice_srv;
DROP SERVER IF EXISTS bad_srv;

-- 注：openGauss 不支持 DROP EXTENSION，故幂等创建、且测试不删 extension
CREATE EXTENSION IF NOT EXISTS iceberg_fdw;

-- 正向：合法 server（catalog 后端配置）
CREATE SERVER ice_srv FOREIGN DATA WRAPPER iceberg_fdw
  OPTIONS (catalog_kind 'pg_native', catalog_schema 'iceberg_catalog');

-- 正向：合法外表（表身份齐全）
CREATE FOREIGN TABLE ice_t (id int, name text)
  SERVER ice_srv OPTIONS (namespace 'ns1', table_name 'tbl1');

-- EXPECT ERROR：server 上出现未知 option
CREATE SERVER bad_srv FOREIGN DATA WRAPPER iceberg_fdw OPTIONS (bogus 'x');

-- EXPECT ERROR：外表缺少 table_name（表身份不完整）
CREATE FOREIGN TABLE bad_t1 (id int) SERVER ice_srv OPTIONS (namespace 'ns1');

-- EXPECT ERROR：外表上误用 server 级 option（catalog_kind 不允许出现在 table）
CREATE FOREIGN TABLE bad_t2 (id int)
  SERVER ice_srv OPTIONS (namespace 'ns1', table_name 't', catalog_kind 'x');

-- 清理 server 与外表（extension 保留：openGauss 不支持 DROP EXTENSION）
DROP FOREIGN TABLE IF EXISTS ice_t;
DROP SERVER IF EXISTS ice_srv;
