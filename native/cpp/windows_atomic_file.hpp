// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstddef>
#include <vector>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace archivefs::detail {
namespace fs=std::filesystem;
using namespace std::chrono_literals;
struct FileHandle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit FileHandle(HANDLE h):value(h){}
    ~FileHandle(){close();}
    FileHandle(const FileHandle&)=delete;
    FileHandle& operator=(const FileHandle&)=delete;
    void close() noexcept {
        if(value!=INVALID_HANDLE_VALUE){CloseHandle(value);value=INVALID_HANDLE_VALUE;}
    }
};
[[noreturn]] inline void file_error(DWORD code,const char* action){
    throw std::system_error(static_cast<int>(code),std::system_category(),action);
}
// Open a stable snapshot while permitting an atomic replacement by a writer.
inline std::string read_text(const fs::path& path,std::size_t limit){
    FileHandle file(CreateFileW(path.c_str(),GENERIC_READ,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
    if(file.value==INVALID_HANDLE_VALUE)file_error(GetLastError(),"open settings");
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(file.value,&size))file_error(GetLastError(),"size settings");
    if(size.QuadPart<0||static_cast<std::uint64_t>(size.QuadPart)>limit)
        throw std::length_error("settings file exceeds size limit");
    std::string text(static_cast<std::size_t>(size.QuadPart),'\0');
    std::size_t offset=0;
    while(offset<text.size()){
        DWORD n=0;
        const auto chunk=static_cast<DWORD>(std::min<std::size_t>(text.size()-offset,1u<<20));
        if(!ReadFile(file.value,text.data()+offset,chunk,&n,nullptr))
            file_error(GetLastError(),"read settings");
        if(!n)throw std::runtime_error("settings file was truncated during reading");
        offset+=n;
    }
    return text;
}
// Write and flush a unique temporary file in the destination directory. Rename
// only after closing the file; briefly wait for external readers (e.g. Explorer,
// antivirus, PowerShell) which did not request FILE_SHARE_DELETE.
inline void write_text_atomic(const fs::path& path,std::string_view text,const char* action,
                              std::chrono::milliseconds timeout=2500ms){
    fs::create_directories(path.parent_path());
    static std::atomic<unsigned long long> sequence{0};
    fs::path temp;
    HANDLE raw=INVALID_HANDLE_VALUE;
    for(unsigned attempt=0;attempt<64;++attempt){
        temp=path;
        temp+=L"."+std::to_wstring(GetCurrentProcessId())+L"."+
            std::to_wstring(GetCurrentThreadId())+L"."+std::to_wstring(++sequence)+L".tmp";
        raw=CreateFileW(temp.c_str(),GENERIC_WRITE|DELETE,
                       FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,CREATE_NEW,
                       FILE_ATTRIBUTE_NORMAL,nullptr);
        if(raw!=INVALID_HANDLE_VALUE)break;
        auto code=GetLastError();
        if(code!=ERROR_FILE_EXISTS&&code!=ERROR_ALREADY_EXISTS)file_error(code,action);
    }
    if(raw==INVALID_HANDLE_VALUE)file_error(ERROR_ALREADY_EXISTS,action);
    struct Cleanup{
        fs::path path;
        ~Cleanup(){std::error_code ec;fs::remove(path,ec);}
    } cleanup{temp};
    FileHandle file(raw);
    std::size_t offset=0;
    while(offset<text.size()){
        DWORD n=0;
        const auto chunk=static_cast<DWORD>(std::min<std::size_t>(text.size()-offset,1u<<20));
        if(!WriteFile(file.value,text.data()+offset,chunk,&n,nullptr))
            file_error(GetLastError(),action);
        if(!n)file_error(ERROR_WRITE_FAULT,action);
        offset+=n;
    }
    if(!FlushFileBuffers(file.value))file_error(GetLastError(),action);
    // FileRenameInfoEx with POSIX semantics preserves open reader handles to the
    // old file while publishing the new directory entry. Use the SDK layout
    // even when targeting Windows versions predating the named Flags member.
    struct RenameInfo {DWORD flags;HANDLE root;DWORD name_length;wchar_t name[1];};
    static_assert(offsetof(RenameInfo,name)==offsetof(FILE_RENAME_INFO,FileName));
    auto destination=fs::absolute(path).wstring();
    if(destination.size()>32767)throw std::length_error("settings path too long");
    const auto bytes=offsetof(RenameInfo,name)+(destination.size()+1)*sizeof(wchar_t);
    std::vector<std::max_align_t> storage((bytes+sizeof(std::max_align_t)-1)/sizeof(std::max_align_t));
    auto* rename=reinterpret_cast<RenameInfo*>(storage.data());
    rename->flags=0x1u|0x2u; // REPLACE_IF_EXISTS | POSIX_SEMANTICS
    rename->root=nullptr;
    rename->name_length=static_cast<DWORD>(destination.size()*sizeof(wchar_t));
    std::memcpy(rename->name,destination.c_str(),(destination.size()+1)*sizeof(wchar_t));
    const auto deadline=std::chrono::steady_clock::now()+timeout;
    auto delay=10ms;
    bool extended=true;
    for(;;){
        DWORD error=ERROR_SUCCESS;
        if(extended){
            if(SetFileInformationByHandle(file.value,FileRenameInfoEx,rename,static_cast<DWORD>(bytes)))return;
            error=GetLastError();
            if(error==ERROR_INVALID_PARAMETER||error==ERROR_NOT_SUPPORTED||error==ERROR_INVALID_FUNCTION){
                extended=false;
                file.close();
            }
        }
        if(!extended){
            if(MoveFileExW(temp.c_str(),path.c_str(),
                           MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return;
            error=GetLastError();
        }
        if((error!=ERROR_ACCESS_DENIED&&error!=ERROR_SHARING_VIOLATION&&
            error!=ERROR_LOCK_VIOLATION)||std::chrono::steady_clock::now()>=deadline)
            file_error(error,action);
        std::this_thread::sleep_for(delay);
        delay=std::min(delay*2,100ms);
    }
}
}
