#!/bin/sh
# Builds native/launcher (32-bit ARM Linux, the TV's userland) from launcher.c on any computer with zig
# (https://ziglang.org, tested with 0.15.2). It needs only SDL2's public headers, which it downloads from libsdl.org
# and checks; the TV's own SDL2, SDL2_ttf and SDL2_image are used at run time, so nothing of SDL is shipped.
set -e
cd "$(dirname "$0")"
command -v zig >/dev/null || { echo "zig not found: install it from https://ziglang.org/download/"; exit 1; }
sha() { if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fetch() {   # fetch <url> <sha256> <tarball member glob to extract headers from>
  curl -fsSL "$1" -o "$T/$(basename "$1")"
  [ "$(sha "$T/$(basename "$1")")" = "$2" ] || { echo "checksum mismatch for $1"; exit 1; }
  mkdir -p "$T/src"; tar -xzf "$T/$(basename "$1")" -C "$T/src"
}
fetch https://libsdl.org/release/SDL2-2.0.14.tar.gz d8215b571a581be1332d2106f8036fcb03d12a70bae01e20f424976d275432bc
fetch https://libsdl.org/projects/SDL_ttf/release/SDL2_ttf-2.0.15.tar.gz a9eceb1ad88c1f1545cd7bd28e7cbc0b2c14191d40238f531a15b01b1b22cd33
fetch https://libsdl.org/projects/SDL_image/release/SDL2_image-2.0.5.tar.gz bdd5f6e026682f7d7e1be0b6051b209da2f402a2dd8bd1c4bd9c25ad263108d0
mkdir -p "$T/inc"; cp "$T"/src/SDL2-2.0.14/include/*.h "$T/inc/"; cp "$T/src/SDL2_ttf-2.0.15/SDL_ttf.h" "$T/src/SDL2_image-2.0.5/SDL_image.h" "$T/inc/"
# empty stand-ins for the TV's libraries, so the linker records the right library names (the real ones are used on the TV):
# one stub function for each SDL_, TTF_ and IMG_ symbol the compiled program imports
zig cc -target arm-linux-gnueabi.2.35 -mcpu=cortex_a9 -mfloat-abi=softfp -O2 -D_GNU_SOURCE -I"$T/inc" -c launcher.c -o "$T/launcher.o"
python3 - "$T" <<'PY'
import struct, sys
T = sys.argv[1]; d = open(T + '/launcher.o', 'rb').read()
shoff, = struct.unpack_from('<I', d, 32); shentsize, shnum = struct.unpack_from('<HH', d, 46)
secs = [struct.unpack_from('<IIIIIIIIII', d, shoff + i * shentsize) for i in range(shnum)]
names = {'SDL': [], 'TTF': [], 'IMG': []}
for s in secs:
    if s[1] == 2:   # SHT_SYMTAB
        strtab = secs[s[6]]
        for i in range(s[5] // 16):
            nm, val, size, info, other, shndx = struct.unpack_from('<IIIBBH', d, s[4] + i * 16)
            if shndx == 0 and nm:
                n = d[strtab[4] + nm:d.index(b'\0', strtab[4] + nm)].decode()
                if n[:4] in ('SDL_', 'TTF_', 'IMG_'): names[n[:3]].append(n)
for pre, lib in (('SDL', 'libSDL2-2.0.so.0'), ('TTF', 'libSDL2_ttf-2.0.so.0'), ('IMG', 'libSDL2_image-2.0.so.0')):
    open('%s/%s.c' % (T, lib), 'w').write(''.join('void %s(void){}\n' % n for n in sorted(set(names[pre]))))
PY
for lib in libSDL2-2.0.so.0 libSDL2_ttf-2.0.so.0 libSDL2_image-2.0.so.0; do
  zig cc -target arm-linux-gnueabi.2.35 -shared -nostdlib -Wl,-soname,$lib -o "$T/$lib" "$T/$lib.c"
done
zig cc -target arm-linux-gnueabi.2.35 -mcpu=cortex_a9 -mfloat-abi=softfp -O2 -s -Wall -Wno-unused-function -Wno-unused-variable -D_GNU_SOURCE \
  -I"$T/inc" launcher.c "$T/libSDL2-2.0.so.0" "$T/libSDL2_ttf-2.0.so.0" "$T/libSDL2_image-2.0.so.0" -lm -lpthread -ldl -o launcher
ls -l launcher
