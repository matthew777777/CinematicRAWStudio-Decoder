// SPDX-License-Identifier: Apache-2.0
// Lossless desktop compaction. Keep opaque chunks, timestamps and JSON exact.
#include <craw/Reader.h>
#include <craw/Encoder.h>
#include "../src/Crc32c.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
namespace fs=std::filesystem;
static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static void put(std::vector<uint8_t>& v,uint64_t n,int bytes){for(int i=0;i<bytes;++i)v.push_back(uint8_t(n>>(8*i)));}
struct Output {
    fs::path temporary; FILE* f=nullptr; uint64_t pos=0;
    explicit Output(const fs::path& path){
        std::random_device rng;
        for(int i=0;i<32&&!f;++i){temporary=path.string()+".partial."+std::to_string(rng());f=std::fopen(temporary.string().c_str(),"wbx");}
        require(f!=nullptr,"cannot create output temporary file");
    }
    ~Output(){if(f)std::fclose(f);std::error_code ec;fs::remove(temporary,ec);}
    void write(const void* p,size_t n){require(std::fwrite(p,1,n,f)==n,"output write failed");pos+=n;}
    void finish(const fs::path& path){const int result=std::fclose(f);f=nullptr;require(result==0,"output close failed");
        // Hard-link publication is atomic and refuses an existing destination.
        fs::create_hard_link(temporary,path);fs::remove(temporary);}
};
int main(int argc,char**argv){
    if(argc!=3){std::fprintf(stderr,"usage: craw_compact in.craw out.craw\n");return 2;}
    try{
        const fs::path source=fs::canonical(argv[1]),destination=fs::absolute(argv[2]);
        require(!fs::exists(destination),"output already exists; choose a new path");
        craw::Reader reader(source.string());
        require(!reader.recovered(),"input is recovered/incomplete; repair it before compaction");
        for(const auto& c:reader.chunks())
            require(std::memcmp(c.fourcc.data(),"META",4)!=0,"input requires recovery; repair it before compaction");
        std::ifstream input(source,std::ios::binary);
        input.seekg(-8,std::ios::end);char endMagic[8];input.read(endMagic,8);
        require(bool(input)&&std::memcmp(endMagic,"CRAWEND\0",8)==0,"input has no complete trailer");
        Output output(destination);
        auto copy=[&](uint64_t offset,uint64_t bytes){
            std::array<char,65536> buf{};input.clear();input.seekg(std::streamoff(offset));
            while(bytes){size_t n=size_t(std::min<uint64_t>(bytes,buf.size()));input.read(buf.data(),std::streamsize(n));require(bool(input),"input read failed");output.write(buf.data(),n);bytes-=n;}
        };
        copy(0,16);
        input.clear();input.seekg(24);uint8_t metaLength[8];input.read(reinterpret_cast<char*>(metaLength),8);require(bool(input),"cannot read META length");
        uint64_t metaBytes=0;for(int i=0;i<8;++i)metaBytes|=uint64_t(metaLength[i])<<(8*i);
        craw::ChunkRef meta{{'M','E','T','A'},16,metaBytes,0};
        require(reader.crcOk(meta),"input META CRC mismatch");copy(16,metaBytes+20);
        std::vector<uint8_t> index;uint32_t count=0;size_t frame=0;
        std::vector<uint16_t> raw,back;std::vector<uint8_t> encoded;
        auto chunk=[&](const char* tag,uint32_t flags,const std::vector<uint8_t>& p){
            std::vector<uint8_t> head(tag,tag+4);put(head,flags,4);put(head,p.size(),8);output.write(head.data(),head.size());output.write(p.data(),p.size());
            std::vector<uint8_t> tail;put(tail,craw::detail::crc32c(p.data(),p.size()),4);output.write(tail.data(),tail.size());
        };
        for(const auto& c:reader.chunks()){
            if(std::memcmp(c.fourcc.data(),"INDX",4)==0)continue;
            input.clear();input.seekg(std::streamoff(c.offset));uint8_t header[16];input.read(reinterpret_cast<char*>(header),16);
            require(bool(input),"input chunk header read failed");
            uint64_t chunkBytes=0;for(int i=0;i<8;++i)chunkBytes|=uint64_t(header[8+i])<<(8*i);
            require(std::memcmp(header,c.fourcc.data(),4)==0&&chunkBytes==c.size,"input chunk header disagrees with index");
            require(reader.crcOk(c),"input chunk CRC mismatch");
            if(std::memcmp(c.fourcc.data(),"META",4)!=0){index.insert(index.end(),c.fourcc.begin(),c.fourcc.end());put(index,0,4);put(index,output.pos,8);put(index,uint64_t(c.timestampNs),8);++count;}
            if(std::memcmp(c.fourcc.data(),"FRAM",4)!=0){copy(c.offset,c.size+20);continue;}
            auto rec=reader.frame(frame++);
            auto info=craw::decodeFrame(rec.payload,rec.payloadSize,raw,1);
            craw::FrameDesc d;d.width=info.width;d.height=info.height;d.bitDepth=info.bitDepth;d.cfa=info.cfa;d.tile=info.tile;d.riceInit=info.riceInit;d.codec=4;d.frameCrc=true;
            craw::encodeFrame(raw.data(),d.width,d,encoded,1);
            craw::decodeFrame(encoded.data(),encoded.size(),back,1);
            require(raw==back,"compact round trip mismatch");
            // Keep the source payload when this frame cannot become smaller.
            const bool smaller=encoded.size()<rec.payloadSize;
            auto payload=reader.payload(c);
            const size_t prefix=size_t(reinterpret_cast<const char*>(rec.payload)-payload.data());
            std::vector<uint8_t> p(payload.begin(),payload.begin()+prefix);
            if(smaller)p.insert(p.end(),encoded.begin(),encoded.end());else p.insert(p.end(),rec.payload,rec.payload+rec.payloadSize);
            // Original chunk flags are opaque and must survive too.
            const uint32_t flagsValue=uint32_t(header[4])|(uint32_t(header[5])<<8)|(uint32_t(header[6])<<16)|(uint32_t(header[7])<<24);
            chunk("FRAM",flagsValue,p);
            std::fprintf(stderr,"\r%zu/%zu frames",frame,reader.frameCount());
        }
        std::vector<uint8_t> ix;put(ix,count,4);ix.insert(ix.end(),index.begin(),index.end());
        const uint64_t ixOffset=output.pos;chunk("INDX",0,ix);
        std::vector<uint8_t> trailer;put(trailer,ixOffset,8);const char magic[8]={'C','R','A','W','E','N','D',0};trailer.insert(trailer.end(),magic,magic+8);output.write(trailer.data(),trailer.size());
        const uint64_t bytes=output.pos;output.finish(destination);
        std::fprintf(stderr,"\n%zu frames; %ju -> %llu bytes\n",frame,uintmax_t(fs::file_size(source)),static_cast<unsigned long long>(bytes));return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"craw compact: %s\n",e.what());return 1;}
}
