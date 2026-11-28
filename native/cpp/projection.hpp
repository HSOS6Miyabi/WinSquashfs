// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "archive_tree.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>
namespace archivefs {
using ArchiveOpener=std::function<std::shared_ptr<ArchiveTree>(const std::filesystem::path&,SourcePolicy)>;
ArchiveOpener make_archive_opener(std::filesystem::path decoders,std::size_t cache_bytes=64u<<20);
std::vector<std::filesystem::path> extract_batch(const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& directory,const ArchiveOpener& opener,std::stop_token stop={});
class ImagePool {
    struct Entry;
    struct State;
public:
    class Lease {
    public:
        ~Lease();
        const std::shared_ptr<ArchiveTree>& tree()const noexcept{return tree_;}
    private:
        friend class ImagePool;
        Lease(std::shared_ptr<State> state,std::shared_ptr<Entry> entry,std::shared_ptr<ArchiveTree> tree):state_(std::move(state)),entry_(std::move(entry)),tree_(std::move(tree)){}
        std::shared_ptr<State> state_;std::shared_ptr<Entry> entry_;std::shared_ptr<ArchiveTree> tree_;
    };
    struct Settings {std::chrono::milliseconds idle_time{60000};std::size_t idle_images=8;};
    ImagePool(ArchiveOpener opener,Settings settings);
    ~ImagePool();
    ImagePool(const ImagePool&)=delete;
    ImagePool& operator=(const ImagePool&)=delete;
    std::shared_ptr<Lease> acquire(const std::filesystem::path& path);
    std::size_t size()const;
    void sweep();
private:
    std::shared_ptr<State> state_;
    std::jthread worker_;
};
struct ProjectionInfo {bool directory;std::uint64_t size;std::int64_t modified;std::uint64_t index;};
struct ProjectedNode {
    std::u16string name,path;
    std::filesystem::path physical;
    ProjectionInfo info{};
    std::shared_ptr<ArchiveTree> archive;
    ArchiveTree::NodePtr entry;
    std::shared_ptr<ImagePool::Lease> lease;
    bool synthetic=false;
};
using ProjectedNodePtr=std::shared_ptr<const ProjectedNode>;
class Projection {
public:
    virtual ~Projection()=default;
    virtual ProjectedNodePtr open(std::u16string_view path)const=0;
    virtual std::vector<ProjectedNodePtr> list(const ProjectedNodePtr& directory)const=0;
    virtual std::uint64_t capacity()const=0;
    virtual void validate(const ProjectedNodePtr& node)const;
    std::size_t read(const ProjectedNodePtr& node,std::uint64_t offset,std::span<std::byte> output)const;
};
std::shared_ptr<Projection> mount_projection(std::shared_ptr<ArchiveTree> tree);
struct Share {std::u16string name;std::filesystem::path root;bool network=false;std::u16string server=u"sfs$";};
std::shared_ptr<Projection> unc_projection(Share share,std::shared_ptr<ImagePool> pool);
bool is_image_extension(std::u16string_view name);
std::optional<std::u16string> image_unc_path(const Share& share,const std::filesystem::path& image);
}
