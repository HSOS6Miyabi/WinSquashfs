#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
input="$(realpath "$1")"
mkdir -p "$2"
output="$(realpath "$2")"
version="$3"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]
architectures=(x64 x86 arm64)
common=(WinArchiveFSPro.exe archivefs.exe WinArchiveFSPro.Unc.exe winsqfs-native.dll archivefs_core.dll winsqfs-erofs.dll tools/mkfs.erofs.exe tools/fsck.erofs.exe tools/mkdwarfs.exe README.md README.zh-CN.md LICENSE THIRD-PARTY.md prerequisites/WinFsp.msi "winsqfs-shell-$version.dll")
for arch in "${architectures[@]}"; do
  dir="$input/dist"
  tools=(tools/mksquashfs.exe tools/unsquashfs.exe)
  if [[ "$arch" != x64 ]]; then
    dir="$input/dist-$arch"
    tools=(tools/archivefs-tar2sqfs.exe tools/gensquashfs.exe tools/rdsquashfs.exe tools/sqfs2tar.exe tools/sqfsdiff.exe)
  fi
  test -s "$input/WinArchiveFSPro-Setup-$arch.exe"
  for name in "${common[@]}" "${tools[@]}"; do
    if [[ ! -s "$dir/$name" ]]; then
      echo "Missing or empty $arch distribution file: $name" >&2
      exit 1
    fi
  done
done
assets=()
for arch in "${architectures[@]}"; do
  dir="$input/dist"
  [[ "$arch" == x64 ]] || dir="$input/dist-$arch"
  setup="WinArchiveFSPro-Setup-$arch.exe"
  portable="WinArchiveFSPro-portable-$arch.zip"
  cp "$input/$setup" "$output/$setup"
  rm -f "$output/$portable"
  (cd "$dir" && zip -qr "$output/$portable" .)
  unzip -tq "$output/$portable"
  assets+=("$setup" "$portable")
done
(cd "$output" && sha256sum "${assets[@]}" > SHA256SUMS.txt && sha256sum -c SHA256SUMS.txt)
