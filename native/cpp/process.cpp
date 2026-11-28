// SPDX-License-Identifier: GPL-3.0-only
#include "process.hpp"
#include <algorithm>
#include <array>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
namespace archivefs {
namespace {
#ifdef _WIN32
using Raw = HANDLE;
constexpr Raw empty = nullptr;
void close(Raw h) noexcept {CloseHandle(h);}
[[noreturn]] void fail(const char* action) {throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),action);}
#else
using Raw = int;
constexpr Raw empty = -1;
void close(Raw h) noexcept {::close(h);}
[[noreturn]] void fail(const char* action) {throw std::system_error(errno,std::generic_category(),action);}
#endif
struct Handle {
    Raw h=empty;
    Handle()=default;
    explicit Handle(Raw value):h(value){}
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    Handle(Handle&& other) noexcept:h(std::exchange(other.h,empty)){}
    ~Handle(){reset();}
    void reset() noexcept {if(h!=empty) close(std::exchange(h,empty));}
};
struct Pipe {
    Handle read,write;
    Pipe() {
#ifdef _WIN32
        SECURITY_ATTRIBUTES attrs{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
        if(!CreatePipe(&read.h,&write.h,&attrs,0)) fail("CreatePipe");
#else
        int fd[2];if(pipe2(fd,O_CLOEXEC))fail("pipe2");read.h=fd[0];write.h=fd[1];
#endif
    }
};
void read_log(Raw fd,const LogSink& sink,std::mutex& lock) {
    std::array<char,8192> buffer{};
    std::string line;
    auto emit=[&] {if(sink){std::lock_guard guard(lock);sink(line);}line.clear();};
    for(;;) {
#ifdef _WIN32
        DWORD count=0;
        if(!ReadFile(fd,buffer.data(),static_cast<DWORD>(buffer.size()),&count,nullptr)) {
            if(GetLastError()==ERROR_BROKEN_PIPE)break;fail("ReadFile pipe");
        }
#else
        auto count=::read(fd,buffer.data(),buffer.size());
        if(count<0){if(errno==EINTR)continue;fail("read pipe");}
#endif
        if(!count)break;
        for(std::size_t i=0;i<static_cast<std::size_t>(count);++i) {
            char c=buffer[i];if(c=='\n') {if(!line.empty()&&line.back()=='\r')line.pop_back();emit();}
            else {line+=c;if(line.size()>=65536)emit();}
        }
    }
    if(!line.empty())emit();
}
#ifdef _WIN32
std::wstring quote(std::wstring_view arg) {
    std::wstring result=L"\"";std::size_t slashes=0;
    for(auto c:arg){if(c==L'\\'){++slashes;continue;}if(c==L'\"'){result.append(slashes*2+1,L'\\');result+=c;}else{result.append(slashes,L'\\');result+=c;}slashes=0;}
    result.append(slashes*2,L'\\');result+=L'\"';return result;
}
struct AttributeList {
    std::vector<std::byte> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST list=nullptr;
    explicit AttributeList(std::array<HANDLE,3>& handles) {
        SIZE_T size=0;InitializeProcThreadAttributeList(nullptr,1,0,&size);
        if(!size)fail("InitializeProcThreadAttributeList");storage.resize(size);
        list=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if(!InitializeProcThreadAttributeList(list,1,0,&size))fail("InitializeProcThreadAttributeList");
        if(!UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles.data(),sizeof(handles),nullptr,nullptr)){
            const auto error=GetLastError();DeleteProcThreadAttributeList(list);SetLastError(error);fail("UpdateProcThreadAttribute");}
    }
    ~AttributeList(){DeleteProcThreadAttributeList(list);}
};
#else
struct SpawnActions {
    posix_spawn_file_actions_t value;
    SpawnActions(){auto e=posix_spawn_file_actions_init(&value);if(e)throw std::system_error(e,std::generic_category());}
    ~SpawnActions(){posix_spawn_file_actions_destroy(&value);}
    void duplicate(int from,int to){auto e=posix_spawn_file_actions_adddup2(&value,from,to);if(e)throw std::system_error(e,std::generic_category());}
};
struct SpawnAttr {
    posix_spawnattr_t value;
    SpawnAttr(){auto e=posix_spawnattr_init(&value);if(e)throw std::system_error(e,std::generic_category());}
    ~SpawnAttr(){posix_spawnattr_destroy(&value);}
};
// Block SIGPIPE only on the writer thread; restore the caller's mask and consume
// a newly generated pending SIGPIPE before restoring an unblocked mask.
struct PipeSignalGuard {
    sigset_t blocked{},previous{};bool pending=false;
    PipeSignalGuard(){sigemptyset(&blocked);sigaddset(&blocked,SIGPIPE);auto e=pthread_sigmask(SIG_BLOCK,&blocked,&previous);if(e)throw std::system_error(e,std::generic_category());sigset_t signals{};sigpending(&signals);pending=sigismember(&signals,SIGPIPE)==1;}
    ~PipeSignalGuard(){if(!pending&&sigismember(&previous,SIGPIPE)!=1){timespec ts{};while(sigtimedwait(&blocked,nullptr,&ts)<0&&errno==EINTR){}}pthread_sigmask(SIG_SETMASK,&previous,nullptr);}
};
#endif
}
void run_process(const std::filesystem::path& executable,const std::vector<std::filesystem::path::string_type>& args,
                 LogSink log,std::stop_token stop,ProcessInput input) {
    if(stop.stop_requested())throw std::runtime_error("operation cancelled");
    Pipe out,err,in;
#ifdef _WIN32
    for(auto h:{out.read.h,err.read.h,in.write.h})if(!SetHandleInformation(h,HANDLE_FLAG_INHERIT,0))fail("SetHandleInformation");
    std::array<HANDLE,3> inherited{in.read.h,out.write.h,err.write.h};AttributeList attrs(inherited);
    STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput=in.read.h;startup.StartupInfo.hStdOutput=out.write.h;startup.StartupInfo.hStdError=err.write.h;startup.lpAttributeList=attrs.list;
    Handle job(CreateJobObjectW(nullptr,nullptr));if(!job.h)fail("CreateJobObjectW");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!SetInformationJobObject(job.h,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))fail("SetInformationJobObject");
    auto absolute=std::filesystem::absolute(executable);auto command=quote(absolute.native());for(auto& arg:args){command+=L' ';command+=quote(arg);}
    PROCESS_INFORMATION pi{};
    if(!CreateProcessW(absolute.c_str(),command.data(),nullptr,nullptr,TRUE,EXTENDED_STARTUPINFO_PRESENT|CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,nullptr,&startup.StartupInfo,&pi))fail("CreateProcessW");
    Handle process(pi.hProcess),thread(pi.hThread);
    if(!AssignProcessToJobObject(job.h,process.h)){auto error=GetLastError();TerminateProcess(process.h,1);WaitForSingleObject(process.h,INFINITE);SetLastError(error);fail("AssignProcessToJobObject");}
    auto terminate=[&]{TerminateJobObject(job.h,1);};
    if(ResumeThread(thread.h)==static_cast<DWORD>(-1)){auto error=GetLastError();terminate();WaitForSingleObject(process.h,INFINITE);SetLastError(error);fail("ResumeThread");}
#else
    SpawnActions actions;actions.duplicate(in.read.h,STDIN_FILENO);actions.duplicate(out.write.h,STDOUT_FILENO);actions.duplicate(err.write.h,STDERR_FILENO);
    SpawnAttr attr;auto e=posix_spawnattr_setflags(&attr.value,POSIX_SPAWN_SETPGROUP);if(!e)e=posix_spawnattr_setpgroup(&attr.value,0);if(e)throw std::system_error(e,std::generic_category());
    std::vector<std::string> strings{std::filesystem::absolute(executable).native()};strings.insert(strings.end(),args.begin(),args.end());
    std::vector<char*> argv;for(auto& s:strings)argv.push_back(s.data());argv.push_back(nullptr);
    pid_t child=0;e=posix_spawn(&child,strings.front().c_str(),&actions.value,&attr.value,argv.data(),environ);if(e)throw std::system_error(e,std::generic_category(),"posix_spawn");
    auto terminate=[&]{::kill(-child,SIGKILL);};
#endif
    out.write.reset();err.write.reset();in.read.reset();
    std::mutex log_mutex,error_mutex;std::exception_ptr error;
    auto report_error=[&]{std::lock_guard lock(error_mutex);if(!error)error=std::current_exception();terminate();};
    std::optional<std::stop_callback<decltype(terminate)>> cancel;cancel.emplace(stop,terminate);
    std::jthread stdout_thread,stderr_thread,writer;
    auto drain=[&](Raw h){try{read_log(h,log,log_mutex);}catch(...){report_error();}};
    try {
        stdout_thread=std::jthread(drain,out.read.h);stderr_thread=std::jthread(drain,err.read.h);
        if(input)writer=std::jthread([&]{try{
#ifndef _WIN32
            PipeSignalGuard guard;
#endif
            input([&](const void* source,std::size_t remaining){auto p=static_cast<const char*>(source);while(remaining){if(stop.stop_requested())throw std::runtime_error("operation cancelled");
#ifdef _WIN32
                DWORD count=0;if(!WriteFile(in.write.h,p,static_cast<DWORD>(std::min<std::size_t>(remaining,1u<<20)),&count,nullptr))fail("WriteFile pipe");
#else
                auto count=::write(in.write.h,p,std::min<std::size_t>(remaining,1u<<20));if(count<0){if(errno==EINTR)continue;fail("write pipe");}
#endif
                if(!count)throw std::runtime_error("short pipe write");
                p+=count;remaining-=static_cast<std::size_t>(count);}});
        }catch(...){report_error();}in.write.reset();});
        else in.write.reset();
    }catch(...){report_error();in.write.reset();}
    int code=1;
#ifdef _WIN32
    if(WaitForSingleObject(process.h,INFINITE)!=WAIT_OBJECT_0){terminate();fail("WaitForSingleObject");}DWORD status=1;if(!GetExitCodeProcess(process.h,&status))fail("GetExitCodeProcess");code=static_cast<int>(status);terminate();
#else
    siginfo_t exit_info{};int result;do{result=waitid(P_PID,static_cast<id_t>(child),&exit_info,WEXITED|WNOWAIT);}while(result<0&&errno==EINTR);if(result<0){terminate();fail("waitid");}
    // Retain the exited process until the streams and cancellation callback
    // are finished; its process group cannot be reused during termination.
    code=exit_info.si_code==CLD_EXITED?exit_info.si_status:128+exit_info.si_status;terminate();
#endif
    if(writer.joinable())writer.join();
    if(stdout_thread.joinable())stdout_thread.join();
    if(stderr_thread.joinable())stderr_thread.join();
    cancel.reset();
#ifndef _WIN32
    int reaped_status=0;pid_t reaped;do{reaped=waitpid(child,&reaped_status,0);}while(reaped<0&&errno==EINTR);if(reaped<0)fail("waitpid");
#endif
    if(stop.stop_requested())throw std::runtime_error("operation cancelled");
    if(code)throw std::runtime_error("archive tool exited with code "+std::to_string(code));
    if(error)std::rethrow_exception(error);
}
}
