// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "projection.hpp"
#include "remote_transport.hpp"

namespace archivefs {
using RemoteTransportFactory=std::function<std::shared_ptr<RemoteTransport>(RemoteConnection)>;
std::shared_ptr<Projection> remote_projection(RemoteProtocol protocol,
    std::vector<RemoteConnection> connections,const std::filesystem::path& cache_directory,
    std::function<void(std::string_view)> error={},RemoteTransportFactory factory=make_remote_transport);
}
