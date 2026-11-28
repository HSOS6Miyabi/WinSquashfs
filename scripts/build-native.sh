#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .deps artifacts/native/tools artifacts/native/licenses
pkg=squashfs-tools-ng-1.3.2-mingw64
if [[ ! -d .deps/$pkg ]]; then
  curl --fail --location --retry 3 "https://infraroot.at/pub/squashfs/windows/$pkg.zip" -o ".deps/$pkg.zip"
  unzip -q ".deps/$pkg.zip" -d .deps
fi
root="$PWD/.deps/$pkg"
test -f "$root/include/sqfs/data_reader.h"
test -f "$root/include/lzo/lzo1x.h"
gcc -std=c11 -O2 -Wall -Wextra -Werror -shared -I"$root/include" native/cpp/squashfs_parallel.c \
  -L"$root/lib" -lsquashfs -llzo2 -o artifacts/native/winsqfs-native.dll
cp "$root"/bin/*.dll artifacts/native/
g++ -std=c++17 -O2 -Wall -Wextra -Werror -shared -static -finput-charset=UTF-8 \
  native/cpp/shell.cpp -lole32 -lshell32 -ladvapi32 -luuid -luser32 \
  -o artifacts/native/winsqfs-shell.dll
mkdir -p artifacts/native-tests
g++ -std=c++17 -O2 -municode -static native/tests/pack-shell.cpp -lole32 -lshell32 -luuid -luser32 -o artifacts/native-tests/pack-shell.exe
for exe in mksquashfs unsquashfs sqfscat sqfstar; do
  cp "/usr/bin/$exe.exe" artifacts/native/tools/
done
# Include the DLLs reported by the installed MSYS2 tools, not the host system DLLs.
bin="$(cygpath -u "$(cygpath -w /usr/bin)")"
for exe in /usr/bin/mksquashfs.exe /usr/bin/unsquashfs.exe /usr/bin/sqfscat.exe /usr/bin/sqfstar.exe; do
  while IFS= read -r line; do
    path="$(printf '%s' "$line" | sed -E 's/^[[:space:]]+//;s/\r$//')"
    if [[ "$path" =~ ^[A-Za-z]:\\ ]]; then
      unixpath="$(cygpath -u "$path")"
      if [[ "$unixpath" == /usr/bin/* || "$unixpath" == "$bin/"* ]]; then
        [[ "$unixpath" == *.dll ]] && cp -n "$unixpath" artifacts/native/tools/ || true
      fi
    fi
  done < <(cygcheck "$exe")
  while IFS= read -r dll; do
    [[ -f "$dll" ]] && cp -n "$dll" artifacts/native/tools/ || true
  done < <(ldd "$exe" | grep -oE '/usr/bin/[^[:space:]]+\.dll' || true)
done
# The package dependencies have stable MSYS2 names; include them independently of cygcheck's output format.
for pattern in msys-2.0.dll 'msys-lz4*.dll' 'msys-lzma*.dll' 'msys-lzo*.dll' 'msys-zstd*.dll' 'msys-z.dll' 'msys-gcc_s*.dll' 'msys-stdc++*.dll'; do
  for dll in /usr/bin/$pattern; do
    [[ -f "$dll" ]] && cp -n "$dll" artifacts/native/tools/ || true
  done
done
cp -r "$root/licenses" artifacts/native/licenses/squashfs-tools-ng
for p in squashfs-tools msys2-runtime gcc-libs liblz4 liblzma liblzo2 libzstd zlib; do
  if [[ -d "/usr/share/licenses/$p" ]]; then
    cp -r "/usr/share/licenses/$p" "artifacts/native/licenses/$p"
  fi
done
pacman -Q squashfs-tools msys2-runtime gcc-libs liblz4 liblzma liblzo2 libzstd zlib > artifacts/native/licenses/msys2-versions.txt
find artifacts/native -maxdepth 2 -type f | sort
