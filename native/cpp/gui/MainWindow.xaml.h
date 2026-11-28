// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "pch.h"
#include "MainWindow.g.h"
namespace winrt::WinArchiveFSPro::implementation {
struct MainWindow:MainWindowT<MainWindow> {
    MainWindow();
    void apply_command_line();
private:
    using Task=std::function<std::function<void()>(std::stop_token)>;
    using Node=archivefs::ArchiveTree::NodePtr;
    bool chinese_=false,busy_=false,closing_=false;
    std::stop_source stop_;
    std::shared_ptr<archivefs::ArchiveTree> archive_;
    Node directory_;
    std::filesystem::path image_;
    std::vector<Node> visible_;
    std::vector<std::filesystem::path> sources_;
    std::vector<std::shared_ptr<archivefs::MountedVolume>> mounts_;
    archivefs::Settings settings_;
    Microsoft::UI::Xaml::Controls::StackPanel navigation_{nullptr},body_{nullptr};
    Microsoft::UI::Xaml::Controls::ContentControl nav_host_{nullptr};
    Microsoft::UI::Xaml::Controls::ScrollViewer scroll_{nullptr};
    Microsoft::UI::Dispatching::DispatcherQueueTimer mount_timer_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock status_{nullptr};
    Microsoft::UI::Xaml::Controls::Button cancel_{nullptr};
    Microsoft::UI::Xaml::Controls::ListView entries_{nullptr},source_list_{nullptr},mount_list_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBox address_{nullptr},filter_{nullptr},preview_{nullptr};
    hstring tr(std::wstring_view en,std::wstring_view zh)const{return hstring(chinese_?zh:en);}
    HWND hwnd();
    fire_and_forget run(Task task,hstring message);
    void report(std::exception_ptr error);
    void navigate(int page);
    void browser();void creator();void mount_page();void unc_page();void about_page();
    void open_image(std::filesystem::path path,int mode=0);
    void refresh_entries();void preview_selected();void extract(bool all);void mount_image();
    void refresh_sources();void refresh_mounts();
    std::vector<std::filesystem::path> pick(bool folder,bool multi=false,bool save=false);
};
}
namespace winrt::WinArchiveFSPro::factory_implementation {struct MainWindow:MainWindowT<MainWindow,implementation::MainWindow>{};}
