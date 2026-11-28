// SPDX-License-Identifier: GPL-3.0-only
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#ifdef _WIN32
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C" __attribute__((visibility("default")))
#endif

using SourceRead = int(*)(std::uint64_t, void*, std::size_t);
using Visit = int(*)(std::uint64_t, const char*, std::size_t, std::uint32_t, std::uint64_t, std::uint64_t);
struct Node { const char* name; std::uint32_t inode, type; std::uint64_t size; const Node* next; const Node* child; };
static const Node nested{"small.txt", 4, 2, 20, nullptr, nullptr};
static const Node folder{"folder", 3, 1, 0, nullptr, &nested};
static const Node file{"data.bin", 2, 2, 8193, &folder, nullptr};
static const Node root{"", 1, 1, 0, nullptr, &file};
struct Image { SourceRead read; std::uint64_t size; };
static int open_image(std::uint64_t size, SourceRead read, void** out) {
    *out = nullptr;
    char head[6]{};
    if (size < 1028 + 8193 || read(0, head, sizeof(head))) return -1;
    auto image = new(std::nothrow) Image{read, size};
    if (!image) return -1;
    *out = image;
    return 0;
}
static int read_image(Image* image, std::uint64_t inode, std::uint64_t offset, void* output, std::uint32_t count) {
    std::uint64_t length = inode == 2 ? 8193 : inode == 4 ? 20 : 0;
    if (offset > length || count > length - offset) return -1;
    if (count && image->read(1028 + offset, output, count)) return -1;
    return static_cast<int>(count);
}
EXPORT int ws_open_source(std::uint64_t size, SourceRead read, void** out) { return open_image(size, read, out); }
EXPORT void ws_close(void* image) { delete static_cast<Image*>(image); }
EXPORT const void* ws_root(void*) { return &root; }
EXPORT const void* ws_child(const void* n) { return static_cast<const Node*>(n)->child; }
EXPORT const void* ws_next(const void* n) { return static_cast<const Node*>(n)->next; }
EXPORT const char* ws_name(const void* n) { return static_cast<const Node*>(n)->name; }
EXPORT std::uint32_t ws_inode(const void* n) { return static_cast<const Node*>(n)->inode; }
EXPORT std::uint32_t ws_type(const void* n) { return static_cast<const Node*>(n)->type; }
EXPORT std::uint32_t ws_mtime(const void*) { return 1700000000; }
EXPORT std::uint64_t ws_size(const void* n) { return static_cast<const Node*>(n)->size; }
EXPORT std::uint64_t ws_image_size(void* image) { return static_cast<Image*>(image)->size; }
EXPORT std::uint32_t ws_block_size(void*) { return 4096; }
EXPORT std::uint32_t ws_compression(void*) { return 6; }
EXPORT int ws_read(void* image, const void* n, std::uint64_t offset, void* output, std::uint32_t count) {
    return read_image(static_cast<Image*>(image), static_cast<const Node*>(n)->inode, offset, output, count);
}
EXPORT int wa_open(std::uint64_t size, SourceRead read, void** out, std::uint64_t* id, std::uint32_t* block, std::uint64_t* modified) {
    *id=1; *block=4096; *modified=1700000000;
    return open_image(size, read, out);
}
EXPORT void wa_close(void* image) { ws_close(image); }
EXPORT int wa_list(void*, std::uint64_t inode, Visit visit) {
    auto node = inode == 1 ? root.child : inode == 3 ? folder.child : nullptr;
    if (!node) return -1;
    for (; node; node=node->next) {
        if (visit(node->inode,node->name,std::strlen(node->name),node->type==1?0040755:0100644,node->size,1700000000)) return -1;
    }
    return 0;
}
EXPORT int wa_read(void* image,std::uint64_t inode,std::uint64_t offset,void* output,std::uint32_t count) {
    return read_image(static_cast<Image*>(image),inode,offset,output,count);
}
EXPORT void* ws_read_context_create(void* image) {return new(std::nothrow) Image(*static_cast<Image*>(image));}
EXPORT void ws_read_context_destroy(void* context) {delete static_cast<Image*>(context);}
