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
    bool chinese_=false,busy_=false,closing_=false,source_check_pending_=false;
    int page_=-1;
    std::uint64_t preview_revision_=0;
    std::stop_source preview_stop_;
    std::stop_source stop_;
    std::shared_ptr<archivefs::ArchiveTree> archive_;
    Node directory_;
    std::filesystem::path image_;
    std::vector<Node> visible_;
    std::vector<std::filesystem::path> sources_;
    std::vector<std::shared_ptr<archivefs::MountedVolume>> mounts_;
    archivefs::Settings settings_;
    Microsoft::UI::Xaml::Controls::StackPanel body_{nullptr},page_actions_{nullptr};
    Microsoft::UI::Xaml::Controls::NavigationView nav_host_{nullptr};
    std::array<Microsoft::UI::Xaml::Controls::NavigationViewItem,5> nav_items_{{nullptr,nullptr,nullptr,nullptr,nullptr}};
    Microsoft::UI::Xaml::Controls::ScrollViewer scroll_{nullptr};
    Microsoft::UI::Dispatching::DispatcherQueueTimer mount_timer_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock status_{nullptr};
    Microsoft::UI::Xaml::Controls::ProgressRing progress_{nullptr};
    Microsoft::UI::Xaml::Controls::Button cancel_{nullptr},mount_button_{nullptr},
        close_button_{nullptr},up_button_{nullptr},verify_button_{nullptr},
        extract_selected_button_{nullptr},extract_all_button_{nullptr},
        open_unc_button_{nullptr},copy_unc_button_{nullptr},mount_open_button_{nullptr},mount_eject_button_{nullptr};
    Microsoft::UI::Xaml::Controls::ComboBox drive_choice_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock image_summary_{nullptr},mount_empty_{nullptr};
    Microsoft::UI::Xaml::Controls::ListView entries_{nullptr},source_list_{nullptr},mount_list_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBox address_{nullptr},filter_{nullptr},preview_{nullptr};
    hstring tr(std::wstring_view en,std::wstring_view zh)const{return hstring(chinese_?zh:en);}
    HWND hwnd();
    fire_and_forget run(Task task,hstring message);
    void report(std::exception_ptr error);
    void navigate(int page);
    void browser();void creator();void mount_page();void unc_page();void about_page();
    void open_image(std::filesystem::path path,int mode=0);
    void refresh_entries();fire_and_forget preview_selected();void extract(bool all);void mount_image();
    void mount_path(std::filesystem::path path,std::u16string drive);
    void refresh_drive_choices();
    void update_browser_buttons();
    fire_and_forget check_source();
    fire_and_forget accept_drop(Microsoft::UI::Xaml::DragEventArgs args,bool create);
    void refresh_sources();void refresh_mounts();
    std::vector<std::filesystem::path> pick(bool folder,bool multi=false,bool save=false);
};
}
namespace winrt::WinArchiveFSPro::factory_implementation {struct MainWindow:MainWindowT<MainWindow,implementation::MainWindow>{};}
