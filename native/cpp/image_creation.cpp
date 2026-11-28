// SPDX-License-Identifier: GPL-3.0-only
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif
#include "image_creation.hpp"
#include "archive_tree.hpp"
#include "decoder_backend.hpp"
#include "file_time.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <bit>
#include <chrono>
#include <fstream>
#include <map>
#include <random>
#include <set>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
namespace archivefs {
namespace fs=std::filesystem;
namespace {
using Args=std::vector<fs::path::string_type>;
void add(Args& args,std::string_view value){args.push_back(fs::path(to_utf16(value)).native());}
void add(Args& args,std::string_view option,std::string_view value){add(args,option);add(args,value);}
void add_num(Args& args,std::string_view option,std::uint64_t value){add(args,option,std::to_string(value));}
void flag(Args& args,bool value,std::string_view option){if(value)add(args,option);}
void cancelled(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("operation cancelled");}
void require(bool condition,const char* message){if(!condition)throw std::invalid_argument(message);}
bool equal_path_component(const fs::path& a,const fs::path& b){WindowsNameLess less;return !less(a.u16string(),b.u16string())&&!less(b.u16string(),a.u16string());}
bool beneath(const fs::path& parent,const fs::path& child){auto a=parent.begin(),b=child.begin();for(;a!=parent.end();++a,++b)if(b==child.end()||!equal_path_component(*a,*b))return false;return true;}
fs::path base_name(const fs::path& path){auto p=path;while(p.has_relative_path()&&p.filename().empty())p=p.parent_path();if(!p.filename().empty())return p.filename();auto name=p.root_name().u16string();if(name.size()==2&&name[1]==u':')name.pop_back();if(name.empty())throw std::invalid_argument("source root has no archive name");return fs::path(name);}
std::vector<fs::path> normalize(const std::vector<fs::path>& sources){
    std::vector<fs::path> items;std::set<std::u16string,WindowsNameLess> paths,names;
    for(const auto& p:sources){auto absolute=fs::absolute(p).lexically_normal();while(absolute.has_relative_path()&&absolute.filename().empty())absolute=absolute.parent_path();auto status=fs::symlink_status(absolute);require(fs::exists(status),"source does not exist");if(paths.insert(absolute.u16string()).second)items.push_back(absolute);}
    require(!items.empty(),"add at least one source");
    if(items.size()>1)for(auto& item:items)require(names.insert(base_name(item).u16string()).second,"duplicate source names at image root");
    return items;
}
std::vector<fs::path> sorted_children(const fs::path& path){std::vector<fs::path> items;for(const auto& entry:fs::directory_iterator(path))items.push_back(entry.path());std::sort(items.begin(),items.end());return items;}
class Workspace {
public:
    explicit Workspace(const fs::path& parent){std::random_device random;for(int i=0;i<32;++i){auto candidate=parent/(".archivefs-"+std::to_string(random())+"-"+std::to_string(random())+".partial");if(fs::create_directory(candidate)){path=std::move(candidate);return;}}throw std::runtime_error("cannot create creation workspace");}
    ~Workspace(){std::error_code error;fs::remove_all(path,error);}
    fs::path path;
};
void publish(const fs::path& source,const fs::path& destination){
#ifdef _WIN32
    if(!MoveFileExW(source.c_str(),destination.c_str(),0))throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"publish archive");
#else
    if(::syscall(SYS_renameat2,AT_FDCWD,source.c_str(),AT_FDCWD,destination.c_str(),RENAME_NOREPLACE)!=0)throw std::system_error(errno,std::generic_category(),"publish archive");
#endif
}
void stage(const fs::path& from,const fs::path& to,std::stop_token stop){
    std::vector<std::pair<fs::path,fs::path>> pending{{from,to}},dirs;
    std::vector<char> buffer(1u<<20);
    while(!pending.empty()){
        cancelled(stop);auto [src,dst]=std::move(pending.back());pending.pop_back();auto status=fs::symlink_status(src);
        if(fs::is_symlink(status)){auto link=fs::read_symlink(src);if(fs::is_directory(src))fs::create_directory_symlink(link,dst);else fs::create_symlink(link,dst);}
        else if(fs::is_directory(status)){fs::create_directory(dst);dirs.emplace_back(src,dst);for(auto& child:sorted_children(src))pending.emplace_back(child,dst/child.filename());}
        else if(fs::is_regular_file(status)){
            std::error_code error;fs::create_hard_link(src,dst,error);if(error){std::ifstream input(src,std::ios::binary);std::ofstream output(dst,std::ios::binary);if(!input||!output)throw std::runtime_error("cannot open staging file");while(input){cancelled(stop);input.read(buffer.data(),static_cast<std::streamsize>(buffer.size()));output.write(buffer.data(),input.gcount());if(!output)throw std::runtime_error("cannot write staging file");}if(!input.eof())throw std::runtime_error("cannot read staging file");output.close();if(!output)throw std::runtime_error("cannot close staging file");fs::last_write_time(dst,fs::last_write_time(src));}
        }else throw std::runtime_error("unsupported source file type");
    }
    for(auto it=dirs.rbegin();it!=dirs.rend();++it)fs::last_write_time(it->second,fs::last_write_time(it->first));
}
std::uint32_t useful_block(const fs::path& root,std::stop_token stop){std::uint64_t total=0;constexpr std::uint64_t maximum=1ULL<<30;for(const auto& e:fs::recursive_directory_iterator(root)){cancelled(stop);if(fs::is_regular_file(e.symlink_status())){total+=std::min(maximum,e.file_size());if(total>=maximum)return static_cast<std::uint32_t>(maximum);}}return static_cast<std::uint32_t>(std::bit_ceil(std::max<std::uint64_t>(total,1ULL<<20)));}
fs::path executable(const fs::path& tools,std::string_view name){
#ifdef _WIN32
    return tools/(std::string(name)+".exe");
#else
    return tools/name;
#endif
}
std::uint64_t validate_image(const fs::path& image,const fs::path& decoders,ArchiveFormat format){auto source=open_file_source(image);auto backend=open_decoder_backend(source,decoders,0);if(backend->info().format!=format)throw std::runtime_error("unexpected output archive format");return backend->info().image_size;}
struct TarWriter {
    struct archive* value=archive_write_new();
    std::exception_ptr error;
    const std::function<void(const void*,std::size_t)>& sink;
    explicit TarWriter(const std::function<void(const void*,std::size_t)>& callback):sink(callback){if(!value)throw std::bad_alloc();}
    ~TarWriter(){archive_write_free(value);}
    void check(int result){if(error)std::rethrow_exception(error);if(result<ARCHIVE_OK)throw std::runtime_error(archive_error_string(value)?archive_error_string(value):"TAR operation failed");}
    static la_ssize_t output(struct archive*,void* data,const void* buffer,std::size_t size) noexcept {auto& self=*static_cast<TarWriter*>(data);try{self.sink(buffer,size);return static_cast<la_ssize_t>(size);}catch(...){self.error=std::current_exception();return -1;}}
};
struct TarEntry {struct archive_entry* value=archive_entry_new();TarEntry(){if(!value)throw std::bad_alloc();}~TarEntry(){archive_entry_free(value);}};
void set_tar_time(struct archive_entry* entry,const fs::path& path){const auto time=std::chrono::time_point_cast<std::chrono::nanoseconds>(file_time_to_system(fs::last_write_time(path)));const auto seconds=std::chrono::floor<std::chrono::seconds>(time);archive_entry_set_mtime(entry,static_cast<time_t>(seconds.time_since_epoch().count()),static_cast<long>((time-seconds).count()));}
}
CreationOptions CreationOptions::for_preset(ArchiveFormat format,CompressionPreset preset){
    const auto index=static_cast<unsigned>(preset)-1;require(index<8,"invalid compression preset");CreationOptions o;o.format=format;o.preset=preset;o.extreme=index==7;
    if(format==ArchiveFormat::squashfs){o.codec=std::array{"lz4","lz4","zstd","zstd","zstd","zstd","xz","xz"}[index];o.block_size=std::array<std::uint32_t,8>{131072,65536,131072,131072,262144,1048576,1048576,1048576}[index];if(index>=2&&index<=5)o.level=std::array{1,3,9,19}[index-2];o.no_data_compression=o.no_inode_compression=o.no_fragment_compression=index==0;o.always_fragments=index>=6;if(index>=6)o.xz_dictionary="100%";if(o.extreme)o.xz_filters="x86,arm,armthumb,powerpc,sparc,ia64";}
    else if(format==ArchiveFormat::dwarfs){o.codec=std::array{"null","lz4","zstd","zstd","zstd","zstd","lzma","lzma"}[index];o.block_size=1u<<std::array{20,20,21,22,23,24,26,30}[index];if(index>=2)o.level=std::array{1,5,12,19,8,9}[index-2];}
    else if(format==ArchiveFormat::erofs){o.codec=std::array{"none","lz4","lz4hc","lz4hc","zstd","zstd","lzma","lzma"}[index];o.block_size=std::array<std::uint32_t,8>{4096,4096,16384,32768,65536,262144,1048576,1048576}[index];if(index>=2)o.level=std::array{3,9,9,22,9,109}[index-2];o.fragments=index>=4;o.deduplicate=index>=6;}
    else throw std::invalid_argument("unknown format");
    return o;
}
std::vector<fs::path::string_type> CreationOptions::arguments()const{
    require(std::has_single_bit(block_size),"block size must be a power of two");require(workers<=1024,"invalid worker count");Args a;
    if(format==ArchiveFormat::squashfs){
        require(codec=="gzip"||codec=="lzo"||codec=="xz"||codec=="lz4"||codec=="zstd","invalid SquashFS codec");require(block_size>=4096&&block_size<=1048576,"invalid SquashFS block size");require(!(no_fragments&&always_fragments),"conflicting fragment options");
        if(level)require((codec=="gzip"&&*level>=1&&*level<=9)||(codec=="zstd"&&*level>=-131072&&*level<=22&&*level!=0)||(codec=="lzo"&&*level>=1&&*level<=9),"invalid compression level");
        add(a,"-noappend");add(a,"-comp",codec);add_num(a,"-b",block_size);add(a,"-no-progress");add(a,"-exit-on-error");if(workers)add_num(a,"-processors",workers);if(level)add(a,"-Xcompression-level",std::to_string(*level));
        flag(a,no_fragments,"-no-fragments");flag(a,always_fragments,"-tailends");flag(a,no_duplicates,"-no-duplicates");flag(a,no_xattrs,"-no-xattrs");flag(a,all_root,"-all-root");flag(a,no_exports,"-no-exports");flag(a,no_sparse,"-no-sparse");flag(a,no_data_compression,"-noD");flag(a,no_inode_compression,"-noI");flag(a,no_fragment_compression,"-noF");flag(a,keep_directory,"-keep-as-directory");
        if(memory_mib&&!extreme){require(*memory_mib>=64,"memory limit must be at least 64 MiB");add(a,"-mem",std::to_string(*memory_mib)+"M");}if(timestamp){add_num(a,"-inode-time",*timestamp);add_num(a,"-mkfs-time",*timestamp);}
        if(lz4_high_compression){require(codec=="lz4","LZ4 HC requires LZ4");add(a,"-Xhc");}if(!xz_dictionary.empty()){require(codec=="xz","XZ dictionary requires XZ");add(a,"-Xdict-size",xz_dictionary);}if(!xz_filters.empty()){require(codec=="xz","XZ filters require XZ");add(a,"-Xbcj",xz_filters);}if(gzip_window){require(codec=="gzip"&&*gzip_window>=8&&*gzip_window<=15,"invalid gzip window");add_num(a,"-Xwindow-size",static_cast<unsigned>(*gzip_window));}if(!lzo_algorithm.empty()){require(codec=="lzo","LZO algorithm requires LZO");add(a,"-Xalgorithm",lzo_algorithm);}if(exclude_file){add(a,"-ef");a.push_back(fs::path(fs::absolute(*exclude_file).generic_u16string()).native());}
    }else if(format==ArchiveFormat::dwarfs){
        require(codec=="null"||codec=="lz4"||codec=="lz4hc"||codec=="zstd"||codec=="lzma","invalid DwarFS codec");require(block_size>=1024&&block_size<=1u<<30,"invalid DwarFS block size");const auto index=static_cast<unsigned>(preset)-1;require(index<8,"invalid compression preset");const auto bits=std::countr_zero(block_size);auto compression=codec;
        if(level&&(codec=="zstd"||codec=="lzma"||codec=="lz4hc"))compression+=":level="+std::to_string(*level);
        if(extreme&&codec=="lzma")compression+=":extreme:dict_size="+std::to_string(std::max(12,bits));
        add_num(a,"-l",std::array<unsigned,8>{0,1,2,4,6,7,8,9}[index]);add_num(a,"-S",static_cast<unsigned>(bits));add(a,"-C",compression);add(a,"--no-progress");add(a,"--schema-compression="+std::string(codec=="null"?"null":extreme?"lzma:level=9:extreme":"zstd:level=16"));add(a,"--metadata-compression="+std::string(extreme?"lzma:level=9:extreme":codec=="null"?"null":"zstd"));if(workers)add_num(a,"-N",workers);if(memory_mib&&!extreme)add(a,"-L",std::to_string(*memory_mib)+"m");
    }else if(format==ArchiveFormat::erofs){
        require(codec=="none"||codec=="lz4"||codec=="lz4hc"||codec=="zstd"||codec=="lzma"||codec=="deflate"||codec=="libdeflate","invalid EROFS codec");require(block_size>=4096&&block_size<=1048576,"invalid EROFS cluster size");add(a,"--tar=f");add(a,"-b","4096");if(codec!="none"){add(a,"-z"+codec+(level?",level="+std::to_string(*level):"")+(extreme&&codec=="lzma"?",dictsize=8388608":""));add_num(a,"-C",block_size);if(fragments)add(a,extreme?"-Eall-fragments,ztailpacking":"-Efragments,ztailpacking");flag(a,deduplicate,"-Ededupe");}if(deduplicate&&codec!="none")add(a,"--workers=1");else if(workers)add(a,"--workers="+std::to_string(workers));if(timestamp)add_num(a,"-T",*timestamp);
    }else throw std::invalid_argument("unknown archive format");
    return a;
}
void write_tar(const std::vector<fs::path>& sources,const std::function<void(const void*,std::size_t)>& write,std::stop_token stop){
    auto items=normalize(sources);TarWriter tar(write);tar.check(archive_write_set_format_pax_restricted(tar.value));tar.check(archive_write_set_bytes_per_block(tar.value,65536));tar.check(archive_write_set_bytes_in_last_block(tar.value,512));tar.check(archive_write_open(tar.value,&tar,nullptr,TarWriter::output,nullptr));
    std::vector<std::pair<fs::path,fs::path>> pending;
    if(items.size()==1&&fs::is_directory(fs::symlink_status(items.front())))for(auto& child:sorted_children(items.front()))pending.emplace_back(child,child.filename());else for(auto& item:items)pending.emplace_back(item,base_name(item));std::reverse(pending.begin(),pending.end());std::vector<char> buffer(1u<<20);
    while(!pending.empty()){
        cancelled(stop);auto [source,name]=std::move(pending.back());pending.pop_back();auto status=fs::symlink_status(source);TarEntry entry;auto name_utf8=to_utf8(name.generic_u16string());archive_entry_set_pathname_utf8(entry.value,name_utf8.c_str());archive_entry_set_uid(entry.value,0);archive_entry_set_gid(entry.value,0);archive_entry_set_perm(entry.value,fs::is_directory(status)?0755:0644);archive_entry_set_size(entry.value,0);
        if(fs::is_symlink(status)){archive_entry_set_filetype(entry.value,AE_IFLNK);auto link=to_utf8(fs::read_symlink(source).generic_u16string());archive_entry_set_symlink_utf8(entry.value,link.c_str());archive_entry_set_perm(entry.value,0777);tar.check(archive_write_header(tar.value,entry.value));}
        else if(fs::is_directory(status)){archive_entry_set_filetype(entry.value,AE_IFDIR);set_tar_time(entry.value,source);tar.check(archive_write_header(tar.value,entry.value));auto children=sorted_children(source);for(auto it=children.rbegin();it!=children.rend();++it)pending.emplace_back(*it,name/it->filename());}
        else if(fs::is_regular_file(status)){auto file=open_file_source(source);auto size=file->size();require(size<=static_cast<std::uint64_t>(INT64_MAX),"file too large for TAR");archive_entry_set_filetype(entry.value,AE_IFREG);archive_entry_set_size(entry.value,static_cast<la_int64_t>(size));set_tar_time(entry.value,source);tar.check(archive_write_header(tar.value,entry.value));auto session=file->begin_read();for(std::uint64_t pos=0;pos<size;){cancelled(stop);auto count=static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(),size-pos));session->read_exact(pos,std::as_writable_bytes(std::span(buffer.data(),count)));auto n=archive_write_data(tar.value,buffer.data(),count);if(n<0)tar.check(static_cast<int>(n));if(static_cast<std::size_t>(n)!=count)throw std::runtime_error("short TAR write");pos+=count;}session->finish();}
        else throw std::runtime_error("unsupported TAR entry type");
        tar.check(archive_write_finish_entry(tar.value));
    }
    cancelled(stop);tar.check(archive_write_close(tar.value));
}
std::vector<fs::path> plan_batch_outputs(const std::vector<fs::path>& sources,const fs::path& directory,std::u16string_view suffix){
    require(!sources.empty(),"no source files selected");
    require(fs::is_directory(directory),"output directory does not exist");
    auto output=fs::canonical(directory);
    std::set<std::u16string,WindowsNameLess> names;
    std::vector<fs::path> result;
    for(auto const& input:sources){
        require(fs::exists(input),"source does not exist");
        auto source=fs::absolute(input).lexically_normal();
        if(source.filename().empty()&&source!=source.root_path())source=source.parent_path();
        auto filename=source.filename().u16string();
        require(!filename.empty(),"source needs a file or directory name");
        filename+=suffix;
        require(names.insert(filename).second,"duplicate batch output name");
        auto path=output/fs::path(filename);
        require(!fs::exists(fs::symlink_status(path)),"batch output already exists");
        result.push_back(std::move(path));
    }
    return result;
}
std::vector<fs::path> ImageCreator::create_batch(const std::vector<fs::path>& sources,const fs::path& directory,
                                               CreationOptions options,LogSink log,std::stop_token stop)const{
    const auto suffix=options.format==ArchiveFormat::squashfs?u".squashfs":options.format==ArchiveFormat::dwarfs?u".dwarfs":options.format==ArchiveFormat::erofs?u".erofs":u"";
    require(*suffix!=0,"unknown archive format");
    auto outputs=plan_batch_outputs(sources,directory,suffix);
    // Check directory placement before starting any individual creation.
    const auto parent=fs::canonical(directory);
    for(auto const& source:sources)if(fs::is_directory(source))require(!beneath(fs::canonical(source),parent),"output must be outside the source directory");
    for(std::size_t i=0;i<sources.size();++i){
        cancelled(stop);
        if(log)log("Creating "+to_utf8(outputs[i].u16string()));
        create({sources[i]},outputs[i],options,log,stop);
    }
    return outputs;
}
void ImageCreator::create(const std::vector<fs::path>& sources,const fs::path& destination,CreationOptions options,LogSink log,std::stop_token stop)const{
    auto items=normalize(sources);auto output=fs::absolute(destination).lexically_normal();require(!fs::exists(fs::symlink_status(output)),"output already exists");require(fs::is_directory(output.parent_path()),"output directory does not exist");auto parent=fs::canonical(output.parent_path());for(auto& item:items)if(fs::is_directory(item))require(!beneath(fs::canonical(item),parent),"output must be outside the source directory");cancelled(stop);Workspace workspace(parent);auto image=workspace.path/"image";
    auto squash=[&](const fs::path& target,const CreationOptions& settings){Args args;for(auto& item:items)args.push_back(fs::path(item.generic_u16string()).native());args.push_back(fs::path(target.generic_u16string()).native());auto flags=settings.arguments();args.insert(args.end(),flags.begin(),flags.end());run_process(executable(tools_,"mksquashfs"),args,log,stop);};
    if(options.format==ArchiveFormat::squashfs&&options.extreme){
        options.block_size=1048576;options.memory_mib.reset();options.no_fragments=false;options.always_fragments=true;options.no_data_compression=options.no_inode_compression=options.no_fragment_compression=options.no_duplicates=false;options.gzip_window.reset();options.lzo_algorithm.clear();options.lz4_high_compression=false;options.xz_dictionary.clear();options.xz_filters.clear();options.level.reset();
        std::uintmax_t best=UINTMAX_MAX;std::uint64_t best_used=UINT64_MAX;for(unsigned i=0;i<4;++i){auto c=options;c.codec=std::array{"xz","xz","zstd","gzip"}[i];if(i<2){c.xz_dictionary="100%";if(i==0)c.xz_filters="x86,arm,armthumb,powerpc,sparc,ia64";}else c.level=i==2?22:9;auto trial=workspace.path/("candidate-"+std::to_string(i));if(log)log("Testing "+c.codec);squash(trial,c);auto used=validate_image(trial,decoders_,options.format);auto size=fs::file_size(trial);if(size<best||(size==best&&used<best_used)){if(fs::exists(image))fs::remove(image);fs::rename(trial,image);best=size;best_used=used;}else fs::remove(trial);}
    }else if(options.format==ArchiveFormat::squashfs)squash(image,options);
    else if(options.format==ArchiveFormat::dwarfs){auto root=items.front();if(items.size()!=1||!fs::is_directory(fs::symlink_status(root))){root=workspace.path/"sources";fs::create_directory(root);for(auto& item:items)stage(item,root/base_name(item),stop);}if(options.extreme&&options.block_size==1u<<30)options.block_size=useful_block(root,stop);auto args=options.arguments();add(args,"-i");args.push_back(root.native());add(args,"-o");args.push_back(image.native());run_process(executable(tools_,"mkdwarfs"),args,log,stop);}
    else if(options.format==ArchiveFormat::erofs){auto args=options.arguments();args.push_back(image.native());run_process(executable(tools_,"mkfs.erofs"),args,log,stop,[&](const auto& sink){write_tar(items,sink,stop);});}
    else throw std::invalid_argument("unknown format");
    cancelled(stop);validate_image(image,decoders_,options.format);cancelled(stop);publish(image,output);
}
}
