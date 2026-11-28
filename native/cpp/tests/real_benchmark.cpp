// SPDX-License-Identifier: GPL-3.0-only
#include "archive_reader.hpp"
#include "archive_tree.hpp"
#include "decoder_backend.hpp"
#include <atomic>
#include <barrier>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <thread>
#include <cstring>

using namespace archivefs;
namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void benchmark(const fs::path& image,const fs::path& decoder,const std::vector<std::byte>& original,std::size_t budget,unsigned threads,bool sequential){
    auto source=open_file_source(image);
    auto backend=open_decoder_backend(source,decoder,budget);
    auto info=backend->info();
    auto reader=std::make_shared<ArchiveReader>(backend,budget,recommended_page_size(info));
    ArchiveTree tree(reader);
    auto file=tree.find(u"payload.bin");
    check(file&&file->info.size==original.size(),"benchmark file size mismatch");
    constexpr std::size_t random_size=4096, sequential_size=1u<<20;
    const std::size_t block=sequential?sequential_size:random_size;
    const std::size_t total=sequential?original.size()/block:4096;
    std::atomic<bool> valid=true;
    std::atomic<std::size_t> next=0;
    std::vector<double> latencies(total);
    std::vector<std::size_t> offsets(total);
    std::mt19937_64 rng(82973);
    for(std::size_t i=0;i<total;++i)offsets[i]=sequential?i*block:static_cast<std::size_t>(rng()%(original.size()-block+1));
    std::barrier ready(static_cast<std::ptrdiff_t>(threads+1));
    std::vector<std::jthread> workers;
    for(unsigned i=0;i<threads;++i)workers.emplace_back([&]{
        std::vector<std::byte> buffer(block);
        ready.arrive_and_wait();
        try{
            for(;;){
                auto index=next.fetch_add(1,std::memory_order_relaxed);
                if(index>=total)break;
                const auto start=Clock::now();
                auto n=tree.read(file,offsets[index],buffer);
                latencies[index]=std::chrono::duration<double,std::micro>(Clock::now()-start).count();
                if(n!=block||std::memcmp(buffer.data(),original.data()+offsets[index],block)!=0)valid=false;
            }
        }catch(...){valid=false;}
    });
    const auto start=Clock::now();
    ready.arrive_and_wait();
    workers.clear();
    const auto seconds=std::chrono::duration<double>(Clock::now()-start).count();
    check(valid.load(),"benchmark read mismatch");
    std::sort(latencies.begin(),latencies.end());
    std::cout<<std::fixed<<std::setprecision(3)<<budget/(1u<<20)<<','<<threads<<','
             <<(sequential?"sequential":"random")<<','<<total*block/(1024.0*1024.0)/seconds<<','
             <<latencies[total/2]<<','<<latencies[total*95/100]<<'\n';
}
int main(int argc,char** argv){
    try{
        check(argc==4,"usage: archivefs-benchmark IMAGE DECODER_DIRECTORY ORIGINAL");
        std::ifstream in(argv[3],std::ios::binary|std::ios::ate);
        check(static_cast<bool>(in),"cannot open reference file");
        auto size=in.tellg();
        check(size>=8*1024*1024&&size<=256*1024*1024,"reference file size out of range");
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        in.seekg(0);in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
        check(static_cast<bool>(in),"cannot read reference file");
        std::cout<<"cache_mib,threads,pattern,mib_per_second,p50_us,p95_us\n";
        for(auto budget:{std::size_t{0},std::size_t{64u<<20}}){
            benchmark(argv[1],argv[2],bytes,budget,1,true);
            benchmark(argv[1],argv[2],bytes,budget,1,false);
            benchmark(argv[1],argv[2],bytes,budget,8,false);
        }
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
