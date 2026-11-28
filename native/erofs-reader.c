// SPDX-License-Identifier: GPL-3.0-only
#include "erofs/internal.h"
#include "erofs/dir.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define API __declspec(dllexport)
typedef int (*source_read_t)(uint64_t, void *, size_t);
typedef int (*visit_t)(uint64_t, const char *, size_t, uint32_t, uint64_t, uint64_t);

typedef struct {
    struct erofs_sb_info sbi;
    source_read_t read;
    uint64_t length;
} image_t;

static ssize_t read_source(struct erofs_vfile *vf, void *buffer, size_t length, u64 offset)
{
    image_t *im;
    memcpy(&im, vf->payload, sizeof(im));
    if (offset > im->length) return 0;
    if (length > im->length - offset) length = (size_t)(im->length - offset);
    if (length && im->read(offset, buffer, length)) return -EIO;
    return (ssize_t)length;
}

static struct erofs_vfops source_ops = { .pread = read_source };

API void wa_close(image_t *im)
{
    if (!im) return;
    erofs_put_super(&im->sbi);
    free(im);
}

API int wa_open(uint64_t length, source_read_t reader, image_t **out,
    uint64_t *root, uint32_t *block, uint64_t *mtime)
{
    *out = NULL;
    image_t *im = calloc(1, sizeof(*im));
    if (!im) return -ENOMEM;
    im->read = reader;
    im->length = length;
    im->sbi.bdev.ops = &source_ops;
    memcpy(im->sbi.bdev.payload, &im, sizeof(im));
    im->sbi.devsz = length;
    int rc = erofs_read_superblock(&im->sbi);
    if (!rc && im->sbi.extra_devices) rc = -ENOTSUP;
    if (!rc && im->sbi.dif0.blocks > length / erofs_blksiz(&im->sbi)) rc = -EIO;
    struct erofs_inode inode = { .sbi = &im->sbi, .nid = im->sbi.root_nid };
    if (!rc) rc = erofs_read_inode_from_disk(&inode);
    if (!rc && !S_ISDIR(inode.i_mode)) rc = -EIO;
    if (rc) { wa_close(im); return rc; }
    *root = inode.nid;
    *block = (uint32_t)erofs_blksiz(&im->sbi);
    *mtime = inode.i_mtime;
    *out = im;
    return 0;
}

struct list_context { struct erofs_dir_context ctx; visit_t visit; };
static int visit_entry(struct erofs_dir_context *ctx)
{
    struct list_context *list = (struct list_context *)ctx;
    if (ctx->dot_dotdot) return 0;
    struct erofs_inode inode = { .sbi = ctx->dir->sbi, .nid = ctx->de_nid };
    int rc = erofs_read_inode_from_disk(&inode);
    if (rc) return rc;
    return list->visit(inode.nid, ctx->dname, ctx->de_namelen,
        inode.i_mode, inode.i_size, inode.i_mtime) ? -EIO : 0;
}

API int wa_list(image_t *im, uint64_t id, visit_t visit)
{
    struct erofs_inode inode = { .sbi = &im->sbi, .nid = id };
    int rc = erofs_read_inode_from_disk(&inode);
    if (rc) return rc;
    struct list_context list = { .ctx = { .dir = &inode, .cb = visit_entry }, .visit = visit };
    return erofs_iterate_dir(&list.ctx, false);
}

API int wa_read(image_t *im, uint64_t id, uint64_t offset, void *buffer, uint32_t length)
{
    if (length > INT32_MAX) return -EINVAL;
    struct erofs_inode inode = { .sbi = &im->sbi, .nid = id };
    int rc = erofs_read_inode_from_disk(&inode);
    if (rc) return rc;
    if (!S_ISREG(inode.i_mode) && !S_ISLNK(inode.i_mode)) return -EINVAL;
    if (offset >= inode.i_size) return 0;
    if (length > inode.i_size - offset) length = (uint32_t)(inode.i_size - offset);
    struct erofs_vfile vf = {0};
    rc = erofs_iopen(&vf, &inode);
    if (!rc) rc = erofs_pread(&vf, buffer, length, offset);
    return rc ? rc : (int)length;
}
