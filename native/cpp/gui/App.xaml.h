// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "pch.h"
#include "App.xaml.g.h"
namespace winrt::WinArchiveFSPro::implementation {
struct App:AppT<App> {
    App(){InitializeComponent();}
    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);
private:
    Microsoft::UI::Xaml::Window window_{nullptr};
};
}
