// SPDX-License-Identifier: GPL-3.0-only
#include "windows_services.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fstream>
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR command,int){
    try{if(std::wstring_view(command)==L"--stop"){archivefs::stop_agent();return 0;}return archivefs::run_agent();}
    catch(const std::exception& e){try{auto path=archivefs::settings_directory();std::filesystem::create_directories(path);std::ofstream out(path/L"unc.log",std::ios::binary|std::ios::trunc);out<<e.what()<<'\n';}catch(...){}return 1;}
}
