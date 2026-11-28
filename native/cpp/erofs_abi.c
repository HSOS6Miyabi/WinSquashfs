// SPDX-License-Identifier: GPL-3.0-only
#define wa_open wa_impl_open
#define wa_close wa_impl_close
#define wa_list wa_impl_list
#define wa_read wa_impl_read
#include "erofs-reader-impl.c"
#undef wa_open
#undef wa_close
#undef wa_list
#undef wa_read
API int wa_open(uint64_t length, source_read_t reader, void **out, uint64_t *root, uint32_t *block, uint64_t *mtime) {
    image_t *image = NULL;
    int result = wa_impl_open(length, reader, &image, root, block, mtime);
    *out = image;
    return result;
}
API void wa_close(void *image) { wa_impl_close(image); }
API int wa_list(void *image, uint64_t id, visit_t visit) { return wa_impl_list(image, id, visit); }
API int wa_read(void *image, uint64_t id, uint64_t offset, void *buffer, uint32_t length) { return wa_impl_read(image, id, offset, buffer, length); }
