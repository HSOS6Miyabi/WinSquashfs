// SPDX-License-Identifier: GPL-3.0-only
// Use the existing SquashFS metadata and block validation implementation.
// Build this translation unit instead of native/reader.c.
#define ws_close ws_impl_close
#define ws_open ws_impl_open
#define ws_open_source ws_impl_open_source
#define ws_root ws_impl_root
#define ws_child ws_impl_child
#define ws_next ws_impl_next
#define ws_name ws_impl_name
#define ws_type ws_impl_type
#define ws_mtime ws_impl_mtime
#define ws_inode ws_impl_inode
#define ws_size ws_impl_size
#define ws_image_size ws_impl_image_size
#define ws_block_size ws_impl_block_size
#define ws_compression ws_impl_compression
#define ws_read ws_impl_read
#include "../reader.c"
#undef ws_close
#undef ws_open
#undef ws_open_source
#undef ws_root
#undef ws_child
#undef ws_next
#undef ws_name
#undef ws_type
#undef ws_mtime
#undef ws_inode
#undef ws_size
#undef ws_image_size
#undef ws_block_size
#undef ws_compression
#undef ws_read

API void ws_close(void *image) { ws_impl_close(image); }
API int ws_open(const char *path, void **out) { image_t *image = NULL; int rc = ws_impl_open(path, &image); *out = image; return rc; }
API int ws_open_source(uint64_t size, source_read_t reader, void **out) { image_t *image = NULL; int rc = ws_impl_open_source(size, reader, &image); *out = image; return rc; }
API const void *ws_root(void *image) { return ws_impl_root(image); }
API const void *ws_child(const void *node) { return ws_impl_child(node); }
API const void *ws_next(const void *node) { return ws_impl_next(node); }
API const char *ws_name(const void *node) { return ws_impl_name(node); }
API uint32_t ws_type(const void *node) { return ws_impl_type(node); }
API uint32_t ws_mtime(const void *node) { return ws_impl_mtime(node); }
API uint32_t ws_inode(const void *node) { return ws_impl_inode(node); }
API uint64_t ws_size(const void *node) { return ws_impl_size(node); }
API uint64_t ws_image_size(void *image) { return ws_impl_image_size(image); }
API uint32_t ws_block_size(void *image) { return ws_impl_block_size(image); }
API uint32_t ws_compression(void *image) { return ws_impl_compression(image); }
API int ws_read(void *image, const void *node, uint64_t offset, void *buffer, uint32_t size) { return ws_impl_read(image, node, offset, buffer, size); }

/* The source callback is positioned and thread-local in the C++ adapter.
 * Each worker owns the mutable decompressor, block buffers and checkpoints.
 * Metadata tree and fragment table are immutable for the image lifetime. */
API void ws_read_context_destroy(void *context) {
    image_t *ctx = context;
    if (!ctx) return;
    free(ctx->scratch);
    free(ctx->data.data);
    free(ctx->fragment.data);
    free(ctx->checkpoints);
    sqfs_destroy(ctx->cmp);
    sqfs_destroy(ctx->file);
    free(ctx);
}

API void *ws_read_context_create(void *image) {
    image_t *owner = image;
    if (!owner || !owner->file || owner->file->read_at != source_read_at)
        return NULL;
    image_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->super = owner->super;
    ctx->fragments = owner->fragments;
    ctx->file = sqfs_copy(owner->file);
    ctx->cmp = sqfs_copy(owner->cmp);
    ctx->scratch = malloc(owner->super.block_size);
    ctx->data.data = malloc(owner->super.block_size);
    ctx->fragment.data = malloc(owner->super.block_size);
    if (!ctx->file || !ctx->cmp || !ctx->scratch || !ctx->data.data || !ctx->fragment.data) {
        ws_read_context_destroy(ctx);
        return NULL;
    }
    return ctx;
}
