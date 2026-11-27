// SPDX-License-Identifier: GPL-3.0-only
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sqfs/predef.h>
#include <sqfs/io.h>
#include <sqfs/super.h>
#include <sqfs/compressor.h>
#include <sqfs/dir_reader.h>
#include <sqfs/data_reader.h>
#include <sqfs/id_table.h>
#include <sqfs/inode.h>
#include <sqfs/error.h>
#include <lzo/lzo1x.h>

#ifdef _WIN32
#define API __declspec(dllexport)
#else
#define API __attribute__((visibility("default")))
#endif

typedef struct {
    sqfs_file_t *file;
    sqfs_compressor_t *cmp;
    sqfs_dir_reader_t *dir;
    sqfs_id_table_t *ids;
    sqfs_data_reader_t *data;
    sqfs_tree_node_t *root;
    sqfs_super_t super;
} image_t;

/* LZO is an optional GPL codec outside the LGPL libsquashfs library. */
static void lzo_destroy(sqfs_object_t *p) { free(p); }
static sqfs_object_t *lzo_copy(const sqfs_object_t *p) {
    sqfs_compressor_t *copy = malloc(sizeof(*copy));
    if (copy) memcpy(copy, p, sizeof(*copy));
    return (sqfs_object_t *)copy;
}
static sqfs_s32 lzo_decode(sqfs_compressor_t *cmp, const sqfs_u8 *in,
                           sqfs_u32 size, sqfs_u8 *out, sqfs_u32 capacity) {
    (void)cmp;
    lzo_uint n = capacity;
    int rc = lzo1x_decompress_safe(in, size, out, &n, NULL);
    return rc == LZO_E_OK && n <= capacity ? (sqfs_s32)n : SQFS_ERROR_COMPRESSOR;
}
static int lzo_options(sqfs_compressor_t *cmp, sqfs_file_t *file) {
    (void)cmp; (void)file; return 0;
}
static sqfs_compressor_t *lzo_create(void) {
    if (lzo_init() != LZO_E_OK) return NULL;
    sqfs_compressor_t *p = calloc(1, sizeof(*p));
    if (p) {
        p->base.destroy = lzo_destroy;
        p->base.copy = lzo_copy;
        p->do_block = lzo_decode;
        p->read_options = lzo_options;
    }
    return p;
}

API void ws_close(image_t *im) {
    if (!im) return;
    if (im->root) sqfs_dir_tree_destroy(im->root);
    sqfs_destroy(im->data);
    sqfs_destroy(im->ids);
    sqfs_destroy(im->dir);
    sqfs_destroy(im->cmp);
    sqfs_destroy(im->file);
    free(im);
}

API int ws_open(const char *path, image_t **out) {
    *out = NULL;
    int rc = SQFS_ERROR_ALLOC;
    image_t *im = calloc(1, sizeof(*im));
    if (!im) return rc;
    im->file = sqfs_open_file(path, SQFS_FILE_OPEN_READ_ONLY);
    if (!im->file) { rc = SQFS_ERROR_IO; goto fail; }
    rc = sqfs_super_read(&im->super, im->file);
    if (rc) goto fail;
    if (im->super.bytes_used > im->file->get_size(im->file)) {
        rc = SQFS_ERROR_CORRUPTED; goto fail;
    }
    if (im->super.compression_id == SQFS_COMP_LZO) {
        im->cmp = lzo_create();
        if (!im->cmp) { rc = SQFS_ERROR_ALLOC; goto fail; }
    } else {
        sqfs_compressor_config_t cfg;
        rc = sqfs_compressor_config_init(&cfg, im->super.compression_id,
                     im->super.block_size, SQFS_COMP_FLAG_UNCOMPRESS);
        if (rc) goto fail;
        rc = sqfs_compressor_create(&cfg, &im->cmp);
        if (rc) goto fail;
    }
    if (im->super.flags & SQFS_FLAG_COMPRESSOR_OPTIONS) {
        rc = im->cmp->read_options(im->cmp, im->file);
        if (rc) goto fail;
    }
    im->dir = sqfs_dir_reader_create(&im->super, im->cmp, im->file, 0);
    im->ids = sqfs_id_table_create(0);
    im->data = sqfs_data_reader_create(im->file, im->super.block_size, im->cmp, 0);
    if (!im->dir || !im->ids || !im->data) { rc = SQFS_ERROR_ALLOC; goto fail; }
    rc = sqfs_id_table_read(im->ids, im->file, &im->super, im->cmp);
    if (rc) goto fail;
    rc = sqfs_data_reader_load_fragment_table(im->data, &im->super);
    if (rc) goto fail;
    rc = sqfs_dir_reader_get_full_hierarchy(im->dir, im->ids, NULL, 0, &im->root);
    if (rc) goto fail;
    *out = im;
    return 0;
fail:
    ws_close(im);
    return rc;
}

API const sqfs_tree_node_t *ws_root(image_t *im) { return im->root; }
API const sqfs_tree_node_t *ws_child(const sqfs_tree_node_t *n) { return n->children; }
API const sqfs_tree_node_t *ws_next(const sqfs_tree_node_t *n) { return n->next; }
API const char *ws_name(const sqfs_tree_node_t *n) { return (const char *)n->name; }
API uint32_t ws_type(const sqfs_tree_node_t *n) { return n->inode->base.type; }
API uint32_t ws_mtime(const sqfs_tree_node_t *n) { return n->inode->base.mod_time; }
API uint32_t ws_inode(const sqfs_tree_node_t *n) { return n->inode->base.inode_number; }
API uint64_t ws_size(const sqfs_tree_node_t *n) {
    sqfs_u64 size = 0;
    if (n->inode->base.type == SQFS_INODE_FILE || n->inode->base.type == SQFS_INODE_EXT_FILE)
        sqfs_inode_get_file_size(n->inode, &size);
    return size;
}
API uint64_t ws_image_size(image_t *im) { return im->super.bytes_used; }
API uint32_t ws_block_size(image_t *im) { return im->super.block_size; }
API uint32_t ws_compression(image_t *im) { return im->super.compression_id; }
API int ws_read(image_t *im, const sqfs_tree_node_t *n, uint64_t offset,
                void *buffer, uint32_t size) {
    if (n->inode->base.type != SQFS_INODE_FILE && n->inode->base.type != SQFS_INODE_EXT_FILE)
        return SQFS_ERROR_ARG_INVALID;
    if (size > INT32_MAX) return SQFS_ERROR_ARG_INVALID;
    return sqfs_data_reader_read(im->data, n->inode, offset, buffer, size);
}
