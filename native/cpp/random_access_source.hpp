// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace archivefs {

class ReadSession {
public:
    virtual ~ReadSession() = default;
    virtual void read_exact(std::uint64_t offset, std::span<std::byte> buffer) const = 0;
    virtual void finish() = 0;
};

class RandomAccessSource {
public:
    virtual ~RandomAccessSource() = default;
    virtual std::uint64_t size() const noexcept = 0;
    // Concurrent calls must be safe. A request outside the source is an error.
    void read_exact(std::uint64_t offset, std::span<std::byte> buffer) const {
        auto session = begin_read();
        session->read_exact(offset, buffer);
        session->finish();
    }
    virtual std::unique_ptr<ReadSession> begin_read() const = 0;
    // Check whether a cached generation can still be used.
    virtual void validate() const = 0;
    virtual void validate_path() const { validate(); }
    // True when the source may have changed or requires a periodic identity check.
    virtual bool source_changed() const noexcept { return false; }
};

enum class SourcePolicy { protected_handle, transient_handle };

std::shared_ptr<RandomAccessSource> open_file_source(
    std::filesystem::path path, SourcePolicy policy = SourcePolicy::protected_handle);

} // namespace archivefs
