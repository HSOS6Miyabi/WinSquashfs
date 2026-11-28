// SPDX-License-Identifier: GPL-3.0-only
#include "device_provider.hpp"
#include "archive_tree.hpp"
#include "windows_services.hpp"
#include <windows.h>
#include <portabledeviceapi.h>
#include <portabledevicetypes.h>
#include <portabledevice.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <nlohmann/json.hpp>
#include <fcntl.h>
#include <io.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <oleauto.h>
#include <mutex>
#include <thread>
#include <stop_token>
namespace archivefs {
namespace {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr std::size_t max_message=32u<<20;
constexpr std::size_t max_read=8u<<20;
std::wstring wide(std::string_view utf8){auto s=to_utf16(utf8);return {s.begin(),s.end()};}
std::string utf8(std::wstring_view value){return to_utf8(std::u16string(value.begin(),value.end()));}
[[noreturn]] void failure(DeviceError kind,std::string message){throw DeviceFailure(kind,std::move(message));}
void check(HRESULT hr,const char* operation){
    if(SUCCEEDED(hr))return;
    DeviceError code=hr==E_ACCESSDENIED?DeviceError::permission:
        hr==HRESULT_FROM_WIN32(ERROR_NOT_FOUND)||hr==HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)?DeviceError::missing:
        hr==E_NOTIMPL||hr==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)?DeviceError::unsupported:
        DeviceError::offline;
    failure(code,std::string(operation)+" (HRESULT "+std::to_string(static_cast<unsigned long>(hr))+")");
}
struct Handle {
    HANDLE h=nullptr;
    explicit Handle(HANDLE value=nullptr):h(value){}
    ~Handle(){reset();}
    Handle(Handle const&)=delete;
    Handle& operator=(Handle const&)=delete;
    Handle(Handle&& other)noexcept:h(std::exchange(other.h,nullptr)){}
    Handle& operator=(Handle&& other)noexcept{if(this!=&other){reset();h=std::exchange(other.h,nullptr);}return *this;}
    void reset(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);h=nullptr;}
    explicit operator bool()const noexcept{return h&&h!=INVALID_HANDLE_VALUE;}
};
struct CoMemory {PWSTR value=nullptr;~CoMemory(){CoTaskMemFree(value);}};
template<class T>ComPtr<T> create(REFCLSID clsid){
    ComPtr<T> result;check(CoCreateInstance(clsid,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(result.GetAddressOf())),"Create portable device interface");return result;
}
std::string string_value(IPortableDeviceValues* values,REFPROPERTYKEY key){
    CoMemory result;if(FAILED(values->GetStringValue(key,&result.value))||!result.value)return {};
    return utf8(result.value);
}
Json to_json(RemoteObject const& o){return Json{{"id",o.id},{"name",o.name},{"directory",o.directory},{"size",o.size},{"modified",o.modified},{"revision",o.revision}};}
RemoteObject object_from(Json const& value){return {value.at("id").get<std::string>(),value.at("name").get<std::string>(),value.at("directory").get<bool>(),value.at("size").get<std::uint64_t>(),value.at("modified").get<std::int64_t>(),value.at("revision").get<std::string>()};}
std::vector<RemoteDevice> attached(){
    auto manager=create<IPortableDeviceManager>(CLSID_PortableDeviceManager);
    check(manager->RefreshDeviceList(),"Refresh portable devices");
    DWORD count=0;check(manager->GetDevices(nullptr,&count),"Count portable devices");
    if(count>4096)failure(DeviceError::protocol,"Too many portable devices");
    std::vector<PWSTR> ids(count,nullptr);
    struct Free{std::vector<PWSTR>& ids;~Free(){for(auto id:ids)CoTaskMemFree(id);}}free{ids};
    if(count){auto capacity=count;auto hr=manager->GetDevices(ids.data(),&count);check(hr,"Enumerate portable devices");count=std::min(count,capacity);}
    std::vector<RemoteDevice> result;
    for(DWORD i=0;i<count;++i){
        if(!ids[i])continue;
        DWORD n=0;std::string name;
        manager->GetDeviceFriendlyName(ids[i],nullptr,&n);
        if(n>0&&n<32768){
            std::wstring label(n,L'\0');
            if(SUCCEEDED(manager->GetDeviceFriendlyName(ids[i],label.data(),&n)))name=utf8(std::wstring_view(label.c_str()));
        }
        auto id=utf8(ids[i]);if(name.empty())name=id;
        result.push_back({std::move(id),std::move(name),"online"});
    }
    return result;
}
ComPtr<IPortableDeviceKeyCollection> keys(){
    auto result=create<IPortableDeviceKeyCollection>(CLSID_PortableDeviceKeyCollection);
    const PROPERTYKEY list[]={WPD_OBJECT_ID,WPD_OBJECT_NAME,WPD_OBJECT_ORIGINAL_FILE_NAME,WPD_OBJECT_CONTENT_TYPE,
        WPD_OBJECT_SIZE,WPD_OBJECT_DATE_MODIFIED,WPD_OBJECT_PERSISTENT_UNIQUE_ID};
    for(auto& key:list)check(result->Add(key),"Add portable device property");
    return result;
}
RemoteObject parse(IPortableDeviceValues* v){
    auto id=string_value(v,WPD_OBJECT_ID);if(id.empty())failure(DeviceError::protocol,"Missing portable device object identifier");
    auto name=string_value(v,WPD_OBJECT_ORIGINAL_FILE_NAME);if(name.empty())name=string_value(v,WPD_OBJECT_NAME);if(name.empty())name=id;
    GUID content{};check(v->GetGuidValue(WPD_OBJECT_CONTENT_TYPE,&content),"Read portable device content type");
    bool directory=IsEqualGUID(content,WPD_CONTENT_TYPE_FOLDER)||IsEqualGUID(content,WPD_CONTENT_TYPE_FUNCTIONAL_OBJECT);
    ULONGLONG size=0;
    if(!directory)check(v->GetUnsignedLargeIntegerValue(WPD_OBJECT_SIZE,&size),"Read portable device object size");
    std::int64_t modified=0;PROPVARIANT date;PropVariantInit(&date);
    if(SUCCEEDED(v->GetValue(WPD_OBJECT_DATE_MODIFIED,&date))){
        FILETIME ft{};bool valid=false;
        if(date.vt==VT_DATE){SYSTEMTIME st{};valid=VariantTimeToSystemTime(date.date,&st)&&SystemTimeToFileTime(&st,&ft);}
        else if(date.vt==VT_FILETIME){ft=date.filetime;valid=true;}
        if(valid){ULARGE_INTEGER value{};value.LowPart=ft.dwLowDateTime;value.HighPart=ft.dwHighDateTime;modified=static_cast<std::int64_t>(value.QuadPart/10000000ULL)-11644473600LL;}
    }
    PropVariantClear(&date);
    auto revision=string_value(v,WPD_OBJECT_PERSISTENT_UNIQUE_ID);
    if(revision.empty())revision=id;
    return {std::move(id),std::move(name),directory,size,modified,std::move(revision)};
}
class BulkCallback final:public RuntimeClass<RuntimeClassFlags<ClassicCom>,IPortableDevicePropertiesBulkCallback> {
public:
    BulkCallback():event_(CreateEventW(nullptr,TRUE,FALSE,nullptr)){}
    HANDLE event()const{return event_.h;}
    HRESULT STDMETHODCALLTYPE OnStart(REFGUID)override{return S_OK;}
    HRESULT STDMETHODCALLTYPE OnProgress(REFGUID,IPortableDeviceValuesCollection* values)override{
        try{
            DWORD n=0;auto hr=values->GetCount(&n);if(FAILED(hr))return hr;
            std::lock_guard lock(mutex_);
            for(DWORD i=0;i<n;++i){ComPtr<IPortableDeviceValues> item;if(SUCCEEDED(values->GetAt(i,item.GetAddressOf()))){try{auto entry=parse(item.Get());items_[entry.id]=std::move(entry);}catch(DeviceFailure const&){}}}
            return S_OK;
        }catch(...){return E_FAIL;}
    }
    HRESULT STDMETHODCALLTYPE OnEnd(REFGUID,HRESULT status)override{status_.store(status);if(event_)SetEvent(event_.h);return S_OK;}
    HRESULT status()const{return status_.load();}
    std::map<std::string,RemoteObject> take(){std::lock_guard lock(mutex_);return std::move(items_);}
private:
    Handle event_;std::atomic<HRESULT> status_{E_FAIL};std::mutex mutex_;std::map<std::string,RemoteObject> items_;
};
class Session {
public:
    void connect(std::string const& device){
        if(id_==device&&device_)return;
        streams_.clear();resources_.Reset();properties_.Reset();content_.Reset();device_.Reset();id_.clear();
        auto instance=create<IPortableDevice>(CLSID_PortableDeviceFTM);
        auto client=create<IPortableDeviceValues>(CLSID_PortableDeviceValues);
        check(client->SetStringValue(WPD_CLIENT_NAME,L"WinArchiveFSPro"),"Set portable device client name");
        check(client->SetUnsignedIntegerValue(WPD_CLIENT_MAJOR_VERSION,1),"Set portable device client version");
        check(client->SetUnsignedIntegerValue(WPD_CLIENT_MINOR_VERSION,0),"Set portable device client version");
        check(client->SetUnsignedIntegerValue(WPD_CLIENT_DESIRED_ACCESS,GENERIC_READ),"Set portable device access");
        check(instance->Open(wide(device).c_str(),client.Get()),"Open portable device");
        ComPtr<IPortableDeviceContent> content;ComPtr<IPortableDeviceProperties> properties;ComPtr<IPortableDeviceResources> resources;
        check(instance->Content(content.GetAddressOf()),"Open portable device content");
        check(content->Properties(properties.GetAddressOf()),"Open portable device properties");
        check(content->Transfer(resources.GetAddressOf()),"Open portable device resources");
        device_=std::move(instance);content_=std::move(content);properties_=std::move(properties);resources_=std::move(resources);id_=device;
    }
    RemoteObject stat(std::string_view id){
        auto requested=keys();ComPtr<IPortableDeviceValues> value;
        check(properties_->GetValues(wide(id).c_str(),requested.Get(),value.GetAddressOf()),"Get portable device object properties");
        return parse(value.Get());
    }
    std::vector<RemoteObject> list(std::string_view id){
        ComPtr<IEnumPortableDeviceObjectIDs> iterator;
        check(content_->EnumObjects(0,wide(id).c_str(),nullptr,iterator.GetAddressOf()),"Enumerate portable device objects");
        std::vector<std::string> ids;
        for(;;){
            std::array<PWSTR,128> names{};DWORD count=0;auto hr=iterator->Next(static_cast<DWORD>(names.size()),names.data(),&count);
            struct Free{std::array<PWSTR,128>& values;~Free(){for(auto item:values)CoTaskMemFree(item);}}free{names};
            check(hr,"Enumerate portable device object identifiers");
            for(DWORD i=0;i<count&&i<names.size();++i)if(names[i])ids.push_back(utf8(names[i]));
            if(ids.size()>1000000)failure(DeviceError::protocol,"Portable device directory is too large");
            if(hr==S_FALSE||!count)break;
        }
        std::map<std::string,RemoteObject> bulk_result;
        ComPtr<IPortableDevicePropertiesBulk> bulk;
        if(SUCCEEDED(properties_.As(&bulk))&&!ids.empty()){
            auto collection=create<IPortableDevicePropVariantCollection>(CLSID_PortableDevicePropVariantCollection);
            for(auto& object:ids){auto value=wide(object);PROPVARIANT variant;PropVariantInit(&variant);variant.vt=VT_LPWSTR;variant.pwszVal=value.data();check(collection->Add(&variant),"Queue portable device identifier");}
            auto callback=Make<BulkCallback>();auto requested=keys();GUID context{};
            if(callback&&callback->event()&&SUCCEEDED(bulk->QueueGetValuesByObjectList(collection.Get(),requested.Get(),callback.Get(),&context))&&SUCCEEDED(bulk->Start(context))){
                if(WaitForSingleObject(callback->event(),10000)==WAIT_OBJECT_0&&SUCCEEDED(callback->status()))bulk_result=callback->take();
                else bulk->Cancel(context);
            }
        }
        std::vector<RemoteObject> result;result.reserve(ids.size());
        for(auto& object:ids){
            if(auto it=bulk_result.find(object);it!=bulk_result.end()){result.push_back(std::move(it->second));continue;}
            try{result.push_back(stat(object));}catch(DeviceFailure const& e){if(e.code()!=DeviceError::missing&&e.code()!=DeviceError::unsupported)throw;}
        }
        return result;
    }
    std::vector<std::byte> read(std::string const& object,std::uint64_t offset,std::size_t size,const SeekProgress& progress){
        if(size>max_read||offset>INT64_MAX||size>static_cast<std::uint64_t>(INT64_MAX)-offset)failure(DeviceError::protocol,"Portable device read range is invalid");
        if(streams_.size()>=4&&!streams_.contains(object))streams_.erase(streams_.begin());
        auto& state=streams_[object];
        auto reopen=[&]{state.stream.Reset();state.position=0;state.buffer=0;check(resources_->GetStream(wide(object).c_str(),WPD_RESOURCE_DEFAULT,STGM_READ,&state.buffer,state.stream.GetAddressOf()),"Open portable device stream");};
        if(!state.stream)reopen();
        try{
            auto reported=Clock::time_point::min();
            auto report=[&](bool force=false){
                auto now=Clock::now();
                if(progress&&(force||reported==Clock::time_point::min()||now-reported>=250ms)){progress(state.position);reported=now;}
            };
            if(state.position!=offset){
                bool positioned=false;
                if(state.seekable){
                    LARGE_INTEGER move{};move.QuadPart=static_cast<LONGLONG>(offset);
                    ULARGE_INTEGER actual{};
                    auto hr=state.stream->Seek(move,STREAM_SEEK_SET,&actual);
                    if(SUCCEEDED(hr)&&actual.QuadPart==offset){
                        state.position=offset;positioned=true;report(true);
                    }else{
                        state.seekable=false;
                        // A failed seek can leave the stream position undefined.
                        // Reopen once, then retain the sequential stream for forward reads.
                        reopen();
                    }
                }
                if(!positioned){
                    if(offset<state.position)reopen();
                    std::vector<std::byte> scratch(std::clamp<std::size_t>(state.buffer,64u<<10,4u<<20));
                    while(state.position<offset){auto amount=static_cast<ULONG>(std::min<std::uint64_t>(scratch.size(),offset-state.position));ULONG got=0;check(state.stream->Read(scratch.data(),amount,&got),"Seek portable device stream");if(!got||got>amount)failure(DeviceError::offline,"Portable device stream ended while seeking");state.position+=got;report();}
                }
            }
            std::vector<std::byte> result(size);std::size_t done=0;
            while(done<size){auto amount=static_cast<ULONG>(std::min<std::size_t>(size-done,std::clamp<std::size_t>(state.buffer,64u<<10,4u<<20)));ULONG got=0;
                check(state.stream->Read(result.data()+done,amount,&got),"Read portable device stream");
                if(!got||got>amount)failure(DeviceError::offline,"Portable device stream ended unexpectedly");state.position+=got;done+=got;report();}
            report(true);
            return result;
        }catch(...){streams_.erase(object);throw;}
    }
private:
    struct Stream{ComPtr<IStream> stream;std::uint64_t position=0;DWORD buffer=0;bool seekable=true;};
    std::string id_;
    ComPtr<IPortableDevice> device_;ComPtr<IPortableDeviceContent> content_;
    ComPtr<IPortableDeviceProperties> properties_;ComPtr<IPortableDeviceResources> resources_;
    std::map<std::string,Stream> streams_;
};
struct Response{Json meta;std::vector<std::byte> bytes;};
void write_all(HANDLE pipe,const void* raw,std::size_t size){
    auto p=static_cast<const std::byte*>(raw);
    while(size){DWORD n=0;auto count=static_cast<DWORD>(std::min<std::size_t>(size,64u<<10));if(!WriteFile(pipe,p,count,&n,nullptr)||!n)failure(DeviceError::offline,"Portable device worker pipe closed");size-=n;p+=n;}
}
void timed_read(HANDLE pipe,HANDLE process,void* raw,std::size_t size,Clock::time_point deadline,std::stop_token stop){
    auto p=static_cast<std::byte*>(raw);
    while(size){
        if(stop.stop_requested())failure(DeviceError::offline,"Portable device operation cancelled");
        if(Clock::now()>=deadline)failure(DeviceError::timeout,"Portable device operation timed out");
        DWORD available=0;
        if(!PeekNamedPipe(pipe,nullptr,0,nullptr,&available,nullptr))failure(DeviceError::offline,"Portable device worker disconnected");
        if(!available){if(WaitForSingleObject(process,5)==WAIT_OBJECT_0)failure(DeviceError::offline,"Portable device worker exited");continue;}
        DWORD got=0;auto count=static_cast<DWORD>(std::min<std::size_t>(size,available));
        if(!ReadFile(pipe,p,count,&got,nullptr)||!got)failure(DeviceError::offline,"Portable device worker pipe closed");
        p+=got;size-=got;
    }
}
class Worker {
public:
    explicit Worker(std::stop_token stop):stop_(stop){}
    ~Worker(){reset();}
    Response request(Json const& request,const SeekProgress& progress={}){
        std::unique_lock lock(mutex_,std::defer_lock);
        const auto lock_deadline=Clock::now()+20s;
        while(!lock.try_lock_for(100ms)){
            if(stop_.stop_requested())failure(DeviceError::offline,"Portable device operation cancelled");
            if(Clock::now()>=lock_deadline)failure(DeviceError::timeout,"Portable device is busy");
        }
        try{
            if(stop_.stop_requested())failure(DeviceError::offline,"Portable device operation cancelled");
            if(!process_||WaitForSingleObject(process_.h,0)!=WAIT_TIMEOUT){reset();start();}
            auto data=request.dump();
            if(data.size()>32768)failure(DeviceError::protocol,"Portable device request is too large");
            auto size=static_cast<std::uint32_t>(data.size());
            // One small request is written only after the previous reply was consumed.
            write_all(input_.h,&size,sizeof(size));write_all(input_.h,data.data(),data.size());
            auto absolute=Clock::now()+10min;
            auto deadline=Clock::now()+30s;
            Json meta;
            for(;;){
                std::uint32_t length=0;timed_read(output_.h,process_.h,&length,sizeof(length),deadline,stop_);
                if(length>max_message)failure(DeviceError::protocol,"Portable device response is too large");
                std::string metadata(length,'\0');timed_read(output_.h,process_.h,metadata.data(),metadata.size(),deadline,stop_);
                meta=Json::parse(metadata);
                if(!meta.contains("seek"))break;
                if(progress)progress(meta.at("seek").get<std::uint64_t>());
                // Keep a bounded idle deadline while a sequential device is making progress.
                deadline=std::min(absolute,Clock::now()+30s);
            }
            if(!meta.at("ok").get<bool>()){
                auto code=meta.at("code").get<unsigned>();
                if(code>static_cast<unsigned>(DeviceError::unsupported))code=static_cast<unsigned>(DeviceError::protocol);
                throw DeviceFailure(static_cast<DeviceError>(code),meta.at("error").get<std::string>());
            }
            auto bytes=meta.value("bytes",std::size_t{0});
            if(bytes>max_read)failure(DeviceError::protocol,"Portable device binary response is too large");
            Response result{std::move(meta),std::vector<std::byte>(bytes)};
            timed_read(output_.h,process_.h,result.bytes.data(),bytes,deadline,stop_);
            return result;
        }catch(DeviceFailure const& e){if(e.code()==DeviceError::timeout||e.code()==DeviceError::offline||e.code()==DeviceError::protocol)reset();throw;}
        catch(...){reset();throw;}
    }
private:
    std::timed_mutex mutex_;
    std::stop_token stop_;
    Handle process_,input_,output_,job_;
    void reset()noexcept{
        if(process_&&WaitForSingleObject(process_.h,0)==WAIT_TIMEOUT){TerminateProcess(process_.h,1);WaitForSingleObject(process_.h,250);}
        input_.reset();output_.reset();job_.reset();process_.reset();
    }
    void start(){
        SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
        Handle child_input,child_output,host_input,host_output;
        if(!CreatePipe(&child_input.h,&host_input.h,&sa,65536)||!CreatePipe(&host_output.h,&child_output.h,&sa,65536))failure(DeviceError::unavailable,"Cannot create portable device worker pipes");
        if(!SetHandleInformation(host_input.h,HANDLE_FLAG_INHERIT,0)||!SetHandleInformation(host_output.h,HANDLE_FLAG_INHERIT,0))failure(DeviceError::unavailable,"Cannot configure portable device worker pipes");
        Handle error(CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr));
        if(!error)failure(DeviceError::unavailable,"Cannot open worker error stream");
        SIZE_T n=0;InitializeProcThreadAttributeList(nullptr,1,0,&n);
        std::vector<std::byte> storage(n);
        auto attributes=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if(!InitializeProcThreadAttributeList(attributes,1,0,&n))failure(DeviceError::unavailable,"Cannot initialize worker attributes");
        struct Attributes{LPPROC_THREAD_ATTRIBUTE_LIST value;~Attributes(){DeleteProcThreadAttributeList(value);}}guard{attributes};
        HANDLE inherited[]={child_input.h,child_output.h,error.h};
        if(!UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr))failure(DeviceError::unavailable,"Cannot restrict portable device worker handles");
        STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput=child_input.h;startup.StartupInfo.hStdOutput=child_output.h;startup.StartupInfo.hStdError=error.h;startup.lpAttributeList=attributes;
        auto executable=application_directory()/L"archivefs.exe";
        if(!std::filesystem::exists(executable))executable=application_directory()/L"archivefs-read.exe";
        auto command=L"\""+executable.wstring()+L"\" mtp-worker";PROCESS_INFORMATION pi{};
        job_=Handle(CreateJobObjectW(nullptr,nullptr));
        if(!job_)failure(DeviceError::unavailable,"Cannot create portable device worker job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(!SetInformationJobObject(job_.h,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))failure(DeviceError::unavailable,"Cannot configure portable device worker job");
        if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,nullptr,executable.parent_path().c_str(),&startup.StartupInfo,&pi))failure(DeviceError::unavailable,"Cannot start portable device worker");
        process_=Handle(pi.hProcess);Handle thread(pi.hThread);
        if(!AssignProcessToJobObject(job_.h,process_.h))failure(DeviceError::unavailable,"Cannot isolate portable device worker");
        if(ResumeThread(thread.h)==static_cast<DWORD>(-1))failure(DeviceError::unavailable,"Cannot resume portable device worker");
        input_=std::move(host_input);output_=std::move(host_output);
    }
};
class Mtp final:public DeviceProvider {
    struct Entry{std::shared_ptr<Worker> worker;Clock::time_point last;};
public:
    std::string root_id()const override{return "DEVICE";}
    void cancel()noexcept override{stop_.request_stop();}
    std::vector<RemoteDevice> devices()override{
        auto response=enumerator_.request(Json{{"op","devices"}});
        std::vector<RemoteDevice> result;std::set<std::string> alive;
        for(auto& row:response.meta.at("result")){
            RemoteDevice d{row.at("id").get<std::string>(),row.at("name").get<std::string>(),"online"};
            alive.insert(d.id);result.push_back(std::move(d));
        }
        std::lock_guard lock(mutex_);
        std::erase_if(workers_,[&](auto const& row){return !alive.contains(row.first)&&row.second.worker.use_count()==1;});
        return result;
    }
    std::vector<RemoteObject> children(std::string_view device,std::string_view parent)override{
        auto response=worker(device)->request(Json{{"op","list"},{"device",device},{"object",parent}});
        std::vector<RemoteObject> result;for(auto& row:response.meta.at("result"))result.push_back(object_from(row));return result;
    }
    RemoteObject stat(std::string_view device,std::string_view object)override{
        return object_from(worker(device)->request(Json{{"op","stat"},{"device",device},{"object",object}}).meta.at("result"));
    }
    void read_exact(std::string_view device,std::string_view object,std::uint64_t offset,std::span<std::byte> output)override{
        read_exact_progress(device,object,offset,output,{});
    }
    void read_exact_progress(std::string_view device,std::string_view object,std::uint64_t offset,std::span<std::byte> output,const SeekProgress& progress)override{
        auto session=worker(device);
        while(!output.empty()){
            auto count=std::min(output.size(),max_read);
            auto result=session->request(Json{{"op","read"},{"device",device},{"object",object},{"offset",offset},{"length",count}},progress);
            if(result.bytes.size()!=count)failure(DeviceError::protocol,"Incorrect portable device read size");
            std::copy(result.bytes.begin(),result.bytes.end(),output.begin());output=output.subspan(count);offset+=count;
        }
    }
private:
    std::stop_source stop_;
    Worker enumerator_{stop_.get_token()};
    std::mutex mutex_;std::map<std::string,Entry> workers_;
    std::shared_ptr<Worker> worker(std::string_view device){
        std::lock_guard lock(mutex_);
        auto now=Clock::now();
        std::erase_if(workers_,[&](auto const& e){return e.second.worker.use_count()==1&&now-e.second.last>2min;});
        auto id=std::string(device);
        if(auto it=workers_.find(id);it!=workers_.end()){it->second.last=now;return it->second.worker;}
        if(workers_.size()>=64)failure(DeviceError::timeout,"Too many open portable devices");
        auto result=std::make_shared<Worker>(stop_.get_token());workers_.emplace(std::move(id),Entry{result,now});return result;
    }
};
}
std::shared_ptr<DeviceProvider> make_mtp_provider(){return std::make_shared<Mtp>();}
int run_mtp_worker(){
    if(FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)))return 1;
    struct Apartment{~Apartment(){CoUninitialize();}}apartment;
    _setmode(_fileno(stdin),_O_BINARY);_setmode(_fileno(stdout),_O_BINARY);
    Session session;
    try{
        for(;;){
            std::uint32_t length=0;
            if(!std::cin.read(reinterpret_cast<char*>(&length),sizeof(length)))return std::cin.eof()?0:1;
            if(length>32768)return 1;
            std::string data(length,'\0');if(!std::cin.read(data.data(),length))return 1;
            Json result;std::vector<std::byte> bytes;
            try{
                auto request=Json::parse(data);auto op=request.at("op").get<std::string>();
                result={{"ok",true}};
                if(op=="devices"){
                    auto values=Json::array();
                    for(auto& d:attached())values.push_back({{"id",d.id},{"name",d.name}});
                    result["result"]=std::move(values);
                }else{
                    session.connect(request.at("device").get<std::string>());auto object=request.at("object").get<std::string>();
                    if(op=="list"){auto values=Json::array();for(auto& item:session.list(object))values.push_back(to_json(item));result["result"]=std::move(values);}
                    else if(op=="stat")result["result"]=to_json(session.stat(object));
                    else if(op=="read"){
                        auto seek=[](std::uint64_t position){
                            auto text=Json{{"seek",position}}.dump();
                            auto size=static_cast<std::uint32_t>(text.size());
                            std::cout.write(reinterpret_cast<const char*>(&size),sizeof(size));
                            std::cout.write(text.data(),static_cast<std::streamsize>(text.size()));
                            std::cout.flush();
                            if(!std::cout)failure(DeviceError::offline,"Portable device progress pipe closed");
                        };
                        bytes=session.read(object,request.at("offset").get<std::uint64_t>(),request.at("length").get<std::size_t>(),seek);result["bytes"]=bytes.size();
                    }
                    else failure(DeviceError::protocol,"Unknown portable device operation");
                }
            }catch(DeviceFailure const& e){result={{"ok",false},{"code",static_cast<unsigned>(e.code())},{"error",e.what()}};bytes.clear();}
            catch(std::exception const& e){result={{"ok",false},{"code",static_cast<unsigned>(DeviceError::protocol)},{"error",e.what()}};bytes.clear();}
            auto text=result.dump(-1,' ',false,Json::error_handler_t::replace);
            if(text.size()>max_message)return 1;
            auto size=static_cast<std::uint32_t>(text.size());
            std::cout.write(reinterpret_cast<const char*>(&size),sizeof(size));std::cout.write(text.data(),static_cast<std::streamsize>(text.size()));
            if(!bytes.empty())std::cout.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
            std::cout.flush();if(!std::cout)return 1;
        }
    }catch(...){return 1;}
}
}
