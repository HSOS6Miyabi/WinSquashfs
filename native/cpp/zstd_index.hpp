// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "random_access_source.hpp"

namespace archivefs {
struct IndexedZstd {
    std::shared_ptr<const RandomAccessSource> source;
    std::size_t reserved=0;
};
IndexedZstd index_zstd_tar(std::shared_ptr<const RandomAccessSource> source,std::size_t budget);
}
