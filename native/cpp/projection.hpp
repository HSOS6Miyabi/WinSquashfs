// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "archive_tree.hpp"
#include "device_provider.hpp"
#include "open_progress.hpp"
#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <thread>
#include <vector>
namespace archivefs {
using ArchiveSourceOpener=std::function<std::shared_ptr<ArchiveTree>(std::shared_ptr<const RandomAccessSource>)>;
ArchiveSourceOpener make_archive_source_opener(std::filesystem::path decoders,std::size_t cache_bytes=64u<<20,
                                  std::size_t total_cache_bytes=std::numeric_limits<std::size_t>::max());
using ArchiveOpener=std::function<std::shared_ptr<ArchiveTree>(const std::filesystem::path&,SourcePolicy)>;
ArchiveOpener make_archive_opener(std::filesystem::path decoders,std::size_t cache_bytes=64u<<20,
                                  std::size_t total_cache_bytes=std::numeric_limits<std::size_t>::max());
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
    ImagePool(ArchiveOpener opener,Settings settings,ArchiveSourceOpener source_opener={},OpenProgressSink progress={});
    ~ImagePool();
    ImagePool(const ImagePool&)=delete;
    ImagePool& operator=(const ImagePool&)=delete;
    std::shared_ptr<Lease> acquire(const std::filesystem::path& path);
    std::shared_ptr<Lease> acquire_source(std::u16string key,std::shared_ptr<const RandomAccessSource> source,std::u16string display={});
    std::size_t size()const;
    void sweep();
private:
    std::shared_ptr<Lease> acquire_key(std::u16string key,std::u16string display,const std::function<std::shared_ptr<ArchiveTree>(std::shared_ptr<OpenProgress>)>& open);
    std::shared_ptr<State> state_;
    std::jthread worker_;
};
struct ProjectionInfo {bool directory;std::uint64_t size;std::int64_t modified;std::uint64_t index;bool unavailable=false;};
class Projection;
struct RemoteFileState;
struct ProjectedNode {
    std::u16string name,path;
    std::filesystem::path physical;
    ProjectionInfo info{};
    std::shared_ptr<ArchiveTree> archive;
    ArchiveTree::NodePtr entry;
    std::shared_ptr<ImagePool::Lease> lease;
    bool synthetic=false;
    std::shared_ptr<const RandomAccessSource> source;
    std::string device_id,object_id;
    std::shared_ptr<RemoteFileState> remote;
    std::shared_ptr<Projection> route;
    std::shared_ptr<const ProjectedNode> routed;
};
using ProjectedNodePtr=std::shared_ptr<const ProjectedNode>;
class Projection {
public:
    virtual ~Projection()=default;
    virtual ProjectedNodePtr open(std::u16string_view path)const=0;
    virtual std::vector<ProjectedNodePtr> list(const ProjectedNodePtr& directory)const=0;
    virtual std::uint64_t capacity()const=0;
    virtual void validate(const ProjectedNodePtr& node)const;
    virtual std::size_t read(const ProjectedNodePtr& node,std::uint64_t offset,std::span<std::byte> output)const;
    virtual ProjectionInfo query_info(const ProjectedNodePtr& node)const{validate(node);return node->info;}
    virtual bool writable_volume()const noexcept{return false;}
    virtual bool writable(const ProjectedNodePtr&)const noexcept{return false;}
    virtual ProjectedNodePtr create_file(std::u16string_view,bool){throw std::runtime_error("read-only projection");}
    virtual std::size_t write(const ProjectedNodePtr&,std::uint64_t,std::span<const std::byte>,bool,bool){throw std::runtime_error("read-only projection");}
    virtual void resize(const ProjectedNodePtr&,std::uint64_t){throw std::runtime_error("read-only projection");}
    virtual void flush(const ProjectedNodePtr&){}
    virtual void check_delete(const ProjectedNodePtr&){throw std::runtime_error("read-only projection");}
    virtual void remove(const ProjectedNodePtr&){throw std::runtime_error("read-only projection");}
    virtual void rename(const ProjectedNodePtr&,std::u16string_view,bool){throw std::runtime_error("read-only projection");}
    virtual void operation_error(std::string_view)noexcept{}
};
class NamespaceProjection : public Projection {
public:
    virtual void replace_routes(std::vector<std::pair<std::u16string,std::shared_ptr<Projection>>> routes)=0;
};
std::shared_ptr<NamespaceProjection> namespace_projection();
std::shared_ptr<Projection> mount_projection(std::shared_ptr<ArchiveTree> tree);
struct Share {std::u16string name;std::filesystem::path root;bool network=false;std::u16string server=u"sfs$";};
std::shared_ptr<Projection> unc_projection(Share share,std::shared_ptr<ImagePool> pool);
// Resolve nested archive paths directly through parent readers, without reentering WinFsp.
ProjectedNodePtr project_archive(const std::shared_ptr<ImagePool>& pool,std::shared_ptr<ImagePool::Lease> lease,
    std::filesystem::path physical,std::u16string_view prefix,std::u16string_view relative);
std::vector<ProjectedNodePtr> list_projected_archive(const ProjectedNodePtr& directory);
struct DeviceListing {std::u16string component;RemoteDevice device;};
std::vector<DeviceListing> list_connected_devices(const std::shared_ptr<DeviceProvider>& provider);
std::shared_ptr<Projection> device_projection(std::shared_ptr<DeviceProvider> provider,
    std::shared_ptr<ImagePool> pool,std::u16string namespace_id,
    std::function<void(const DeviceFailure&)> unavailable={});
bool is_image_extension(std::u16string_view name);
std::optional<std::u16string> image_unc_path(const Share& share,const std::filesystem::path& image);
}
