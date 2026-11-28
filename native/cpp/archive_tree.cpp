// SPDX-License-Identifier: GPL-3.0-only
#include "archive_tree.hpp"
#include <array>
#include <bit>
#include <chrono>
#include <fstream>
#include <limits>
#include <random>
#include <unordered_set>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace archivefs {
namespace {
constexpr char16_t hex[] = u"0123456789ABCDEF";
void append_hex(std::u16string& out, std::uint16_t value) {
    out.push_back(u'%');
    for (int shift = 12; shift >= 0; shift -= 4) out.push_back(hex[(value >> shift) & 15]);
}
std::u16string digest(std::string_view input) {
    std::array<unsigned char,32> hash{};
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("cannot initialize SHA256");
    struct Alg { BCRYPT_ALG_HANDLE handle; ~Alg() { BCryptCloseAlgorithmProvider(handle,0); } } owner{algorithm};
    BCRYPT_HASH_HANDLE state = nullptr;
    if (BCryptCreateHash(algorithm,&state,nullptr,0,nullptr,0,0)<0)
        throw std::runtime_error("cannot create SHA256 context");
    struct Hash { BCRYPT_HASH_HANDLE handle; ~Hash() { BCryptDestroyHash(handle); } } hash_owner{state};
    while (!input.empty()) {
        auto size=static_cast<ULONG>(std::min<std::size_t>(input.size(),std::numeric_limits<ULONG>::max()));
        if (BCryptHashData(state,reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),size,0)<0)
            throw std::runtime_error("SHA256 failed");
        input.remove_prefix(size);
    }
    if (BCryptFinishHash(state,hash.data(),static_cast<ULONG>(hash.size()),0)<0) throw std::runtime_error("SHA256 failed");
#else
    unsigned length=0;
    if (EVP_Digest(input.data(),input.size(),hash.data(),&length,EVP_sha256(),nullptr)!=1||length!=hash.size())
        throw std::runtime_error("SHA256 failed");
#endif
    std::u16string result;
    for (std::size_t i=0;i<12;++i) { result.push_back(hex[hash[i]>>4]);result.push_back(hex[hash[i]&15]); }
    return result;
}
char16_t uppercase(char16_t ch) { return ch >= u'a' && ch <= u'z' ? static_cast<char16_t>(ch-u'a'+u'A') : ch; }
bool reserved_name(std::u16string_view value) {
    auto prefix=std::u16string(value.substr(0,value.find(u'.')));
    while (!prefix.empty() && prefix.back()==u' ') prefix.pop_back();
    std::transform(prefix.begin(),prefix.end(),prefix.begin(),uppercase);
    if (prefix==u"CON"||prefix==u"PRN"||prefix==u"AUX"||prefix==u"NUL") return true;
    if (prefix.size()==4 && (prefix.starts_with(u"COM")||prefix.starts_with(u"LPT"))) {
        auto ch=prefix.back();
        return (ch>=u'1'&&ch<=u'9')||ch==u'¹'||ch==u'²'||ch==u'³';
    }
    return false;
}
void check_stop(std::stop_token token) { if (token.stop_requested()) throw std::runtime_error("operation cancelled"); }
void set_modified(const std::filesystem::path& path,std::int64_t seconds) {
#ifdef _WIN32
    constexpr std::int64_t epoch = 11644473600LL;
    if (seconds < -epoch || seconds > static_cast<std::int64_t>(UINT64_MAX / 10000000ULL) - epoch)
        throw std::out_of_range("timestamp outside Windows FILETIME range");
    const auto ticks = static_cast<std::uint64_t>(seconds + epoch) * 10000000ULL;
    FILETIME modified{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    const auto handle=CreateFileW(path.c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(handle==INVALID_HANDLE_VALUE) throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"CreateFileW");
    const auto success=SetFileTime(handle,nullptr,nullptr,&modified);
    const auto error=GetLastError();
    CloseHandle(handle);
    if(!success) throw std::system_error(static_cast<int>(error),std::system_category(),"SetFileTime");
#else
    timespec times[2]{{0,UTIME_OMIT},{static_cast<time_t>(seconds),0}};
    if(static_cast<std::int64_t>(times[1].tv_sec)!=seconds) throw std::out_of_range("timestamp outside time_t range");
    if(::utimensat(AT_FDCWD,path.c_str(),times,0)!=0) throw std::system_error(errno,std::generic_category(),"utimensat");
#endif
}
void move_new(const std::filesystem::path& source,const std::filesystem::path& destination) {
#ifdef _WIN32
    if(!MoveFileExW(source.c_str(),destination.c_str(),0))
        throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"MoveFileExW");
#else
    if(::syscall(SYS_renameat2,AT_FDCWD,source.c_str(),AT_FDCWD,destination.c_str(),RENAME_NOREPLACE)!=0)
        throw std::system_error(errno,std::generic_category(),"renameat2");
#endif
}
class StagingDirectory {
public:
    explicit StagingDirectory(const std::filesystem::path& parent) {
        std::random_device random;
        for(int i=0;i<32;++i) {
            auto name=".archivefs-"+std::to_string(random())+"-"+std::to_string(random())+".partial";
            auto candidate=parent/name;
            if(std::filesystem::create_directory(candidate)) { path=std::move(candidate);return; }
        }
        throw std::runtime_error("cannot create extraction staging directory");
    }
    ~StagingDirectory() { std::error_code error;std::filesystem::remove_all(path,error); }
    std::filesystem::path path;
};
} // namespace

std::u16string to_utf16(std::string_view utf8) {
    std::u16string result;
    result.reserve(utf8.size());
    for(std::size_t i=0;i<utf8.size();) {
        const auto first=static_cast<unsigned char>(utf8[i++]);
        std::uint32_t cp=first;
        unsigned tail=0;
        std::uint32_t minimum=0;
        if(first<0x80) {}
        else if(first>=0xc2&&first<=0xdf){cp=first&0x1f;tail=1;minimum=0x80;}
        else if(first>=0xe0&&first<=0xef){cp=first&0xf;tail=2;minimum=0x800;}
        else if(first>=0xf0&&first<=0xf4){cp=first&7;tail=3;minimum=0x10000;}
        else throw std::invalid_argument("invalid UTF-8");
        if(tail>utf8.size()-i)throw std::invalid_argument("truncated UTF-8");
        for(unsigned j=0;j<tail;++j){
            const auto next=static_cast<unsigned char>(utf8[i++]);
            if((next&0xc0)!=0x80)throw std::invalid_argument("invalid UTF-8 continuation");
            cp=(cp<<6)|(next&0x3f);
        }
        if(cp<minimum||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))
            throw std::invalid_argument("invalid Unicode scalar");
        if(cp<0x10000)result.push_back(static_cast<char16_t>(cp));
        else{
            cp-=0x10000;
            result.push_back(static_cast<char16_t>(0xd800+(cp>>10)));
            result.push_back(static_cast<char16_t>(0xdc00+(cp&0x3ff)));
        }
    }
    return result;
}
std::string to_utf8(std::u16string_view utf16) {
    std::string result;
    result.reserve(utf16.size());
    for(std::size_t i=0;i<utf16.size();) {
        std::uint32_t cp=utf16[i++];
        if(cp>=0xd800&&cp<=0xdbff){
            if(i>=utf16.size()||utf16[i]<0xdc00||utf16[i]>0xdfff)
                throw std::invalid_argument("invalid UTF-16 surrogate pair");
            cp=0x10000+((cp-0xd800)<<10)+(utf16[i++]-0xdc00);
        }else if(cp>=0xdc00&&cp<=0xdfff)throw std::invalid_argument("invalid UTF-16 surrogate");
        if(cp<0x80)result.push_back(static_cast<char>(cp));
        else if(cp<0x800){
            result.push_back(static_cast<char>(0xc0|(cp>>6)));
            result.push_back(static_cast<char>(0x80|(cp&63)));
        }else if(cp<0x10000){
            result.push_back(static_cast<char>(0xe0|(cp>>12)));
            result.push_back(static_cast<char>(0x80|((cp>>6)&63)));
            result.push_back(static_cast<char>(0x80|(cp&63)));
        }else{
            result.push_back(static_cast<char>(0xf0|(cp>>18)));
            result.push_back(static_cast<char>(0x80|((cp>>12)&63)));
            result.push_back(static_cast<char>(0x80|((cp>>6)&63)));
            result.push_back(static_cast<char>(0x80|(cp&63)));
        }
    }
    return result;
}
std::u16string encode_windows_name(std::string_view input) {
    if(input.empty()) return u"%0000";
    std::u16string name;
    bool raw=false;
    try{name=to_utf16(input);}
    catch(const std::invalid_argument&){
        raw=true;
        for(unsigned char byte:input)name.push_back(static_cast<char16_t>(byte));
    }
    auto device=reserved_name(name);
    auto trailing=name.find_last_not_of(u" .");
    auto trailing_start=trailing==std::u16string::npos?0:trailing+1;
    std::u16string result;
    for(std::size_t i=0;i<name.size();++i) {
        const auto ch=name[i];
        if(raw||ch<32||std::u16string_view(u"<>:\"/\\|?*%").find(ch)!=std::u16string_view::npos||
           (i==0&&device)||(i>=trailing_start&&(ch==u'.'||ch==u' '))) append_hex(result,ch);
        else result.push_back(ch);
    }
    if(result.size()>220) {
        std::size_t end=190;
        if(result[end-1]>=0xd800&&result[end-1]<=0xdbff) --end;
        result.resize(end);
        result+=u"~"+digest(input);
    }
    return result;
}
bool WindowsNameLess::operator()(std::u16string_view a,std::u16string_view b) const {
    if(a.empty()||b.empty())return a.empty()&&!b.empty();
#ifdef _WIN32
    static_assert(sizeof(wchar_t)==sizeof(char16_t));
    if(a.size()>static_cast<std::size_t>(INT_MAX)||b.size()>static_cast<std::size_t>(INT_MAX))
        throw std::length_error("file name too long");
    const auto result=CompareStringOrdinal(reinterpret_cast<LPCWCH>(a.data()),static_cast<int>(a.size()),
        reinterpret_cast<LPCWCH>(b.data()),static_cast<int>(b.size()),TRUE);
    if(!result) throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"CompareStringOrdinal");
    return result==CSTR_LESS_THAN;
#else
    return std::lexicographical_compare(a.begin(),a.end(),b.begin(),b.end(),[](char16_t x,char16_t y){return uppercase(x)<uppercase(y);});
#endif
}

ArchiveTree::ArchiveTree(std::shared_ptr<ArchiveReader> reader):reader_(std::move(reader)) {
    if(!reader_) throw std::invalid_argument("missing archive reader");
    root_=std::make_shared<Node>(Node{"",u"",reader_->stat(reader_->info().root),{},owner_});
    if(root_->info.kind!=FileKind::directory) throw std::runtime_error("invalid archive root");
}
void ArchiveTree::validate_node(const NodePtr& node) const {
    if(!node) throw std::invalid_argument("missing archive node");
    if(node->owner!=owner_) throw std::invalid_argument("node belongs to another archive");
}
std::shared_ptr<const ArchiveTree::Directory> ArchiveTree::load(const NodePtr& directory) const {
    validate_node(directory);
    if(directory->info.kind!=FileKind::directory) throw std::invalid_argument("not a directory");
    std::shared_ptr<std::promise<std::shared_ptr<const Directory>>> producer;
    std::shared_future<std::shared_ptr<const Directory>> future;
    {
        std::lock_guard lock(directories_mutex_);
        auto it=directories_.find(directory.get());
        if(it!=directories_.end())future=it->second;
        else {
            producer=std::make_shared<std::promise<std::shared_ptr<const Directory>>>();
            future=producer->get_future().share();
            directories_.emplace(directory.get(),future);
        }
    }
    if(!producer)return future.get();
    try {
        auto result=std::make_shared<Directory>();
        for(const auto& entry:reader_->list(directory->info.inode)) {
            auto base=encode_windows_name(entry.name), name=base;
            for(std::uint64_t n=2;result->names.contains(name);++n)
                name=base+u"~"+to_utf16(std::to_string(n));
            auto node=std::make_shared<Node>(Node{entry.name,std::move(name),entry.info,directory,owner_});
            result->names.emplace(node->windows_name,node);
        }
        for(const auto& [name,node]:result->names) { (void)name;result->nodes.push_back(node); }
        producer->set_value(result);
        return result;
    }catch(...){
        auto error=std::current_exception();
        {std::lock_guard lock(directories_mutex_);directories_.erase(directory.get());}
        producer->set_exception(error);
        std::rethrow_exception(error);
    }
}
std::vector<ArchiveTree::NodePtr> ArchiveTree::children(const NodePtr& directory) const {
    // Reject stale source generations even when the directory is already cached.
    validate_node(directory);
    reader_->stat(directory->info.inode);
    return load(directory)->nodes;
}
ArchiveTree::NodePtr ArchiveTree::find(std::u16string_view path) const {
    auto current=root_;
    while(!path.empty()) {
        auto n=path.find_first_not_of(u"\\/");
        if(n==std::u16string_view::npos) break;
        path.remove_prefix(n);
        n=path.find_first_of(u"\\/");
        auto component=path.substr(0,n);
        if(component==u"."||component==u".."||current->info.kind!=FileKind::directory) return {};
        auto directory=load(current);
        auto found=directory->names.find(std::u16string(component));
        if(found==directory->names.end()) return {};
        current=found->second;
        if(n==std::u16string_view::npos) break;
        path.remove_prefix(n);
    }
    reader_->stat(current->info.inode);
    return current;
}
std::u16string ArchiveTree::path_of(const NodePtr& node) const {
    validate_node(node);
    std::vector<NodePtr> components;
    for(auto current=node;current&&current!=root_;current=current->parent.lock())components.push_back(current);
    std::u16string path;
    for(auto it=components.rbegin();it!=components.rend();++it){path+=u'/';path+=(*it)->windows_name;}
    return path.empty()?u"/":path;
}
ArchiveTree::SearchReport ArchiveTree::search(const NodePtr& directory,std::u16string_view query,
                                               std::size_t limit,std::stop_token token) const {
    validate_node(directory);check_stop(token);
    if(directory->info.kind!=FileKind::directory||query.empty()||!limit)
        throw std::invalid_argument("invalid search parameters");
    auto matches=[query](std::u16string_view value){
        if(value.size()<query.size())return false;
        WindowsNameLess less;
        for(std::size_t i=0;i<=value.size()-query.size();++i){
            auto candidate=value.substr(i,query.size());
            if(!less(candidate,query)&&!less(query,candidate))return true;
        }
        return false;
    };
    SearchReport result;
    std::vector<NodePtr> pending{directory};
    std::unordered_set<InodeId> visited;
    while(!pending.empty()){
        check_stop(token);
        auto current=std::move(pending.back());pending.pop_back();
        if(!visited.insert(current->info.inode).second)throw std::runtime_error("archive directory cycle");
        auto entries=children(current);
        for(auto& node:entries){
            check_stop(token);
            bool matched=matches(node->windows_name);
            if(!matched){try{matched=matches(to_utf16(node->name));}catch(const std::invalid_argument&){}}
            if(matched){
                if(result.nodes.size()==limit){result.truncated=true;return result;}
                result.nodes.push_back(node);
            }
        }
        for(auto it=entries.rbegin();it!=entries.rend();++it)
            if((*it)->info.kind==FileKind::directory)pending.push_back(*it);
    }
    return result;
}
std::size_t ArchiveTree::read(const NodePtr& node,std::uint64_t offset,std::span<std::byte> output,std::stop_token token) const {
    validate_node(node);
    return reader_->read_at(node->info.inode,offset,output,token);
}
ArchiveTree::VerifyReport ArchiveTree::verify(std::stop_token token) const {
    VerifyReport report;
    std::vector<NodePtr> pending{root_};
    std::unordered_set<InodeId> seen,visited_dirs;
    std::vector<std::byte> buffer(1u<<20);
    while(!pending.empty()) {
        check_stop(token);
        auto node=std::move(pending.back());pending.pop_back();
        if(node->info.kind==FileKind::directory) {
            if(!visited_dirs.insert(node->info.inode).second)throw std::runtime_error("archive directory cycle");
            for(auto& child:children(node)) pending.push_back(std::move(child));
        } else if(node->info.kind==FileKind::regular&&seen.insert(node->info.inode).second) {
            for(std::uint64_t offset=0;offset<node->info.size;) {
                auto count=read(node,offset,buffer,token);
                if(!count||count>node->info.size-offset) throw std::runtime_error("short archive read");
                if(count>std::numeric_limits<std::uint64_t>::max()-report.bytes) throw std::overflow_error("verification size overflow");
                offset+=count;report.bytes+=count;
            }
            ++report.files;
        }
    }
    return report;
}
void ArchiveTree::extract_file(const NodePtr& node,const std::filesystem::path& output,std::stop_token token) const {
    check_stop(token);
    std::ofstream stream(output,std::ios::binary|std::ios::trunc);
    if(!stream) throw std::runtime_error("cannot create output file");
    std::vector<std::byte> buffer(1u<<20);
    for(std::uint64_t offset=0;offset<node->info.size;) {
        auto count=read(node,offset,buffer,token);
        if(!count||count>node->info.size-offset) throw std::runtime_error("short archive read");
        stream.write(reinterpret_cast<const char*>(buffer.data()),static_cast<std::streamsize>(count));
        if(!stream) throw std::runtime_error("cannot write output file");
        offset+=count;
    }
    stream.close();
    if(!stream) throw std::runtime_error("cannot close output file");
    set_modified(output,node->info.modified);
}
void ArchiveTree::extract(const NodePtr& node,const std::filesystem::path& destination,std::stop_token token) const {
    validate_node(node);check_stop(token);
    if(node->info.kind!=FileKind::regular&&node->info.kind!=FileKind::directory) throw std::invalid_argument("unsupported archive entry");
    auto target=std::filesystem::absolute(destination);
    if(std::filesystem::exists(std::filesystem::symlink_status(target))) throw std::runtime_error("output already exists");
    StagingDirectory staging(target.parent_path());
    auto payload=staging.path/"payload";
    if(node->info.kind==FileKind::regular) extract_file(node,payload,token);
    else {
        std::filesystem::create_directory(payload);
        using Item=std::pair<NodePtr,std::filesystem::path>;
        std::vector<Item> pending{{node,payload}}, directories;
        std::unordered_set<InodeId> visited_dirs;
        while(!pending.empty()) {
            check_stop(token);
            auto item=std::move(pending.back());pending.pop_back();
            if(!visited_dirs.insert(item.first->info.inode).second)throw std::runtime_error("archive directory cycle");
            directories.push_back(item);
            for(auto& child:children(item.first)) {
                check_stop(token);
                auto path=item.second/std::filesystem::path(child->windows_name);
                if(child->info.kind==FileKind::directory) {
                    if(!std::filesystem::create_directory(path)) throw std::runtime_error("duplicate output directory");
                    pending.emplace_back(child,std::move(path));
                } else if(child->info.kind==FileKind::regular) extract_file(child,path,token);
            }
        }
        for(auto it=directories.rbegin();it!=directories.rend();++it) set_modified(it->second,it->first->info.modified);
    }
    check_stop(token);
    move_new(payload,target);
}
} // namespace archivefs
