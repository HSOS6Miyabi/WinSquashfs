// SPDX-License-Identifier: GPL-3.0-only
#include "stream_archive_backend.hpp"
#include "decoder_backend.hpp"
#include "gzip_index.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

namespace archivefs {
namespace {
ArchiveFormat format_of(struct archive* a) {
    switch(archive_format(a)&ARCHIVE_FORMAT_BASE_MASK){
    case ARCHIVE_FORMAT_ZIP:return ArchiveFormat::zip;
    case ARCHIVE_FORMAT_7ZIP:return ArchiveFormat::seven_zip;
    case ARCHIVE_FORMAT_RAR:case ARCHIVE_FORMAT_RAR_V5:return ArchiveFormat::rar;
    case ARCHIVE_FORMAT_TAR:return ArchiveFormat::tar;
    case ARCHIVE_FORMAT_CPIO:return ArchiveFormat::cpio;
    case ARCHIVE_FORMAT_CAB:return ArchiveFormat::cab;
    case ARCHIVE_FORMAT_ISO9660:return ArchiveFormat::iso9660;
    case ARCHIVE_FORMAT_AR:return ArchiveFormat::ar;
    case ARCHIVE_FORMAT_LHA:return ArchiveFormat::lha;
    case ARCHIVE_FORMAT_RAW:return ArchiveFormat::compressed_stream;
    default:return ArchiveFormat::unknown;
    }
}
std::string error_text(struct archive* a) {
    const auto* message=archive_error_string(a);
    return message?message:"archive decoder failed";
}
class Cursor {
public:
    explicit Cursor(std::shared_ptr<const RandomAccessSource> source,ArchiveFormat hint)
        :source_(std::move(source)),archive_(archive_read_new(),archive_read_free) {
        if(!archive_)throw std::bad_alloc();
        if(source_->size()>static_cast<std::uint64_t>(INT64_MAX))throw std::length_error("archive source exceeds signed 64-bit offsets");
        auto* a=archive_.get();
        // All filters must use linked libraries. A missing filter must not
        // silently launch a command from the user's PATH.
        for(auto support:{archive_read_support_filter_none,archive_read_support_filter_gzip,
                          archive_read_support_filter_bzip2,archive_read_support_filter_compress,
                          archive_read_support_filter_xz,archive_read_support_filter_lzma,
                          archive_read_support_filter_lzip,archive_read_support_filter_lz4,
                          archive_read_support_filter_zstd})
            if(support(a)!=ARCHIVE_OK)throw std::runtime_error("required in-process archive filter is unavailable: "+error_text(a));
        // A recognized signature selects its own parser, including for archives
        // containing other archive signatures in their member data.
        switch(hint){
        case ArchiveFormat::tar:check(archive_read_support_format_tar(a));break;
        case ArchiveFormat::zip:check(archive_read_support_format_zip(a));break;
        case ArchiveFormat::seven_zip:check(archive_read_support_format_7zip(a));break;
        case ArchiveFormat::rar:check(archive_read_support_format_rar(a));check(archive_read_support_format_rar5(a));break;
        case ArchiveFormat::cpio:check(archive_read_support_format_cpio(a));break;
        case ArchiveFormat::cab:check(archive_read_support_format_cab(a));break;
        case ArchiveFormat::iso9660:check(archive_read_support_format_iso9660(a));break;
        case ArchiveFormat::ar:check(archive_read_support_format_ar(a));break;
        case ArchiveFormat::lha:check(archive_read_support_format_lha(a));break;
        case ArchiveFormat::compressed_stream:check(archive_read_support_format_raw(a));check(archive_read_support_format_empty(a));break;
        default:
        for(auto support:{archive_read_support_format_tar,archive_read_support_format_zip,
                          archive_read_support_format_7zip,archive_read_support_format_rar,
                          archive_read_support_format_rar5,archive_read_support_format_cpio,
                          archive_read_support_format_cab,archive_read_support_format_iso9660,
                          archive_read_support_format_ar,archive_read_support_format_lha,
                          archive_read_support_format_raw,archive_read_support_format_empty})
            if(support(a)!=ARCHIVE_OK)throw std::runtime_error(error_text(a));
        break;
        }
        check(archive_read_set_callback_data(a,this));
        check(archive_read_set_read_callback(a,&read_source));
        check(archive_read_set_skip_callback(a,&skip_source));
        check(archive_read_set_seek_callback(a,&seek_source));
        begin();
        const auto rc=archive_read_open1(a);
        if(failure_)std::rethrow_exception(failure_);
        if(rc<ARCHIVE_OK){
            if(hint==ArchiveFormat::unknown&&format_of(a)==ArchiveFormat::unknown)throw UnknownArchiveFormat();
            throw std::runtime_error(error_text(a));
        }
        finish();
    }
    ~Cursor(){archive_.reset();}
    void begin(){failure_=nullptr;session_=source_->begin_read();}
    void finish(){
        if(failure_)std::rethrow_exception(failure_);
        session_->finish();session_.reset();
    }
    struct archive* handle()const noexcept{return archive_.get();}
    struct archive_entry* next(ArchiveFormat hint) {
        struct archive_entry* e=nullptr;
        const auto rc=archive_read_next_header(handle(),&e);
        if(failure_)std::rethrow_exception(failure_);
        if(rc==ARCHIVE_EOF)return nullptr;
        if(rc<ARCHIVE_WARN||!e){
            if(ordinal==0&&hint==ArchiveFormat::unknown&&format_of(handle())==ArchiveFormat::unknown)
                throw UnknownArchiveFormat();
            throw std::runtime_error(error_text(handle()));
        }
        ++ordinal;offset=0;block_=nullptr;block_start_=block_end_=0;eof_=false;
        return e;
    }
    void skip_entry(){block_=nullptr;check(archive_read_data_skip(handle()));}
    // Use data blocks rather than materializing holes in a sparse file.
    // The returned buffer remains valid until the next libarchive call.
    std::uint64_t scan_size(){
        std::uint64_t size=0;
        while(next_block()){
            if(block_start_!=size)throw std::runtime_error("non-contiguous compressed stream");
            size=block_end_;
        }
        return size;
    }
    void read_at(std::uint64_t position,std::span<std::byte> output,std::uint64_t size,bool holes){
        if(position<offset||position>size||output.size()>size-position)
            throw std::out_of_range("invalid decoder cursor position");
        offset=position;
        while(!output.empty()){
            while(!eof_&&(!block_||offset>=block_end_))next_block();
            if(block_&&block_end_>size)throw std::runtime_error("archive entry exceeds declared size");
            if(eof_||offset<block_start_){
                if(!holes)throw std::runtime_error("short or discontinuous archive entry");
                auto n=static_cast<std::size_t>(std::min<std::uint64_t>(output.size(),eof_?size-offset:block_start_-offset));
                std::fill_n(output.data(),n,std::byte{0});offset+=n;output=output.subspan(n);
            }else{
                auto n=static_cast<std::size_t>(std::min<std::uint64_t>(output.size(),block_end_-offset));
                std::copy_n(block_+static_cast<std::size_t>(offset-block_start_),n,output.data());
                offset+=n;output=output.subspan(n);
            }
        }
        // A final decoder call validates checksums for formats such as ZIP.
        if(offset==size&&!eof_&&next_block())throw std::runtime_error("archive entry exceeds declared size");
    }
    std::uint64_t ordinal=0,offset=0;
private:
    bool next_block(){
        if(eof_)return false;
        for(unsigned attempt=0;attempt<1024;++attempt){
            const void* data=nullptr;size_t count=0;la_int64_t position=0;
            auto rc=archive_read_data_block(handle(),&data,&count,&position);
            if(failure_)std::rethrow_exception(failure_);
            if(rc==ARCHIVE_EOF){eof_=true;block_=nullptr;return false;}
            if(rc!=ARCHIVE_OK)throw std::runtime_error(error_text(handle()));
            if(position<0||static_cast<std::uint64_t>(position)<block_end_||
               count>static_cast<std::uint64_t>(INT64_MAX)-static_cast<std::uint64_t>(position))
                throw std::runtime_error("invalid archive data block range");
            if(!count)continue;
            if(!data)throw std::runtime_error("missing archive data block");
            block_=static_cast<const std::byte*>(data);block_start_=static_cast<std::uint64_t>(position);
            block_end_=block_start_+count;return true;
        }
        throw std::runtime_error("archive decoder did not advance");
    }
    void check(int rc){
        if(failure_)std::rethrow_exception(failure_);
        if(rc<ARCHIVE_OK)throw std::runtime_error(error_text(handle()));
    }
    static la_ssize_t read_source(struct archive* a,void* opaque,const void** buffer)noexcept{
        auto& self=*static_cast<Cursor*>(opaque);
        try{
            *buffer=self.buffer_.data();
            auto n=static_cast<std::size_t>(std::min<std::uint64_t>(self.buffer_.size(),self.source_->size()-self.position_));
            if(n){
                if(!self.session_)throw std::logic_error("archive callback without read session");
                self.session_->read_exact(self.position_,std::span(self.buffer_).first(n));
                self.position_+=n;
            }
            return static_cast<la_ssize_t>(n);
        }catch(...){self.failure_=std::current_exception();archive_set_error(a,EIO,"archive source read failed");return -1;}
    }
    static la_int64_t skip_source(struct archive*,void* opaque,la_int64_t request)noexcept{
        auto& self=*static_cast<Cursor*>(opaque);
        if(request<=0)return 0;
        auto n=std::min<std::uint64_t>(static_cast<std::uint64_t>(request),self.source_->size()-self.position_);
        self.position_+=n;return static_cast<la_int64_t>(n);
    }
    static la_int64_t seek_source(struct archive* a,void* opaque,la_int64_t offset,int whence)noexcept{
        auto& self=*static_cast<Cursor*>(opaque);
        std::uint64_t base=0;
        if(whence==SEEK_CUR)base=self.position_;
        else if(whence==SEEK_END)base=self.source_->size();
        else if(whence!=SEEK_SET){archive_set_error(a,EINVAL,"invalid archive seek origin");return ARCHIVE_FATAL;}
        if(offset<0){
            auto magnitude=static_cast<std::uint64_t>(-(offset+1))+1;
            if(magnitude>base){archive_set_error(a,EINVAL,"archive seek before beginning");return ARCHIVE_FATAL;}
            base-=magnitude;
        }else{
            if(static_cast<std::uint64_t>(offset)>self.source_->size()-base){archive_set_error(a,EINVAL,"archive seek beyond end");return ARCHIVE_FATAL;}
            base+=static_cast<std::uint64_t>(offset);
        }
        self.position_=base;return static_cast<la_int64_t>(base);
    }
    std::shared_ptr<const RandomAccessSource> source_;
    std::unique_ptr<struct archive,decltype(&archive_read_free)> archive_;
    std::unique_ptr<ReadSession> session_;
    std::exception_ptr failure_;
    std::array<std::byte,65536> buffer_{};
    std::uint64_t position_=0,block_start_=0,block_end_=0;
    const std::byte* block_=nullptr;
    bool eof_=false;
};

std::string entry_name(struct archive_entry* e) {
    auto p=archive_entry_pathname_utf8(e);
    if(!p)p=archive_entry_pathname(e);
    if(!p)throw std::runtime_error("archive entry has no pathname");
    return p;
}
std::vector<std::string_view> path_parts(std::string_view path) {
    if(path.size()>32768)throw std::length_error("archive pathname too long");
    if(path.starts_with('/')||path.starts_with('\\')||
       (path.size()>=2&&path[1]==':'&&((path[0]>='A'&&path[0]<='Z')||(path[0]>='a'&&path[0]<='z'))))
        throw std::runtime_error("absolute archive pathname");
    std::vector<std::string_view> parts;
    while(!path.empty()){
        auto end=path.find('/');
        auto part=path.substr(0,end);
        if(part=="..")throw std::runtime_error("archive pathname escapes its root");
        if(!part.empty()&&part!=".")parts.push_back(part);
        if(parts.size()>256)throw std::length_error("archive pathname too deep");
        if(end==std::string_view::npos)break;
        path.remove_prefix(end+1);
    }
    return parts;
}
std::string raw_filename(const RandomAccessSource& source){
    auto name=source.filename(),lower=name;
    for(auto& c:lower)if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');
    constexpr std::string_view container_suffixes[]={".tgz",".tbz",".tbz2",".txz",".tzst",".tlz",".tar.gz",".tar.bz2",".tar.xz",".tar.zst",".tar.lz4",".tar.lz",".tar.lzma",".tar.z",".cpio.gz",".cpio.xz",".cpio.zst"};
    for(auto suffix:container_suffixes)if(lower.ends_with(suffix))
        throw std::runtime_error("compressed archive does not contain a supported archive container");
    constexpr std::string_view suffixes[]={".gz",".bz2",".xz",".zst",".lz4",".lz",".lzma",".z"};
    for(auto suffix:suffixes)if(lower.ends_with(suffix)){name.resize(name.size()-suffix.size());break;}
    if(name.empty()||name=="."||name==".."||name.find_first_of("/\\")!=std::string::npos)return "data";
    return name;
}
struct Extent {std::uint64_t logical,physical,length;};
struct Node {
    FileInfo info{};
    std::map<std::string,InodeId,std::less<>> children;
    std::uint64_t ordinal=0;
    std::optional<std::uint64_t> direct;
    std::shared_ptr<const std::vector<Extent>> sparse;
    std::string link;
    bool hard=false,encrypted=false,holes=false;
};
class StreamBackend final:public ArchiveBackend {
public:
    StreamBackend(std::shared_ptr<const RandomAccessSource> source,ArchiveFormat hint,
                  std::optional<std::uint64_t> gzip_size={},std::size_t reserved=0)
        :source_(std::move(source)),hint_(hint),reserved_(reserved){
        nodes_.emplace_back();nodes_.back().info={1,FileKind::directory,0,0};
        Cursor cursor(source_,hint_);
        cursor.begin();
        while(auto* entry=cursor.next(hint_)){
            auto* a=cursor.handle();
            auto f=format_of(a);
            if(f==ArchiveFormat::unknown)throw UnknownArchiveFormat();
            if(hint_!=ArchiveFormat::unknown&&f!=hint_)throw std::runtime_error("archive signature and decoded format differ");
            if(info_.format==ArchiveFormat::unknown)
                info_={f,1,source_->size(),256u<<10,static_cast<std::uint32_t>(archive_filter_code(a,0))};
            if(f!=info_.format)throw std::runtime_error("archive format changed during scan");
            if(f==ArchiveFormat::compressed_stream&&(hint_!=ArchiveFormat::unknown||info_.compression==ARCHIVE_FILTER_NONE))
                throw UnknownArchiveFormat();
            auto name=f==ArchiveFormat::compressed_stream?raw_filename(*source_):entry_name(entry);
            if(f==ArchiveFormat::ar&&(name=="/"||name=="//"||name=="/SYM64/")){cursor.skip_entry();continue;}
            auto parts=path_parts(name);
            auto type=archive_entry_filetype(entry);
            if(parts.empty()){
                if(type!=AE_IFDIR)throw std::runtime_error("invalid archive root entry");
                cursor.skip_entry();continue;
            }
            auto hard=archive_entry_hardlink_utf8(entry);
            if(!hard)hard=archive_entry_hardlink(entry);
            auto kind=hard?FileKind::regular:type==AE_IFDIR?FileKind::directory:
                      type==AE_IFREG?FileKind::regular:type==AE_IFLNK?FileKind::symlink:FileKind::special;
            InodeId id=1;
            for(std::size_t i=0;i<parts.size();++i){
                const bool leaf=i+1==parts.size();
                auto& parent=at(id);
                if(parent.info.kind!=FileKind::directory)throw std::runtime_error("file used as archive directory");
                auto existing=parent.children.find(parts[i]);
                if(existing!=parent.children.end()){
                    id=existing->second;
                    if(leaf&&(at(id).info.kind==FileKind::directory)!=(kind==FileKind::directory))
                        throw std::runtime_error("conflicting archive path types");
                }else{
                    if(nodes_.size()>=500000||parts[i].size()>64u*1024u*1024u-name_bytes_)
                        throw std::length_error("archive metadata limit exceeded");
                    name_bytes_+=parts[i].size();
                    auto child=static_cast<InodeId>(nodes_.size()+1);
                    parent.children.emplace(std::string(parts[i]),child);
                    nodes_.emplace_back();id=child;
                    at(id).info={id,leaf?kind:FileKind::directory,0,0};
                }
            }
            auto& node=at(id);
            auto size=archive_entry_size(entry);
            if(f==ArchiveFormat::compressed_stream)size=static_cast<la_int64_t>(cursor.scan_size());
            if(size<0)throw std::runtime_error("negative archive entry size");
            node.info={id,kind,kind==FileKind::regular?static_cast<std::uint64_t>(size):0,
                       std::clamp<std::int64_t>(archive_entry_mtime(entry),-11644473600LL,253402300799LL)};
            node.ordinal=cursor.ordinal;node.direct.reset();node.sparse.reset();node.link.clear();node.hard=hard!=nullptr;
            node.holes=archive_entry_sparse_count(entry)>0;
            node.encrypted=archive_entry_is_encrypted(entry)>0;
            if(hard)node.link=hard;
            else if(kind==FileKind::symlink){
                auto target=archive_entry_symlink_utf8(entry);if(!target)target=archive_entry_symlink(entry);
                if(target)node.link=target;
                node.info.size=node.link.size();
            }
            if(node.link.size()>65536||node.link.size()>64u*1024u*1024u-name_bytes_)
                throw std::length_error("archive link metadata limit exceeded");
            name_bytes_+=node.link.size();
            // For an unfiltered TAR, the consumed offset after PAX/GNU
            // extension records points at the packed file payload. Sparse
            // maps translate logical ranges to the packed data on disk.
            if(f==ArchiveFormat::tar&&info_.compression==ARCHIVE_FILTER_NONE&&kind==FileKind::regular&&
               !node.hard&&!node.encrypted)index_tar(cursor,entry,node);
            if(f!=ArchiveFormat::compressed_stream)cursor.skip_entry();
        }
        if(info_.format==ArchiveFormat::unknown){
            auto f=format_of(cursor.handle());
            auto filter=archive_filter_code(cursor.handle(),0);
            if(archive_format(cursor.handle())==ARCHIVE_FORMAT_EMPTY&&filter!=ARCHIVE_FILTER_NONE&&hint_==ArchiveFormat::unknown){
                auto name=raw_filename(*source_);
                at(1).children.emplace(std::move(name),2);
                nodes_.emplace_back();at(2).info={2,FileKind::regular,0,0};
                f=ArchiveFormat::compressed_stream;
            }
            if(f==ArchiveFormat::unknown)throw UnknownArchiveFormat();
            info_={f,1,source_->size(),256u<<10,static_cast<std::uint32_t>(filter)};
        }
        cursor.finish();
        // In newc CPIO the last member of a hard-link group may carry
        // the data while the first member only declares the inode.
        if(info_.format==ArchiveFormat::cpio){
            for(const auto& node:nodes_)if(node.hard&&node.info.size){
                auto target=hard_target(node);
                if(target){
                    auto& first=at(*target);
                    if(first.info.size&&first.info.size!=node.info.size)throw std::runtime_error("inconsistent CPIO hard-link sizes");
                    if(!first.info.size)copy_payload(first,node);
                }
            }
        }
        for(auto& node:nodes_)if(node.hard){
            auto target=hard_target(node);
            if(!target){node.info.kind=FileKind::special;node.info.size=0;continue;}
            copy_payload(node,at(*target));
        }
        if(gzip_size){info_.image_size=*gzip_size;info_.compression=ARCHIVE_FILTER_GZIP;}
        source_->validate();
    }
    ArchiveInfo info()const override{return info_;}
    std::size_t cache_reservation_bytes()const noexcept override{return reserved_;}
    FileInfo stat(InodeId id)const override{return at(id).info;}
    std::vector<DirectoryEntry> list(InodeId id)const override{
        const auto& node=at(id);
        if(node.info.kind!=FileKind::directory)throw std::invalid_argument("not an archive directory");
        std::vector<DirectoryEntry> result;result.reserve(node.children.size());
        for(const auto& [name,child]:node.children)result.push_back({name,at(child).info});
        return result;
    }
    std::string read_link(InodeId id)const override{
        const auto& node=at(id);
        if(node.info.kind!=FileKind::symlink)throw std::invalid_argument("not a symbolic link");
        return node.link;
    }
    void read_exact(InodeId id,std::uint64_t offset,std::span<std::byte> output)const override{
        const auto& node=at(id);
        if(node.info.kind!=FileKind::regular||offset>node.info.size||output.size()>node.info.size-offset)
            throw std::out_of_range("archive entry read outside file");
        if(node.encrypted)throw std::runtime_error("encrypted archive entry requires a password");
        if(output.empty())return;
        if(node.direct){
            if(node.sparse)read_sparse(node,offset,output);
            else source_->read_exact(*node.direct+offset,output);
            return;
        }
        // Keep a bounded set of decoder cursors. Forward sequential requests
        // reuse the existing decoder; backward requests reopen the source.
        // The common ArchiveReader caches decoded pages across cursors.
        std::size_t index=0;
        std::unique_ptr<Cursor> cursor;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock,[&]{return !slots_[0].busy||!slots_[1].busy;});
            bool found=false;std::uint64_t best_ordinal=0,best_offset=0;
            for(std::size_t i=0;i<slots_.size();++i){
                auto& slot=slots_[i];if(slot.busy)continue;
                if(!found){index=i;found=true;}
                if(slot.cursor&&slot.cursor->ordinal<=node.ordinal&&
                   (slot.cursor->ordinal!=node.ordinal||slot.cursor->offset<=offset)){
                    auto o=slot.cursor->ordinal,p=slot.cursor->offset;
                    if(o>best_ordinal||(o==best_ordinal&&p>=best_offset)){index=i;best_ordinal=o;best_offset=p;}
                }
            }
            slots_[index].busy=true;cursor=std::move(slots_[index].cursor);
        }
        auto release=[&]{
            {std::lock_guard lock(mutex_);slots_[index].cursor=std::move(cursor);slots_[index].busy=false;}
            ready_.notify_one();
        };
        try{
            if(!cursor||cursor->ordinal>node.ordinal||(cursor->ordinal==node.ordinal&&cursor->offset>offset))
                cursor=std::make_unique<Cursor>(source_,info_.format);
            cursor->begin();
            while(cursor->ordinal<node.ordinal){
                if(cursor->ordinal)cursor->skip_entry();
                if(!cursor->next(info_.format))throw std::runtime_error("archive entry disappeared");
            }
            cursor->read_at(offset,output,node.info.size,node.holes);
            cursor->finish();release();
        }catch(...){cursor.reset();release();throw;}
    }
    void validate_source()const override{source_->validate();}
    void validate_path()const override{source_->validate_path();}
    bool source_changed()const noexcept override{return source_->source_changed();}
private:
    static void copy_payload(Node& destination,const Node& data){
        destination.info.size=data.info.size;destination.ordinal=data.ordinal;
        destination.direct=data.direct;destination.sparse=data.sparse;destination.holes=data.holes;
        destination.encrypted=data.encrypted;
    }
    void index_tar(Cursor& cursor,struct archive_entry* entry,Node& node){
        auto position=archive_filter_bytes(cursor.handle(),0);
        if(position<0||static_cast<std::uint64_t>(position)>source_->size())throw std::runtime_error("invalid TAR payload offset");
        auto physical=static_cast<std::uint64_t>(position);
        node.direct=physical;
        if(!node.holes){
            if(node.info.size>source_->size()-physical)throw std::runtime_error("TAR entry exceeds source size");
            return;
        }
        auto count=archive_entry_sparse_reset(entry);
        if(count<0||static_cast<std::size_t>(count)>1000000-extent_count_)throw std::length_error("TAR sparse metadata limit exceeded");
        auto extents=std::make_shared<std::vector<Extent>>();extents->reserve(static_cast<std::size_t>(count));
        std::uint64_t previous=0;
        for(int i=0;i<count;++i){
            la_int64_t offset=0,length=0;
            if(archive_entry_sparse_next(entry,&offset,&length)!=ARCHIVE_OK||offset<0||length<0)
                throw std::runtime_error("invalid TAR sparse map");
            auto logical=static_cast<std::uint64_t>(offset),size=static_cast<std::uint64_t>(length);
            if(logical<previous||logical>node.info.size||size>node.info.size-logical||size>source_->size()-physical)
                throw std::runtime_error("TAR sparse range exceeds file size");
            if(size)extents->push_back({logical,physical,size});
            previous=logical+size;physical+=size;
        }
        extent_count_+=extents->size();node.sparse=std::move(extents);
    }
    void read_sparse(const Node& node,std::uint64_t offset,std::span<std::byte> output)const{
        std::fill(output.begin(),output.end(),std::byte{0});
        const auto end=offset+output.size();
        const auto& extents=*node.sparse;
        auto it=std::lower_bound(extents.begin(),extents.end(),offset,[](const Extent& e,std::uint64_t p){return e.logical+e.length<=p;});
        if(it==extents.end()||it->logical>=end)return;
        auto session=source_->begin_read();
        for(;it!=extents.end()&&it->logical<end;++it){
            auto first=std::max(offset,it->logical),last=std::min(end,it->logical+it->length);
            session->read_exact(it->physical+first-it->logical,output.subspan(static_cast<std::size_t>(first-offset),static_cast<std::size_t>(last-first)));
        }
        session->finish();
    }
    Node& at(InodeId id){return const_cast<Node&>(std::as_const(*this).at(id));}
    const Node& at(InodeId id)const{
        if(!id||id>nodes_.size())throw std::out_of_range("unknown archive inode");
        return nodes_[static_cast<std::size_t>(id-1)];
    }
    std::optional<InodeId> hard_target(const Node& first)const{
        const Node* node=&first;
        for(unsigned depth=0;depth<256;++depth){
            if(!node->hard)return node->info.kind==FileKind::regular?std::optional<InodeId>(node->info.inode):std::nullopt;
            InodeId id=1;
            try{
                for(auto part:path_parts(node->link)){
                    const auto& parent=at(id);
                    auto found=parent.children.find(part);
                    if(found==parent.children.end())return {};
                    id=found->second;
                }
            }catch(const std::runtime_error&){return {};}
            node=&at(id);
        }
        return {};
    }
    struct Slot {bool busy=false;std::unique_ptr<Cursor> cursor;};
    std::shared_ptr<const RandomAccessSource> source_;
    ArchiveFormat hint_;
    ArchiveInfo info_{ArchiveFormat::unknown,1,0,0};
    std::vector<Node> nodes_;
    std::size_t name_bytes_=0,extent_count_=0;
    const std::size_t reserved_;
    mutable std::mutex mutex_;
    mutable std::condition_variable ready_;
    mutable std::array<Slot,2> slots_;
};
}
std::shared_ptr<const ArchiveBackend> open_stream_archive_backend(
    std::shared_ptr<const RandomAccessSource> source,ArchiveFormat hint,std::size_t cache_bytes){
    if(hint==ArchiveFormat::unknown){
        auto indexed=index_gzip_tar(source,cache_bytes/2);
        if(indexed.source)return std::make_shared<StreamBackend>(std::move(indexed.source),
            ArchiveFormat::tar,source->size(),indexed.reserved);
    }
    return std::make_shared<StreamBackend>(std::move(source),hint);
}
}
