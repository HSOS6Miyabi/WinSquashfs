#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
from pathlib import Path
import sys

p = Path(sys.argv[1])
s = p.read_text(encoding='utf-8')
needle = 'int erofs_iostream_open(struct erofs_iostream *ios, int fd, int decoder)\n{'
if s.count(needle) != 1:
    raise RuntimeError('Unexpected EROFS input-stream interface')
if '#include <unistd.h>' not in s:
    raise RuntimeError('Unexpected EROFS headers')
s = s.replace('#include <unistd.h>', '#include <unistd.h>\n#ifdef _WIN32\n#include <io.h>\n#include <fcntl.h>\n#endif', 1)
s = s.replace(needle, needle + '\n#ifdef _WIN32\n\tif (_setmode(fd, _O_BINARY) < 0) {\n\t\tint err = errno;\n\t\tclose(fd);\n\t\treturn -err;\n\t}\n#endif', 1)
p.write_text(s, encoding='utf-8')

# Keep the Zstandard input-size search within the remaining interval.
# A 32-bit size_t cannot represent the cluster-size multiplication.
zstd = p.with_name('compressor_libzstd.c')
s = zstd.read_text(encoding='utf-8')
needle = 'm = (dstsize * m) / csize;'
if s.count(needle) != 1:
    raise RuntimeError('Unexpected EROFS Zstandard size search')
s = s.replace(needle, '''u64 estimate = (u64)dstsize * m / csize;
			size_t span = r - l;

			if (estimate <= l + span / 4 || estimate >= r - span / 4)
				m = l + span / 2;
			else
				m = (size_t)estimate;''')
needle = 'm = (l + r) / 2;'
if s.count(needle) != 1:
    raise RuntimeError('Unexpected EROFS Zstandard midpoint')
s = s.replace(needle, 'm = l + (r - l) / 2;')
zstd.write_text(s, encoding='utf-8')

