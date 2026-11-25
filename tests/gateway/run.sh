#!/bin/sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
OUT="$ROOT/tests/gateway/build"
mkdir -p "$OUT"
CC="${CC:-cc}"
for test in test_gateway test_host_link test_usb test_radio; do
    set --
    if [ "$test" = "test_host_link" ]; then
        set -- "$ROOT/firmware/gateway/app/host_link.c"
    elif [ "$test" = "test_usb" ]; then
        set -- -DTEST_USB_CDC -DAPP_GW_ID=254 -I"$ROOT/firmware/sdk/SRC/StdPeriphDriver/inc" \
            "$ROOT/firmware/gateway/app/usb_cdc.c" "$ROOT/firmware/gateway/app/usb_control.c"
    fi
    "$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
        -I"$ROOT/tests/gateway/stubs" -I"$ROOT/firmware/gateway/app" -I"$ROOT/firmware/common" \
        "$ROOT/tests/gateway/$test.c" "$ROOT/firmware/gateway/app/gateway_core.c" \
        "$ROOT/firmware/gateway/app/host_parser.c" "$ROOT/firmware/common/proto.c" \
        "$ROOT/firmware/common/framing.c" "$ROOT/firmware/common/crc16.c" \
        "$@" -o "$OUT/$test"
    "$OUT/$test"
done
# PA 板（AT2401C）构建：PA5 让给 TX_EN，验证回调不再驱动测点、背压与校准窗口关前端、
# 校准后重新登记 PA 控制。
"$CC" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -DGW_BOARD_PA=1 \
    -I"$ROOT/tests/gateway/stubs" -I"$ROOT/firmware/gateway/app" -I"$ROOT/firmware/common" \
    "$ROOT/tests/gateway/test_radio.c" "$ROOT/firmware/gateway/app/gateway_core.c" \
    "$ROOT/firmware/gateway/app/host_parser.c" "$ROOT/firmware/common/proto.c" \
    "$ROOT/firmware/common/framing.c" "$ROOT/firmware/common/crc16.c" \
    -o "$OUT/test_radio_pa"
"$OUT/test_radio_pa"
