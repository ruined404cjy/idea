#!/usr/bin/env bash
#
# 构建并运行 FDW 执行期索引扫描的 mock 验证。
#   A) 对 mock bridge：跑通执行器数据面，打印固定结果集。
#   B) 对真实 bridge：校验 ABI 接缝（abi_version==3、storage_open、search 错误契约）。
#
# 依赖：FDW include 头（iceberg_index_abi.h / iceberg_bridge_abi.h）、真实 bridge .so。
# 注意：编译需避开 openGauss 的旧 libstdc++（用 env -u LD_LIBRARY_PATH 让系统 gcc 用系统库）。

set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

FDW_INCLUDE="${FDW_INCLUDE:-/sda/iceberg_fdw/include}"
REAL_BRIDGE="${REAL_BRIDGE:-/sda/iceberg-rust-bridge/target/release/libiceberg_rust_bridge.so}"

echo "== 编译 mock bridge .so =="
env -u LD_LIBRARY_PATH cc -shared -fPIC -I"$FDW_INCLUDE" \
  "$HERE/mock_index_bridge.c" -o "$HERE/libmock_index_bridge.so"

echo "== 编译 harness =="
env -u LD_LIBRARY_PATH cc -I"$FDW_INCLUDE" \
  "$HERE/index_exec_harness.c" -ldl -o "$HERE/index_exec_harness"

echo
echo "######## A) 对 mock bridge：执行器数据面跑通 ########"
env -u LD_LIBRARY_PATH "$HERE/index_exec_harness" \
  "$HERE/libmock_index_bridge.so" "file:///tmp/mock/v1.metadata.json"

echo
echo "######## B) 对真实 bridge：ABI 接缝 + 错误契约 ########"
if [[ -f "$REAL_BRIDGE" ]]; then
  env -u LD_LIBRARY_PATH "$HERE/index_exec_harness" \
    "$REAL_BRIDGE" "file:///tmp/does-not-exist/v1.metadata.json"
else
  echo "SKIP: 未找到真实 bridge $REAL_BRIDGE"
fi
