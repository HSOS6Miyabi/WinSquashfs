// SPDX-License-Identifier: GPL-3.0-only
#include "projection.hpp"
#include "remote_transport.hpp"
#include <map>
#include <shared_mutex>
namespace archivefs {
namespace {
using RouteMap=std::map<std::u16string,std::shared_ptr<Projection>,WindowsNameLess>;
class Namespace final:public NamespaceProjection {
public:
    void replace_routes(std::vector<std::pair<std::u16string,std::shared_ptr<Projection>>> routes)override{
        RouteMap next;
        for(auto& [name,p]:routes){if(!p||name.empty()||name.find_first_of(u"\\/")!=name.npos||!next.emplace(std::move(name),std::move(p)).second)throw std::invalid_argument("Invalid namespace route");}
        std::unique_lock lock(mutex_);routes_.swap(next);
    }
    ProjectedNodePtr open(std::u16string_view path)const override{
        if(path.empty()||path==u"\\"){auto n=std::make_shared<ProjectedNode>();n->path=u"\\";n->info={true,0,0,1,false};n->synthetic=true;return n;}
        auto [name,p,rest]=resolve(path);if(!p)return {};
        auto node=p->open(rest);return node?wrap(name,p,node):ProjectedNodePtr{};
    }
    std::vector<ProjectedNodePtr> list(const ProjectedNodePtr& node)const override{
        if(!node)throw std::invalid_argument("Missing namespace node");
        std::vector<ProjectedNodePtr> result;
        if(!node->route){
            RouteMap routes;{std::shared_lock lock(mutex_);routes=routes_;}
            for(auto& [name,p]:routes){auto n=std::make_shared<ProjectedNode>();n->name=name;n->path=u"\\"+name;n->info={true,0,0,index(name,0),false};n->synthetic=true;result.push_back(n);}
        }else{
            auto name=route_name(node->path);
            for(auto& child:node->route->list(node->routed))result.push_back(wrap(name,node->route,child));
        }
        return result;
    }
    std::uint64_t capacity()const override{return 1ULL<<40;}
    bool writable_volume()const noexcept override{return true;}
    bool writable(const ProjectedNodePtr& n)const noexcept override{return n&&n->route&&n->route->writable(n->routed);}
    void validate(const ProjectedNodePtr& n)const override{if(!n)throw std::invalid_argument("Missing namespace node");if(n->route)n->route->validate(n->routed);}
    ProjectionInfo query_info(const ProjectedNodePtr& n)const override{
        validate(n);if(!n->route)return n->info;auto result=n->route->query_info(n->routed);result.index=index(route_name(n->path),result.index);return result;
    }
    std::size_t read(const ProjectedNodePtr& n,std::uint64_t offset,std::span<std::byte> bytes)const override{require(n);return n->route->read(n->routed,offset,bytes);}
    ProjectedNodePtr create_file(std::u16string_view path,bool dir)override{
        auto [name,p,rest]=resolve(path);if(!p||!p->writable_volume()||rest==u"\\")throw RemoteFailure(RemoteError::permission,"Namespace path is read-only");
        return wrap(name,p,p->create_file(rest,dir));
    }
    std::size_t write(const ProjectedNodePtr& n,std::uint64_t offset,std::span<const std::byte> bytes,bool append,bool constrained)override{require(n);return n->route->write(n->routed,offset,bytes,append,constrained);}
    void resize(const ProjectedNodePtr& n,std::uint64_t size)override{require(n);n->route->resize(n->routed,size);}
    void flush(const ProjectedNodePtr& n)override{
        if(n){if(n->route)n->route->flush(n->routed);return;}
        RouteMap routes;{std::shared_lock lock(mutex_);routes=routes_;}
        for(auto& [name,p]:routes){(void)name;p->flush({});}
    }
    void check_delete(const ProjectedNodePtr& n)override{require(n);n->route->check_delete(n->routed);}
    void remove(const ProjectedNodePtr& n)override{require(n);n->route->remove(n->routed);}
    void rename(const ProjectedNodePtr& n,std::u16string_view path,bool replace)override{
        require(n);auto [name,p,rest]=resolve(path);(void)name;
        if(p!=n->route||rest==u"\\")throw RemoteFailure(RemoteError::unsupported,"Cannot rename across namespace shares");
        p->rename(n->routed,rest,replace);
    }
    void recover(const ProjectedNodePtr& n,bool discard)override{require(n);n->route->recover(n->routed,discard);}
    bool server_copy(const ProjectedNodePtr& n,std::u16string_view path,bool replace)override{
        require(n);auto [name,p,rest]=resolve(path);(void)name;
        return p==n->route&&rest!=u"\\"&&p->server_copy(n->routed,rest,replace);
    }
private:
    mutable std::shared_mutex mutex_;RouteMap routes_;
    struct Resolved{std::u16string name;std::shared_ptr<Projection> projection;std::u16string rest;};
    static std::u16string route_name(std::u16string_view path){if(path.starts_with(u"\\"))path.remove_prefix(1);return std::u16string(path.substr(0,path.find(u'\\')));}
    Resolved resolve(std::u16string_view path)const{
        if(path.starts_with(u"\\"))path.remove_prefix(1);
        auto end=path.find(u'\\');auto name=path.substr(0,end);
        std::shared_lock lock(mutex_);auto i=routes_.find(name);if(i==routes_.end())return {};
        return {i->first,i->second,end==path.npos?u"\\":std::u16string(path.substr(end))};
    }
    static std::uint64_t index(std::u16string_view name,std::uint64_t i){std::uint64_t value=14695981039346656037ULL;for(auto c:name){value^=c;value*=1099511628211ULL;}return (value^i)|2;}
    static void require(const ProjectedNodePtr& n){if(!n||!n->route)throw RemoteFailure(RemoteError::permission,"Namespace root is read-only");}
    static ProjectedNodePtr wrap(const std::u16string& name,const std::shared_ptr<Projection>& p,const ProjectedNodePtr& n){
        if(!n)return {};
        auto value=std::make_shared<ProjectedNode>(*n);value->path=u"\\"+name+(n->path==u"\\"?std::u16string{}:n->path);
        if(n->path==u"\\")value->name=name;
        value->info.index=index(name,value->info.index);value->route=p;value->routed=n;return value;
    }
};
}
std::shared_ptr<NamespaceProjection> namespace_projection(){return std::make_shared<Namespace>();}
}
