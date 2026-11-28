// SPDX-License-Identifier: GPL-3.0-only
#include "archive_tree.hpp"
#include "archive_reader.hpp"
#include <cstdlib>
#include <cstring>
#include <string_view>

using namespace archivefs;
namespace {
class Input final: public RandomAccessSource {
    struct Session final:ReadSession{
        std::span<const std::byte> data;
        explicit Session(std::span<const std::byte> bytes):data(bytes){}
        void read_exact(std::uint64_t offset,std::span<std::byte> out)const override{
            if(offset>data.size()||out.size()>data.size()-offset)std::abort();
            if(!out.empty())std::memcpy(out.data(),data.data()+offset,out.size());
        }
        void finish()override{}
    };
    std::span<const std::byte> data_;
public:
    explicit Input(std::span<const std::byte> bytes):data_(bytes){}
    std::uint64_t size()const noexcept override{return data_.size();}
    std::unique_ptr<ReadSession> begin_read()const override{return std::make_unique<Session>(data_);}
    void validate()const override{}
};
}
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,std::size_t size){
    auto bytes=std::string_view(reinterpret_cast<const char*>(data),size);
    try{
        auto u=to_utf16(bytes);
        if(to_utf8(u)!=bytes)std::abort();
    }catch(const std::invalid_argument&){}
    auto name=encode_windows_name(bytes);
    if(name.empty()||name.size()>255||name==u"."||name==u".."||name.back()==u' '||name.back()==u'.')std::abort();
    for(auto ch:name)if(ch<32||std::u16string_view(u"<>:\"/\\|?*").find(ch)!=std::u16string_view::npos)std::abort();
    auto round=to_utf16(to_utf8(name));
    if(round!=name)std::abort();
    Input input(std::as_bytes(std::span(data,size)));
    (void)detect_format(input);
    return 0;
}
