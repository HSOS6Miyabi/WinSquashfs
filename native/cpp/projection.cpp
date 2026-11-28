// SPDX-License-Identifier: GPL-3.0-only
#include "projection.hpp"
#include "decoder_backend.hpp"
#include "file_time.hpp"
#include <condition_variable>
#include <future>
#include <map>
#include <set>
#include <thread>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace archivefs {
namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
ArchiveOpener make_archive_opener(fs::path decoders,std::size_t cache_bytes){return [decoders=std::move(decoders),cache_bytes](const fs::path& path,SourcePolicy policy){auto backend=open_decoder_backend(open_file_source(path,policy),decoders);auto info=backend->info();auto page=info.block_size?std::clamp<std::size_t>(info.block_size,4096,1u<<20):256u<<10;return std::make_shared<ArchiveTree>(std::make_shared<ArchiveReader>(std::move(backend),cache_bytes,page));};}
struct ImagePool::Entry {std::u16string key;std::shared_future<std::shared_ptr<ArchiveTree>> future;std::size_t active=0;Clock::time_point idle=Clock::time_point::max();};
struct ImagePool::State {
    State(ArchiveOpener fn,Settings cfg):opener(std::move(fn)),settings(cfg){}
    ArchiveOpener opener;Settings settings;mutable std::mutex mutex;std::condition_variable_any wake;std::map<std::u16string,std::shared_ptr<Entry>,WindowsNameLess> images;
    void erase_if_current(const std::shared_ptr<Entry>& entry){auto it=images.find(entry->key);if(it!=images.end()&&it->second==entry)images.erase(it);}
    void trim(){
        auto now=Clock::now();std::vector<std::shared_ptr<Entry>> idle;
        for(auto it=images.begin();it!=images.end();){auto entry=it->second;if(!entry->active&&entry->idle+settings.idle_time<=now)it=images.erase(it);else{if(!entry->active)idle.push_back(entry);++it;}}
        if(idle.size()>settings.idle_images){std::sort(idle.begin(),idle.end(),[](auto& a,auto& b){return a->idle<b->idle;});for(std::size_t i=0;i<idle.size()-settings.idle_images;++i)erase_if_current(idle[i]);}
    }
    void release(const std::shared_ptr<Entry>& entry)noexcept{try{std::lock_guard lock(mutex);if(entry->active&&!--entry->active){entry->idle=Clock::now();trim();}wake.notify_all();}catch(...){}}
    void sweep(){
        std::vector<std::shared_ptr<Entry>> check;
        {std::lock_guard lock(mutex);trim();for(auto& [key,entry]:images)if(entry->future.wait_for(std::chrono::seconds(0))==std::future_status::ready)check.push_back(entry);}
        // Active readers may retain their own generation. Drop the cache
        // entry if the named source has disappeared or been replaced.
        for(auto& entry:check){try{entry->future.get()->info();}catch(...){std::lock_guard lock(mutex);erase_if_current(entry);}}
    }
};
ImagePool::Lease::~Lease(){state_->release(entry_);}
ImagePool::ImagePool(ArchiveOpener opener,Settings settings):state_(std::make_shared<State>(std::move(opener),settings)){
    if(!state_->opener||settings.idle_time.count()<0||settings.idle_time>std::chrono::hours(24)||settings.idle_images>1024)throw std::invalid_argument("invalid image cache configuration");
    worker_=std::jthread([state=state_](std::stop_token stop){while(!stop.stop_requested()){std::unique_lock lock(state->mutex);if(state->images.empty())state->wake.wait(lock,stop,[&]{return !state->images.empty();});else state->wake.wait_for(lock,stop,std::chrono::milliseconds(500),[]{return false;});lock.unlock();if(!stop.stop_requested())state->sweep();}});
}
ImagePool::~ImagePool(){worker_.request_stop();state_->wake.notify_all();if(worker_.joinable())worker_.join();}
std::size_t ImagePool::size()const{std::lock_guard lock(state_->mutex);return state_->images.size();}
void ImagePool::sweep(){state_->sweep();}
std::shared_ptr<ImagePool::Lease> ImagePool::acquire(const fs::path& path){
    auto absolute=fs::absolute(path).lexically_normal();auto key=absolute.u16string();
    for(int attempt=0;attempt<2;++attempt){std::shared_ptr<Entry> entry;std::shared_ptr<std::promise<std::shared_ptr<ArchiveTree>>> promise;
        {std::lock_guard lock(state_->mutex);auto it=state_->images.find(key);if(it!=state_->images.end())entry=it->second;else{promise=std::make_shared<std::promise<std::shared_ptr<ArchiveTree>>>();entry=std::make_shared<Entry>();entry->key=key;entry->future=promise->get_future().share();state_->images.emplace(key,entry);state_->wake.notify_all();}++entry->active;}
        try{
            if(promise){try{promise->set_value(state_->opener(absolute,SourcePolicy::transient_handle));}catch(...){promise->set_exception(std::current_exception());}}
            auto tree=entry->future.get();if(!tree)throw std::runtime_error("archive opener returned no image");tree->info();return std::shared_ptr<Lease>(new Lease(state_,entry,std::move(tree)));
        }catch(...){state_->release(entry);{std::lock_guard lock(state_->mutex);state_->erase_if_current(entry);}if(promise||attempt)throw;}
    }throw std::runtime_error("cannot acquire image");
}
namespace {
bool equal(std::u16string_view a,std::u16string_view b){WindowsNameLess less;return !less(a,b)&&!less(b,a);}
bool valid_component(std::u16string_view part){if(part.empty()||part.size()>255||part==u"."||part==u".."||part.back()==u'.'||part.back()==u' ')return false;for(char16_t c:part)if(c<32||std::u16string_view(u"<>:\"/\\|?*").find(c)!=std::u16string_view::npos)return false;return true;}
std::vector<std::u16string> components(std::u16string_view path){std::vector<std::u16string> parts;while(!path.empty()){auto split=path.find_first_of(u"\\/");auto part=path.substr(0,split);if(!part.empty()){if(!valid_component(part))throw std::invalid_argument("invalid projected path component");parts.emplace_back(part);}if(split==std::u16string_view::npos)break;path.remove_prefix(split+1);}return parts;}
std::u16string join(const std::vector<std::u16string>& parts,std::size_t from,std::size_t to){std::u16string result;for(auto i=from;i<to;++i){if(!result.empty())result+=u'\\';result+=parts[i];}return result;}
std::u16string child_path(std::u16string_view path,std::u16string_view child){std::u16string result(path);if(result.empty()||result.back()!=u'\\')result+=u'\\';result+=child;return result;}
std::uint64_t index_for(std::u16string_view text,std::uint64_t inode){std::uint64_t value=14695981039346656037ULL;for(auto c:text){if(c>=u'a'&&c<=u'z')c=static_cast<char16_t>(c-u'a'+u'A');value^=c;value*=1099511628211ULL;}value^=inode;return value?value:1;}
std::int64_t modified(const fs::path& path){std::error_code ec;auto t=fs::last_write_time(path,ec);if(ec)return 0;return std::chrono::duration_cast<std::chrono::seconds>(file_time_to_system(t).time_since_epoch()).count();}
bool supported(const ArchiveTree::NodePtr& n){return n&& (n->info.kind==FileKind::directory||n->info.kind==FileKind::regular);}
ProjectedNodePtr archive_node(std::shared_ptr<ArchiveTree> archive,ArchiveTree::NodePtr entry,const fs::path& physical,std::u16string path,std::u16string name,std::shared_ptr<ImagePool::Lease> lease={}){
    if(!supported(entry))return {};
    auto node=std::make_shared<ProjectedNode>();node->archive=std::move(archive);node->entry=std::move(entry);node->physical=physical;node->path=std::move(path);node->name=std::move(name);node->lease=std::move(lease);const auto& info=node->entry->info;node->info={info.kind==FileKind::directory,info.kind==FileKind::regular?info.size:0,info.modified,index_for(physical.u16string(),info.inode)};return node;
}
ProjectedNodePtr directory_node(const fs::path& physical,std::u16string path,std::u16string name,bool synthetic=false){auto node=std::make_shared<ProjectedNode>();node->physical=physical;node->path=std::move(path);node->name=std::move(name);node->synthetic=synthetic;node->info={true,0,synthetic?0:modified(physical),index_for(physical.u16string(),0)};return node;}
std::vector<ProjectedNodePtr> archive_children(const ProjectedNodePtr& parent){if(!parent||!parent->entry||!parent->info.directory)throw std::invalid_argument("not a projected directory");std::vector<ProjectedNodePtr> result;for(auto& child:parent->archive->children(parent->entry))if(supported(child))result.push_back(archive_node(parent->archive,child,parent->physical,child_path(parent->path,child->windows_name),child->windows_name,parent->lease));return result;}
bool projected_host(std::u16string_view host){return equal(host,u"sfs$")||equal(host,u"sfs.localhost")||(host.size()>4&&equal(host.substr(0,4),u"sfs.")&&host.size()>=10&&equal(host.substr(host.size()-10),u".localhost"));}
bool projected_source(const fs::path& path){
#ifdef _WIN32
    auto value=path.u16string();if(value.starts_with(u"\\\\?\\UNC\\"))value=u"\\\\"+value.substr(8);if(value.starts_with(u"\\\\")){auto end=value.find(u'\\',2);return projected_host(std::u16string_view(value).substr(2,end==std::u16string::npos?end:end-2));}
    auto root=path.root_path();if(!root.empty()){wchar_t format[64]{};if(GetVolumeInformationW(root.c_str(),nullptr,0,nullptr,nullptr,nullptr,format,64)){if(_wcsicmp(format,L"ArchiveFS")==0||_wcsicmp(format,L"SquashFS")==0)return true;}}
#else
    (void)path;
#endif
    return false;
}
class Disk final:public Projection {
public:explicit Disk(std::shared_ptr<ArchiveTree> archive):archive_(std::move(archive)){}
    ProjectedNodePtr open(std::u16string_view path)const override{auto parts=components(path);auto n=archive_->find(join(parts,0,parts.size()));return archive_node(archive_,n,{},u"\\"+join(parts,0,parts.size()),parts.empty()?u"":parts.back());}
    std::vector<ProjectedNodePtr> list(const ProjectedNodePtr& node)const override{return archive_children(node);}
    std::uint64_t capacity()const override{return archive_->info().image_size;}
private:std::shared_ptr<ArchiveTree> archive_;
};
class Unc final:public Projection {
public:Unc(Share share,std::shared_ptr<ImagePool> pool):share_(std::move(share)),pool_(std::move(pool)){if(!pool_||!valid_component(share_.name)||!valid_component(share_.server))throw std::invalid_argument("invalid UNC share");if(!share_.network){share_.root=fs::absolute(share_.root).lexically_normal();if(projected_source(share_.root))throw std::invalid_argument("recursive source projection");}}
    ProjectedNodePtr open(std::u16string_view path)const override{
        auto parts=components(path);auto physical=share_.root;std::size_t start=0;
        if(share_.network){if(parts.empty())return directory_node({},u"\\",u"",true);if(projected_host(parts.front()))return {};if(parts.size()==1)return directory_node(fs::path(u"\\\\"+parts[0]),u"\\"+parts[0],parts[0],true);physical=fs::path(u"\\\\"+parts[0]+u"\\"+parts[1]);start=2;}
        auto physical_at=[&](std::size_t count){auto result=physical;for(auto i=start;i<count;++i)result/=fs::path(parts[i]);return result;};
        auto remember=[&]{if(share_.network){std::lock_guard lock(known_mutex_);known_[parts[0]].insert(parts[1]);}};
        auto image=[&](std::size_t count,const fs::path& source)->ProjectedNodePtr{if(projected_source(source))return {};auto lease=pool_->acquire(source);auto tree=lease->tree();auto n=tree->find(join(parts,count,parts.size()));if(!supported(n))return {};remember();return archive_node(tree,n,source,u"\\"+join(parts,0,parts.size()),parts.back(),std::move(lease));};
        // Try known image suffixes first. Direct paths do not require enumeration
        // permissions on intermediate directories of a remote share.
        for(std::size_t i=start+1;i<=parts.size();++i)if(is_image_extension(parts[i-1])){auto p=physical_at(i);std::error_code ec;if(fs::is_regular_file(p,ec))return image(i,p);}
        for(std::size_t i=parts.size();i>start;--i){auto p=physical_at(i);std::error_code ec;auto st=fs::status(p,ec);if(ec)continue;if(fs::is_regular_file(st))return image(i,p);if(fs::is_directory(st)){if(i!=parts.size())return {};if(projected_source(p))return {};remember();return directory_node(p,u"\\"+join(parts,0,parts.size()),parts.back());}}
        if(parts.size()==start){if(share_.network){remember();return directory_node(physical,u"\\"+join(parts,0,parts.size()),parts.back(),true);}std::error_code ec;if(fs::is_directory(physical,ec))return directory_node(physical,u"\\",u"");}return {};
    }
    std::vector<ProjectedNodePtr> list(const ProjectedNodePtr& directory)const override{
        if(!directory||!directory->info.directory)throw std::invalid_argument("not a directory");
        if(directory->entry)return archive_children(directory);
        std::vector<ProjectedNodePtr> nodes;
        if(share_.network&&directory->synthetic){auto parts=components(directory->path);std::lock_guard lock(known_mutex_);if(parts.empty()){for(auto& [server,shares]:known_)nodes.push_back(directory_node(fs::path(u"\\\\"+server),child_path(directory->path,server),server,true));return nodes;}if(parts.size()==1){auto it=known_.find(parts[0]);if(it!=known_.end())for(auto& share:it->second)nodes.push_back(directory_node(fs::path(u"\\\\"+parts[0]+u"\\"+share),child_path(directory->path,share),share,true));return nodes;}}
        if(projected_source(directory->physical))return nodes;
        for(auto& item:fs::directory_iterator(directory->physical)){auto name=item.path().filename().u16string();if(!valid_component(name))continue;std::error_code ec;auto status=item.status(ec);if(ec)continue;if(fs::is_directory(status))nodes.push_back(directory_node(item.path(),child_path(directory->path,name),name));else if(fs::is_regular_file(status)&&is_image_extension(name))nodes.push_back(directory_node(item.path(),child_path(directory->path,name),name));}
        std::sort(nodes.begin(),nodes.end(),[](auto& a,auto& b){return WindowsNameLess{}(a->name,b->name);});return nodes;
    }
    std::uint64_t capacity()const override{return 1ULL<<40;}
private:Share share_;std::shared_ptr<ImagePool> pool_;mutable std::mutex known_mutex_;mutable std::map<std::u16string,std::set<std::u16string,WindowsNameLess>,WindowsNameLess> known_;
};
}
void Projection::validate(const ProjectedNodePtr& node)const{if(!node)throw std::invalid_argument("missing projected node");if(node->archive)node->archive->info();}
std::size_t Projection::read(const ProjectedNodePtr& node,std::uint64_t offset,std::span<std::byte> output)const{validate(node);if(!node->archive||!node->entry||node->info.directory)throw std::invalid_argument("not a projected file");return node->archive->read(node->entry,offset,output);}
std::shared_ptr<Projection> mount_projection(std::shared_ptr<ArchiveTree> tree){if(!tree)throw std::invalid_argument("missing archive");return std::make_shared<Disk>(std::move(tree));}
std::shared_ptr<Projection> unc_projection(Share share,std::shared_ptr<ImagePool> pool){return std::make_shared<Unc>(std::move(share),std::move(pool));}
bool is_image_extension(std::u16string_view name){auto ext=name.rfind(u'.');if(ext==std::u16string_view::npos)return false;auto e=name.substr(ext);return equal(e,u".squashfs")||equal(e,u".sqfs")||equal(e,u".sfs")||equal(e,u".dwarfs")||equal(e,u".erofs");}
std::optional<std::u16string> image_unc_path(const Share& share,const fs::path& image){auto prefix=u"\\\\"+share.server+u"\\"+share.name;auto path=fs::absolute(image).lexically_normal();if(share.network){auto value=path.u16string();if(!value.starts_with(u"\\\\")||projected_source(path))return {};auto parts=components(std::u16string_view(value).substr(2));if(parts.size()<3)return {};return prefix+u"\\"+join(parts,0,parts.size());}auto root=fs::absolute(share.root).lexically_normal();auto a=root.begin(),b=path.begin();for(;a!=root.end();++a,++b)if(b==path.end()||!equal(a->u16string(),b->u16string()))return {};if(b==path.end())return {};std::vector<std::u16string> parts;for(;b!=path.end();++b)parts.push_back(b->u16string());return prefix+u"\\"+join(parts,0,parts.size());}
}
