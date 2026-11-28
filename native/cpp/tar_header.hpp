// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cstddef>
#include <span>
#include <string>

namespace archivefs {
inline bool tar_header(std::span<const std::byte> data,std::string name){
    if(data.size()!=512)return false;
    if(std::all_of(data.begin(),data.end(),[](std::byte b){return b==std::byte{};})){
        for(auto& c:name)if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');
        return name.ends_with(".tar.gz")||name.ends_with(".tgz")||name.ends_with(".tar.xz")||name.ends_with(".txz")||name.ends_with(".tar.zst")||name.ends_with(".tzst");
    }
    // Also recognize pre-ustar headers using their octal checksum.
    unsigned stored=0;bool digits=false,ended=false;
    for(auto b:data.subspan(148,8)){
        auto c=std::to_integer<unsigned>(b);
        if(c==' '||c==0){if(digits)ended=true;continue;}
        if(ended||c<'0'||c>'7')return false;
        digits=true;stored=stored*8+c-'0';
    }
    unsigned sum=0;
    for(std::size_t i=0;i<data.size();++i)
        sum+=i>=148&&i<156?32u:std::to_integer<unsigned>(data[i]);
    return digits&&stored==sum;
}

}
