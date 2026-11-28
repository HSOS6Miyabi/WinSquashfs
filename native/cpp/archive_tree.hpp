// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "archive_reader.hpp"
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <string_view>

namespace archivefs {

std::u16string to_utf16(std::string_view utf8);
std::string to_utf8(std::u16string_view utf16);
std::u16string encode_windows_name(std::string_view utf8);
struct WindowsNameLess {
    using is_transparent=void;
    bool operator()(std::u16string_view a, std::u16string_view b) const;
};

class ArchiveTree {
public:
    struct Node {
        std::string name;
        std::u16string windows_name;
        FileInfo info;
        std::weak_ptr<const Node> parent;
        std::shared_ptr<const void> owner;
    };
    using NodePtr = std::shared_ptr<const Node>;
    struct VerifyReport { std::uint64_t files = 0, bytes = 0; };
    struct SearchReport { std::vector<NodePtr> nodes; bool truncated=false; };

    explicit ArchiveTree(std::shared_ptr<ArchiveReader> reader);
    NodePtr root() const { return root_; }
    ArchiveInfo info() const { return reader_->info(); }
    void validate_path() const { reader_->validate_path(); }
    bool source_changed() const noexcept { return reader_->source_changed(); }
    std::vector<NodePtr> children(const NodePtr& directory) const;
    NodePtr find(std::u16string_view path) const;
    NodePtr find_child(const NodePtr& directory,std::u16string_view name) const;
    NodePtr resolve_link(const NodePtr& node) const;
    std::string read_link(const NodePtr& node) const;
    std::u16string path_of(const NodePtr& node) const;
    SearchReport search(const NodePtr& directory,std::u16string_view query,
                        std::size_t limit=10000,std::stop_token token={}) const;
    std::size_t read(const NodePtr& node, std::uint64_t offset, std::span<std::byte> output,
                     std::stop_token token = {}) const;
    VerifyReport verify(std::stop_token token = {}) const;
    void extract(const NodePtr& node, const std::filesystem::path& destination,
                 std::stop_token token = {}) const;
private:
    struct Directory {
        std::vector<NodePtr> nodes;
        std::map<std::u16string, NodePtr, WindowsNameLess> names;
        std::map<std::string,NodePtr,std::less<>> original_names;
    };
    std::shared_ptr<const Directory> load(const NodePtr& directory) const;
    void validate_node(const NodePtr& node) const;
    void extract_file(const NodePtr& node, const std::filesystem::path& output, std::stop_token token) const;
    const std::shared_ptr<const void> owner_ = std::make_shared<int>(0);
    std::shared_ptr<ArchiveReader> reader_;
    NodePtr root_;
    mutable std::mutex directories_mutex_;
    mutable std::map<const Node*, std::shared_future<std::shared_ptr<const Directory>>> directories_;
};

} // namespace archivefs
