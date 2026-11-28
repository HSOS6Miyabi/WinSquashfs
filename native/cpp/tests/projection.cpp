// SPDX-License-Identifier: GPL-3.0-only
#include "projection.hpp"
#include <array>
#include <atomic>
#include <barrier>
#include <fstream>
#include <iostream>
#include <random>
#include <thread>
namespace fs=std::filesystem;
using namespace archivefs;
void check(bool b,const char* msg){if(!b)throw std::runtime_error(msg);}
void wait_size(const std::shared_ptr<ImagePool>& pool,std::size_t expected,const char* message){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(pool->size()!=expected&&std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(pool->size()==expected,message);
}
struct Temp {fs::path p;Temp(){std::random_device r;p=fs::temp_directory_path()/("archivefs-projection-"+std::to_string(r())+"-"+std::to_string(r()));check(fs::create_directory(p),"create temp");}~Temp(){std::error_code ec;fs::remove_all(p,ec);}};
void image(const fs::path& path,unsigned seed){std::vector<unsigned char> b(1028+8193);std::copy_n("hsqs",4,b.begin());for(std::size_t i=1028;i<b.size();++i)b[i]=static_cast<unsigned char>((i-1028+seed)%251);std::ofstream o(path,std::ios::binary);o.write(reinterpret_cast<const char*>(b.data()),static_cast<std::streamsize>(b.size()));check(static_cast<bool>(o),"write image");}
int main(int argc,char** argv){try{check(argc==2,"module dir");Temp temp;auto root=temp.p/"root";fs::create_directory(root);fs::create_directory(root/"directory");auto first=root/"first.squashfs",second=root/"directory"/"other.image";image(first,10);image(second,20);std::ofstream(root/"regular.txt")<<"plain";std::atomic<unsigned> opened=0;auto opener=make_archive_opener(argv[1],1u<<20);auto pool=std::make_shared<ImagePool>([&](const fs::path& p,SourcePolicy policy){++opened;std::this_thread::sleep_for(std::chrono::milliseconds(5));return opener(p,policy);},ImagePool::Settings{std::chrono::milliseconds(80),1});auto proj=unc_projection({u"path",root},pool);
    auto list=proj->list(proj->open(u"\\"));check(list.size()==2,"root filters plain files");check(image_unc_path({u"path",root},first)==u"\\\\sfs$\\path\\first.squashfs","UNC path");check(!image_unc_path({u"path",root},temp.p/"outside"),"outside path");
    std::barrier sync(8);std::vector<std::jthread> threads;for(unsigned i=0;i<8;++i)threads.emplace_back([&,i]{sync.arrive_and_wait();auto file=proj->open(u"\\first.squashfs\\data.bin");check(file&&!file->info.directory,"open projected file");std::array<std::byte,100> data{};check(proj->read(file,i*19,data)==data.size(),"projected read");for(std::size_t k=0;k<data.size();++k)check(data[k]==std::byte((i*19+k+10)%251),"projected bytes");});threads.clear();check(opened==1,"concurrent opens should coalesce");
    auto held=proj->open(u"\\first.squashfs\\folder\\small.txt");check(held&&held->info.size==20,"nested file");auto other=proj->open(u"\\directory\\other.image\\data.bin");check(other&&other->info.size==8193,"arbitrary extension");if(opened!=2)throw std::runtime_error("second generation: expected 2 opens, got "+std::to_string(opened.load()));other.reset();wait_size(pool,1,"idle expiry keeps active image");
    fs::remove(first);wait_size(pool,0,"automatic deletion invalidation");bool invalid=false;try{std::array<std::byte,5> b{};proj->read(held,0,b);}catch(...){invalid=true;}check(invalid,"old handles must fail after deletion");image(first,50);auto replaced=proj->open(u"first.squashfs/data.bin");check(replaced&&opened==3,"reopen replacement");std::array<std::byte,1> one{};proj->read(replaced,0,one);check(one[0]==std::byte{50},"replacement data");
    // An idle image with a long cache lifetime must disappear promptly when
    // deleted, without evicting other valid generations.
    {
        auto idle_pool=std::make_shared<ImagePool>(opener,ImagePool::Settings{std::chrono::minutes(10),8});
        auto idle_path=root/"idle.squashfs";image(idle_path,70);
        auto keep=idle_pool->acquire(first);
        auto idle=idle_pool->acquire(idle_path);
        idle.reset();
        check(idle_pool->size()==2,"idle source cached");
        fs::remove(idle_path);
        wait_size(idle_pool,1,"idle deleted image removed before timeout");
        check(keep->tree()->info().format==ArchiveFormat::squashfs,"other image remains valid");
        image(idle_path,80);
        auto reopened=idle_pool->acquire(idle_path);
        auto data_node=reopened->tree()->find(u"data.bin");
        std::array<std::byte,1> data{};
        check(reopened->tree()->read(data_node,0,data)==1&&data[0]==std::byte{80},"new generation after idle deletion");
    }
    auto disk=mount_projection(opener(first,SourcePolicy::protected_handle));auto r=disk->open(u"\\");check(r&&r->info.directory,"disk root");check(disk->list(r).size()==2,"disk listing");check(!disk->open(u"missing"),"missing file");bool traversal=false;try{disk->open(u"folder/../data.bin");}catch(...){traversal=true;}check(traversal,"traversal rejected");std::cout<<"PASS projected paths, concurrent open, leases, idle eviction and source invalidation\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
