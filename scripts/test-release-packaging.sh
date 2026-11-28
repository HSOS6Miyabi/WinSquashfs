#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
root="$(mktemp -d)"
trap 'rm -rf "$root"' EXIT
version=0.2.0
for arch in x64 x86 arm64; do
  dir="$root/input/dist"
  tools=(tools/mksquashfs.exe tools/unsquashfs.exe)
  if [[ "$arch" != x64 ]]; then
    dir="$root/input/dist-$arch"
    tools=(tools/archivefs-tar2sqfs.exe tools/gensquashfs.exe tools/rdsquashfs.exe tools/sqfs2tar.exe tools/sqfsdiff.exe)
  fi
  mkdir -p "$dir/tools" "$dir/prerequisites"
  for name in WinArchiveFSPro.exe archivefs.exe WinArchiveFSPro.Unc.exe winsqfs-native.dll archivefs_core.dll winsqfs-erofs.dll tools/mkfs.erofs.exe tools/fsck.erofs.exe tools/mkdwarfs.exe README.md README.zh-CN.md LICENSE THIRD-PARTY.md prerequisites/WinFsp.msi "winsqfs-shell-$version.dll" "${tools[@]}"; do
    printf '%s %s\n' "$arch" "$name" > "$dir/$name"
  done
  printf '%s setup\n' "$arch" > "$root/input/WinArchiveFSPro-Setup-$arch.exe"
done
bash scripts/package-release.sh "$root/input" "$root/release" "$version"
test "$(wc -l < "$root/release/SHA256SUMS.txt")" -eq 6
for arch in x64 x86 arm64; do
  [[ "$(unzip -p "$root/release/WinArchiveFSPro-portable-$arch.zip" archivefs.exe)" == "$arch archivefs.exe" ]]
done
rm "$root/input/dist-arm64/tools/mkdwarfs.exe"
if bash scripts/package-release.sh "$root/input" "$root/incomplete" "$version"; then
  echo "Incomplete distribution was accepted" >&2
  exit 1
fi
test ! -e "$root/incomplete/SHA256SUMS.txt"
echo "PASS three native distributions, SHA-256 checksums, archive integrity and rejection of incomplete builds"
