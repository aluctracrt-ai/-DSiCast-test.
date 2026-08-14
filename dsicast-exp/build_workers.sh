#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
DSWIFI="${1:?usage: build_workers.sh /path/to/dswifi-0.4.2}"
OUT="$ROOT/build"
mkdir -p "$OUT"

: "${DEVKITARM:?DEVKITARM is required}"
: "${LIBNDS:?LIBNDS is required}"
CC="$DEVKITARM/bin/arm-none-eabi-gcc"
OBJCOPY="$DEVKITARM/bin/arm-none-eabi-objcopy"
SIZE="$DEVKITARM/bin/arm-none-eabi-size"

COMMON_INC=(
  -I"$ROOT"
  -I"$DSWIFI/include"
  -I"$DSWIFI/common/source"
  -I"$LIBNDS/include"
)

"$CC" -c "$ROOT/arm9_entry.s" -o "$OUT/arm9_entry.o" \
  -marm -march=armv5te -mtune=arm946e-s
"$CC" -c "$ROOT/arm9_worker.c" -o "$OUT/arm9_worker.o" \
  -marm -march=armv5te -mtune=arm946e-s -Os -Wall \
  -ffunction-sections -fdata-sections -fomit-frame-pointer -fno-strict-aliasing \
  "${COMMON_INC[@]}" -I"$DSWIFI/arm9/source" -DARM9 -DWIFI_USE_TCP_SGIP

"$CC" -nostartfiles -marm -march=armv5te -mtune=arm946e-s \
  -T"$ROOT/arm9.ld" -Wl,--gc-sections,--nmagic,-Map,"$OUT/dsicast_arm9.map" \
  "$OUT/arm9_entry.o" "$OUT/arm9_worker.o" "$DSWIFI/lib/libdswifi9.a" \
  -L"$LIBNDS/lib" -lnds9 -lc -lgcc -o "$OUT/dsicast_arm9.elf"
"$OBJCOPY" -O binary "$OUT/dsicast_arm9.elf" "$OUT/dsicast_arm9.bin"

"$CC" -c "$ROOT/arm7_entry.s" -o "$OUT/arm7_entry.o" \
  -marm -march=armv4t -mtune=arm7tdmi
"$CC" -c "$ROOT/arm7_worker.c" -o "$OUT/arm7_worker.o" \
  -marm -march=armv4t -mtune=arm7tdmi -Os -Wall \
  -ffunction-sections -fdata-sections -fomit-frame-pointer -fno-strict-aliasing \
  "${COMMON_INC[@]}" -I"$DSWIFI/arm7/source" -DARM7

"$CC" -nostartfiles -marm -march=armv4t -mtune=arm7tdmi \
  -T"$ROOT/arm7.ld" -Wl,--gc-sections,--nmagic,-Map,"$OUT/dsicast_arm7.map" \
  "$OUT/arm7_entry.o" "$OUT/arm7_worker.o" "$DSWIFI/lib/libdswifi7.a" \
  -L"$LIBNDS/lib" -lnds7 -lc -lgcc -o "$OUT/dsicast_arm7.elf"
"$OBJCOPY" -O binary "$OUT/dsicast_arm7.elf" "$OUT/dsicast_arm7.bin"

echo '=== DSiCast resident sizes ==='
"$SIZE" "$OUT/dsicast_arm9.elf" "$OUT/dsicast_arm7.elf"
ls -lh "$OUT/dsicast_arm9.bin" "$OUT/dsicast_arm7.bin"
