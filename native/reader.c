// SPDX-License-Identifier: GPL-3.0-only
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sqfs/predef.h>
#include <sqfs/io.h>
#include <sqfs/super.h>
#include <sqfs/compressor.h>
#include <sqfs/dir_reader.h>
#include <sqfs/frag_table.h>
#include <sqfs/block.h>
#include <sqfs/dir.h>
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
    sqfs_u8 *data;
    sqfs_u64 offset;
    sqfs_u32 encoded_size;
    size_t length;
    int valid;
} block_cache_t;

typedef struct {
    sqfs_file_t *file;
    sqfs_compressor_t *cmp;
    sqfs_dir_reader_t *dir;
    sqfs_id_table_t *ids;
    sqfs_frag_table_t *fragments;
    sqfs_u8 *scratch;
    block_cache_t data, fragment;
    const sqfs_inode_generic_t *indexed_inode;
    sqfs_u64 *checkpoints;
    size_t indexed_count;
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

static int is_directory(const sqfs_inode_generic_t *inode) {
    return inode->base.type == SQFS_INODE_DIR || inode->base.type == SQFS_INODE_EXT_DIR;
}

/* Build and release the metadata tree iteratively, including malformed images
 * with very deep directory structures. Library-owned allocations use sqfs_free. */
static void destroy_tree(sqfs_tree_node_t *root) {
    while (root) {
        if (root->children) {
            sqfs_tree_node_t *child = root->children;
            root->children = child->next;
            root = child;
        } else {
            sqfs_tree_node_t *parent = root->parent;
            sqfs_free(root->inode);
            free(root);
            root = parent;
        }
    }
}

static int make_node(image_t *im, sqfs_tree_node_t *parent, sqfs_inode_generic_t *inode,
                     const char *name, size_t length, sqfs_tree_node_t **out) {
    if (!inode->base.inode_number || inode->base.inode_number > im->super.inode_count ||
        inode->base.type < SQFS_INODE_DIR || inode->base.type > SQFS_INODE_EXT_SOCKET)
        return SQFS_ERROR_CORRUPTED;
    sqfs_tree_node_t *node = calloc(1, sizeof(*node) + length + 1);
    if (!node) return SQFS_ERROR_ALLOC;
    int rc = sqfs_id_table_index_to_id(im->ids, inode->base.uid_idx, &node->uid);
    if (!rc) rc = sqfs_id_table_index_to_id(im->ids, inode->base.gid_idx, &node->gid);
    if (rc) { free(node); return rc; }
    node->parent = parent;
    node->inode = inode;
    memcpy(node->name, name, length);
    *out = node;
    return 0;
}

static int build_tree(image_t *im) {
    sqfs_inode_generic_t *inode = NULL;
    int rc = sqfs_dir_reader_get_root_inode(im->dir, &inode);
    if (rc) return rc;
    if (!is_directory(inode)) { sqfs_free(inode); return SQFS_ERROR_CORRUPTED; }
    rc = make_node(im, NULL, inode, "", 0, &im->root);
    if (rc) { sqfs_free(inode); return rc; }

    /* Directory hard links are invalid in SquashFS. A bitmap also rejects cycles. */
    size_t seen_size = ((size_t)im->super.inode_count + 7) / 8;
    unsigned char *seen = calloc(seen_size, 1);
    if (!seen) return SQFS_ERROR_ALLOC;
    sqfs_tree_node_t *node = im->root;
    while (node) {
        if (is_directory(node->inode)) {
            size_t number = (size_t)node->inode->base.inode_number - 1;
            unsigned char mask = (unsigned char)(1u << (number & 7));
            if (seen[number / 8] & mask) { rc = SQFS_ERROR_LINK_LOOP; break; }
            seen[number / 8] |= mask;
            rc = sqfs_dir_reader_open_dir(im->dir, node->inode, SQFS_DIR_OPEN_NO_DOT_ENTRIES);
            if (rc) break;
            sqfs_tree_node_t **tail = &node->children;
            for (;;) {
                sqfs_dir_entry_t *ent = NULL;
                rc = sqfs_dir_reader_read(im->dir, &ent);
                if (rc > 0) { rc = 0; break; }
                if (rc < 0) break;
                size_t length = (size_t)ent->size + 1;
                const char *name = (const char *)ent->name;
                if (length > 256 || memchr(name, 0, length) || memchr(name, '/', length) ||
                    (length == 1 && name[0] == '.') || (length == 2 && name[0] == '.' && name[1] == '.')) {
                    sqfs_free(ent); rc = SQFS_ERROR_CORRUPTED; break;
                }
                inode = NULL;
                rc = sqfs_dir_reader_get_inode(im->dir, &inode);
                if (!rc) {
                    rc = make_node(im, node, inode, name, length, tail);
                    if (!rc) tail = &(*tail)->next;
                    else sqfs_free(inode);
                }
                sqfs_free(ent);
                if (rc) break;
            }
            if (rc) break;
        }
        if (node->children) node = node->children;
        else {
            while (node && !node->next) node = node->parent;
            if (node) node = node->next;
        }
    }
    free(seen);
    return rc;
}

API void ws_close(image_t *im) {
    if (!im) return;
    destroy_tree(im->root);
    free(im->scratch);
    free(im->data.data);
    free(im->fragment.data);
    free(im->checkpoints);
    sqfs_destroy(im->fragments);
    sqfs_destroy(im->ids);
    sqfs_destroy(im->dir);
    sqfs_destroy(im->cmp);
    sqfs_destroy(im->file);
    free(im);
}

typedef int (*source_read_t)(sqfs_u64 offset, void *buffer, size_t size);
typedef struct {
    sqfs_file_t base;
    source_read_t read;
    sqfs_u64 size;
} source_file_t;

static void source_destroy(sqfs_object_t *obj) { free(obj); }
static sqfs_object_t *source_copy(const sqfs_object_t *obj) {
    source_file_t *copy = malloc(sizeof(*copy));
    if (copy) memcpy(copy, obj, sizeof(*copy));
    return (sqfs_object_t *)copy;
}
static int source_read_at(sqfs_file_t *base, sqfs_u64 offset, void *buffer, size_t size) {
    source_file_t *file = (source_file_t *)base;
    if (offset > file->size || size > file->size - offset) return SQFS_ERROR_OUT_OF_BOUNDS;
    return size ? file->read(offset, buffer, size) : 0;
}
static int source_write_at(sqfs_file_t *base, sqfs_u64 offset, const void *buffer, size_t size) {
    (void)base; (void)offset; (void)buffer; (void)size;
    return SQFS_ERROR_UNSUPPORTED;
}
static sqfs_u64 source_size(const sqfs_file_t *base) { return ((const source_file_t *)base)->size; }
static int source_truncate(sqfs_file_t *base, sqfs_u64 size) {
    (void)base; (void)size; return SQFS_ERROR_UNSUPPORTED;
}

static int open_image(sqfs_file_t *file, image_t **out) {
    *out = NULL;
    if (!file) return SQFS_ERROR_IO;
    int rc = SQFS_ERROR_ALLOC;
    image_t *im = calloc(1, sizeof(*im));
    if (!im) { sqfs_destroy(file); return rc; }
    im->file = file;
    rc = sqfs_super_read(&im->super, im->file);
    if (rc) goto fail;
    if (im->super.bytes_used > im->file->get_size(im->file) ||
        im->super.inode_count == 0 || im->super.inode_table_start < sizeof(sqfs_super_t) ||
        im->super.directory_table_start < im->super.inode_table_start ||
        im->super.id_table_start < im->super.directory_table_start ||
        im->super.id_table_start >= im->super.bytes_used) {
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
    im->fragments = sqfs_frag_table_create(0);
    im->scratch = malloc(im->super.block_size);
    im->data.data = malloc(im->super.block_size);
    im->fragment.data = malloc(im->super.block_size);
    if (!im->dir || !im->ids || !im->fragments || !im->scratch || !im->data.data || !im->fragment.data) {
        rc = SQFS_ERROR_ALLOC; goto fail;
    }
    rc = sqfs_id_table_read(im->ids, im->file, &im->super, im->cmp);
    if (rc) goto fail;
    rc = sqfs_frag_table_read(im->fragments, im->file, &im->super, im->cmp);
    if (rc) goto fail;
    rc = build_tree(im);
    if (rc) goto fail;
    *out = im;
    return 0;
fail:
    ws_close(im);
    return rc;
}

API int ws_open(const char *path, image_t **out) {
    return open_image(sqfs_open_file(path, SQFS_FILE_OPEN_READ_ONLY), out);
}

API int ws_open_source(sqfs_u64 size, source_read_t read, image_t **out) {
    *out = NULL;
    if (!read) return SQFS_ERROR_ARG_INVALID;
    source_file_t *file = calloc(1, sizeof(*file));
    if (!file) return SQFS_ERROR_ALLOC;
    file->base.base.destroy = source_destroy;
    file->base.base.copy = source_copy;
    file->base.read_at = source_read_at;
    file->base.write_at = source_write_at;
    file->base.get_size = source_size;
    file->base.truncate = source_truncate;
    file->read = read;
    file->size = size;
    return open_image(&file->base, out);
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
    else if (n->inode->base.type == SQFS_INODE_SLINK)
        size = n->inode->data.slink.target_size;
    else if (n->inode->base.type == SQFS_INODE_EXT_SLINK)
        size = n->inode->data.slink_ext.target_size;
    return size;
}
API uint64_t ws_image_size(image_t *im) { return im->super.bytes_used; }
API uint32_t ws_block_size(image_t *im) { return im->super.block_size; }
API uint32_t ws_compression(image_t *im) { return im->super.compression_id; }
/* Data and fragment blocks are decoded into bounded buffers. Validate the actual
 * decompressed size before copying, and use 64-bit fragment arithmetic. */
static int block_location_valid(image_t *im, sqfs_u64 offset, sqfs_u32 size) {
    sqfs_u32 length = SQFS_ON_DISK_BLOCK_SIZE(size);
    if ((size & 0xFE000000u) || length > im->super.block_size) return 0;
    if (!length) return size == 0;
    return offset >= sizeof(sqfs_super_t) && offset <= im->super.inode_table_start &&
        length <= im->super.inode_table_start - offset;
}

static int load_block(image_t *im, block_cache_t *block, sqfs_u64 offset, sqfs_u32 size) {
    if (block->valid && block->offset == offset && block->encoded_size == size) return 0;
    block->valid = 0;
    if (!block_location_valid(im, offset, size) || SQFS_IS_SPARSE_BLOCK(size))
        return SQFS_ERROR_CORRUPTED;
    sqfs_u32 length = SQFS_ON_DISK_BLOCK_SIZE(size);
    int rc;
    if (SQFS_IS_BLOCK_COMPRESSED(size)) {
        rc = im->file->read_at(im->file, offset, im->scratch, length);
        if (rc) return rc;
        sqfs_s32 decoded = im->cmp->do_block(im->cmp, im->scratch, length, block->data, im->super.block_size);
        if (decoded <= 0 || (sqfs_u32)decoded > im->super.block_size)
            return SQFS_ERROR_CORRUPTED;
        block->length = (size_t)decoded;
    } else {
        rc = im->file->read_at(im->file, offset, block->data, length);
        if (rc) return rc;
        block->length = length;
    }
    block->offset = offset;
    block->encoded_size = size;
    block->valid = 1;
    return 0;
}

#define CHECKPOINT_STRIDE 256u

static int index_file(image_t *im, const sqfs_inode_generic_t *inode, sqfs_u64 file_size,
                      sqfs_u32 fragment_index) {
    if (im->indexed_inode == inode) return 0;
    size_t count = sqfs_inode_get_file_block_count(inode);
    sqfs_u64 full = file_size / im->super.block_size;
    sqfs_u64 tail = file_size % im->super.block_size;
    sqfs_u64 expected = full + (tail && fragment_index == UINT32_MAX ? 1u : 0u);
    if ((sqfs_u64)count != expected || (tail == 0 && fragment_index != UINT32_MAX))
        return SQFS_ERROR_CORRUPTED;
    size_t checkpoints = count / CHECKPOINT_STRIDE + 1;
    sqfs_u64 *offsets = malloc(checkpoints * sizeof(*offsets));
    if (!offsets) return SQFS_ERROR_ALLOC;
    sqfs_u64 offset;
    int rc = sqfs_inode_get_file_block_start(inode, &offset);
    if (rc) { free(offsets); return rc; }
    offsets[0] = offset;
    for (size_t i = 0; i < count; i++) {
        if (i % CHECKPOINT_STRIDE == 0) offsets[i / CHECKPOINT_STRIDE] = offset;
        sqfs_u32 size = inode->extra[i];
        if (!block_location_valid(im, offset, size)) {
            free(offsets); return SQFS_ERROR_CORRUPTED;
        }
        offset += SQFS_ON_DISK_BLOCK_SIZE(size);
    }
    if (count % CHECKPOINT_STRIDE == 0) offsets[count / CHECKPOINT_STRIDE] = offset;
    free(im->checkpoints);
    im->checkpoints = offsets;
    im->indexed_inode = inode;
    im->indexed_count = count;
    return 0;
}

API int ws_read(image_t *im, const sqfs_tree_node_t *n, uint64_t offset,
                void *buffer, uint32_t size) {
    const sqfs_inode_generic_t *inode = n->inode;
    if (size > INT32_MAX) return SQFS_ERROR_ARG_INVALID;
    if (inode->base.type == SQFS_INODE_SLINK || inode->base.type == SQFS_INODE_EXT_SLINK) {
        uint64_t length = ws_size(n);
        if (length > inode->payload_bytes_used) return SQFS_ERROR_CORRUPTED;
        if (offset >= length || !size) return 0;
        if (length - offset < size) size = (uint32_t)(length - offset);
        memcpy(buffer, (const unsigned char *)inode->extra + (size_t)offset, size);
        return (int)size;
    }
    if (inode->base.type != SQFS_INODE_FILE && inode->base.type != SQFS_INODE_EXT_FILE)
        return SQFS_ERROR_ARG_INVALID;
    sqfs_u64 file_size;
    sqfs_u32 frag_idx, frag_off;
    int rc = sqfs_inode_get_file_size(inode, &file_size);
    if (rc) return rc;
    rc = sqfs_inode_get_frag_location(inode, &frag_idx, &frag_off);
    if (rc) return rc;
    rc = index_file(im, inode, file_size, frag_idx);
    if (rc) return rc;
    if (offset >= file_size || !size) return 0;
    if (file_size - offset < size) size = (sqfs_u32)(file_size - offset);
    sqfs_u32 remaining = size;
    unsigned char *out = buffer;
    size_t index = (size_t)(offset / im->super.block_size);
    sqfs_u32 within = (sqfs_u32)(offset % im->super.block_size);
    sqfs_u64 physical = 0;
    if (index < im->indexed_count) {
        size_t checkpoint = index / CHECKPOINT_STRIDE;
        physical = im->checkpoints[checkpoint];
        for (size_t i = checkpoint * CHECKPOINT_STRIDE; i < index; i++)
            physical += SQFS_ON_DISK_BLOCK_SIZE(inode->extra[i]);
    }
    while (remaining && index < im->indexed_count) {
        sqfs_u64 start = (sqfs_u64)index * im->super.block_size;
        sqfs_u32 expected = (sqfs_u32)((file_size - start < im->super.block_size) ?
                                       file_size - start : im->super.block_size);
        if (within >= expected) return SQFS_ERROR_CORRUPTED;
        sqfs_u32 take = expected - within;
        if (take > remaining) take = remaining;
        sqfs_u32 encoded = inode->extra[index];
        if (SQFS_IS_SPARSE_BLOCK(encoded)) memset(out, 0, take);
        else {
            rc = load_block(im, &im->data, physical, encoded);
            if (rc) return rc;
            if (im->data.length != expected) return SQFS_ERROR_CORRUPTED;
            memcpy(out, im->data.data + within, take);
        }
        physical += SQFS_ON_DISK_BLOCK_SIZE(encoded);
        remaining -= take; out += take; index++; within = 0;
    }
    if (remaining) {
        sqfs_fragment_t frag;
        if (frag_idx == UINT32_MAX) return SQFS_ERROR_CORRUPTED;
        rc = sqfs_frag_table_lookup(im->fragments, frag_idx, &frag);
        if (rc) return rc;
        rc = load_block(im, &im->fragment, frag.start_offset, frag.size);
        if (rc) return rc;
        sqfs_u64 start = (sqfs_u64)frag_off + within;
        if (start > im->fragment.length || remaining > im->fragment.length - start)
            return SQFS_ERROR_CORRUPTED;
        memcpy(out, im->fragment.data + (size_t)start, remaining);
    }
    return (int)size;
}
