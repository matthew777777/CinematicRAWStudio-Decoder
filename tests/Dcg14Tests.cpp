// SPDX-License-Identifier: Apache-2.0
#include "common/Clip.h"
#include "Crc32c.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
namespace fs=std::filesystem;
static void check(bool ok){if(!ok)throw std::runtime_error("DCG14 preservation regression");}
static void le(std::vector<uint8_t>& b,uint64_t v,int n){for(int i=0;i<n;++i)b.push_back(uint8_t(v>>(8*i)));}
static void chunk(std::vector<uint8_t>& b,const char*t,const std::vector<uint8_t>&p){b.insert(b.end(),t,t+4);le(b,0,4);le(b,p.size(),8);b.insert(b.end(),p.begin(),p.end());le(b,craw::detail::crc32c(p.data(),p.size()),4);}
static std::vector<uint8_t> read(const fs::path&p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
int main(){
 const fs::path vectors=CRAW_VECTORS_DIR,path=fs::temp_directory_path()/"craw_dcg14_test.craw";
 try{
  for(int codec=1;codec<=4;++codec){
   const auto name="dcg14_gbrg_codec"+std::to_string(codec);
   const auto payload=read(vectors/(name+".payload")),expected=read(vectors/(name+".raw"));check(!payload.empty());
   std::vector<uint8_t>b{'C','R','A','W',0,0,2,0,0,0,0,0,0,0,0,0};
   const std::string meta=R"({"sensor":{"cfa":"GBRG","white_level":1023,"black_level":[64,64,64,64]}})";
   chunk(b,"META",{meta.begin(),meta.end()});
   const std::string frameMeta=R"({"dynamicWhiteLevel":15424,"dynamicBlackLevel":[1024,1023.5,1023.5,1024]})";
   std::vector<uint8_t>f;le(f,0,8);le(f,0,8);le(f,frameMeta.size(),4);f.insert(f.end(),frameMeta.begin(),frameMeta.end());f.insert(f.end(),payload.begin(),payload.end());chunk(b,"FRAM",f);
   {std::ofstream o(path,std::ios::binary);o.write(reinterpret_cast<const char*>(b.data()),b.size());}
   craw::tools::Clip clip(path.string());const auto raw=clip.frameRaw(0);
   check(raw.width==80&&raw.height==64&&raw.cfa==2&&raw.white==15424&&raw.black[1]==1023.5);
   check(raw.pixels.size()*2==expected.size());
   for(size_t i=0;i<raw.pixels.size();++i)check(raw.pixels[i]==(expected[2*i]|expected[2*i+1]<<8));
   clip.setPackedDng(true);check(clip.dngPixelBytes()==80*64*14/8);
   const auto dng=clip.frameDng(0,4096,1);check(dng.size()>4096);
   // TIFF strip offsets/counts are encoded by the shared DNG builder. Decode
   // every packed pixel so a wrong 10-bit export cannot pass on byte size alone.
   auto u16=[&](size_t p){return unsigned(dng[p])|unsigned(dng[p+1])<<8;};
   auto u32=[&](size_t p){return u16(p)|(uint32_t(u16(p+2))<<16);};
   const size_t ifd=u32(4);size_t strip=0;unsigned bits=0;
   for(unsigned n=0;n<u16(ifd);++n){const size_t p=ifd+2+n*12;if(u16(p)==273)strip=u32(p+8);if(u16(p)==258)bits=u16(p+8);}
   check(bits==14&&strip>0);
   for(size_t i=0;i<raw.pixels.size();++i){unsigned value=0;for(unsigned k=0;k<14;++k){size_t bit=i*14+k;value=(value<<1)|((dng[strip+bit/8]>>(7-bit%8))&1);}check(value==raw.pixels[i]);}
  }
  fs::remove(path);return 0;
 }catch(const std::exception&e){fs::remove(path);std::fprintf(stderr,"%s\n",e.what());return 1;}
}
