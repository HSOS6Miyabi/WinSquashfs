// SPDX-License-Identifier: GPL-3.0-only
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cwchar>
#include <vector>
#include <ole2.h>

int wmain(int argc,wchar_t **argv)
{
    if(argc<3)return 1;
    const bool paste=wcscmp(argv[1],L"--server-paste")==0;
    if(paste&&argc<4)return 1;
    const int first=paste?3:1;
    HRESULT hr=OleInitialize(nullptr);
    if(FAILED(hr))return 2;
    const CLSID clsid=paste?CLSID{0xa0bd4e92,0x67d5,0x4f50,{0x9d,0xc7,0xa1,0x61,0x8d,0xe8,0x2a,0x63}}:CLSID{0x57ab2c11,0x7881,0x46eb,{0x9a,0xf8,0x33,0x19,0x4e,0xdf,0x60,0x71}};
    std::vector<PIDLIST_ABSOLUTE> absolute;
    std::vector<PCUITEMID_CHILD> children;
    PIDLIST_ABSOLUTE parent=nullptr;
    IDataObject *data=nullptr,*old_clipboard=nullptr;
    PIDLIST_ABSOLUTE target=nullptr;
    bool clipboard_changed=false;
    IShellExtInit *init=nullptr;
    IContextMenu *menu=nullptr;
    HMENU popup=nullptr;
    int result=3;
    do {
        for(int i=first;i<argc;++i)
        {
            PIDLIST_ABSOLUTE path=nullptr;
            if(FAILED(SHParseDisplayName(argv[i],nullptr,&path,0,nullptr)))break;
            absolute.push_back(path);
            children.push_back(ILFindLastID(path));
        }
        if(absolute.size()!=static_cast<size_t>(argc-first))break;
        parent=ILCloneFull(absolute[0]);
        if(!parent||!ILRemoveLastID(parent))break;
        hr=SHCreateDataObject(parent,static_cast<UINT>(children.size()),children.data(),nullptr,IID_IDataObject,reinterpret_cast<void**>(&data));
        if(FAILED(hr))break;
        hr=CoCreateInstance(clsid,nullptr,CLSCTX_INPROC_SERVER,IID_IShellExtInit,reinterpret_cast<void**>(&init));
        if(FAILED(hr))break;
        if(paste){
            hr=SHParseDisplayName(argv[2],nullptr,&target,0,nullptr);
            if(FAILED(hr))break;
            (void)OleGetClipboard(&old_clipboard);
            hr=OleSetClipboard(data);
            if(FAILED(hr))break;
            clipboard_changed=true;
        }
        hr=init->Initialize(paste?target:parent,paste?nullptr:data,nullptr);
        if(FAILED(hr))break;
        hr=init->QueryInterface(IID_IContextMenu,reinterpret_cast<void**>(&menu));
        if(FAILED(hr))break;
        popup=CreatePopupMenu();
        hr=menu->QueryContextMenu(popup,0,1,100,CMF_NORMAL);
        if(FAILED(hr)||HRESULT_CODE(hr)!=1)break;
        CMINVOKECOMMANDINFO command={};
        command.cbSize=sizeof(command);
        command.lpVerb=MAKEINTRESOURCEA(0);
        command.nShow=SW_SHOWNORMAL;
        hr=menu->InvokeCommand(&command);
        if(FAILED(hr))break;
        result=0;
    } while(false);
    if(result)std::fwprintf(stderr,L"Pack shell failure HRESULT=0x%08lx\n",static_cast<unsigned long>(hr));
    if(popup)DestroyMenu(popup);
    if(menu)menu->Release();
    if(init)init->Release();
    if(clipboard_changed)(void)OleSetClipboard(old_clipboard);
    if(old_clipboard)old_clipboard->Release();
    if(target)ILFree(target);
    if(data)data->Release();
    if(parent)ILFree(parent);
    for(auto p:absolute)ILFree(p);
    OleUninitialize();
    return result;
}
