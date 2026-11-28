// SPDX-License-Identifier: GPL-3.0-only
#include "projection.hpp"
#include "decoder_backend.hpp"
#include "cache_budget.hpp"
#include "image_creation.hpp"
#include "file_time.hpp"
#include <condition_variable>
#include <future>
#include <map>
#include <set>
#include <thread>
#include <cstdio>
#include <cstdlib>
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
ArchiveSourceOpener make_archive_source_opener(fs::path decoders,std::size_t cache_bytes,std::size_t total_cache_bytes){
    auto budget=std::make_shared<CacheBudget>(total_cache_bytes);
    return [decoders=std::move(decoders),cache_bytes,budget](std::shared_ptr<const RandomAccessSource> source){
        struct TreeLifetime {std::shared_ptr<CacheBudget::Reservation> reservation;std::shared_ptr<ArchiveTree> tree;};
        auto reservation=std::make_shared<CacheBudget::Reservation>(budget,cache_bytes);
        const auto bytes=reservation->bytes();
        auto backend=open_decoder_backend(std::move(source),decoders,bytes);
        auto page=recommended_page_size(backend->info());
        auto tree=std::make_shared<ArchiveTree>(std::make_shared<ArchiveReader>(std::move(backend),bytes,page));
        auto holder=std::make_shared<TreeLifetime>(TreeLifetime{std::move(reservation),std::move(tree)});
        return std::shared_ptr<ArchiveTree>(holder,holder->tree.get());
    };
}
ArchiveOpener make_archive_opener(fs::path decoders,std::size_t cache_bytes,std::size_t total_cache_bytes){
    auto open=make_archive_source_opener(std::move(decoders),cache_bytes,total_cache_bytes);
    return [open=std::move(open)](const fs::path& path,SourcePolicy policy){return open(open_file_source(path,policy));};
}
std::vector<fs::path> extract_batch(const std::vector<fs::path>& sources,const fs::path& directory,
                                    const ArchiveOpener& opener,std::stop_token stop){
    if(!opener)throw std::invalid_argument("missing archive opener");
    auto outputs=plan_batch_outputs(sources,directory,u".extracted");
    for(auto const& source:sources)if(!fs::is_regular_file(source))throw std::invalid_argument("source is not an archive file");
    for(std::size_t i=0;i<sources.size();++i){
        if(stop.stop_requested())throw std::runtime_error("operation cancelled");
        auto tree=opener(sources[i],SourcePolicy::transient_handle);
        if(!tree)throw std::runtime_error("archive opener returned no image");
        tree->extract(tree->root(),outputs[i],stop);
    }
    return outputs;
}
struct ImagePool::Entry {std::u16string key;std::shared_future<std::shared_ptr<ArchiveTree>> future;std::size_t active=0;Clock::time_point idle=Clock::time_point::max();};
struct ImagePool::State {
    State(ArchiveOpener fn,Settings cfg,ArchiveSourceOpener source,OpenProgressSink progress):opener(std::move(fn)),source_opener(std::move(source)),settings(cfg),progress_sink(std::move(progress)){}
    ArchiveOpener opener;ArchiveSourceOpener source_opener;Settings settings;OpenProgressSink progress_sink;mutable std::mutex mutex;std::condition_variable_any wake;std::uint64_t revision=0;std::map<std::u16string,std::shared_ptr<Entry>,WindowsNameLess> images;
    void changed(){++revision;wake.notify_all();}
    Clock::time_point deadline() const {
        auto next=Clock::time_point::max();
        for(auto& [key,entry]:images)if(!entry->active)next=std::min(next,entry->idle+settings.idle_time);
        return next;
    }
    void erase_if_current(const std::shared_ptr<Entry>& entry){auto it=images.find(entry->key);if(it!=images.end()&&it->second==entry)images.erase(it);}
    void trim(){
        auto now=Clock::now();std::vector<std::shared_ptr<Entry>> idle;
        for(auto it=images.begin();it!=images.end();){auto entry=it->second;if(!entry->active&&entry->idle+settings.idle_time<=now)it=images.erase(it);else{if(!entry->active)idle.push_back(entry);++it;}}
        if(idle.size()>settings.idle_images){std::sort(idle.begin(),idle.end(),[](auto& a,auto& b){return a->idle<b->idle;});for(std::size_t i=0;i<idle.size()-settings.idle_images;++i)erase_if_current(idle[i]);}
    }
    void release(const std::shared_ptr<Entry>& entry)noexcept{try{std::lock_guard lock(mutex);if(entry->active&&!--entry->active){entry->idle=Clock::now();trim();}changed();}catch(...){}}
    void sweep(bool changed_only=false){
        std::vector<std::shared_ptr<Entry>> check;
        {std::lock_guard lock(mutex);trim();for(auto& [key,entry]:images)if(entry->future.wait_for(std::chrono::seconds(0))==std::future_status::ready)check.push_back(entry);}
        // Active readers may retain their own generation. Drop the cache
        // entry if the named source has disappeared or been replaced.
        for(auto& entry:check){
            try{
                auto tree=entry->future.get();
                if(!changed_only||tree->source_changed())tree->validate_path();
            }catch(...){
                std::lock_guard lock(mutex);
                erase_if_current(entry);
            }
        }
    }
};
ImagePool::Lease::~Lease(){state_->release(entry_);}
ImagePool::ImagePool(ArchiveOpener opener,Settings settings,ArchiveSourceOpener source_opener,OpenProgressSink progress):state_(std::make_shared<State>(std::move(opener),settings,std::move(source_opener),std::move(progress))){
    if(!state_->opener||settings.idle_time.count()<0||settings.idle_time>std::chrono::hours(24)||settings.idle_images>1024)throw std::invalid_argument("invalid image cache configuration");
    worker_=std::jthread([state=state_](std::stop_token stop){
        std::unique_lock lock(state->mutex);
        constexpr auto check_interval=std::chrono::milliseconds(500);
        auto next_check=Clock::now()+check_interval;
        while(!stop.stop_requested()){
            state->trim();
            if(Clock::now()>=next_check){
                // A slow or remote filesystem must not hold the pool mutex.
                lock.unlock();
                state->sweep(true);
                lock.lock();
                next_check=Clock::now()+check_interval;
                continue;
            }
            const auto next=std::min(state->deadline(),next_check);
            const auto revision=state->revision;
            state->wake.wait_until(lock,stop,next,[&]{return state->revision!=revision;});
        }
    });
}
ImagePool::~ImagePool(){worker_.request_stop();state_->wake.notify_all();if(worker_.joinable())worker_.join();}
std::size_t ImagePool::size()const{std::lock_guard lock(state_->mutex);return state_->images.size();}
void ImagePool::sweep(){state_->sweep();}
std::shared_ptr<ImagePool::Lease> ImagePool::acquire(const fs::path& path){
    auto absolute=fs::absolute(path).lexically_normal();
    return acquire_key(absolute.u16string(),absolute.u16string(),[&](std::shared_ptr<OpenProgress> progress){
        if(progress&&state_->source_opener){
            auto source=open_file_source(absolute,SourcePolicy::transient_handle);
            return state_->source_opener(std::make_shared<ProgressSource>(std::move(source),std::move(progress)));
        }
        return state_->opener(absolute,SourcePolicy::transient_handle);
    });
}
std::shared_ptr<ImagePool::Lease> ImagePool::acquire_source(std::u16string key,std::shared_ptr<const RandomAccessSource> source,std::u16string display){
    if(!state_->source_opener||!source)throw std::invalid_argument("nested source opening is unavailable");
    return acquire_key(u"@"+key,std::move(display),[&](std::shared_ptr<OpenProgress> progress){
        if(progress)return state_->source_opener(std::make_shared<ProgressSource>(source,std::move(progress)));
        return state_->source_opener(source);
    });
}
std::shared_ptr<ImagePool::Lease> ImagePool::acquire_key(std::u16string key,std::u16string display,const std::function<std::shared_ptr<ArchiveTree>(std::shared_ptr<OpenProgress>)>& open){
    for(int attempt=0;attempt<2;++attempt){std::shared_ptr<Entry> entry;std::shared_ptr<std::promise<std::shared_ptr<ArchiveTree>>> promise;
        {std::lock_guard lock(state_->mutex);auto it=state_->images.find(key);if(it!=state_->images.end())entry=it->second;else{promise=std::make_shared<std::promise<std::shared_ptr<ArchiveTree>>>();entry=std::make_shared<Entry>();entry->key=key;entry->future=promise->get_future().share();state_->images.emplace(key,entry);state_->changed();}++entry->active;}
        try{
            if(promise){
                std::shared_ptr<OpenProgress> progress;
                if(state_->progress_sink&&!display.empty())try{
                    progress=std::make_shared<OpenProgress>(display);state_->progress_sink(progress);
                }catch(...){}
                struct Finish{std::shared_ptr<OpenProgress> value;~Finish(){if(value)value->done.store(true,std::memory_order_release);}} finish{progress};
                try{auto opened=open(progress);if(!opened)throw std::runtime_error("archive opener returned no image");opened->validate_path();promise->set_value(std::move(opened));}
                catch(...){promise->set_exception(std::current_exception());}
            }
            auto tree=entry->future.get();if(!tree)throw std::runtime_error("archive opener returned no image");tree->validate_path();return std::shared_ptr<Lease>(new Lease(state_,entry,std::move(tree)));
        }catch(...){if(std::getenv("ARCHIVEFS_TRACE")){try{throw;}catch(const std::exception& e){std::fprintf(stderr,"Image acquisition failed: %s\n",e.what());}catch(...){}}state_->release(entry);{std::lock_guard lock(state_->mutex);state_->erase_if_current(entry);}if(promise||attempt)throw;}
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
    if(!entry)return {};
    auto target=archive->resolve_link(entry);
    const bool available=supported(target);
    auto node=std::make_shared<ProjectedNode>();node->archive=std::move(archive);node->entry=available?std::move(target):std::move(entry);node->physical=physical;node->path=std::move(path);node->name=std::move(name);node->lease=std::move(lease);const auto& info=node->entry->info;node->info={available&&info.kind==FileKind::directory,available&&info.kind==FileKind::regular?info.size:0,info.modified,index_for(physical.u16string(),info.inode),!available};return node;
}
ProjectedNodePtr directory_node(const fs::path& physical,std::u16string path,std::u16string name,bool synthetic=false){auto node=std::make_shared<ProjectedNode>();node->physical=physical;node->path=std::move(path);node->name=std::move(name);node->synthetic=synthetic;node->info={true,0,synthetic?0:modified(physical),index_for(physical.u16string(),0)};return node;}
std::vector<ProjectedNodePtr> archive_children(const ProjectedNodePtr& parent){if(!parent||!parent->entry||!parent->info.directory)throw std::invalid_argument("not a projected directory");std::vector<ProjectedNodePtr> result;for(auto& child:parent->archive->children(parent->entry))result.push_back(archive_node(parent->archive,child,parent->physical,child_path(parent->path,child->windows_name),child->windows_name,parent->lease));return result;}
bool projected_host(std::u16string_view host){return equal(host,u"sfs$")||equal(host,u"sfs.localhost")||(host.size()>4&&equal(host.substr(0,4),u"sfs.")&&host.size()>=10&&equal(host.substr(host.size()-10),u".localhost"));}
bool projected_source(const fs::path& path){
#ifdef _WIN32
    auto value=path.u16string();if(value.starts_with(u"\\\\?\\UNC\\"))value=u"\\\\"+value.substr(8);if(value.starts_with(u"\\\\")){
        auto end=value.find(u'\\',2);auto host=std::u16string_view(value).substr(2,end==std::u16string::npos?end:end-2);
        if(projected_host(host))return true;
        if(host.size()>4&&equal(host.substr(0,4),u"sfs.")&&end!=std::u16string::npos){
            auto share_end=value.find(u'\\',end+1);
            auto root=value.substr(0,share_end==std::u16string::npos?value.size():share_end);
            if(root.back()!=u'\\')root+=u'\\';
            wchar_t format[64]{};
            if(GetVolumeInformationW(fs::path(root).c_str(),nullptr,0,nullptr,nullptr,nullptr,format,64))
                return !_wcsicmp(format,L"ArchiveFS")||!_wcsicmp(format,L"SquashFS");
        }
        return false;
    }
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
        auto image=[&](std::size_t count,const fs::path& source)->ProjectedNodePtr{if(projected_source(source))return {};auto lease=pool_->acquire(source);auto projected=project_archive(pool_,std::move(lease),source,u"\\"+join(parts,0,count),join(parts,count,parts.size()));if(projected)remember();return projected;};
        // Try known image suffixes first. Direct paths do not require enumeration
        // permissions on intermediate directories of a remote share.
        for(std::size_t i=start+1;i<=parts.size();++i)if(is_image_extension(parts[i-1])){auto p=physical_at(i);std::error_code ec;if(fs::is_regular_file(p,ec))return image(i,p);}
        for(std::size_t i=parts.size();i>start;--i){auto p=physical_at(i);std::error_code ec;auto st=fs::status(p,ec);if(ec)continue;if(fs::is_regular_file(st))return image(i,p);if(fs::is_directory(st)){if(i!=parts.size())return {};if(projected_source(p))return {};remember();return directory_node(p,u"\\"+join(parts,0,parts.size()),parts.back());}}
        if(parts.size()==start){if(share_.network){remember();return directory_node(physical,u"\\"+join(parts,0,parts.size()),parts.back(),true);}std::error_code ec;if(fs::is_directory(physical,ec))return directory_node(physical,u"\\",u"");}return {};
    }
    std::vector<ProjectedNodePtr> list(const ProjectedNodePtr& directory)const override{
        if(!directory||!directory->info.directory)throw std::invalid_argument("not a directory");
        if(directory->entry)return list_projected_archive(directory);
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
namespace {
class NestedSource final:public RandomAccessSource {
    class Session final:public ReadSession {
    public:
        Session(std::shared_ptr<ArchiveTree> tree,ArchiveTree::NodePtr node):tree_(std::move(tree)),node_(std::move(node)){}
        void read_exact(std::uint64_t offset,std::span<std::byte> buffer)const override{
            if(tree_->read(node_,offset,buffer)!=buffer.size())throw std::runtime_error("short nested image read");
        }
        void finish()override{tree_->info();}
    private:std::shared_ptr<ArchiveTree> tree_;ArchiveTree::NodePtr node_;
    };
public:
    NestedSource(std::shared_ptr<ArchiveTree> tree,ArchiveTree::NodePtr node):tree_(std::move(tree)),node_(std::move(node)){}
    std::uint64_t size()const noexcept override{return node_->info.size;}
    std::unique_ptr<ReadSession> begin_read()const override{return std::make_unique<Session>(tree_,node_);}
    void validate()const override{tree_->info();}
    void validate_path()const override{tree_->validate_path();}
    bool source_changed()const noexcept override{return tree_->source_changed();}
private:std::shared_ptr<ArchiveTree> tree_;ArchiveTree::NodePtr node_;
};
}
ProjectedNodePtr project_archive(const std::shared_ptr<ImagePool>& pool,std::shared_ptr<ImagePool::Lease> lease,
    fs::path physical,std::u16string_view prefix,std::u16string_view relative){
    if(!pool||!lease)throw std::invalid_argument("missing archive projection");
    auto parts=components(relative);
    auto path=std::u16string(prefix);
    auto tree=lease->tree();
    auto node=tree->root();
    unsigned depth=0;
    for(std::size_t i=0;i<parts.size();++i){
        if(node->info.kind!=FileKind::directory)return {};
        auto found=tree->children(node);
        auto it=std::find_if(found.begin(),found.end(),[&](auto const& n){return equal(n->windows_name,parts[i]);});
        if(it==found.end())return {};
        node=*it;path=child_path(path,node->windows_name);
        auto target=tree->resolve_link(node);
        if(!supported(target)){if(i+1<parts.size())return {};break;}
        auto projected_name=node->windows_name;
        node=std::move(target);
        if(node->info.kind==FileKind::regular&&(is_image_extension(projected_name)||i+1<parts.size())){
            if(depth>=32)throw std::runtime_error("nested image depth exceeds 32");
            auto source=std::make_shared<NestedSource>(tree,node);
            auto key=u"nested:"+to_utf16(std::to_string(reinterpret_cast<std::uintptr_t>(tree.get())))+u":"+tree->path_of(node);
            try{lease=pool->acquire_source(std::move(key),source,path);}
            catch(const UnknownArchiveFormat&){if(i+1<parts.size())return {};continue;}
            ++depth;tree=lease->tree();node=tree->root();
        }
    }
    auto name=path.substr(path.find_last_of(u'\\')+1);
    return archive_node(tree,node,physical,std::move(path),std::move(name),std::move(lease));
}
std::vector<ProjectedNodePtr> list_projected_archive(const ProjectedNodePtr& directory){
    auto result=archive_children(directory);
    for(auto& child:result)if(!child->info.unavailable&&!child->info.directory&&is_image_extension(child->name)){
        auto candidate=std::make_shared<ProjectedNode>(*child);
        candidate->info.directory=true;candidate->info.size=0;child=std::move(candidate);
    }
    return result;
}
void Projection::validate(const ProjectedNodePtr& node)const{if(!node)throw std::invalid_argument("missing projected node");if(node->archive)node->archive->info();if(node->source)node->source->validate();}
std::size_t Projection::read(const ProjectedNodePtr& node,std::uint64_t offset,std::span<std::byte> output)const{validate(node);if(node->info.unavailable)throw std::invalid_argument("archive object has no readable file target");if(node->info.directory)throw std::invalid_argument("not a projected file");if(node->source){if(offset>=node->info.size)return 0;auto n=static_cast<std::size_t>(std::min<std::uint64_t>(output.size(),node->info.size-offset));node->source->read_exact(offset,output.first(n));return n;}if(!node->archive||!node->entry)throw std::invalid_argument("not a projected file");return node->archive->read(node->entry,offset,output);}
std::shared_ptr<Projection> mount_projection(std::shared_ptr<ArchiveTree> tree){if(!tree)throw std::invalid_argument("missing archive");return std::make_shared<Disk>(std::move(tree));}
std::shared_ptr<Projection> unc_projection(Share share,std::shared_ptr<ImagePool> pool){return std::make_shared<Unc>(std::move(share),std::move(pool));}
bool is_image_extension(std::u16string_view name){auto ext=name.rfind(u'.');if(ext==std::u16string_view::npos)return false;auto e=name.substr(ext);return equal(e,u".squashfs")||equal(e,u".sqfs")||equal(e,u".sfs")||equal(e,u".dwarfs")||equal(e,u".erofs");}
std::optional<std::u16string> image_unc_path(const Share& share,const fs::path& image){auto prefix=u"\\\\"+share.server+u"\\"+share.name;auto path=fs::absolute(image).lexically_normal();if(share.network){auto value=path.u16string();if(!value.starts_with(u"\\\\")||projected_source(path))return {};auto parts=components(std::u16string_view(value).substr(2));if(parts.size()<3)return {};return prefix+u"\\"+join(parts,0,parts.size());}auto root=fs::absolute(share.root).lexically_normal();while(root.has_relative_path()&&!root.has_filename())root=root.parent_path();auto a=root.begin(),b=path.begin();for(;a!=root.end();++a,++b)if(b==path.end()||!equal(a->u16string(),b->u16string()))return {};if(b==path.end())return {};std::vector<std::u16string> parts;for(;b!=path.end();++b)parts.push_back(b->u16string());return prefix+u"\\"+join(parts,0,parts.size());}
}
