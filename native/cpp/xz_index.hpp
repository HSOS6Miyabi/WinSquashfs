// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "random_access_source.hpp"

namespace archivefs {
struct IndexedXz {
    std::shared_ptr<const RandomAccessSource> source;
    std::size_t reserved=0;
};
// Decode the XZ stream indexes without reading unrelated compressed blocks.
IndexedXz index_xz_tar(std::shared_ptr<const RandomAccessSource> source,std::size_t budget);
}
