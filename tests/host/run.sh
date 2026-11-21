#!/usr/bin/env bash
# 主机侧回归测试（无需交叉工具链、无需真机）
set -euo pipefail

cd "$(dirname "$0")"
ROOT=../..

CC="${CC:-gcc}"
CFLAGS="-std=gnu11 -Wall -Wextra"

echo "=== [1/2] Modbus 协议回归 ==="
"$CC" $CFLAGS -Ishim \
    -I"$ROOT/components/protocol" \
    -I"$ROOT/components/crc16" \
    modbus_test.c stubs.c \
    "$ROOT/components/protocol/modbus_slave.c" \
    "$ROOT/components/crc16/crc16.c" \
    -o modbus_test
./modbus_test

echo
echo "=== [2/2] 加热状态机回归（含关机失效用例） ==="
"$CC" $CFLAGS \
    -I"$ROOT/app/service" \
    -I"$ROOT/components/pid" \
    heat_control_test.c \
    "$ROOT/app/service/heat_control.c" \
    "$ROOT/components/pid/pid.c" \
    -lm -o heat_control_test
./heat_control_test

echo
echo "=== 全部主机测试通过 ==="
