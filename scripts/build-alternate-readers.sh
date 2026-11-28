#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
arch="$1"
case "$arch:$MSYSTEM" in
  x86:MINGW32) export CC=gcc CXX=g++; host=i686-w64-mingw32; export AR=ar RANLIB=ranlib WINDRES=windres; dump=objdump ;;
  arm64:CLANGARM64) export CC=clang CXX=clang++; host=aarch64-w64-mingw32; export AR=llvm-ar RANLIB=llvm-ranlib WINDRES=llvm-windres; dump=llvm-objdump ;;
  *) echo "Unsupported compiler environment: $arch:$MSYSTEM" >&2; exit 1 ;;
esac
export PKG_CONFIG_LIBDIR="$MINGW_PREFIX/lib/pkgconfig:$MINGW_PREFIX/share/pkgconfig"
export PKG_CONFIG_PATH=
root="$PWD"
deps="$root/.deps/$arch"
out="$root/artifacts/readers-$arch"
sq="$deps/squashfs-ng"
er="$deps/erofs-utils"
prefix="$deps/squashfs-install"
mkdir -p "$deps" "$out/tools" "$out/licenses"
git clone --depth 1 --branch v1.3.2 https://github.com/AgentD/squashfs-tools-ng.git "$sq"
(
  cd "$sq"
  # Explicitly declare wctype() with both MinGW CRT header variants.
  sed -i '/^#include <stdlib.h>$/a #include <wctype.h>' lib/compat/fnmatch.c
  python3 "$root/native/cpp/scripts/squashfs_creator_patch.py" "$sq"
  autoreconf -fi
  ./configure --host="$host" --prefix="$prefix" --enable-shared --disable-static \
    --with-gzip --with-xz --with-lzo --with-lz4 --with-zstd --without-selinux
  make -j2
  make install
)
"$CC" -std=c11 -O2 -Wall -Wextra -shared -I"$prefix/include" \
  "$root/native/cpp/squashfs_parallel.c" -L"$prefix/lib" -lsquashfs -llzo2 \
  -o "$out/winsqfs-native.dll"
cp "$prefix"/bin/*.dll "$out/"
cp "$prefix/bin/tar2sqfs.exe" "$out/tools/archivefs-tar2sqfs.exe"
for tool in gensquashfs rdsquashfs sqfs2tar sqfsdiff; do
  cp "$prefix/bin/$tool.exe" "$out/tools/"
done
cp "$sq"/COPYING* "$out/licenses/"
commit=7e52cbffa83fbe9eb10bbf110cb12d503a048081
git init "$er"
git -C "$er" fetch --depth=1 https://github.com/erofs/erofs-utils.git "$commit"
git -C "$er" checkout --detach FETCH_HEAD
python3 "$root/native/cpp/scripts/erofs_binary.py" "$er/lib/tar.c"
cp "$root/native/erofs-reader.c" "$er/erofs-reader-impl.c"
cp "$root/native/cpp/erofs_abi.c" "$er/winsqfs-reader.c"
(
  cd "$er"
  ./autogen.sh
  mkdir build
  cd build
  CPPFLAGS="-I$MINGW_PREFIX/include -I$MINGW_PREFIX/include/tre" \
  LDFLAGS="-L$MINGW_PREFIX/lib" LIBS="-lintl -liconv" \
    ../configure --host="$host" --enable-static --disable-shared \
      --enable-multithreading --enable-lz4 --enable-lzma --with-zlib \
      --with-libzstd --with-libdeflate --without-uuid --without-selinux \
      --disable-s3 --disable-oci --without-libcurl --without-openssl \
      --without-libxml2 --without-json-c --without-libnl3
  make -j2
  libs="$(sed -n 's/^LIBEROFS_LIBS = //p' mkfs/Makefile)"
  # The upstream makefile supplies exactly the enabled compression dependencies.
  "$CC" -O2 -shared -DHAVE_CONFIG_H -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE \
    -I. -I../include -I"$MINGW_PREFIX/include" -I"$MINGW_PREFIX/include/tre" \
    ../winsqfs-reader.c lib/.libs/liberofs.a -L"$MINGW_PREFIX/lib" \
    -Wl,--start-group $libs -ltre -lintl -liconv -lbcrypt -lws2_32 -lcrypt32 -lsecur32 -lwinpthread -Wl,--end-group \
    -o "$out/winsqfs-erofs.dll"
  cp mkfs/mkfs.erofs.exe fsck/fsck.erofs.exe "$out/tools/"
)
cp "$er/COPYING" "$out/licenses/erofs-utils.txt"
printf '%s\n' "$commit" > "$out/licenses/erofs-utils-commit.txt"
# Include only the target toolchain libraries actually imported by each PE.
# Resolve recursively, keeping separate loader directories for the tools.
for folder in "$out" "$out/tools"; do
  while :; do
    copied=0
    for binary in "$folder"/*.exe "$folder"/*.dll; do
      [[ -f "$binary" ]] || continue
      while IFS= read -r name; do
        [[ -n "$name" && ! -f "$folder/$name" ]] || continue
        for source in "$MINGW_PREFIX/bin/$name" "$prefix/bin/$name" "$out/$name"; do
          if [[ -f "$source" ]]; then
            cp "$source" "$folder/$name"
            copied=1
            package=$(pacman -Qoq "$source" 2>/dev/null || true)
            if [[ -n "$package" ]]; then
              pacman -Q "$package" >> "$out/licenses/decoder-packages.txt"
              while IFS= read -r license; do
                [[ -f "$license" ]] || continue
                mkdir -p "$out/licenses/$package"
                cp "$license" "$out/licenses/$package/"
              done < <(pacman -Qlq "$package" | grep '/share/licenses/' || true)
            fi
            break
          fi
        done
      done < <("$dump" -p "$binary" | sed -nE 's/^[[:space:]]*DLL Name:[[:space:]]*(.*)\r?$/\1/p' | tr -d '\r')
    done
    [[ $copied -ne 0 ]] || break
  done
done
