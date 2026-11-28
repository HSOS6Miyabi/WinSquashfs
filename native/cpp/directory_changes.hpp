// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace archivefs {
#ifdef _WIN32
struct DirectoryChangeState {
    std::atomic<std::uint64_t> revision{0};
    std::atomic<bool> complete{true};
};

// Subscriptions keep directory monitors alive. All images in the same
// directory share a single outstanding ReadDirectoryChangesW request.
class DirectoryChanges {
public:
    DirectoryChanges();
    void subscribe(const std::filesystem::path& file);
    std::shared_ptr<DirectoryChangeState> state() const noexcept { return state_; }
private:
    std::shared_ptr<DirectoryChangeState> state_;
    std::vector<std::shared_ptr<void>> subscriptions_;
};
#endif
}
