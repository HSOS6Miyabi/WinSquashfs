// SPDX-License-Identifier: GPL-3.0-only
#include "../archive_reader.hpp"
#include <atomic>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <thread>

using namespace archivefs;
static void require(bool condition) { if (!condition) throw std::runtime_error("archive reader test failed"); }
template<class F> static void require_failure(F operation) {
    bool failed = false;
    try { operation(); } catch (const std::exception&) { failed = true; }
    require(failed);
}
struct TestDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("archivefs-core-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TestDirectory() { require(std::filesystem::create_directory(path)); }
    ~TestDirectory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
static std::byte value(std::uint64_t offset) { return std::byte(static_cast<unsigned char>((offset * 19 + 5) % 251)); }

class TestBackend final : public ArchiveBackend {
public:
    ArchiveInfo info() const override { return {ArchiveFormat::squashfs, 1, 8193, 256}; }
    FileInfo stat(InodeId id) const override {
        if (id == 1) return {id, FileKind::directory, 0, 0};
        if (id == 2) return {id, FileKind::regular, 8193, 0};
        throw std::out_of_range("inode");
    }
    std::vector<DirectoryEntry> list(InodeId id) const override {
        require(id == 1);
        return {{"data.bin", stat(2)}};
    }
    void read_exact(InodeId id, std::uint64_t offset, std::span<std::byte> output) const override {
        require(id == 2 && offset <= 8193 && output.size() <= 8193 - offset);
        ++reads;
        for (auto& byte : output) byte = value(offset++);
    }
    void validate_source() const override { if (!valid) throw std::runtime_error("invalid source"); }
    mutable std::atomic<unsigned> reads{0};
    std::atomic<bool> valid{true};
};

int main() {
    TestDirectory dir;
    const auto path = dir.path / "source.bin";
    std::vector<std::byte> input(8193);
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = value(i);
    auto write = [&](const std::filesystem::path& target, const std::vector<std::byte>& bytes) {
        std::ofstream stream(target, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        require(static_cast<bool>(stream));
    };
    write(path, input);
    for (auto policy : {SourcePolicy::protected_handle, SourcePolicy::transient_handle}) {
        auto source = open_file_source(path, policy);
        require(source->size() == input.size());
        std::vector<std::jthread> workers;
        for (unsigned thread = 0; thread < 8; ++thread) workers.emplace_back([&, thread] {
            std::mt19937 rng(thread);
            for (unsigned round = 0; round < 100; ++round) {
                auto pos = rng() % 8000;
                std::array<std::byte, 193> output{};
                source->read_exact(pos, output);
                require(std::equal(output.begin(), output.end(), input.begin() + pos));
            }
        });
        workers.clear();
        require_failure([&] { std::array<std::byte, 2> out{}; source->read_exact(8192, out); });
    }
    {
        auto source=open_file_source(path,SourcePolicy::transient_handle);
        auto session=source->begin_read();
        std::array<std::byte,100> data{};
        { std::ifstream other(path,std::ios::binary); require(static_cast<bool>(other)); }
        source->validate();
        session->read_exact(7,data);
        require(std::equal(data.begin(),data.end(),input.begin()+7));
        session->finish();
        source->read_exact(9,data);
        require(std::equal(data.begin(),data.end(),input.begin()+9));
    }
    {
        auto source=open_file_source(path,SourcePolicy::transient_handle);
        auto session=source->begin_read();
        source.reset();
        std::array<std::byte,100> data{};
        session->read_exact(7,data);
        require(std::equal(data.begin(),data.end(),input.begin()+7));
        session->finish();
        require_failure([&] { session->read_exact(0,data); });
    }
    {
        auto source=open_file_source(path,SourcePolicy::transient_handle);
        auto session=source->begin_read();
        std::array<std::byte,100> data{};
        session->read_exact(7,data);
        std::filesystem::rename(path,dir.path/"session-old.bin");
        write(path,input);
        require_failure([&] {session->finish();});
        require_failure([&] {source->validate();});
    }
    auto volatile_source = open_file_source(path, SourcePolicy::transient_handle);
    std::filesystem::rename(path, dir.path / "old.bin");
    write(path, input);
    require_failure([&] { volatile_source->validate(); });
    auto disappearing = open_file_source(path, SourcePolicy::transient_handle);
    std::filesystem::remove(path);
    require_failure([&] { disappearing->validate(); });

    for (auto format : {ArchiveFormat::squashfs, ArchiveFormat::dwarfs, ArchiveFormat::erofs, ArchiveFormat::unknown}) {
        std::vector<std::byte> bytes(1028);
        if (format == ArchiveFormat::squashfs) {
            bytes[0]=std::byte{'h'}; bytes[1]=std::byte{'s'}; bytes[2]=std::byte{'q'}; bytes[3]=std::byte{'s'};
        } else if (format == ArchiveFormat::dwarfs) {
            const char magic[] = "DWARFS";
            for (unsigned i = 0; i < 6; ++i) bytes[i] = std::byte(magic[i]);
        } else if (format == ArchiveFormat::erofs) {
            bytes[1024]=std::byte{0xe2}; bytes[1025]=std::byte{0xe1};
            bytes[1026]=std::byte{0xf5}; bytes[1027]=std::byte{0xe0};
        }
        write(path, bytes);
        auto source = open_file_source(path);
        require(detect_format(*source) == format);
    }

    auto backend = std::make_shared<TestBackend>();
    ArchiveReader reader(backend, 16 * 1024, 256);
    require(reader.info().root == 1 && reader.list(1).front().name == "data.bin");
    std::vector<std::jthread> workers;
    for (unsigned thread = 0; thread < 8; ++thread) workers.emplace_back([&, thread] {
        std::mt19937 rng(thread);
        for (unsigned round = 0; round < 1000; ++round) {
            const auto offset = rng() % 8400;
            std::array<std::byte, 600> output{};
            const auto n = reader.read_at(2, offset, output);
            require(n == (offset >= input.size() ? 0 : std::min(output.size(), input.size() - offset)));
            if (n) require(std::equal(output.begin(), output.begin() + n, input.begin() + offset));
        }
    });
    workers.clear();
    require(backend->reads == 33);
    require(reader.cache_statistics().misses == 33);
    auto direct_backend=std::make_shared<TestBackend>();
    ArchiveReader direct(direct_backend,0,256);
    std::uint64_t requests=0;
    for(unsigned thread=0;thread<8;++thread) {
        std::mt19937 rng(thread);
        for(unsigned round=0;round<1000;++round) {
            const auto offset=rng()%8400;
            std::array<std::byte,600> data{};
            auto count=direct.read_at(2,offset,data);
            if(count) ++requests;
        }
    }
    require(direct_backend->reads==requests&&requests>7700);
    std::cout<<"Synthetic workload: "<<requests<<" uncached backend reads; "<<backend->reads<<" cached page reads\n";
    std::array<std::byte, 10> result{};
    require_failure([&] { reader.read_at(1, 0, result); });
    std::stop_source stop;
    stop.request_stop();
    require_failure([&] { reader.read_at(2, 0, result, stop.get_token()); });
    ArchiveReader uncached(backend, 0, 256);
    require(uncached.read_at(2, 255, result) == result.size());
    require(uncached.cache_statistics().entries == 0 && backend->reads == 34);
    backend->valid = false;
    require_failure([&] { reader.read_at(2, 0, result); });
    std::cout << "PASS positioned I/O, source generation, format detection, concurrent random reads, EOF, cancellation\n";
}
