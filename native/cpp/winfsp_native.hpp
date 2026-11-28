// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "projection.hpp"
#include <memory>
#include <string>
namespace archivefs {
class NativeMount {
public:
    // Empty mount point selects a drive letter. A nonempty prefix creates
    // a UNC namespace without assigning a drive letter.
    NativeMount(std::shared_ptr<Projection> projection,std::u16string mount_point={},std::u16string prefix={});
    ~NativeMount();
    NativeMount(const NativeMount&)=delete;
    NativeMount& operator=(const NativeMount&)=delete;
    std::u16string mount_point()const;
    void unmount();
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
std::string winfsp_runtime_info();
}
