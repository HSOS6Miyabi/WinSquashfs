// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include <clocale>
namespace winrt::WinArchiveFSPro::implementation {
void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&){
    std::setlocale(LC_CTYPE,".UTF-8");
    auto window=winrt::make_self<MainWindow>();window_=window.as<Microsoft::UI::Xaml::Window>();
    window_.Activate();window->apply_command_line();
}
}
