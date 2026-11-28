// SPDX-License-Identifier: GPL-3.0-only
#include "projection.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
namespace archivefs {
namespace {
using Clock=std::chrono::steady_clock;
using DataCache=ConcurrentCache<std::string,std::byte>;
using namespace std::chrono_literals;
bool eq(std::u16string_view a,std::u16string_view b){WindowsNameLess less;return !less(a,b)&&!less(b,a);}
std::vector<std::u16string> split(std::u16string_view path){
    std::vector<std::u16string> parts;
    while(!path.empty()){
        auto end=path.find_first_of(u"\\/");
        auto part=path.substr(0,end);
        if(!part.empty()){
            if(part==u"."||part==u".."||part.size()>255||part.find_first_of(u"<>:\"|?*")!=part.npos)throw std::invalid_argument("invalid device path");
            parts.emplace_back(part);
        }
        if(end==path.npos)break;
        path.remove_prefix(end+1);
    }
    return parts;
}
std::u16string append(std::u16string_view path,std::u16string_view name){
    std::u16string out(path);if(out.empty()||out.back()!=u'\\')out+=u'\\';out+=name;return out;
}
template<class T>class SnapshotCache {
    struct Entry{std::shared_future<std::shared_ptr<const T>> future;Clock::time_point expires;};
    std::mutex mutex_;
    std::unordered_map<std::string,Entry> entries_;
public:
    template<class Loader>std::shared_ptr<const T> get(std::string key,Loader loader){
        std::shared_future<std::shared_ptr<const T>> future;
        std::shared_ptr<std::promise<std::shared_ptr<const T>>> producer;
        {
            std::lock_guard lock(mutex_);
            auto now=Clock::now();
            std::erase_if(entries_,[&](auto const& entry){return entry.second.expires<=now&&entry.second.future.wait_for(0s)==std::future_status::ready;});
            if(auto it=entries_.find(key);it!=entries_.end())future=it->second.future;
            else{
                if(entries_.size()>=512)throw DeviceFailure(DeviceError::timeout,"Too many concurrent device directory requests");
                producer=std::make_shared<std::promise<std::shared_ptr<const T>>>();future=producer->get_future().share();
                entries_.emplace(key,Entry{future,Clock::time_point::max()});
            }
        }
        if(!producer)return future.get();
        try{
            auto value=std::make_shared<const T>(loader());
            {std::lock_guard lock(mutex_);entries_.at(key).expires=Clock::now()+1s;producer->set_value(value);}
            return value;
        }catch(...){auto error=std::current_exception();{std::lock_guard lock(mutex_);producer->set_exception(error);entries_.erase(key);}std::rethrow_exception(error);}
    }
};
std::string key_for(std::string_view device,std::string_view object){
    std::string key(device);key+='\0';key+=object;return key;
}
template<class T>struct Named {std::u16string name;T value;};
template<class T>std::vector<Named<T>> names(std::vector<T> items){
    std::sort(items.begin(),items.end(),[](auto const& a,auto const& b){return a.id<b.id;});
    std::set<std::u16string,WindowsNameLess> used;
    std::vector<Named<T>> result;
    for(auto& item:items){
        auto base=encode_windows_name(item.name.empty()?item.id:item.name);
        auto name=base;unsigned collision=1;
        while(!used.insert(name).second)name=base+u"~"+to_utf16(std::to_string(collision++));
        result.push_back({std::move(name),std::move(item)});
    }
    return result;
}
std::uint64_t index(std::string_view id){std::uint64_t v=14695981039346656037ULL;for(unsigned char c:id){v^=c;v*=1099511628211ULL;}return v?v:1;}
class RemoteSource final:public RandomAccessSource,public std::enable_shared_from_this<RemoteSource> {
    class Session final:public ReadSession {
    public:
        explicit Session(std::shared_ptr<const RemoteSource> owner):owner_(std::move(owner)){owner_->validate();}
        void read_exact(std::uint64_t offset,std::span<std::byte> data)const override{owner_->read(offset,data);}
        void finish()override{owner_->validate();}
    private:std::shared_ptr<const RemoteSource> owner_;
    };
public:
    RemoteSource(std::shared_ptr<DeviceProvider> provider,std::shared_ptr<DataCache> cache,std::string device,RemoteObject object)
        :provider_(std::move(provider)),cache_(std::move(cache)),device_(std::move(device)),object_(std::move(object)){
        key_=key_for(device_,object_.id);key_+='\0';key_+=object_.revision;key_+='\0';key_+=std::to_string(object_.size)+":"+std::to_string(object_.modified);
    }
    std::uint64_t size()const noexcept override{return object_.size;}
    std::unique_ptr<ReadSession> begin_read()const override{return std::make_unique<Session>(shared_from_this());}
    bool source_changed()const noexcept override{return invalid_.load()||Clock::now().time_since_epoch().count()>=next_check_.load();}
    void validate_path()const override{validate();}
    void validate()const override{
        if(invalid_.load())throw DeviceFailure(DeviceError::offline,"Device file generation is no longer valid");
        if(!source_changed())return;
        std::lock_guard lock(mutex_);
        if(invalid_.load())throw DeviceFailure(DeviceError::offline,"Device file generation is no longer valid");
        if(!source_changed())return;
        try{
            auto now=provider_->stat(device_,object_.id);
            if(now.directory||now.size!=object_.size||now.modified!=object_.modified||now.revision!=object_.revision)
                throw DeviceFailure(DeviceError::missing,"Device file was replaced or modified");
            next_check_.store((Clock::now()+1s).time_since_epoch().count());
        }catch(...){invalid_.store(true);throw;}
    }
private:
    std::shared_ptr<DeviceProvider> provider_;
    std::shared_ptr<DataCache> cache_;
    std::string device_,key_;
    RemoteObject object_;
    mutable std::mutex mutex_;
    mutable std::atomic<bool> invalid_{false};
    mutable std::atomic<Clock::duration::rep> next_check_{0};
    void read(std::uint64_t offset,std::span<std::byte> output)const{
        if(offset>size()||output.size()>size()-offset)throw std::out_of_range("device read outside file");
        validate();
        constexpr std::uint64_t page_size=512u<<10;
        // Large requests bypass the page cache. Random reads coalesce.
        if(output.size()>=page_size){provider_->read_exact(device_,object_.id,offset,output);return;}
        while(!output.empty()){
            auto page=offset/page_size;
            auto start=page*page_size;
            auto inside=static_cast<std::size_t>(offset-start);
            auto length=static_cast<std::size_t>(std::min(page_size,size()-start));
            auto count=std::min(output.size(),length-inside);
            auto data=cache_->get(key_+":"+std::to_string(page),[&]{
                DataCache::Buffer bytes(length);provider_->read_exact(device_,object_.id,start,bytes);return bytes;
            });
            std::copy_n(data->data()+inside,count,output.data());offset+=count;output=output.subspan(count);
        }
        validate();
    }
};
class Devices final:public Projection {
public:
    Devices(std::shared_ptr<DeviceProvider> provider,std::shared_ptr<ImagePool> pool,std::u16string id,std::function<void(const DeviceFailure&)> unavailable)
        :provider_(std::move(provider)),pool_(std::move(pool)),id_(std::move(id)),unavailable_(std::move(unavailable)),cache_(std::make_shared<DataCache>(64u<<20)){}
    ProjectedNodePtr open(std::u16string_view path)const override{
        auto parts=split(path);
        auto devices=enumerate();
        if(parts.empty())return root();
        auto device=std::find_if(devices->begin(),devices->end(),[&](auto const& d){return eq(d.name,parts[0]);});
        if(device==devices->end())return {};
        if(device->value.state!="device"&&device->value.state!="online"){
            auto code=device->value.state=="unauthorized"?DeviceError::permission:DeviceError::offline;
            throw DeviceFailure(code,"Device "+device->value.id+": "+device->value.state);
        }
        auto node=device_node(*device);
        for(std::size_t i=1;i<parts.size();++i){
            if(!node->info.directory)return {};
            auto objects=enumerate_objects(node->device_id,node->object_id);
            auto it=std::find_if(objects->begin(),objects->end(),[&](auto const& o){return eq(o.name,parts[i]);});
            if(it==objects->end())return {};
            node=object_node(node,*it,true);
            if(node->source&&is_image_extension(node->name)){
                if(detect_format(*node->source)!=ArchiveFormat::unknown){
                    // The key is losslessly encoded to avoid case-insensitive map collisions.
                    auto raw=key_for(node->device_id,node->object_id);
                    std::u16string key=id_+u":";
                    constexpr char16_t hex[]=u"0123456789ABCDEF";
                    for(unsigned char c:raw){key+=hex[c>>4];key+=hex[c&15];}
                    auto lease=pool_->acquire_source(std::move(key),node->source);
                    std::u16string remainder;
                    for(std::size_t j=i+1;j<parts.size();++j)remainder=append(remainder,parts[j]);
                    return project_archive(pool_,std::move(lease),{},node->path,remainder);
                }
            }
        }
        return node;
    }
    std::vector<ProjectedNodePtr> list(const ProjectedNodePtr& directory)const override{
        if(!directory||!directory->info.directory)throw std::invalid_argument("not a device directory");
        if(directory->archive)return list_projected_archive(directory);
        std::vector<ProjectedNodePtr> result;
        if(directory->device_id.empty()){
            for(auto& d:*enumerate())result.push_back(device_node(d));
        }else{
            for(auto& o:*enumerate_objects(directory->device_id,directory->object_id))result.push_back(object_node(directory,o,false));
        }
        return result;
    }
    std::uint64_t capacity()const override{return 1ULL<<40;}
private:
    std::shared_ptr<DeviceProvider> provider_;std::shared_ptr<ImagePool> pool_;
    std::u16string id_;std::function<void(const DeviceFailure&)> unavailable_;
    std::shared_ptr<DataCache> cache_;
    mutable SnapshotCache<std::vector<Named<RemoteDevice>>> devices_;
    mutable SnapshotCache<std::vector<Named<RemoteObject>>> objects_;
    std::shared_ptr<const std::vector<Named<RemoteDevice>>> enumerate()const{
        try{return devices_.get("",[&]{return names(provider_->devices());});}
        catch(DeviceFailure const& e){if(unavailable_&&(e.code()==DeviceError::unavailable||e.code()==DeviceError::timeout))try{unavailable_(e);}catch(...){}throw;}
    }
    std::shared_ptr<const std::vector<Named<RemoteObject>>> enumerate_objects(std::string_view device,std::string_view object)const{
        return objects_.get(key_for(device,object),[&]{return names(provider_->children(device,object));});
    }
    ProjectedNodePtr root()const{
        auto n=std::make_shared<ProjectedNode>();n->path=u"\\";n->synthetic=true;n->info={true,0,0,1};return n;
    }
    ProjectedNodePtr device_node(Named<RemoteDevice> const& device)const{
        auto n=std::make_shared<ProjectedNode>();n->name=device.name;n->path=append(u"\\",n->name);n->device_id=device.value.id;n->object_id=provider_->root_id();
        n->info={true,0,0,index(n->device_id)};return n;
    }
    ProjectedNodePtr object_node(ProjectedNodePtr const& parent,Named<RemoteObject> const& object,bool opened)const{
        auto n=std::make_shared<ProjectedNode>();auto const& o=object.value;n->name=object.name;n->path=append(parent->path,n->name);
        n->device_id=parent->device_id;n->object_id=o.id;
        n->info={o.directory,o.directory?0:o.size,o.modified,index(key_for(n->device_id,n->object_id))};
        if(!o.directory){
            if(opened)n->source=std::make_shared<RemoteSource>(provider_,cache_,n->device_id,o);
            else if(is_image_extension(n->name)){n->info.directory=true;n->info.size=0;}
        }
        return n;
    }
};
}
std::shared_ptr<Projection> device_projection(std::shared_ptr<DeviceProvider> provider,std::shared_ptr<ImagePool> pool,
    std::u16string id,std::function<void(const DeviceFailure&)> unavailable){
    if(!provider||!pool)throw std::invalid_argument("missing device projection");
    return std::make_shared<Devices>(std::move(provider),std::move(pool),std::move(id),std::move(unavailable));
}
}
