// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "open_progress.hpp"
#include <filesystem>
#include <memory>
#include <string>

namespace archivefs {
class DesktopNotifications {
public:
    DesktopNotifications(std::filesystem::path application_directory,bool chinese);
    ~DesktopNotifications();
    DesktopNotifications(const DesktopNotifications&)=delete;
    DesktopNotifications& operator=(const DesktopNotifications&)=delete;
    void opening(std::shared_ptr<OpenProgress> progress);
    void error(std::u16string title,std::u16string message);
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
