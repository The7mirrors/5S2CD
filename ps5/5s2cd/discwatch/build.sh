#!/bin/bash
# discwatch.elf: main.c + tile.c + dw_https.c, the PS5SX2 Installer's util/net (TCP with time limits) and CA bundle,
# and the TLS 1.2 Mbed TLS build PS5SX2 itself uses (ps5/third_party/mbedtls/port). Output: build/discwatch.elf
set -e
: "${PS5_PAYLOAD_SDK:?set PS5_PAYLOAD_SDK to the ps5-payload-dev SDK}"
export PATH=/usr/lib/llvm-18/bin:$PATH
SRC=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$SRC/../../.." && pwd)
INST=$REPO/ps5/installer
MBED=$REPO/ps5/third_party/mbedtls
B=$SRC/build
mkdir -p "$B/src" "$B/obj/mbedtls"
cp "$SRC"/main.c "$SRC"/tile.c "$SRC"/tile.h "$SRC"/dw_https.c "$SRC"/dw_https.h "$SRC"/stb_image.h "$B/src/"
cp "$REPO/ps5/frontend/third_party/stb_truetype.h" "$B/src/"
for f in util.c util.h net.c net.h json.c json.h log.h; do cp "$INST/src/$f" "$B/src/"; done
cd "$B"
python3 - "$INST/ca/cacert.pem" obj/cacert.c <<'PY'
import sys
data = open(sys.argv[1], "rb").read() + b"\0"
with open(sys.argv[2], "w") as f:
    f.write("#include <stddef.h>\nconst unsigned char dw_cacert[] = {\n")
    for i in range(0, len(data), 24):
        f.write(",".join(str(b) for b in data[i:i + 24]) + ",\n")
    f.write("};\nconst size_t dw_cacert_len = sizeof(dw_cacert);\n")
PY
CC=$PS5_PAYLOAD_SDK/bin/prospero-clang
MBEDFLAGS="-DMBEDTLS_CONFIG_FILE=\"ps5sx2_mbedtls_config.h\" -I$MBED/port -I$MBED/include"
CFLAGS="-std=gnu11 -O2 -fPIC $MBEDFLAGS -Isrc"
if [ ! -f obj/libmbedtls_app.a ]; then
	for f in $(grep -oE "MBEDTLS_FILES \+= \S+" "$MBED/port/files.mk" | awk '{print $3}'); do
		$CC $CFLAGS -I"$MBED/library" -c -o "obj/mbedtls/${f%.c}.o" "$MBED/library/$f"
	done
	$CC $CFLAGS -I"$MBED/library" -c -o obj/mbedtls/ps5sx2_mbedtls_platform.o "$MBED/port/ps5sx2_mbedtls_platform.c"
	"$PS5_PAYLOAD_SDK/bin/prospero-ar" rcs obj/libmbedtls_app.a obj/mbedtls/*.o
fi
OBJS=""
for c in src/main.c src/tile.c src/dw_https.c src/util.c src/net.c src/json.c obj/cacert.c; do
	o=obj/$(basename "${c%.c}").o
	$CC $CFLAGS -Wall -Wno-unused-parameter -c -o "$o" "$c"
	OBJS="$OBJS $o"
done
$CC -o discwatch.elf $OBJS obj/libmbedtls_app.a -lSceNet -lSceSystemService -lSceUserService
ls -la discwatch.elf
