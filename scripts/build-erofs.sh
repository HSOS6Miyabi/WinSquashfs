#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .deps artifacts/erofs/tools artifacts/erofs/licenses
commit=7e52cbffa83fbe9eb10bbf110cb12d503a048081
git init .deps/erofs-utils
git -C .deps/erofs-utils fetch --depth=1 https://github.com/erofs/erofs-utils.git "$commit"
git -C .deps/erofs-utils checkout --detach FETCH_HEAD
python3 native/cpp/scripts/erofs_binary.py .deps/erofs-utils/lib/tar.c
cp native/erofs-reader.c .deps/erofs-utils/erofs-reader-impl.c
cp native/cpp/erofs_abi.c .deps/erofs-utils/winsqfs-reader.c
cat >> .deps/erofs-utils/Dockerfile <<'DOCKER'

FROM windows-builder AS winsqfs-builder
RUN cd /build/build-windows && \
    libs="$(sed -n 's/^LIBEROFS_LIBS = //p' mkfs/Makefile)" && \
    $CC -O2 -shared -static -DHAVE_CONFIG_H -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE \
        -I. -I/build/include -I/ucrt64/include -I/ucrt64/include/tre \
        /build/winsqfs-reader.c lib/.libs/liberofs.a -L/ucrt64/lib \
        -Wl,--start-group $libs -ltre -lintl -liconv -lbcrypt -lws2_32 -lcrypt32 -lsecur32 -lwinpthread -Wl,--end-group \
        -o /output/bin/winsqfs-erofs.dll

FROM scratch AS winsqfs
COPY --from=winsqfs-builder /output/bin/ /
DOCKER
docker build --target winsqfs --output type=local,dest=.deps/erofs-output .deps/erofs-utils
cp .deps/erofs-output/winsqfs-erofs.dll artifacts/erofs/
cp .deps/erofs-output/mkfs.erofs.exe artifacts/erofs/tools/
cp .deps/erofs-output/fsck.erofs.exe artifacts/erofs/tools/
cp .deps/erofs-utils/COPYING artifacts/erofs/licenses/erofs-utils.txt
printf '%s\n' "$commit" > artifacts/erofs/licenses/erofs-utils-commit.txt
