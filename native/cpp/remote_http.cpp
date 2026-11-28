// SPDX-License-Identifier: GPL-3.0-only
#include "remote_transport.hpp"
#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <utility>

namespace archivefs {
namespace {
[[noreturn]] void failure(RemoteError c,const char* m){throw RemoteFailure(c,m);}
std::string lower(std::string s){for(auto& c:s)if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');return s;}
std::string trim(std::string_view s){
    while(!s.empty()&&(s.front()==' '||s.front()=='\t'||s.front()=='\r'||s.front()=='\n'))s.remove_prefix(1);
    while(!s.empty()&&(s.back()==' '||s.back()=='\t'||s.back()=='\r'||s.back()=='\n'))s.remove_suffix(1);
    return std::string(s);
}
std::uint64_t number(std::string_view s){
    std::uint64_t n=0;auto r=std::from_chars(s.data(),s.data()+s.size(),n);
    if(r.ec!=std::errc{}||r.ptr!=s.data()+s.size())failure(RemoteError::protocol,"Invalid remote file size");
    return n;
}
std::int64_t date(std::string_view s){
    auto t=curl_getdate(std::string(s).c_str(),nullptr);return t<0?0:static_cast<std::int64_t>(t);
}
bool token(std::string_view value,std::string_view wanted){
    while(!value.empty()){
        auto i=value.find(',');if(lower(trim(value.substr(0,i)))==wanted)return true;
        if(i==value.npos)break;
        value.remove_prefix(i+1);
    }
    return false;
}
bool strong_etag(std::string_view s){return s.size()>=2&&s.front()=='"'&&s.back()=='"'&&s.find_first_of("\r\n")==s.npos;}
struct Response {
    long status=0;
    CURLcode code=CURLE_OK;
    std::map<std::string,std::string,std::less<>> headers;
    std::string body;
    std::string header(std::string_view key)const{auto it=headers.find(key);return it==headers.end()?std::string{}:it->second;}
};
struct Request {
    std::string method,url;
    std::vector<std::string> headers,quote;
    RemoteInput input;
    std::uint64_t length=0;
    RemoteOutput output;
    std::size_t body_limit=16u<<20;
    bool head=false,upload=false,ftp=false,range_abort=false;
    std::string range;
    std::uint64_t skip_on_full=0;
    std::span<std::byte> exact;
};
struct Transfer {
    Request& request;
    Response response;
    std::uint64_t sent=0,skipped=0;
    std::size_t received=0;
    bool done=false;
    std::exception_ptr error;
    static size_t header(char* ptr,size_t s,size_t n,void* opaque)noexcept{
        auto& t=*static_cast<Transfer*>(opaque);auto bytes=s*n;
        try{
            std::string_view line(ptr,bytes);
            if(line.starts_with("HTTP/")){
                auto pos=line.find(' ');
                if(pos!=line.npos){long status=0;std::from_chars(line.data()+pos+1,line.data()+line.size(),status);t.response.status=status;}
                t.response.headers.clear();t.response.body.clear();t.skipped=0;t.received=0;
            }else if(auto colon=line.find(':');colon!=line.npos){
                auto name=lower(trim(line.substr(0,colon))),value=trim(line.substr(colon+1));
                if(value.size()>65536||name.size()>256||t.response.headers.size()>1024)
                    failure(RemoteError::protocol,"Remote response headers exceed limit");
                auto [it,inserted]=t.response.headers.try_emplace(name,value);
                if(!inserted){it->second+=",";it->second+=value;}
            }
            return bytes;
        }catch(...){t.error=std::current_exception();return 0;}
    }
    static size_t write(char* ptr,size_t s,size_t n,void* opaque)noexcept{
        auto& t=*static_cast<Transfer*>(opaque);auto count=s*n;
        try{
            auto data=std::as_bytes(std::span(ptr,count));
            if(!t.request.exact.empty()&&(t.request.ftp||t.response.status==200||t.response.status==206)){
                auto skip=t.request.ftp||t.response.status==200?t.request.skip_on_full:0;
                if(t.skipped<skip){
                    auto take=static_cast<std::size_t>(std::min<std::uint64_t>(data.size(),skip-t.skipped));
                    t.skipped+=take;data=data.subspan(take);
                }
                auto take=std::min(data.size(),t.request.exact.size()-t.received);
                std::copy_n(data.data(),take,t.request.exact.data()+t.received);
                t.received+=take;
                if(t.received==t.request.exact.size()&&(take<data.size()||t.request.range_abort)){
                    t.done=true;return 0;
                }
                return count;
            }
            if(t.request.output&&(t.request.ftp||(t.response.status>=200&&t.response.status<300))){
                t.request.output(data);return count;
            }
            if(count>t.request.body_limit-t.response.body.size())
                failure(RemoteError::protocol,"Remote response exceeds limit");
            t.response.body.append(ptr,count);return count;
        }catch(...){t.error=std::current_exception();return 0;}
    }
    static size_t read(char* ptr,size_t s,size_t n,void* opaque)noexcept{
        auto& t=*static_cast<Transfer*>(opaque);
        try{
            auto amount=static_cast<std::size_t>(std::min<std::uint64_t>(s*n,t.request.length-t.sent));
            if(!amount)return 0;
            auto got=t.request.input(t.sent,std::as_writable_bytes(std::span(ptr,amount)));
            if(!got||got>amount)failure(RemoteError::protocol,"Short upload source");
            t.sent+=got;return got;
        }catch(...){t.error=std::current_exception();return CURL_READFUNC_ABORT;}
    }
    static int seek(void* opaque,curl_off_t offset,int origin)noexcept{
        auto& t=*static_cast<Transfer*>(opaque);
        if(origin!=SEEK_SET||offset<0||static_cast<std::uint64_t>(offset)>t.request.length)return CURL_SEEKFUNC_CANTSEEK;
        t.sent=static_cast<std::uint64_t>(offset);return CURL_SEEKFUNC_OK;
    }
};
struct List {
    curl_slist* value=nullptr;
    ~List(){curl_slist_free_all(value);}
    void add(const std::string& s){auto* next=curl_slist_append(value,s.c_str());if(!next)throw std::bad_alloc();value=next;}
};
struct Xml {
    xmlDocPtr value=nullptr;
    explicit Xml(const std::string& data){
        if(data.size()>static_cast<std::size_t>(INT_MAX))failure(RemoteError::protocol,"XML response too large");
        value=xmlReadMemory(data.data(),static_cast<int>(data.size()),nullptr,nullptr,XML_PARSE_NONET|XML_PARSE_NOERROR|XML_PARSE_NOWARNING);
        if(!value)failure(RemoteError::protocol,"Invalid XML response");
        if(value->intSubset||value->extSubset){xmlFreeDoc(value);value=nullptr;failure(RemoteError::protocol,"XML document types are forbidden");}
    }
    ~Xml(){if(value)xmlFreeDoc(value);}
    xmlNodePtr root()const{return xmlDocGetRootElement(value);}
};
bool named(xmlNodePtr node,const char* name){return node&&node->type==XML_ELEMENT_NODE&&xmlStrEqual(node->name,BAD_CAST name);}
xmlNodePtr child(xmlNodePtr node,const char* name){
    for(auto p=node?node->children:nullptr;p;p=p->next)if(named(p,name))return p;
    return nullptr;
}
std::string content(xmlNodePtr node){
    if(!node)return {};
    auto* raw=xmlNodeGetContent(node);if(!raw)return {};
    struct Free{xmlChar* p;~Free(){xmlFree(p);}} free{raw};return reinterpret_cast<const char*>(raw);
}
std::string field(xmlNodePtr node,const char* name){return content(child(node,name));}
std::string decode_url(std::string_view input){
    auto hex=[](char c)->int{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};
    std::string out;
    for(std::size_t i=0;i<input.size();++i){
        if(input[i]=='%'){
            if(i+2>=input.size()||hex(input[i+1])<0||hex(input[i+2])<0)failure(RemoteError::protocol,"Invalid percent-encoded remote name");
            out+=static_cast<char>(hex(input[i+1])*16+hex(input[i+2]));i+=2;
        }else out+=input[i];
    }
    return out;
}
std::string url_path(std::string_view url){
    auto scheme=url.find("://");
    if(scheme!=url.npos){auto pos=url.find('/',scheme+3);url=pos==url.npos?std::string_view("/"):url.substr(pos);}
    return decode_url(url);
}
std::string strip_slash(std::string s){while(s.size()>1&&s.back()=='/')s.pop_back();return s;}
std::string object_path(std::string_view root,std::string_view path){
    validate_remote_path(path);
    if(root=="/")return std::string(path);
    return path=="/"?std::string(root):std::string(root)+std::string(path);
}
class Transport final:public RemoteTransport {
public:
    explicit Transport(RemoteConnection c):c_(std::move(c)){
        static const struct Global{Global(){if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK)throw std::runtime_error("curl initialization failed");}~Global(){curl_global_cleanup();}} global;
        (void)global;
        base_=strip_slash(c_.endpoint);
        if(is_ftp()){
            if(c_.protocol==RemoteProtocol::ftps&&c_.implicit_tls&&base_.starts_with("ftp://"))base_.replace(0,3,"ftps");
        }
        if(is_s3())base_+="/"+c_.bucket;
        easy_=curl_easy_init();if(!easy_)throw std::bad_alloc();
    }
    ~Transport()override{curl_easy_cleanup(easy_);}
    RemoteStat stat(std::string_view path)override{
        validate_remote_path(path);
        if(path=="/")return {"/",true,0,0,{}};
        if(is_dav()){
            auto result=dav_list(path,0);
            if(result.size()!=1)failure(RemoteError::protocol,"Invalid WebDAV stat response");
            return result.front();
        }
        if(is_s3()){
            auto response=request({"HEAD",url(path)},true);
            if(response.status==404){
                auto entries=s3_list(path,true);
                if(entries.empty())failure(RemoteError::missing,"S3 object not found");
                return {remote_name(path),true,0,0,{}};
            }
            success(response);
            auto length=response.header("content-length");
            return {remote_name(path),false,number(length),date(response.header("last-modified")),response.header("etag")};
        }
        for(auto& entry:ftp_list(remote_parent(path)))if(entry.name==remote_name(path))return entry;
        failure(RemoteError::missing,"FTP entry not found");
    }
    std::vector<RemoteStat> list(std::string_view path)override{
        validate_remote_path(path);
        if(is_dav())return dav_list(path,1);
        if(is_s3())return s3_list(path,false);
        return ftp_list(path);
    }
    void read(std::string_view path,std::uint64_t offset,std::span<std::byte> output,std::string_view revision)override{
        if(output.empty())return;
        if(output.size()-1>UINT64_MAX-offset)throw std::out_of_range("Remote read offset overflow");
        Request req{"GET",url(path)};
        req.exact=output;req.range_abort=true;
        if(is_ftp()){
            req.ftp=true;req.method.clear();
            req.range=std::to_string(offset)+"-"+std::to_string(offset+output.size()-1);
            auto response=request(req);
            if(response.code==CURLE_BAD_DOWNLOAD_RESUME||response.code==CURLE_RANGE_ERROR){
                req.range.clear();req.skip_on_full=offset;response=request(req);
            }
            success(response);return;
        }
        req.range=std::to_string(offset)+"-"+std::to_string(offset+output.size()-1);
        req.skip_on_full=offset;
        if(strong_etag(revision))req.headers.push_back("If-Match: "+std::string(revision));
        auto response=request(req);
        success(response);
        if(response.status!=200&&response.status!=206)failure(RemoteError::protocol,"Unexpected HTTP range response");
        if(response.status==206){
            const auto expected="bytes "+req.range+"/";
            if(!response.header("content-range").starts_with(expected))
                failure(RemoteError::protocol,"HTTP server returned a different byte range");
        }
    }
    bool write_at(std::string_view path,std::uint64_t offset,std::span<const std::byte> bytes,std::string_view revision)override{
        writable();
        if(!is_dav())return false;
        if(bytes.empty())return true;
        if(bytes.size()-1>UINT64_MAX-offset)throw std::out_of_range("Remote write offset overflow");
        auto options=request({"OPTIONS",url(path)});success(options);
        bool patch=token(options.header("dav"),"sabredav-partialupdate")||
            token(options.header("accept-patch"),"application/x-sabredav-partialupdate");
        if(!patch&&!c_.partial_put)return false;
        Request req{patch?"PATCH":"PUT",url(path)};
        auto range=std::to_string(offset)+"-"+std::to_string(offset+bytes.size()-1);
        req.headers.push_back(patch?"Content-Type: application/x-sabredav-partialupdate":"Content-Type: application/octet-stream");
        req.headers.push_back(patch?"X-Update-Range: bytes="+range:"Content-Range: bytes "+range+"/*");
        if(strong_etag(revision))req.headers.push_back("If-Match: "+std::string(revision));
        req.upload=true;req.length=bytes.size();
        req.input=[bytes](std::uint64_t pos,std::span<std::byte> out){auto n=std::min(out.size(),bytes.size()-static_cast<std::size_t>(pos));std::copy_n(bytes.data()+static_cast<std::size_t>(pos),n,out.data());return n;};
        auto response=request(req);
        if(response.code==CURLE_OK&&(response.status==405||response.status==415||response.status==501))return false;
        success(response);
        if(response.status!=200&&response.status!=204)failure(RemoteError::protocol,"Unexpected partial update response");
        return true;
    }
    void replace(std::string_view path,std::uint64_t size,const RemoteInput& input,std::string_view revision,bool exclusive)override{
        writable();if(path=="/")failure(RemoteError::permission,"Cannot replace connection root");
        if(is_ftp()&&exclusive){
            try{(void)stat(path);failure(RemoteError::exists,"Remote file already exists");}
            catch(const RemoteFailure& e){if(e.code()!=RemoteError::missing)throw;}
        }
        if(is_ftp()&&!revision.empty()&&stat(path).revision!=revision)failure(RemoteError::conflict,"FTP file changed");
        Request req{is_ftp()?"":"PUT",url(path)};
        req.ftp=is_ftp();req.upload=true;req.length=size;req.input=input;
        if(!is_ftp()){
            req.headers.emplace_back("Content-Type: application/octet-stream");
            if(exclusive)req.headers.emplace_back("If-None-Match: *");
            else if(strong_etag(revision))req.headers.push_back("If-Match: "+std::string(revision));
        }
        auto response=request(req);success(response);
    }
    void mkdir(std::string_view path)override{
        writable();if(path=="/")failure(RemoteError::exists,"Connection root already exists");
        if(is_dav()){success(request({"MKCOL",url(path)}));return;}
        if(is_ftp()){ftp_command({"MKD "+physical(path)});return;}
        Request req{"PUT",url(path)+"/"};req.upload=true;req.headers={"If-None-Match: *"};success(request(req));
    }
    void remove(std::string_view path,bool directory)override{
        writable();if(path=="/")failure(RemoteError::permission,"Cannot remove connection root");
        if(directory&&!list(path).empty())failure(RemoteError::not_empty,"Remote directory is not empty");
        if(is_ftp()){ftp_command({std::string(directory?"RMD ":"DELE ")+physical(path)});return;}
        success(request({"DELETE",url(path)+(is_s3()&&directory?"/":"")}));
    }
    void rename(std::string_view from,std::string_view to,bool overwrite)override{
        writable();if(from=="/"||to=="/")failure(RemoteError::permission,"Cannot rename connection root");
        if(is_dav()){
            Request req{"MOVE",url(from)};req.headers={"Destination: "+url(to),std::string("Overwrite: ")+(overwrite?"T":"F")};success(request(req));return;
        }
        if(is_ftp()){
            if(!overwrite){try{(void)stat(to);failure(RemoteError::exists,"Destination exists");}catch(const RemoteFailure& e){if(e.code()!=RemoteError::missing)throw;}}
            ftp_command({"RNFR "+physical(from),"RNTO "+physical(to)});return;
        }
        if(stat(from).directory)failure(RemoteError::unsupported,"S3 directory rename requires recursive object migration");
        if(!copy(from,to,overwrite))failure(RemoteError::unsupported,"Remote copy unsupported");
        remove(from,false);
    }
    bool copy(std::string_view from,std::string_view to,bool overwrite)override{
        writable();
        if(is_dav()){
            Request req{"COPY",url(from)};req.headers={"Destination: "+url(to),std::string("Overwrite: ")+(overwrite?"T":"F"),"Depth: infinity"};success(request(req));return true;
        }
        if(!is_s3())return false;
        if(stat(from).directory)return false;
        Request req{"PUT",url(to)};
        req.headers={"x-amz-copy-source: /"+c_.bucket+remote_url_path(physical(from))};
        if(!overwrite)req.headers.push_back("If-None-Match: *");
        auto response=request(req);success(response);
        Xml xml(response.body);if(!named(xml.root(),"CopyObjectResult"))failure(RemoteError::protocol,"S3 CopyObject failed");
        return true;
    }
private:
    RemoteConnection c_;
    std::string base_;
    CURL* easy_=nullptr;
    std::mutex mutex_;
    bool is_dav()const{return c_.protocol==RemoteProtocol::dav;}
    bool is_s3()const{return c_.protocol==RemoteProtocol::s3;}
    bool is_ftp()const{return !is_dav()&&!is_s3();}
    void writable()const{if(c_.read_only)failure(RemoteError::permission,"Remote connection is read-only");}
    std::string physical(std::string_view path)const{return object_path(c_.root,path);}
    std::string url(std::string_view path)const{
        auto p=physical(path);
        if(is_ftp()){
            // Double slash selects an absolute FTP path rather than a path
            // relative to the login directory.
            return base_+"/"+remote_url_path(p);
        }
        return base_+remote_url_path(p);
    }
    template<class T>void set(CURLoption option,T value){if(curl_easy_setopt(easy_,option,value)!=CURLE_OK)failure(RemoteError::protocol,"Cannot configure remote request");}
    Response request(Request req,bool head=false){
        std::lock_guard lock(mutex_);
        curl_easy_reset(easy_);
        Transfer t{req,{},0,0,0,false,{}};
        List headers,quote;
        set(CURLOPT_URL,req.url.c_str());set(CURLOPT_NOSIGNAL,1L);
        set(CURLOPT_PROTOCOLS_STR,is_ftp()?"ftp,ftps":"http,https");
        set(CURLOPT_FOLLOWLOCATION,0L);set(CURLOPT_CONNECTTIMEOUT,static_cast<long>(std::min(c_.timeout_seconds,30u)));
        set(CURLOPT_TIMEOUT,static_cast<long>(c_.timeout_seconds));
        set(CURLOPT_SSL_VERIFYPEER,1L);set(CURLOPT_SSL_VERIFYHOST,2L);
        set(CURLOPT_HEADERFUNCTION,&Transfer::header);set(CURLOPT_HEADERDATA,&t);
        set(CURLOPT_WRITEFUNCTION,&Transfer::write);set(CURLOPT_WRITEDATA,&t);
        set(CURLOPT_USERNAME,c_.username.c_str());set(CURLOPT_PASSWORD,c_.secret.c_str());
        if(is_s3()){
            auto signature="aws:amz:"+c_.region+":s3";
            set(CURLOPT_AWS_SIGV4,signature.c_str());
            if(!c_.session_token.empty())headers.add("x-amz-security-token: "+c_.session_token);
            if(req.upload)headers.add("x-amz-content-sha256: UNSIGNED-PAYLOAD");
        }else if(!is_ftp())set(CURLOPT_HTTPAUTH,static_cast<long>(CURLAUTH_BASIC));
        if(c_.protocol==RemoteProtocol::ftps)set(CURLOPT_USE_SSL,static_cast<long>(CURLUSESSL_ALL));
        if(!req.method.empty())set(CURLOPT_CUSTOMREQUEST,req.method.c_str());
        if(head||req.head)set(CURLOPT_NOBODY,1L);
        if(!req.range.empty())set(CURLOPT_RANGE,req.range.c_str());
        if(req.upload){
            if(req.length>static_cast<std::uint64_t>(std::numeric_limits<curl_off_t>::max()))failure(RemoteError::unsupported,"Upload exceeds signed 64-bit size");
            set(CURLOPT_UPLOAD,1L);set(CURLOPT_INFILESIZE_LARGE,static_cast<curl_off_t>(req.length));
            set(CURLOPT_READFUNCTION,&Transfer::read);set(CURLOPT_READDATA,&t);
            set(CURLOPT_SEEKFUNCTION,&Transfer::seek);set(CURLOPT_SEEKDATA,&t);
            headers.add("Expect:");
        }
        for(auto& h:req.headers)headers.add(h);
        for(auto& q:req.quote)quote.add(q);
        if(headers.value)set(CURLOPT_HTTPHEADER,headers.value);
        if(quote.value)set(CURLOPT_QUOTE,quote.value);
        auto rc=curl_easy_perform(easy_);
        if(t.error)std::rethrow_exception(t.error);
        curl_easy_getinfo(easy_,CURLINFO_RESPONSE_CODE,&t.response.status);
        if(rc==CURLE_WRITE_ERROR&&t.done)rc=CURLE_OK;
        t.response.code=rc;
        if(rc==CURLE_OK&&!req.exact.empty()&&t.received!=req.exact.size()&&
            (t.response.status==200||t.response.status==206||is_ftp()))
            failure(RemoteError::protocol,"Short remote file read");
        return std::move(t.response);
    }
    static void success(const Response& response){
        if(response.code!=CURLE_OK){
            switch(response.code){
            case CURLE_OPERATION_TIMEDOUT:failure(RemoteError::timeout,"Remote request timed out");
            case CURLE_REMOTE_FILE_NOT_FOUND:failure(RemoteError::missing,"Remote file not found");
            case CURLE_REMOTE_ACCESS_DENIED:case CURLE_LOGIN_DENIED:failure(RemoteError::permission,"Remote access denied");
            case CURLE_COULDNT_CONNECT:case CURLE_COULDNT_RESOLVE_HOST:failure(RemoteError::offline,"Cannot connect to remote host");
            default:throw RemoteFailure(RemoteError::protocol,std::string("Remote request failed: ")+curl_easy_strerror(response.code));
            }
        }
        auto s=response.status;
        if(s>=200&&s<300)return;
        if(s==401||s==403||s==530)failure(RemoteError::permission,"Remote access denied");
        if(s==404||s==410||s==550)failure(RemoteError::missing,"Remote path not found");
        if(s==409||s==412||s==423)failure(RemoteError::conflict,"Remote resource changed or is locked");
        if(s==405||s==501)failure(RemoteError::unsupported,"Remote operation unsupported");
        if(s==507)failure(RemoteError::no_space,"Remote storage is full");
        throw RemoteFailure(RemoteError::protocol,"Remote server returned status "+std::to_string(s));
    }
    std::vector<RemoteStat> dav_list(std::string_view path,unsigned depth){
        static const std::string body="<?xml version=\"1.0\"?><d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/><d:getcontentlength/><d:getlastmodified/><d:getetag/></d:prop></d:propfind>";
        Request req{"PROPFIND",url(path)};
        if(depth&&req.url.back()!='/')req.url+='/';
        req.headers={"Depth: "+std::to_string(depth),"Content-Type: application/xml; charset=utf-8"};
        req.upload=true;req.length=body.size();
        req.input=[](std::uint64_t pos,std::span<std::byte> out){auto n=std::min(out.size(),body.size()-static_cast<std::size_t>(pos));std::memcpy(out.data(),body.data()+static_cast<std::size_t>(pos),n);return n;};
        auto response=request(req);success(response);
        if(response.status!=207)failure(RemoteError::protocol,"Expected WebDAV multistatus");
        Xml xml(response.body);
        if(!named(xml.root(),"multistatus"))failure(RemoteError::protocol,"Invalid WebDAV multistatus");
        auto base_path=strip_slash(url_path(url(path)));
        std::vector<RemoteStat> entries;
        for(auto r=xml.root()->children;r;r=r->next){
            if(!named(r,"response"))continue;
            auto href=strip_slash(url_path(field(r,"href")));
            std::string name;
            if(!depth){if(href!=base_path)continue;name=remote_name(path);}
            else{
                if(href==base_path)continue;
                auto prefix=base_path=="/"?"/":base_path+"/";
                if(!href.starts_with(prefix))continue;
                name=href.substr(prefix.size());
                if(name.empty()||name.find('/')!=name.npos)continue;
                try{(void)remote_join("/",name);}catch(const std::invalid_argument&){continue;}
            }
            RemoteStat entry{name,false,0,0,{}};
            bool found=false;
            for(auto p=r->children;p;p=p->next){
                if(!named(p,"propstat")||field(p,"status").find(" 200 ")==std::string::npos)continue;
                auto prop=child(p,"prop");if(!prop)continue;found=true;
                if(child(child(prop,"resourcetype"),"collection"))entry.directory=true;
                auto length=field(prop,"getcontentlength");if(!length.empty())entry.size=number(length);
                auto modified=field(prop,"getlastmodified");if(!modified.empty())entry.modified=date(modified);
                auto etag=field(prop,"getetag");if(!etag.empty())entry.revision=etag;
            }
            if(found){if(entry.directory)entry.size=0;entries.push_back(std::move(entry));}
            if(entries.size()>500000)failure(RemoteError::protocol,"Remote directory exceeds entry limit");
        }
        return entries;
    }
    std::vector<RemoteStat> s3_list(std::string_view path,bool probe){
        auto prefix=physical(path);if(prefix.front()=='/')prefix.erase(0,1);
        if(!prefix.empty()&&prefix.back()!='/')prefix+='/';
        std::vector<RemoteStat> result;
        std::string continuation;
        for(unsigned page=0;page<10000;++page){
            auto request_url=base_+"/?list-type=2&delimiter=%2F&prefix="+remote_url_path(prefix);
            if(probe)request_url+="&max-keys=1";
            if(!continuation.empty()){
                auto encoded=remote_url_path(continuation);
                // A continuation token is a query component, not a path.
                for(std::size_t i=0;(i=encoded.find('/',i))!=encoded.npos;i+=3)encoded.replace(i,1,"%2F");
                request_url+="&continuation-token="+encoded;
            }
            auto response=request({"GET",request_url});success(response);
            Xml xml(response.body);
            if(!named(xml.root(),"ListBucketResult"))failure(RemoteError::protocol,"Invalid S3 listing");
            for(auto node=xml.root()->children;node;node=node->next){
                bool directory=named(node,"CommonPrefixes");
                if(!directory&&!named(node,"Contents"))continue;
                auto key=field(node,directory?"Prefix":"Key");
                if(!key.starts_with(prefix))continue;
                auto name=key.substr(prefix.size());
                if(directory&&!name.empty()&&name.back()=='/')name.pop_back();
                if(name.empty()){
                    if(probe)return {{"/",true,0,0,{}}};
                    continue;
                }
                if(name.find('/')!=name.npos)continue;
                try{(void)remote_join("/",name);}catch(const std::invalid_argument&){continue;}
                result.push_back({std::move(name),directory,directory?0:number(field(node,"Size")),date(field(node,"LastModified")),field(node,"ETag")});
                if(probe)return result;
                if(result.size()>500000)failure(RemoteError::protocol,"S3 directory exceeds entry limit");
            }
            if(lower(field(xml.root(),"IsTruncated"))!="true")return result;
            auto next=field(xml.root(),"NextContinuationToken");
            if(next.empty()||next==continuation)failure(RemoteError::protocol,"S3 listing did not advance");
            continuation=std::move(next);
        }
        failure(RemoteError::protocol,"S3 pagination exceeds limit");
    }
    void ftp_command(std::vector<std::string> commands){
        Request req{"",url("/")};req.ftp=true;req.head=true;req.quote=std::move(commands);
        success(request(req));
    }
    std::vector<RemoteStat> ftp_list(std::string_view path){
        Request req{"MLSD",url(path)};if(req.url.back()!='/')req.url+='/';req.ftp=true;
        auto response=request(req);success(response);
        std::istringstream input(response.body);std::string line;
        std::vector<RemoteStat> result;
        while(std::getline(input,line)){
            if(!line.empty()&&line.back()=='\r')line.pop_back();
            auto space=line.find(' ');if(space==line.npos)continue;
            auto name=line.substr(space+1);
            if(name.empty()||name=="."||name=="..")continue;
            try{(void)remote_join("/",name);}catch(const std::invalid_argument&){continue;}
            std::map<std::string,std::string,std::less<>> facts;
            std::string_view fields(line.data(),space);
            while(!fields.empty()){
                auto end=fields.find(';');auto item=fields.substr(0,end);auto eq=item.find('=');
                if(eq!=item.npos)facts.emplace(lower(std::string(item.substr(0,eq))),std::string(item.substr(eq+1)));
                if(end==fields.npos)break;
                fields.remove_prefix(end+1);
            }
            auto type=lower(facts["type"]);
            if(type!="file"&&type!="dir")continue;
            auto size=type=="dir"?0:number(facts["size"]);
            result.push_back({std::move(name),type=="dir",size,0,facts["modify"]+":"+std::to_string(size)+":"+facts["unique"]});
            if(result.size()>500000)failure(RemoteError::protocol,"FTP directory exceeds entry limit");
        }
        return result;
    }
};
}
std::shared_ptr<RemoteTransport> make_http_ftp_transport(RemoteConnection connection){
    validate_remote_connection(connection);
    if(connection.protocol==RemoteProtocol::sftp||connection.protocol==RemoteProtocol::scp)throw std::invalid_argument("Not an HTTP or FTP connection");
    return std::make_shared<Transport>(std::move(connection));
}
}
