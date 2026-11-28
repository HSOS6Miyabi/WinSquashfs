// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace archivefs {
// Private METHOD_BUFFERED control code, requiring a read-access source handle.
// WinFsp forwards only customer-defined device controls, not native ODX FSCTLs.
inline constexpr std::uint32_t remote_control_code=(0x8000u<<16)|(1u<<14)|(0x901u<<2);
enum class RemoteCommand : std::uint32_t { copy=1,retry=2,discard=3 };
struct RemoteControl {
    RemoteCommand command;
    bool replace=false;
    std::u16string destination;
};
inline std::vector<std::byte> encode_remote_control(RemoteCommand command,std::u16string_view destination={},bool replace=false){
    if(destination.size()>6000||destination.find(u'\0')!=destination.npos||
       (command==RemoteCommand::copy?destination.empty():!destination.empty()||replace))
        throw std::invalid_argument("Invalid remote control request");
    std::vector<std::byte> out;out.reserve(16+destination.size()*2);
    auto u32=[&](std::uint32_t n){for(unsigned i=0;i<4;++i)out.push_back(static_cast<std::byte>((n>>(8*i))&255));};
    u32(1);u32(static_cast<std::uint32_t>(command));u32(replace?1:0);u32(static_cast<std::uint32_t>(destination.size()));
    for(auto c:destination){out.push_back(static_cast<std::byte>(c&255));out.push_back(static_cast<std::byte>(c>>8));}
    return out;
}
inline RemoteControl decode_remote_control(std::span<const std::byte> bytes){
    if(bytes.size()<16||bytes.size()>12016)throw std::invalid_argument("Invalid remote control size");
    auto u32=[&](std::size_t at){std::uint32_t n=0;for(unsigned i=0;i<4;++i)n|=std::to_integer<std::uint32_t>(bytes[at+i])<<(8*i);return n;};
    auto version=u32(0),op=u32(4),flags=u32(8),count=u32(12);
    if(version!=1||op<1||op>3||flags>1||count!=(bytes.size()-16)/2||bytes.size()!=16+2ULL*count)
        throw std::invalid_argument("Invalid remote control header");
    RemoteControl result{static_cast<RemoteCommand>(op),flags==1,{}};
    for(std::size_t i=0;i<count;++i)result.destination+=static_cast<char16_t>(std::to_integer<unsigned>(bytes[16+2*i])|(std::to_integer<unsigned>(bytes[17+2*i])<<8));
    (void)encode_remote_control(result.command,result.destination,result.replace);
    return result;
}
}
