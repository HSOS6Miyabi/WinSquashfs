// SPDX-License-Identifier: GPL-3.0-only
#include "dwarfs_backend.hpp"
#include <dwarfs/detail/file_extent_info.h>
#include <dwarfs/detail/file_segment_impl.h>
#include <dwarfs/detail/file_view_impl.h>
#include <dwarfs/file_view.h>
#include <dwarfs/logger.h>
#include <dwarfs/os_access_generic.h>
#include <dwarfs/reader/filesystem_options.h>
#include <dwarfs/reader/filesystem_v2.h>
#include <array>
#include <climits>
#include <limits>
#include <thread>
namespace archivefs {
namespace {
class Segment final : public dwarfs::detail::file_segment_impl {
public:
    Segment(dwarfs::file_range range,std::vector<std::byte> bytes):range_(range),data_(std::move(bytes)){}
    dwarfs::file_off_t offset() const noexcept override{return range_.offset();}
    dwarfs::file_size_t size() const noexcept override{return range_.size();}
    dwarfs::file_range range() const noexcept override{return range_;}
    bool is_zero() const noexcept override{return false;}
    std::span<const std::byte> raw_bytes()const override{return data_;}
    void advise(dwarfs::io_advice,std::error_code& ec)const override{ec.clear();}
    void lock(std::error_code& ec)const override{ec=std::make_error_code(std::errc::operation_not_supported);}
private:
    dwarfs::file_range range_;std::vector<std::byte> data_;
};
// The upstream reader uses file_view segments and can read on its own worker
// threads. This implementation retains only the RandomAccessSource, so it
// also works with short-lived handles for network and automatically released
// images, without holding an OS mapping on the source.
class View final : public dwarfs::detail::file_view_impl,public std::enable_shared_from_this<View>{
public:
    explicit View(std::shared_ptr<const RandomAccessSource> source):source_(std::move(source)),size_(checked_size(source_->size())),extents_{{{dwarfs::extent_kind::data,{0,size_}}}}{}
    dwarfs::file_size_t size()const override{return size_;}
    const std::filesystem::path& path()const override{return name_;}
    dwarfs::file_segment segment_at(dwarfs::file_range range)const override{
        if(!valid_range(range)||range.empty())return {};
        std::vector<std::byte> bytes(static_cast<std::size_t>(range.size()));
        source_->read_exact(static_cast<std::uint64_t>(range.offset()),bytes);
        return dwarfs::file_segment(std::make_shared<Segment>(range,std::move(bytes)));
    }
    dwarfs::file_extents_iterable extents(std::optional<dwarfs::file_range> range)const override{
        auto r=range.value_or(dwarfs::file_range{0,size_});if(!valid_range(r))throw std::out_of_range("DwarFS source extent");return {shared_from_this(),std::span<const dwarfs::detail::file_extent_info>(extents_.data(),size_?1u:0u),r};
    }
    bool supports_raw_bytes()const noexcept override{return false;}
    std::span<const std::byte> raw_bytes()const override{throw std::logic_error("source does not expose an address mapping");}
    void copy_bytes(void* dest,dwarfs::file_range range,std::error_code& ec)const override{
        ec.clear();if(!valid_range(range)||(!dest&&!range.empty())){ec=std::make_error_code(std::errc::result_out_of_range);return;}
        try{source_->read_exact(static_cast<std::uint64_t>(range.offset()),std::span(static_cast<std::byte*>(dest),static_cast<std::size_t>(range.size())));}catch(const std::system_error& e){ec=e.code();}catch(...){ec=std::make_error_code(std::errc::io_error);}
    }
    std::size_t default_segment_size()const override{return 1u<<20;}
    void release_until(dwarfs::file_off_t,std::error_code& ec)const override{ec.clear();}
private:
    static dwarfs::file_size_t checked_size(std::uint64_t size){if(size>static_cast<std::uint64_t>(std::numeric_limits<dwarfs::file_size_t>::max()))throw std::length_error("DwarFS source too large");return static_cast<dwarfs::file_size_t>(size);}
    bool valid_range(dwarfs::file_range range)const noexcept{return range.offset()>=0&&range.size()>=0&&range.offset()<=size_&&range.size()<=size_-range.offset()&&static_cast<std::uint64_t>(range.size())<=SIZE_MAX;}
    const std::shared_ptr<const RandomAccessSource> source_;const dwarfs::file_size_t size_;const std::filesystem::path name_{"archive.dwarfs"};const std::array<dwarfs::detail::file_extent_info,1> extents_;
};
class Backend final:public ArchiveBackend {
public:
    explicit Backend(std::shared_ptr<const RandomAccessSource> source):source_(std::move(source)),filesystem_(logger_,os_,dwarfs::file_view(std::make_shared<View>(source_)),options()){
        auto root=filesystem_.root().inode();root_=root.inode_num();if(!root.is_directory())throw std::runtime_error("DwarFS root is not a directory");source_->validate();
    }
    ArchiveInfo info()const override{return {ArchiveFormat::dwarfs,root_,source_->size(),0,0};}
    FileInfo stat(InodeId id)const override{return metadata(inode(id));}
    std::vector<DirectoryEntry> list(InodeId id)const override{
        auto dir=filesystem_.opendir(inode(id));if(!dir)throw std::invalid_argument("not a DwarFS directory");std::vector<DirectoryEntry> result;result.reserve(dir->entry_count());
        for(const auto& child:*dir){auto name=child.name();if(name=="."||name=="..")continue;if(name.empty()||name.find('/')!=std::string::npos||name.find('\0')!=std::string::npos)throw std::runtime_error("invalid DwarFS directory name");result.push_back({std::move(name),metadata(child.inode())});}return result;
    }
    void read_exact(InodeId id,std::uint64_t offset,std::span<std::byte> data)const override{
        auto file=inode(id);auto info=metadata(file);if(info.kind!=FileKind::regular||offset>info.size||data.size()>info.size-offset)throw std::out_of_range("invalid DwarFS read");if(data.empty())return;
        std::error_code ec;auto n=filesystem_.read(file.inode_num(),reinterpret_cast<char*>(data.data()),data.size(),static_cast<dwarfs::file_off_t>(offset),ec);if(ec)throw std::system_error(ec);if(n!=data.size())throw std::runtime_error("short DwarFS read");
    }
    void validate_source()const override{source_->validate();}
private:
    static dwarfs::reader::filesystem_options options(){dwarfs::reader::filesystem_options o;o.block_cache.max_bytes=128u<<20;o.block_cache.num_workers=std::clamp(std::thread::hardware_concurrency(),2u,8u);o.metadata.readonly=true;o.metadata.check_consistency=true;o.inode_reader.readahead=1u<<20;return o;}
    dwarfs::reader::inode_view inode(InodeId id)const{if(id>static_cast<std::uint64_t>(INT_MAX))throw std::out_of_range("DwarFS inode");auto result=filesystem_.find(static_cast<int>(id));if(!result)throw std::out_of_range("DwarFS inode not found");return *result;}
    FileInfo metadata(dwarfs::reader::inode_view file)const{auto st=filesystem_.getattr(file);auto kind=st.is_regular_file()?FileKind::regular:st.is_directory()?FileKind::directory:st.is_symlink()?FileKind::symlink:FileKind::special;auto size=st.size();if(size<0)throw std::runtime_error("invalid DwarFS file size");return {file.inode_num(),kind,kind==FileKind::regular?static_cast<std::uint64_t>(size):0,std::clamp<std::int64_t>(st.mtime(),-11644473600LL,253402300799LL)};}
    std::shared_ptr<const RandomAccessSource> source_;dwarfs::null_logger logger_;dwarfs::os_access_generic os_;dwarfs::reader::filesystem_v2_lite filesystem_;InodeId root_=0;
};
}
std::shared_ptr<const ArchiveBackend> open_dwarfs_backend(std::shared_ptr<const RandomAccessSource> source){return std::make_shared<Backend>(std::move(source));}
}
