#!/bin/sh
# Build the four AArch64 executables and libraries in the Picker MAX package.
#
# One source set, two targets:
#   bin/    AArch64, -nostdlib -ffreestanding, for the VRX
#   tests   host, normal libc + ASan/UBSan (see RUN_TESTS.sh)
# The SAME .c files are compiled both ways, which is what makes the host tests
# exercise production code rather than a reimplementation.
#
# Requires: aarch64-linux-gnu-gcc (tested with 13.3.0), binutils for aarch64.
# Override the toolchain with CROSS=... if yours is named differently.

set -eu
CROSS="${CROSS:-aarch64-linux-gnu-}"
CC="${CROSS}gcc"
SRC=$(CDPATH= cd -- "$(dirname -- "$0")/../vrx/map_runtime/src" && pwd)
OUT=${1:?Provide a fresh output directory}
mkdir -p "$OUT"

# -nostdlib + a hand-written _start is deliberate and is carried over from the
# hardware-proven POC: it keeps the shipped binaries independent of the cross
# toolchain's crt objects and glibc version, which we cannot match against the
# VRX exactly. Do not "fix" this by linking normally without re-proving it.
#
# -mno-outline-atomics matters: without it GCC emits __aarch64_ldadd8_sync and
# friends from libgcc, which -nostdlib does not provide and which would leave
# the preload libraries with unresolvable symbols on the target.
COMMON="-O2 -std=gnu11 -ffreestanding -fno-builtin-memcpy \
        -mno-outline-atomics -fvisibility=hidden \
        -ffunction-sections -fdata-sections \
        -Wall -Wextra -Werror -Wno-unused-function"
# The executable links libc dynamically (as the hardware-proven POC did: it
# carried NEEDED libc.so.6 and the standard interpreter). The two preload
# libraries deliberately carry NO DT_NEEDED at all and resolve every symbol
# from the host process's own namespace, which is also what the proven build
# did - it is why they load into ar_ldy_gnd and GlassesUI unchanged.
LDCOMMON="-nostdlib -Wl,--gc-sections -Wl,-z,max-page-size=4096 -Wl,--build-id=none"

echo "== ascent_picker_overlay (AArch64 PIE executable -- picker layer producer) =="
# Its OWN frame channel; the shim composites it as an independent overlay layer.
$CC $COMMON -fPIE \
    -o "$OUT/ascent_picker_overlay" \
    "$SRC"/ascent_picker_probe.c \
    "$SRC"/ascent_frame.c "$SRC"/ascent_pickerbar.c "$SRC"/ascent_render.c \
    $LDCOMMON -Wl,-pie -Wl,-e,_start -lc

echo "== ascent_picker_rt (AArch64 PIE executable -- headless Picker switch daemon) =="
# The one VRX-local switch engine: RC (ttyS2 0x16) + IPC SELECT -> proven core -> loopback RF
# (50000 RPC + 2767 standby). Thin glue over the host-unit-tested core; runtime behaviour is
# bench-gated. Reuses ascent_crsf/ascent_ring from the map runtime.
$CC $COMMON -fPIE \
    -o "$OUT/ascent_picker_rt" \
    "$SRC"/ascent_picker_rt.c "$SRC"/ascent_picker_ctl.c "$SRC"/ascent_cells.c "$SRC"/ascent_switch.c \
    "$SRC"/ascent_sw_rpc.c "$SRC"/ascent_rc_source.c "$SRC"/ascent_crsf.c "$SRC"/ascent_mavlink.c "$SRC"/ascent_geo.c "$SRC"/ascent_ring.c \
    "$SRC"/ascent_picker_cfg.c "$SRC"/ascent_picker_ipc.c "$SRC"/ascent_picker_osd.c \
    $LDCOMMON -Wl,-pie -Wl,-e,_start -lc

echo "== libascent_frame_overlay.so (GlassesUI blit shim) =="
$CC $COMMON -fPIC -shared -o "$OUT/libascent_frame_overlay.so" \
    "$SRC"/ascent_frame_overlay.c \
    "$SRC"/ascent_frame.c "$SRC"/ascent_render.c \
    "$SRC"/ascent_overlay.c "$SRC"/ascent_surface.c "$SRC"/ascent_vis_policy.c \
    $LDCOMMON

echo "== libascent_uart_mirror_tap.so (non-consuming UART mirror) =="
$CC $COMMON -fPIC -shared -o "$OUT/libascent_uart_mirror_tap.so" \
    "$SRC"/ascent_uart_mirror_tap.c \
    "$SRC"/ascent_tap_core.c "$SRC"/ascent_ring.c \
    $LDCOMMON

cp "$(dirname -- "$0")/picker-start.sh" "$OUT/picker-start.sh"
