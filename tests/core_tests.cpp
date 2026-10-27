#include "jmcomic/client.hpp"
#include "jmcomic/crypto.hpp"
#include "jmcomic/downloader.hpp"
#include "jmcomic/image.hpp"
#include "jmcomic/storage.hpp"
#include <webp/encode.h>
#include <stb_image_write.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif
using namespace jm;
namespace {
int checks{};
void require(bool test,const char* message) { ++checks; if (!test) throw std::runtime_error(message); }
template<class F> void throws(F fn) { bool caught=false;try{fn();}catch(const std::exception&){caught=true;}require(caught,"Expected an exception"); }
struct Scratch {
  fs::path path;
  Scratch() {
    auto base=fs::temp_directory_path();
    path=base/("jmcomic-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(path);
  }
  ~Scratch(){std::error_code ec;fs::remove_all(path,ec);}
};
Image rows(int w,int h) {
  Image image{w,h,Bytes(static_cast<std::size_t>(w*h*4))};
  for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
    auto p=static_cast<std::size_t>((y*w+x)*4);image.rgba[p]=static_cast<unsigned char>(y); image.rgba[p+1]=static_cast<unsigned char>(x);image.rgba[p+2]=91;image.rgba[p+3]=255;
  }
  return image;
}
Progress finish(DownloadManager& manager,std::uint64_t id, std::chrono::seconds timeout=std::chrono::seconds(20)) {
  const auto deadline=std::chrono::steady_clock::now()+timeout;
  while(std::chrono::steady_clock::now()<deadline) {
    for(const auto& p:manager.snapshot())if(p.task==id && (p.state==TaskState::Completed || p.state==TaskState::Cancelled || p.state==TaskState::Failed))return p;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  throw std::runtime_error("Timed out waiting for download");
}
void unit() {
  require(md5("abc")=="900150983cd24fb0d6963f7d28e17f72","MD5 vector");
  require(sha256(Bytes{'a','b','c'})=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA256 vector");
  throws([]{decrypt_data(1,"");}); throws([]{decrypt_data(1,"AAAA");});throws([]{decrypt_data(1,"!!!!!!!!!!!!!!!!!!!!!!!!");});
  require(parse_id("123")==123,"Numeric ID");throws([]{parse_id("1/../../");});throws([]{parse_id("0");});throws([]{parse_id("3suffix");});
  require(safe_name("../bad:/\\name*?  ")=="_bad___name__","Filename sanitizing");
  require(safe_name("CON.txt")=="_CON.txt","Windows device");require(safe_name("...")=="Untitled","Empty filename fallback");
  require(safe_name("中文标题")=="中文标题","Unicode names");
  std::string long_name;for(int i=0;i<100;++i)long_name+="中";
  require(safe_name(long_name).size()==138,"UTF-8 boundary truncation");
  require(display_text("abc\x1b[31m\n")=="abc [31m ","Terminal escapes removed");
  require(safe_name(std::string(139,'a')+".suffix").back()!='.',"Truncation cannot leave trailing dot");
  require(page_name(0)=="001.png" && page_name(999)=="1000.png","Page numbering");
  require(strip_count(220980,220979,"00001")==0,"Unencoded threshold");
  require(strip_count(220980,220980,"00001")==10,"Old strip threshold");
  require(strip_count(220980,268849,"00001")==10,"Old strip upper bound");
  require(strip_count(220980,268850,"00001")==6 && strip_count(220980,421925,"00001")==18 && strip_count(220980,421926,"00001")==14,"Known hash boundary vectors");
  auto image=rows(3,23);auto original=image;
  unscramble(image,10);
  const std::vector<int> expected{18,19,20,21,22,16,17,14,15,12,13,10,11,8,9,6,7,4,5,2,3,0,1};
  for(int y=0;y<23;++y)require(image.rgba[static_cast<std::size_t>(y*3*4)]==expected[static_cast<std::size_t>(y)],"Strip remainder mapping");
  auto evenly=rows(2,20);unscramble(evenly,10);require(evenly.rgba[0]==18 && evenly.rgba[19*2*4]==1,"Even strip mapping");
  auto tiny=rows(1,1);unscramble(tiny,10);require(tiny.rgba==rows(1,1).rgba,"Image shorter than strip count");
  auto unchanged=original;unscramble(unchanged,0);require(unchanged.rgba==original.rgba,"No scrambling");
  throws([&]{unscramble(unchanged,30);});
  auto png=encode_png(original);auto decoded=decode_image(png);
  require(decoded.rgba==original.rgba && decoded.width==3 && decoded.height==23,"PNG pixel exact roundtrip");
  unsigned char* encoded=nullptr;
  auto length=WebPEncodeLosslessRGBA(original.rgba.data(),3,23,12,&encoded);
  require(length>0,"WebP fixture");
  auto webp=decode_image({encoded,length});WebPFree(encoded);
  require(webp.rgba==original.rgba,"WebP lossless roundtrip");
  Bytes jpeg;
  auto sink=[](void* p,void* data,int size){auto& out=*static_cast<Bytes*>(p);auto* b=static_cast<unsigned char*>(data);out.insert(out.end(),b,b+size);};
  require(stbi_write_jpg_to_func(sink,&jpeg,3,23,4,original.rgba.data(),95)!=0,"JPEG fixture");
  require(decode_image(jpeg).width==3,"JPEG decode");
  // A complete 1x1 GIF89a, first frame converted to PNG.
  const Bytes gif={'G','I','F','8','9','a',1,0,1,0,0x80,0,0,0,0,0,255,255,255,0x21,0xf9,4,1,0,0,0,0,0x2c,0,0,0,0,1,0,1,0,0,2,2,0x44,1,0,0x3b};
  require(decode_image(gif).height==1,"GIF decode");
  throws([]{decode_image(Bytes{1,2,3});});throws([&]{decode_image(std::span<const unsigned char>(png.data(),16));});
  Scratch scratch;const auto output=scratch.path/"001.png";const Json identity={{"chapter",123},{"file","001.webp"}};
  require(!verified_page(output,identity),"Missing page");save_page(output,original,identity);
  require(verified_page(output,identity),"Verified page");require(!verified_page(output,Json{{"chapter",124}}),"Source identity mismatch");
  auto corrupt=read_bytes(output);corrupt.back()^=1;atomic_write(output,corrupt);
  require(!verified_page(output,identity),"Corrupt output detected");save_page(output,original,identity);
  fs::remove(output.string()+".json");require(!verified_page(output,identity),"Uncommitted page rejected");
  Settings s;s.destination=scratch.path/"downloads";s.validate();save_settings(scratch.path,s);
  require(load_settings(scratch.path).concurrency==3,"Configuration roundtrip");
#ifndef _WIN32
  struct stat statbuf{};stat((scratch.path/"jmcomic.json").c_str(),&statbuf);require((statbuf.st_mode&077)==0,"Owner only config");
#endif
  s.concurrency=9;throws([&]{s.validate();});s.concurrency=3;s.api_url="http://example.com";throws([&]{s.validate();});
  const auto comic=parse_comic({{"id","123"},{"name","Title"},{"series",Json::array({{{"id","125"},{"sort","2"},{"name","B"}},{{"id","124"},{"sort","1"},{"name","A"}}})}});
  require(comic.chapters[0].id==124,"Chapter order");
  require(parse_comic({{"id",1},{"name","Single"}}).chapters[0].id==1,"Single chapter fallback");
  require(parse_listing({{"redirect_aid",123}},false).redirect==123,"Search redirect");
  require(parse_listing({{"total","0"},{"content",Json::array()}},false).comics.empty(),"Empty search");
}
void integration(const std::string& url) {
  Scratch scratch;Settings settings;settings.api_url=url;settings.image_url=url;settings.destination=scratch.path/"out";
  settings.request_interval_ms=100;settings.retries=2;settings.concurrency=3;
  auto client=std::make_shared<Client>(settings,scratch.path,true);
  for(const auto* sort:{"mr","mv","tf"})require(client->search("keyword & 中文",2,sort).comics[0].id==240001,"Search and orders");
  require(client->search("redirect",1,"mr").redirect==240001,"Redirect response");
  require(client->search("retry",1,"mr").comics.size()==1,"429 retried");
  require(client->comic(240001).chapters[0].id==240001,"Comic and chapter");
  require(client->chapter(240001).scramble_id==220980,"Scramble template");
  throws([&]{client->favorites(1,0);});throws([&]{client->login("wrong","wrong");});
  require(client->login("test","secret")=="test","Login");require(client->has_session(),"Session created");
  require(client->profile()=="test","Session profile");
  auto restored=std::make_shared<Client>(settings,scratch.path,true);require(restored->profile()=="test","Session restored");
  const auto favorites=client->favorites(2,7);require(favorites.comics.size()==1 && favorites.folders[0].id==7,"Favorites folders and pagination");
#ifndef _WIN32
  struct stat st{};stat((scratch.path/"jmcomic-session.json").c_str(),&st);require((st.st_mode&077)==0,"Session permissions");
#endif
  std::atomic_int notifications{};DownloadManager manager([&]{++notifications;});
  auto comic=client->comic(240001);auto chapter=comic.chapters[0];
  auto task=manager.enqueue(client,comic,chapter,settings.destination);
  require(manager.enqueue(client,comic,chapter,settings.destination/".")==task,"Equivalent paths do not duplicate tasks");
  auto complete=finish(manager,task);
  if(complete.state!=TaskState::Completed)throw std::runtime_error("Download failed: "+complete.error);
  require(complete.done==3,"Every page downloaded");require(notifications>0,"Progress notifications");
  const auto dir=settings.destination/(safe_name(comic.name)+" [240001]")/(safe_name(chapter.name)+" [240001]");
  for(int i=0;i<3;++i) {
    auto image=decode_image(read_bytes(dir/page_name(static_cast<std::size_t>(i))));
    require(image.rgba==rows(3,23).rgba,"Decoded PNG pixels match expected original");
  }
  // A new manager emulates restarting the executable, not a cached in-memory state.
  manager.shutdown();DownloadManager resumed([]{});
  task=resumed.enqueue(client,comic,chapter,settings.destination);auto reused=finish(resumed,task);
  require(reused.state==TaskState::Completed && reused.skipped==3,"Resume without image requests");
  resumed.shutdown();auto corrupt=read_bytes(dir/"002.png");corrupt.back()^=1;atomic_write(dir/"002.png",corrupt);
  DownloadManager repaired([]{});task=repaired.enqueue(client,comic,chapter,settings.destination);auto repair=finish(repaired,task);
  require(repair.state==TaskState::Completed && repair.skipped==2,"Corrupt page repaired");repaired.shutdown();
  DownloadManager cancelling([]{});comic.id=240002;comic.chapters[0].id=240002;
  task=cancelling.enqueue(client,comic,comic.chapters[0],settings.destination);
  std::this_thread::sleep_for(std::chrono::milliseconds(500));cancelling.cancel(task);
  require(finish(cancelling,task).state==TaskState::Cancelled,"Cancellation");
  cancelling.retry(task);auto retried=finish(cancelling,task);require(retried.state==TaskState::Completed,"Retry after cancellation");cancelling.shutdown();
  DownloadManager failing([]{});comic.id=240003;comic.chapters[0].id=240003;
  task=failing.enqueue(client,comic,comic.chapters[0],settings.destination);
  auto failed=finish(failing,task);require(failed.state==TaskState::Failed && failed.failed==1,"Fatal page failure reported");
  failing.retry(task);auto recovered=finish(failing,task);
  require(recovered.state==TaskState::Completed && recovered.skipped==2,"Failed chapter resumes only missing pages");failing.shutdown();
  throws([&]{restored->favorites(1,999);});require(!restored->has_session(),"Expired session cleared");
  client->logout();require(!client->has_session() && !fs::exists(scratch.path/"jmcomic-session.json"),"Logout clears session");
  throws([&]{client->profile();});
}
void live() {
  Scratch scratch;Settings settings;settings.destination=scratch.path/"out";
  auto client=std::make_shared<Client>(settings,scratch.path);
  Id id{};
  for(const auto* sort:{"mr","mv","tf"}) {
    const auto found=client->search("test",1,sort);
    require(!found.comics.empty(),"Live search result");
    if(!id)id=found.comics[0].id;
    std::cout<<"Live search "<<sort<<": "<<found.total<<" results\n";
  }
  auto comic=client->comic(id);require(!comic.chapters.empty(),"Live chapters");
  auto chapter=client->chapter(comic.chapters[0].id);require(!chapter.images.empty(),"Live pages");
  auto source=client->image(chapter.id,chapter.images[0]);auto image=decode_image(source);
  const auto strips=strip_count(chapter.scramble_id,chapter.id,fs::path(chapter.images[0]).stem().string());
  unscramble(image,strips);const auto output=scratch.path/"page.png";
  const Json identity={{"chapter",chapter.id},{"file",chapter.images[0]}};
  save_page(output,image,identity);require(verified_page(output,identity),"Live saved PNG");
  require(decode_image(read_bytes(output)).rgba==image.rgba,"Live PNG lossless pixels");
  DownloadManager manager([]{});
  const auto task=manager.enqueue(client,comic,comic.chapters[0],settings.destination);
  const auto finished=finish(manager,task,std::chrono::seconds(120));
  if(finished.state!=TaskState::Completed)throw std::runtime_error("Live chapter download failed: "+finished.error);
  require(finished.done==chapter.images.size(),"Live complete chapter download");manager.shutdown();
  DownloadManager resumed([]{});
  const auto resumed_id=resumed.enqueue(client,comic,comic.chapters[0],settings.destination);
  const auto verified=finish(resumed,resumed_id,std::chrono::seconds(60));
  require(verified.state==TaskState::Completed && verified.skipped==chapter.images.size(),"Live chapter resume");resumed.shutdown();
  std::cout<<"Live chapter scheduler saved and verified "<<finished.done<<" PNG pages; resume reused "<<verified.skipped<<" pages\n";
  std::cout<<"Live comic "<<id<<", chapter "<<chapter.id<<", "<<chapter.images.size()<<" pages; first "
    <<image.width<<"x"<<image.height<<", "<<strips<<" strips, "<<source.size()<<" source bytes, "<<fs::file_size(output)<<" PNG bytes\n";
}
void benchmark() {
  const auto input=rows(1200,1800);auto png=encode_png(input);
  const auto start=std::chrono::steady_clock::now();
  constexpr int pages=6;
  std::atomic_llong decode_us{},strip_us{},encode_us{};
  auto process=[&] {
    const auto a=std::chrono::steady_clock::now();auto image=decode_image(png);
    const auto b=std::chrono::steady_clock::now();unscramble(image,10);
    const auto c=std::chrono::steady_clock::now();auto result=encode_png(image);
    const auto d=std::chrono::steady_clock::now();
    decode_us+=std::chrono::duration_cast<std::chrono::microseconds>(b-a).count();
    strip_us+=std::chrono::duration_cast<std::chrono::microseconds>(c-b).count();
    encode_us+=std::chrono::duration_cast<std::chrono::microseconds>(d-c).count();
    if(result.empty())throw std::runtime_error("Benchmark encoding failed");
  };
  for(int batch=0;batch<2;++batch) {
    std::vector<std::jthread> threads;
    for(int i=0;i<3;++i)threads.emplace_back(process);
  }
  std::cout<<pages<<" pages at 1200x1800, 3 workers, wall ms "
    <<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count()
    <<", summed phase ms: decode "<<decode_us/1000<<", strips "<<strip_us/1000<<", PNG "<<encode_us/1000<<"\n";
}

}
int main(int argc,char** argv) {
  try { if(argc==3 && std::string(argv[1])=="--integration")integration(argv[2]);else if(argc==2 && std::string(argv[1])=="--live")live();else if(argc==2 && std::string(argv[1])=="--benchmark")benchmark();else unit();std::cout<<checks<<" checks passed\n";return 0; }
  catch(const std::exception& e){std::cerr<<"FAILED after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}
}
