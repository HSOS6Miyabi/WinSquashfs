// SPDX-License-Identifier: GPL-3.0-only
#include "remote_projection.hpp"
#include "remote_recovery.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <utility>

namespace archivefs {
namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
struct RemoteMount;
struct RemoteFileState{
    std::weak_ptr<RemoteMount> owner;
    std::string path;
    RemoteStat meta;
    std::uint64_t index=0,epoch=0;
    Clock::time_point checked{};
    fs::path stage;
    std::uint64_t reserved=0;
    bool dirty=false,deleted=false,recovery_required=false,damaged=false;
    std::uint64_t base_size=0;
};
namespace {
[[noreturn]] void fail(RemoteError c,const char* m){throw RemoteFailure(c,m);}
bool equal(std::u16string_view a,std::u16string_view b){WindowsNameLess less;return !less(a,b)&&!less(b,a);}
std::vector<std::u16string> parts(std::u16string_view path){
    if(path.size()>32767)throw std::invalid_argument("Remote projection path too long");
    std::vector<std::u16string> result;
    while(!path.empty()){
        auto end=path.find_first_of(u"\\/");
        auto part=path.substr(0,end);
        if(!part.empty()){
            if(part==u"."||part==u".."||part.size()>255||part.back()==u'.'||part.back()==u' '||
               part.find_first_of(u"<>:\"|?*")!=part.npos||
               std::any_of(part.begin(),part.end(),[](char16_t c){return c<32;}))
                throw std::invalid_argument("Invalid remote projection component");
            result.emplace_back(part);
        }
        if(end==path.npos)break;
        path.remove_prefix(end+1);
    }
    return result;
}
std::u16string append(std::u16string_view path,std::u16string_view name){
    auto s=std::u16string(path);if(s.empty()||s.back()!=u'\\')s+=u'\\';s+=name;return s;
}
std::uint64_t hash(std::string_view s){
    std::uint64_t n=14695981039346656037ULL;for(unsigned char c:s){n^=c;n*=1099511628211ULL;}return n?n:1;
}
std::string new_component(std::u16string_view name){
    auto utf8=to_utf8(name);
    if(encode_windows_name(utf8)!=name)throw std::invalid_argument("Name is reserved or requires an escaped Windows representation");
    (void)remote_join("/",utf8);return utf8;
}
struct Named{std::u16string name;RemoteStat info;};
std::vector<Named> mapped_names(std::vector<RemoteStat> entries){
    std::sort(entries.begin(),entries.end(),[](auto& a,auto& b){return a.name<b.name;});
    std::set<std::u16string,WindowsNameLess> used;std::vector<Named> out;
    for(auto& entry:entries){
        try{(void)remote_join("/",entry.name);}catch(const std::invalid_argument&){continue;}
        auto base=encode_windows_name(entry.name);
        if(base.empty()||base.size()>240)continue;
        auto name=base;unsigned n=1;
        while(!used.insert(name).second)name=base+u"~"+to_utf16(std::to_string(n++));
        out.push_back({std::move(name),std::move(entry)});
    }
    return out;
}
void check_stream(std::ios& file){if(!file)fail(RemoteError::no_space,"Local remote write cache I/O failed");}
void file_read(const fs::path& path,std::uint64_t offset,std::span<std::byte> out){
    if(offset>static_cast<std::uint64_t>(INT64_MAX))throw std::out_of_range("Cache offset overflow");
    std::ifstream in(path,std::ios::binary);check_stream(in);
    in.seekg(static_cast<std::streamoff>(offset));check_stream(in);
    if(!out.empty()){in.read(reinterpret_cast<char*>(out.data()),static_cast<std::streamsize>(out.size()));check_stream(in);}
}
void file_write(const fs::path& path,std::uint64_t offset,std::span<const std::byte> data){
    if(offset>static_cast<std::uint64_t>(INT64_MAX))throw std::out_of_range("Cache offset overflow");
    std::fstream out(path,std::ios::binary|std::ios::in|std::ios::out);check_stream(out);
    out.seekp(static_cast<std::streamoff>(offset));check_stream(out);
    if(!data.empty())out.write(reinterpret_cast<const char*>(data.data()),static_cast<std::streamsize>(data.size()));
    out.flush();check_stream(out);
}
fs::path unique_stage(const fs::path& root){
    fs::create_directories(root);std::random_device random;
    for(unsigned i=0;i<64;++i){
        auto path=root/("pending-"+std::to_string(random())+"-"+std::to_string(random()));
        if(fs::create_directory(path))return path;
    }
    fail(RemoteError::no_space,"Cannot allocate a unique remote cache directory");
}
}
struct RemoteMount:std::enable_shared_from_this<RemoteMount>{
    struct Listing{Clock::time_point expires;std::vector<Named> entries;};
    RemoteConnection config;
    std::shared_ptr<RemoteTransport> transport;
    fs::path cache;
    std::recursive_mutex mutex;
    std::map<std::string,std::weak_ptr<RemoteFileState>,std::less<>> states;
    std::map<std::string,std::shared_ptr<RemoteFileState>,std::less<>> pending;
    std::map<std::string,Listing,std::less<>> listings;
    std::set<std::string,std::less<>> blocked;
    ConcurrentCache<std::string,std::byte> reads;
    std::uint64_t used=0,limit=0,sequence=0;
    RemoteMount(RemoteConnection c,fs::path dir,std::shared_ptr<RemoteTransport> t):
        config(std::move(c)),transport(std::move(t)),cache(std::move(dir)),
        reads(static_cast<std::size_t>(config.read_cache_mib)<<20),
        limit(static_cast<std::uint64_t>(config.cache_mib)<<20){
        if(!transport)throw std::invalid_argument("Missing remote transport");
        std::error_code ec;
        if(fs::exists(cache,ec)){
            for(auto& d:fs::directory_iterator(cache)){
                auto data=d.path()/"data";
                if(d.is_directory()&&fs::is_regular_file(data)){
                    auto n=fs::file_size(data);used=n>UINT64_MAX-used?UINT64_MAX:used+n;
                }
            }
        }
    }
    void restore(){
        if(!fs::exists(cache))return;
        for(auto& entry:fs::directory_iterator(cache)){
            if(!entry.is_directory()||entry.is_symlink()||!entry.path().filename().string().starts_with("pending-"))continue;
            try{
                auto record=read_remote_recovery(entry.path());
                if(record.identity!=remote_recovery_identity(config))continue;
                if(record.phase=="committed")continue;
                auto path=record.path;
                if(record.phase=="editing"||!fs::is_regular_file(entry.path()/"data")||
                   fs::is_symlink(entry.path()/"data")||fs::file_size(entry.path()/"data")!=record.size){
                    blocked.insert(path);continue;
                }
                if(pending.contains(path)){
                    blocked.insert(path);pending.erase(path);states.erase(path);continue;
                }
                if(blocked.contains(path))continue;
                auto s=state(path,{remote_name(path),false,record.size,0,record.revision});
                s->stage=entry.path();s->reserved=record.size;s->base_size=record.base_size;
                s->dirty=true;s->recovery_required=true;
                pending.emplace(path,std::move(s));
            }catch(const std::exception&){
                // Leave unrecognized or incomplete cache data untouched.
                // It remains counted against the local write-cache limit.
            }
        }
    }
    void journal(const RemoteFileState& s,std::string phase){
        write_remote_recovery(s.stage,{remote_recovery_identity(config),s.path,s.meta.revision,
            std::move(phase),s.base_size,s.meta.size});
    }
    void check_recovery(std::string_view path)const{
        if(blocked.contains(path))fail(RemoteError::conflict,"Incomplete or duplicate local write cache requires manual recovery");
        if(auto it=pending.find(path);it!=pending.end()&&it->second->recovery_required)
            fail(RemoteError::conflict,"Pending write requires explicit recovery");
    }
    void edit_begin(RemoteFileState& s){
        if(s.damaged||s.recovery_required)fail(RemoteError::conflict,"Pending write requires recovery");
        journal(s,"editing");s.damaged=true;
    }
    void edit_end(RemoteFileState& s){
        sync_remote_cache_file(s.stage/"data");
        journal(s,"ready");s.damaged=false;
    }
    void reserve(std::uint64_t bytes){
        if(used>limit||bytes>limit-used)fail(RemoteError::no_space,"Remote write cache capacity exceeded");
        used+=bytes;
    }
    void invalidate(std::string_view path){
        listings.erase(remote_parent(path));listings.erase(std::string(path));
    }
    std::vector<Named> listing(std::string_view path,bool refresh=false){
        std::lock_guard lock(mutex);
        auto now=Clock::now();auto key=std::string(path);
        auto it=listings.find(key);
        if(!refresh&&it!=listings.end()&&it->second.expires>now)return it->second.entries;
        auto rows=transport->list(path);
        std::set<std::string,std::less<>> seen;
        for(auto& row:rows){
            auto full=remote_join(path,row.name);
            seen.insert(full);
            if(auto p=pending.find(full);p!=pending.end())row=p->second->meta;
        }
        for(auto& [full,s]:pending)if(remote_parent(full)==path&&!seen.contains(full))rows.push_back(s->meta);
        auto named=mapped_names(std::move(rows));
        if(listings.size()>=256)listings.clear();
        listings.insert_or_assign(std::move(key),Listing{now+std::chrono::seconds(config.metadata_seconds),named});
        return named;
    }
    std::shared_ptr<RemoteFileState> state(std::string path,RemoteStat meta){
        std::lock_guard lock(mutex);
        if(auto it=states.find(path);it!=states.end())if(auto s=it->second.lock()){
            if(!s->dirty&&!s->deleted){
                if(s->meta.revision!=meta.revision||s->meta.size!=meta.size)++s->epoch;
                s->meta=std::move(meta);s->checked=Clock::now();
            }
            return s;
        }
        auto s=std::make_shared<RemoteFileState>();s->owner=shared_from_this();s->path=std::move(path);
        s->meta=std::move(meta);s->index=hash(config.name+"\n"+s->path+"\n"+std::to_string(++sequence));s->checked=Clock::now();
        if(states.size()>8192)std::erase_if(states,[](auto& x){return x.second.expired();});
        states[s->path]=s;return s;
    }
    void refresh(RemoteFileState& s){
        if(s.deleted)fail(RemoteError::missing,"Remote file has been deleted");
        if(s.dirty||s.path=="/"||Clock::now()-s.checked<std::chrono::seconds(config.metadata_seconds))return;
        auto info=transport->stat(s.path);
        if(info.directory!=s.meta.directory)fail(RemoteError::conflict,"Remote object type changed");
        if(info.revision!=s.meta.revision||info.size!=s.meta.size)++s.epoch;
        s.meta=std::move(info);s.checked=Clock::now();
    }
    void check_writable(RemoteFileState& s){
        if(config.read_only||s.path=="/")fail(RemoteError::permission,"Remote connection or path is read-only");
        check_recovery(s.path);
        if(s.damaged)fail(RemoteError::conflict,"Incomplete local cache write");
        refresh(s);
    }
    void changed(RemoteFileState& s,std::uint64_t size){
        ++s.epoch;invalidate(s.path);s.meta.size=size;s.checked=Clock::time_point{};
        try{s.meta=transport->stat(s.path);s.checked=Clock::now();}
        catch(const std::exception&){s.meta.revision.clear();}
    }
    void prepare(const std::shared_ptr<RemoteFileState>& s,bool preserve){
        if(s->dirty)return;
        auto size=preserve?s->meta.size:0;
        reserve(size);
        fs::path dir;
        try{
            dir=unique_stage(cache);
            std::ofstream out(dir/"data",std::ios::binary|std::ios::trunc);check_stream(out);
            std::vector<std::byte> buffer(1u<<20);
            for(std::uint64_t offset=0;offset<size;){
                auto n=static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(),size-offset));
                transport->read(s->path,offset,std::span(buffer).first(n),s->meta.revision);
                out.write(reinterpret_cast<const char*>(buffer.data()),static_cast<std::streamsize>(n));check_stream(out);offset+=n;
            }
            out.flush();check_stream(out);out.close();check_stream(out);
            if(preserve){
                auto current=transport->stat(s->path);
                if(current.size!=s->meta.size||(!s->meta.revision.empty()&&current.revision!=s->meta.revision))
                    fail(RemoteError::conflict,"Remote source changed while creating the write cache");
            }
            sync_remote_cache_file(dir/"data");
            write_remote_recovery(dir,{remote_recovery_identity(config),s->path,s->meta.revision,
                "ready",s->meta.size,size});
            s->base_size=s->meta.size;
            s->stage=dir;s->reserved=size;s->dirty=true;
            if(!preserve)s->meta.size=0;
            pending[s->path]=s;
        }catch(...){
            used-=size;if(!dir.empty()){std::error_code ec;fs::remove_all(dir,ec);}throw;
        }
    }
    void cache_resize(RemoteFileState& s,std::uint64_t size){
        if(size>static_cast<std::uint64_t>(INT64_MAX))throw std::length_error("Remote file exceeds signed 64-bit offsets");
        auto previous=s.reserved;
        if(size>previous)reserve(size-previous);
        try{fs::resize_file(s.stage/"data",size);}
        catch(...){if(size>previous)used-=size-previous;throw;}
        if(size<previous)used-=previous-size;
        s.reserved=size;s.meta.size=size;++s.epoch;invalidate(s.path);
    }
    void flush(RemoteFileState& s){
        if(!s.dirty)return;
        if(s.recovery_required||s.damaged)fail(RemoteError::conflict,"Pending upload requires explicit recovery");
        const auto data=s.stage/"data";
        auto size=fs::file_size(data);
        if(size!=s.meta.size)fail(RemoteError::protocol,"Remote write cache size mismatch");
        journal(s,"uploading");
        try{
            transport->replace(s.path,size,[data,size](std::uint64_t offset,std::span<std::byte> out){
                if(offset>size)throw std::out_of_range("Cache upload offset");
                auto n=static_cast<std::size_t>(std::min<std::uint64_t>(out.size(),size-offset));
                file_read(data,offset,out.first(n));return n;
            },s.meta.revision);
        }catch(...){s.recovery_required=true;throw;}
        complete(s,size);
    }
    void complete(RemoteFileState& s,std::uint64_t size){
        // Once acknowledged, never replay the upload because local cleanup
        // failed. A surviving uploading record requires explicit recovery.
        auto old=s.stage;
        bool recorded=false;
        try{journal(s,"committed");recorded=true;}catch(...){}
        s.stage.clear();s.dirty=false;s.recovery_required=false;s.damaged=false;
        pending.erase(s.path);
        std::error_code ec;
        fs::remove_all(old,ec);
        if(!ec)used-=s.reserved;
        s.reserved=0;
        changed(s,size);
        if(ec&&!recorded)fail(RemoteError::conflict,"Remote write succeeded but local recovery record could not be retired");
    }
    void recover(RemoteFileState& s,bool discard){
        if(!s.dirty||!s.recovery_required||s.damaged)
            fail(RemoteError::conflict,"No intact pending recovery record for this path");
        if(discard){
            auto current=transport->stat(s.path);
            this->discard(s);s.recovery_required=false;s.meta=std::move(current);s.checked=Clock::now();++s.epoch;invalidate(s.path);return;
        }
        auto current=transport->stat(s.path);
        if(current.directory)fail(RemoteError::conflict,"Remote recovery target is a directory");
        if(current.size==s.meta.size&&!current.revision.empty()){
            // A response may have been lost after the server committed the
            // upload. Compare the complete content before attempting a retry.
            std::vector<std::byte> a(1u<<20),b(a.size());
            bool identical=true;
            for(std::uint64_t offset=0;offset<current.size;){
                auto n=static_cast<std::size_t>(std::min<std::uint64_t>(a.size(),current.size-offset));
                file_read(s.stage/"data",offset,std::span(a).first(n));
                transport->read(s.path,offset,std::span(b).first(n),current.revision);
                if(!std::equal(a.begin(),a.begin()+static_cast<std::ptrdiff_t>(n),b.begin())){identical=false;break;}
                offset+=n;
            }
            if(identical&&transport->stat(s.path).revision==current.revision){complete(s,current.size);return;}
        }
        if(s.meta.revision.empty()||current.revision!=s.meta.revision||current.size!=s.base_size)
            fail(RemoteError::conflict,"Remote version changed; pending local data retained");
        s.recovery_required=false;
        try{flush(s);}catch(...){if(s.dirty)s.recovery_required=true;throw;}
    }
    void discard(RemoteFileState& s){
        if(s.dirty){std::error_code ec;fs::remove_all(s.stage,ec);if(ec)throw std::system_error(ec,"Cannot discard deleted remote cache");
            used-=s.reserved;s.reserved=0;s.stage.clear();s.dirty=false;pending.erase(s.path);}
    }
};
namespace {
std::shared_ptr<RemoteMount> owner(const ProjectedNodePtr& node){
    if(!node||!node->remote)fail(RemoteError::permission,"Remote protocol root is read-only");
    auto mount=node->remote->owner.lock();if(!mount)fail(RemoteError::offline,"Remote connection closed");return mount;
}
ProjectionInfo info_of(const RemoteFileState& s){return {s.meta.directory,s.meta.size,s.meta.modified,s.index,false};}
ProjectedNodePtr node_for(std::shared_ptr<RemoteFileState> s,std::u16string path,std::u16string name){
    auto node=std::make_shared<ProjectedNode>();node->name=std::move(name);node->path=std::move(path);
    node->info=info_of(*s);node->remote=std::move(s);return node;
}
class RemoteProjection final:public Projection{
public:
    RemoteProjection(RemoteProtocol protocol,std::vector<RemoteConnection> connections,const fs::path& directory,
        std::function<void(std::string_view)> error,RemoteTransportFactory factory):error_(std::move(error)){
        for(auto& c:connections){
            if(c.protocol!=protocol)continue;
            validate_remote_connection(c);
            auto name=to_utf16(c.name);
            if(encode_windows_name(c.name)!=name||mounts_.contains(name))throw std::invalid_argument("Invalid or duplicate remote connection name");
            auto path=directory/fs::path(to_utf16(std::string(remote_protocol_name(protocol))))/fs::path(name);
            auto transport=factory(c);
            auto mount=std::make_shared<RemoteMount>(std::move(c),std::move(path),std::move(transport));
            mount->restore();
            mounts_.emplace(std::move(name),std::move(mount));
        }
    }
    bool writable_volume()const noexcept override{return true;}
    bool writable(const ProjectedNodePtr& node)const noexcept override{
        if(!node||!node->remote)return false;
        auto m=node->remote->owner.lock();return m&&!m->config.read_only;
    }
    ProjectedNodePtr open(std::u16string_view path)const override{
        auto components=parts(path);
        if(components.empty())return root();
        auto found=mounts_.find(components.front());if(found==mounts_.end())return {};
        auto m=found->second;std::lock_guard lock(m->mutex);
        auto s=m->state("/",RemoteStat{m->config.name,true,0,0,{}});
        auto display=append(u"\\",found->first);
        if(components.size()==1)return node_for(s,display,found->first);
        try{
            for(std::size_t i=1;i<components.size();++i){
                if(!s->meta.directory)return {};
                auto entries=m->listing(s->path);
                auto match=std::find_if(entries.begin(),entries.end(),[&](auto& row){return equal(row.name,components[i]);});
                if(match==entries.end())return {};
                auto full=remote_join(s->path,match->info.name);
                s=m->state(std::move(full),match->info);display=append(display,match->name);
                if(i+1==components.size())return node_for(s,display,match->name);
            }
        }catch(const RemoteFailure& e){if(e.code()==RemoteError::missing)return {};throw;}
        return {};
    }
    std::vector<ProjectedNodePtr> list(const ProjectedNodePtr& directory)const override{
        if(!directory)throw std::invalid_argument("Missing remote directory");
        std::vector<ProjectedNodePtr> result;
        if(!directory->remote){
            for(auto& [name,m]:mounts_){std::lock_guard lock(m->mutex);result.push_back(node_for(m->state("/",{m->config.name,true,0,0,{}}),append(u"\\",name),name));}
            return result;
        }
        auto m=owner(directory);std::lock_guard lock(m->mutex);auto& s=*directory->remote;m->refresh(s);
        if(!s.meta.directory)throw std::invalid_argument("Not a remote directory");
        for(auto& entry:m->listing(s.path,true)){
            auto child=m->state(remote_join(s.path,entry.info.name),entry.info);
            result.push_back(node_for(std::move(child),append(directory->path,entry.name),entry.name));
        }
        return result;
    }
    std::uint64_t capacity()const override{return 1ULL<<40;}
    void validate(const ProjectedNodePtr& node)const override{(void)query_info(node);}
    ProjectionInfo query_info(const ProjectedNodePtr& node)const override{
        if(!node)throw std::invalid_argument("Missing remote node");
        if(!node->remote)return node->info;
        auto m=owner(node);std::lock_guard lock(m->mutex);m->refresh(*node->remote);return info_of(*node->remote);
    }
    std::size_t read(const ProjectedNodePtr& node,std::uint64_t offset,std::span<std::byte> output)const override{
        auto m=owner(node);std::lock_guard lock(m->mutex);auto& s=*node->remote;m->refresh(s);
        if(s.meta.directory)throw std::invalid_argument("Not a remote file");
        if(offset>=s.meta.size||output.empty())return 0;
        auto n=static_cast<std::size_t>(std::min<std::uint64_t>(output.size(),s.meta.size-offset));
        if(s.dirty){if(s.damaged)fail(RemoteError::conflict,"Local cache write is incomplete");file_read(s.stage/"data",offset,output.first(n));return n;}
        constexpr std::size_t page=256u<<10;
        if(!m->config.read_cache_mib||s.meta.revision.empty()||n>=2*page){
            m->transport->read(s.path,offset,output.first(n),s.meta.revision);return n;
        }
        auto rest=output.first(n);
        while(!rest.empty()){
            auto start=offset/page*page;
            auto count=static_cast<std::size_t>(std::min<std::uint64_t>(page,s.meta.size-start));
            auto key=std::to_string(s.index)+":"+std::to_string(s.epoch)+":"+std::to_string(start)+":"+s.meta.revision;
            auto bytes=m->reads.get(key,[&]{std::vector<std::byte> v(count);m->transport->read(s.path,start,v,s.meta.revision);return v;});
            auto within=static_cast<std::size_t>(offset-start);auto take=std::min(rest.size(),count-within);
            std::copy_n(bytes->data()+within,take,rest.data());offset+=take;rest=rest.subspan(take);
        }
        return n;
    }
    ProjectedNodePtr create_file(std::u16string_view path,bool directory)override{
        auto [parent,name]=parent_of(path);auto m=owner(parent);std::lock_guard lock(m->mutex);
        if(m->config.read_only)fail(RemoteError::permission,"Remote connection is read-only");
        auto& d=*parent->remote;m->refresh(d);
        if(!d.meta.directory)throw std::invalid_argument("Remote parent is not a directory");
        auto entries=m->listing(d.path,true);
        if(std::any_of(entries.begin(),entries.end(),[&](auto& e){return equal(e.name,name);}))fail(RemoteError::exists,"Remote path already exists");
        auto full=remote_join(d.path,new_component(name));
        m->check_recovery(full);
        if(directory)m->transport->mkdir(full);else m->transport->replace(full,0,{}, {},true);
        m->invalidate(full);auto meta=m->transport->stat(full);
        return node_for(m->state(full,std::move(meta)),append(parent->path,name),name);
    }
    std::size_t write(const ProjectedNodePtr& node,std::uint64_t offset,std::span<const std::byte> data,bool append_to_file,bool constrained)override{
        auto m=owner(node);std::lock_guard lock(m->mutex);auto& s=*node->remote;m->check_writable(s);
        if(s.meta.directory)throw std::invalid_argument("Not a remote file");
        if(append_to_file)offset=s.meta.size;
        if(constrained){if(offset>=s.meta.size)return 0;data=data.first(static_cast<std::size_t>(std::min<std::uint64_t>(data.size(),s.meta.size-offset)));}
        if(data.empty())return 0;
        if(offset>static_cast<std::uint64_t>(INT64_MAX)||data.size()>static_cast<std::uint64_t>(INT64_MAX)-offset)throw std::length_error("Remote file exceeds signed 64-bit offsets");
        auto end=offset+data.size(),size=std::max(end,s.meta.size);
        if(!s.dirty&&m->config.cache_mode!="writeback"&&m->transport->write_at(s.path,offset,data,s.meta.revision)){
            m->changed(s,size);return data.size();
        }
        if(m->config.cache_mode=="direct")fail(RemoteError::unsupported,"Server lacks direct random write support");
        if(size>s.meta.size){
            auto extra=size-s.meta.size;
            auto needed=s.dirty?extra:size;
            if(m->used>m->limit||needed>m->limit-m->used)fail(RemoteError::no_space,"Remote write cache capacity exceeded");
        }
        m->prepare(node->remote,true);
        m->edit_begin(s);
        if(size>s.meta.size)m->cache_resize(s,size);
        file_write(s.stage/"data",offset,data);m->edit_end(s);++s.epoch;m->invalidate(s.path);return data.size();
    }
    void resize(const ProjectedNodePtr& node,std::uint64_t size)override{
        auto m=owner(node);std::lock_guard lock(m->mutex);auto& s=*node->remote;m->check_writable(s);
        if(s.meta.directory)throw std::invalid_argument("Not a remote file");
        if(size==s.meta.size)return;
        if(!s.dirty&&m->config.cache_mode!="writeback"){
            if(m->transport->resize(s.path,size)){m->changed(s,size);return;}
            if(size==0){m->transport->replace(s.path,0,{},s.meta.revision);m->changed(s,0);return;}
        }
        if(m->config.cache_mode=="direct")fail(RemoteError::unsupported,"Server lacks direct resize support");
        m->prepare(node->remote,size!=0);m->edit_begin(s);m->cache_resize(s,size);m->edit_end(s);
    }
    void flush(const ProjectedNodePtr& node)override{
        if(node&&node->remote){auto m=owner(node);std::lock_guard lock(m->mutex);m->flush(*node->remote);return;}
        if(node)return;
        for(auto& [name,m]:mounts_){
            (void)name;std::lock_guard lock(m->mutex);
            std::vector<std::shared_ptr<RemoteFileState>> pending;for(auto& [path,s]:m->pending){(void)path;pending.push_back(s);}
            for(auto& s:pending)m->flush(*s);
        }
    }
    void check_delete(const ProjectedNodePtr& node)override{
        auto m=owner(node);std::lock_guard lock(m->mutex);auto& s=*node->remote;m->check_writable(s);
        if(s.meta.directory&&!m->listing(s.path,true).empty())fail(RemoteError::not_empty,"Remote directory is not empty");
    }
    void remove(const ProjectedNodePtr& node)override{
        auto m=owner(node);std::lock_guard lock(m->mutex);auto& s=*node->remote;check_delete(node);
        m->transport->remove(s.path,s.meta.directory);m->discard(s);s.deleted=true;m->invalidate(s.path);m->states.erase(s.path);
    }
    void rename(const ProjectedNodePtr& node,std::u16string_view path,bool replace)override{
        auto m=owner(node);
        auto [parent,name]=parent_of(path);auto dest=owner(parent);
        if(m!=dest)fail(RemoteError::unsupported,"Rename across connections requires a file copy");
        std::lock_guard lock(m->mutex);auto& s=*node->remote;m->check_writable(s);
        auto target=remote_join(parent->remote->path,new_component(name));
        if(target==s.path)return;
        if(target.starts_with(s.path+"/"))throw std::invalid_argument("Cannot move a directory into itself");
        auto rows=m->listing(parent->remote->path,true);
        for(auto& e:rows)if(equal(e.name,name)){
            target=remote_join(parent->remote->path,e.info.name);
            if(!replace&&target!=s.path)fail(RemoteError::exists,"Remote destination exists");
            break;
        }
        if(target==s.path)return;
        m->check_recovery(target);
        // Keep open descendants consistent by refusing to rename directories
        // while another descendant has a live handle or pending write.
        if(s.meta.directory){
            for(auto& [key,value]:m->states)
                if(key.starts_with(s.path+"/")&&!value.expired())fail(RemoteError::conflict,"Remote directory has active descendants");
            for(auto& key:m->blocked)if(key.starts_with(s.path+"/"))fail(RemoteError::conflict,"Remote directory has incomplete cached writes");
        }
        if(auto it=m->states.find(target);it!=m->states.end()&&!it->second.expired())fail(RemoteError::conflict,"Remote destination is open");
        m->flush(s);auto old=s.path;
        m->transport->rename(old,target,replace);m->invalidate(old);m->states.erase(old);
        s.path=target;s.meta.name=remote_name(target);s.checked={};++s.epoch;
        m->states[target]=node->remote;m->invalidate(target);
    }
    void recover(const ProjectedNodePtr& node,bool discard)override{
        auto m=owner(node);std::lock_guard lock(m->mutex);
        if(m->config.read_only)fail(RemoteError::permission,"Remote connection is read-only");
        m->recover(*node->remote,discard);
    }
    bool server_copy(const ProjectedNodePtr& node,std::u16string_view path,bool replace)override{
        auto m=owner(node);
        auto [parent,name]=parent_of(path);auto dest=owner(parent);
        if(m!=dest)return false;
        std::lock_guard lock(m->mutex);auto& source=*node->remote;
        m->check_writable(source);
        if(source.meta.directory)return false;
        auto target=remote_join(parent->remote->path,new_component(name));
        for(auto& entry:m->listing(parent->remote->path,true))if(equal(entry.name,name)){
            if(!replace)fail(RemoteError::exists,"Remote destination exists");
            if(entry.info.directory)fail(RemoteError::permission,"Remote copy destination is a directory");
            target=remote_join(parent->remote->path,entry.info.name);break;
        }
        if(target==source.path)throw std::invalid_argument("Source and destination are identical");
        m->check_recovery(target);
        if(auto it=m->states.find(target);it!=m->states.end()&&!it->second.expired())
            fail(RemoteError::conflict,"Remote destination is open");
        m->flush(source);
        if(!m->transport->copy(source.path,target,replace))return false;
        m->invalidate(target);m->states.erase(target);return true;
    }
    void operation_error(std::string_view message)noexcept override{if(error_)try{error_(message);}catch(...){}}
private:
    std::map<std::u16string,std::shared_ptr<RemoteMount>,WindowsNameLess> mounts_;
    std::function<void(std::string_view)> error_;
    ProjectedNodePtr root()const{
        auto node=std::make_shared<ProjectedNode>();node->path=u"\\";node->synthetic=true;node->info={true,0,0,1,false};return node;
    }
    std::pair<ProjectedNodePtr,std::u16string> parent_of(std::u16string_view path)const{
        auto p=parts(path);if(p.size()<2)fail(RemoteError::permission,"Cannot change a saved connection through UNC");
        auto name=p.back();p.pop_back();std::u16string parent=u"\\";for(auto& part:p)parent=append(parent,part);
        auto node=open(parent);if(!node)fail(RemoteError::missing,"Remote parent not found");return {node,name};
    }
};
}
std::shared_ptr<Projection> remote_projection(RemoteProtocol protocol,std::vector<RemoteConnection> connections,
    const fs::path& cache_directory,std::function<void(std::string_view)> error,RemoteTransportFactory factory){
    return std::make_shared<RemoteProjection>(protocol,std::move(connections),cache_directory,std::move(error),std::move(factory));
}
}
