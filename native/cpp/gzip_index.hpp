// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "random_access_source.hpp"

namespace archivefs {
struct IndexedGzip {
    std::shared_ptr<const RandomAccessSource> source;
    std::size_t reserved=0;
};
// Recognize a GZIP-compressed TAR and index its uncompressed byte stream.
// Return an empty result for other formats or an insufficient cache budget.
IndexedGzip index_gzip_tar(std::shared_ptr<const RandomAccessSource> source,std::size_t budget);
}
