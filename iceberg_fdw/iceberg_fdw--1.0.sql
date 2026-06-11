/* iceberg_fdw/iceberg_fdw--1.0.sql */

-- 仅允许经 CREATE EXTENSION 加载，禁止在 psql 中直接 source
\echo Use "CREATE EXTENSION iceberg_fdw" to load this file. \quit

CREATE FUNCTION pg_catalog.iceberg_fdw_handler()
RETURNS fdw_handler
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT NOT FENCED;

CREATE FUNCTION pg_catalog.iceberg_fdw_validator(text[], oid)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT NOT FENCED;

CREATE FOREIGN DATA WRAPPER iceberg_fdw
  HANDLER iceberg_fdw_handler
  VALIDATOR iceberg_fdw_validator;
