// SPDX-License-Identifier: GPL-3.0-only
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cwchar>
#include <vector>

int wmain(int argc,wchar_t **argv)
{
    if(argc<3)return 1;
    HRESULT hr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(hr))return 2;
    const CLSID clsid={0x57ab2c11,0x7881,0x46eb,{0x9a,0xf8,0x33,0x19,0x4e,0xdf,0x60,0x71}};
    std::vector<PIDLIST_ABSOLUTE> absolute;
    std::vector<PCUITEMID_CHILD> children;
    PIDLIST_ABSOLUTE parent=nullptr;
    IDataObject *data=nullptr;
    IShellExtInit *init=nullptr;
    IContextMenu *menu=nullptr;
    HMENU popup=nullptr;
    int result=3;
    do {
        for(int i=1;i<argc;++i)
        {
            PIDLIST_ABSOLUTE path=nullptr;
            if(FAILED(SHParseDisplayName(argv[i],nullptr,&path,0,nullptr)))break;
            absolute.push_back(path);
            children.push_back(ILFindLastID(path));
        }
        if(absolute.size()!=static_cast<size_t>(argc-1))break;
        parent=ILCloneFull(absolute[0]);
        if(!parent||!ILRemoveLastID(parent))break;
        hr=SHCreateDataObject(parent,static_cast<UINT>(children.size()),children.data(),nullptr,IID_IDataObject,reinterpret_cast<void**>(&data));
        if(FAILED(hr))break;
        hr=CoCreateInstance(clsid,nullptr,CLSCTX_INPROC_SERVER,IID_IShellExtInit,reinterpret_cast<void**>(&init));
        if(FAILED(hr))break;
        hr=init->Initialize(parent,data,nullptr);
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
    if(data)data->Release();
    if(parent)ILFree(parent);
    for(auto p:absolute)ILFree(p);
    CoUninitialize();
    return result;
}
