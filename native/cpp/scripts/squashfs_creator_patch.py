#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Extend the pinned squashfs-tools-ng 1.3.2 TAR creator with creation controls.

All modifications are checked against the original source. The standard tool
behavior remains unchanged when the archivefs options are not used.
"""
from pathlib import Path
import sys

root = Path(sys.argv[1])

def rewrite(relative, replacements):
    path = root / relative
    text = path.read_text(encoding="utf-8")
    for old, new in replacements:
        if text.count(old) != 1:
            raise RuntimeError(f"Unexpected upstream source in {relative}: {old[:80]}")
        text = text.replace(old, new)
    path.write_text(text, encoding="utf-8", newline="\n")

rewrite("include/sqfs/block.h", [
    ("\tSQFS_BLK_DONT_HASH = 0x0020,",
     "\tSQFS_BLK_DONT_HASH = 0x0020,\n\n"
     "\t/* Independent fragment compression policy for file creation. */\n"
     "\tSQFS_BLK_FRAGMENT_COMPRESSION_OVERRIDE = 0x0040,\n"
     "\tSQFS_BLK_FRAGMENT_NO_COMPRESSION = 0x0080,"),
    ("SQFS_BLK_USER_SETTABLE_FLAGS = 0x003F", "SQFS_BLK_USER_SETTABLE_FLAGS = 0x00FF"),
    ("SQFS_BLK_FLAGS_ALL = 0xFC3F", "SQFS_BLK_FLAGS_ALL = 0xFCFF"),
])
rewrite("lib/sqfs/block_processor/backend.c", [
    ("\tproc->stats.total_frag_count += 1;",
     "\tif (frag->flags & SQFS_BLK_FRAGMENT_COMPRESSION_OVERRIDE) {\n"
     "\t\tfrag->flags &= ~SQFS_BLK_DONT_COMPRESS;\n"
     "\t\tif (frag->flags & SQFS_BLK_FRAGMENT_NO_COMPRESSION)\n"
     "\t\t\tfrag->flags |= SQFS_BLK_DONT_COMPRESS;\n"
     "\t}\n\n\tproc->stats.total_frag_count += 1;"),
    ("\t\t\t(SQFS_BLK_DONT_COMPRESS | SQFS_BLK_ALIGN);",
     "\t\t\t(SQFS_BLK_DONT_COMPRESS | SQFS_BLK_ALIGN |\n"
     "\t\t\t SQFS_BLK_IGNORE_SPARSE | SQFS_BLK_DONT_DEDUPLICATE);"),
    ("\t\t\t (SQFS_BLK_DONT_COMPRESS | SQFS_BLK_ALIGN));",
     "\t\t\t (SQFS_BLK_DONT_COMPRESS | SQFS_BLK_ALIGN |\n"
     "\t\t\t  SQFS_BLK_IGNORE_SPARSE | SQFS_BLK_DONT_DEDUPLICATE));"),
    ("\tif (proc->frag_tbl != NULL) {\n\t\terr = SQFS_ERROR_ALLOC;",
     "\tif (proc->frag_tbl != NULL && !(frag->flags & SQFS_BLK_DONT_DEDUPLICATE)) {\n\t\terr = SQFS_ERROR_ALLOC;"),
])
rewrite("include/simple_writer.h", [
    ("\tbool no_pad;", "\tbool no_pad;\n\tsqfs_u32 archivefs_block_flags;\n"
     "\tbool archivefs_no_inode_compression;\n\tbool archivefs_no_tail_pack;"),
])
rewrite("bin/tar2sqfs/options.c", [
    ('#include "tar2sqfs.h"', '#include "tar2sqfs.h"\n#include <errno.h>'),
    ("NO_PAD_OPTION = 1,", "NO_PAD_OPTION = 1,\n\tARCHIVEFS_BLOCK_FLAGS,\n\tARCHIVEFS_NO_INODE_COMPRESSION,"),
    ('\t{ "no-pad", no_argument, NULL, NO_PAD_OPTION },',
     '\t{ "no-pad", no_argument, NULL, NO_PAD_OPTION },\n'
     '\t{ "archivefs-block-flags", required_argument, NULL, ARCHIVEFS_BLOCK_FLAGS },\n'
     '\t{ "archivefs-no-inode-compression", no_argument, NULL, ARCHIVEFS_NO_INODE_COMPRESSION },'),
    ('\t\tcase NO_PAD_OPTION:', '\t\tcase ARCHIVEFS_BLOCK_FLAGS: {\n'
     '\t\t\tchar *end = NULL;\n\t\t\tunsigned long value;\n\t\t\terrno = 0;\n'
     '\t\t\tvalue = strtoul(optarg, &end, 0);\n'
     '\t\t\tif (errno || end == optarg || *end ||\n'
     '\t\t\t    (value & ~(unsigned long)(SQFS_BLK_DONT_COMPRESS | SQFS_BLK_DONT_FRAGMENT |\n'
     '\t\t\t        SQFS_BLK_DONT_DEDUPLICATE | SQFS_BLK_IGNORE_SPARSE |\n'
     '\t\t\t        SQFS_BLK_FRAGMENT_COMPRESSION_OVERRIDE | SQFS_BLK_FRAGMENT_NO_COMPRESSION))) {\n'
     '\t\t\t\tfputs("Invalid archivefs block flags\\n", stderr);\n\t\t\t\tgoto fail_arg;\n\t\t\t}\n'
     '\t\t\tcfg.archivefs_block_flags = (sqfs_u32)value;\n\t\t\tbreak;\n\t\t}\n'
     '\t\tcase ARCHIVEFS_NO_INODE_COMPRESSION:\n'
     '\t\t\tcfg.archivefs_no_inode_compression = true;\n\t\t\tbreak;\n\t\tcase NO_PAD_OPTION:'),
    ("\t\t\tno_tail_pack = true;", "\t\t\tno_tail_pack = true;\n\t\t\tcfg.archivefs_no_tail_pack = true;"),
])
rewrite("bin/tar2sqfs/process_tarball.c", [
    ("\tint flags = 0, ret = 0;", "\tint flags = (int)cfg.archivefs_block_flags, ret = 0;"),
])
rewrite("lib/common/writer/init.c", [
    ("void sqfs_writer_cfg_init(sqfs_writer_cfg_t *cfg)",
     "static sqfs_s32 archivefs_plain_metadata(sqfs_compressor_t *cmp,\n"
     "        const sqfs_u8 *in, sqfs_u32 size, sqfs_u8 *out, sqfs_u32 capacity)\n"
     "{\n\t(void)cmp; (void)in; (void)size; (void)out; (void)capacity;\n"
     "\t/* A zero result instructs the metadata writer to store the input. */\n\treturn 0;\n}\n\n"
     "void sqfs_writer_cfg_init(sqfs_writer_cfg_t *cfg)"),
    ("\tsqfs->idtbl = sqfs_id_table_create(0);",
     "\t/* Data workers already own their compressor copies. */\n"
     "\tif (wrcfg->archivefs_no_inode_compression) {\n"
     "\t\tsqfs->cmp->do_block = archivefs_plain_metadata;\n"
     "\t\tsqfs->super.flags |= SQFS_FLAG_UNCOMPRESSED_INODES |\n"
     "\t\t\tSQFS_FLAG_UNCOMPRESSED_IDS | SQFS_FLAG_UNCOMPRESSED_XATTRS;\n\t}\n"
     "\tif (wrcfg->archivefs_block_flags & SQFS_BLK_DONT_COMPRESS)\n"
     "\t\tsqfs->super.flags |= SQFS_FLAG_UNCOMPRESSED_DATA;\n\n"
     "\tsqfs->idtbl = sqfs_id_table_create(0);"),
])
rewrite("lib/common/writer/finish.c", [
    ("\tif (cfg->exportable) {",
     "\tif (cfg->archivefs_no_tail_pack)\n\t\tsqfs->super.flags &= ~SQFS_FLAG_ALWAYS_FRAGMENTS;\n\n"
     "\tif (cfg->exportable) {"),
])
# A SquashFS compressed block must be smaller than its uncompressed payload.
# LZ4 may expand short metadata such as the single-entry UID/GID table.
rewrite("lib/sqfs/meta_writer.c", [
    ("\tif (ret > 0) {", "\tif (ret > 0 && (size_t)ret < m->offset) {"),
])
# POSIX getsubopt returns the unmatched token through valuep. This is
# required for flag-only compressor options such as extreme and BCJ filters.
rewrite("lib/compat/getsubopt.c", [
    ("\\treturn -1;\\n}", "\\t*val = s;\\n\\treturn -1;\\n}"),
])
rewrite("lib/common/parse_size.c", [
    ("\t\tacc /= 100;\n\t\tbreak;", "\t\tacc /= 100;\n\t\t++in;\n\t\tbreak;"),
])
rewrite("include/sqfs/compressor.h", [
    ("#define SQFS_ZSTD_MIN_LEVEL (1)", "#define SQFS_ZSTD_MIN_LEVEL (-131072)"),
])
rewrite("lib/sqfs/comp/zstd.c", [
    ("\tif (cfg->level < 1 || cfg->level > (unsigned int)ZSTD_maxCLevel())",
     "\tif ((sqfs_s32)cfg->level == 0 || (sqfs_s32)cfg->level < ZSTD_minCLevel() ||\n"
     "\t    (sqfs_s32)cfg->level > ZSTD_maxCLevel())"),
])
# Win32 command-line arguments must be converted from UTF-16, independently
# of the current ANSI code page. Upstream opens UTF-8 filenames with CreateFileW.
rewrite("bin/tar2sqfs/tar2sqfs.c", [
    ("int main(int argc, char **argv)", "static int archivefs_main(int argc, char **argv)"),
])
path = root / "bin/tar2sqfs/tar2sqfs.c"
with path.open("a", encoding="utf-8", newline="\n") as out:
    out.write(r'''
#ifdef _WIN32
#include <windows.h>
int wmain(int argc, wchar_t **wargv)
{
    char **argv = calloc((size_t)argc + 1, sizeof(*argv));
    int result = EXIT_FAILURE;
    if (argv == NULL)
        return EXIT_FAILURE;
    for (int i = 0; i < argc; ++i) {
        int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                      wargv[i], -1, NULL, 0, NULL, NULL);
        if (size <= 0)
            goto done;
        argv[i] = malloc((size_t)size);
        if (argv[i] == NULL ||
            !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                 wargv[i], -1, argv[i], size, NULL, NULL))
            goto done;
    }
    result = archivefs_main(argc, argv);
done:
    for (int i = 0; i < argc; ++i)
        free(argv[i]);
    free(argv);
    return result;
}
#else
int main(int argc, char **argv) { return archivefs_main(argc, argv); }
#endif
''')
rewrite("bin/tar2sqfs/Makemodule.am", [
    ("tar2sqfs_CFLAGS = $(AM_CFLAGS) $(PTHREAD_CFLAGS)",
     "tar2sqfs_CFLAGS = $(AM_CFLAGS) $(PTHREAD_CFLAGS)\n"
     "if WINDOWS\n"
     "tar2sqfs_LDFLAGS = $(AM_LDFLAGS) -municode\n"
     "endif"),
])
