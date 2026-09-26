// Compile/run as Win32: the real loader stores 32-bit pointers in M2Array.
#include "../src/load/M2WalkOwned.cpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
#include <cstdlib>
#undef assert
#define assert(x) do { if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);std::exit(1);} } while(false)
using namespace wxl::runtime::m2native::detail;
static_assert(sizeof(void*)==4);
struct Fixture {
    std::array<uint8_t,4096> body{};
    fmt::M2Sequence sequences[2]{};
    Outcome out{};
    fmt::M2Header* h(){return reinterpret_cast<fmt::M2Header*>(body.data());}
    Fixture(){
        h()->events={2,512};out.needsSkel=1;
        for(unsigned i=0;i<2;++i){
            auto* e=body.data()+512+i*36;
            std::memcpy(e,i?"$SHR":"$SHL",4);
            Wr16(e+26,0xFFFF);Wr32(e+28,2);Wr32(e+32,800+i*16);
            auto* a=reinterpret_cast<fmt::M2Array*>(body.data()+800+i*16);
            a[0]={1,1000+i*4};a[1]={1,64}; // external-file offset stays unresolved
            Wr32(body.data()+1000+i*4,250+i);
        }
        sequences[0].flags=0x20;sequences[1].flags=0;
    }
    void splice(){h()->sequences={2,uint32_t(reinterpret_cast<uintptr_t>(sequences))};}
};
int main(int argc,char** argv){
    {
        Fixture f;auto original=f.body;
        assert(WalkHeaderArrays(f.body.data(),f.body.size(),f.h(),f.out));
        assert(f.h()->events.offset==512);
        assert(std::memcmp(f.body.data()+512,original.data()+512,1024)==0);
        f.splice();assert(WalkDeferredEvents(f.body.data(),f.body.size(),f.h(),f.out));
        for(unsigned i=0;i<2;++i){
            auto* e=reinterpret_cast<uint8_t*>(f.h()->events.offset)+36*i;
            assert(std::memcmp(e,i?"$SHR":"$SHL",4)==0);
            auto* a=reinterpret_cast<fmt::M2Array*>(Rd32(e+32));
            assert(*reinterpret_cast<uint32_t*>(a[0].offset)==250+i);
            assert(a[1].offset==64); // never rebase an external sequence against M2
        }
    }
    {
        Fixture f;f.out.needsSkel=0; // no companion: safely discard unknown track timing
        assert(WalkHeaderArrays(f.body.data(),f.body.size(),f.h(),f.out));
        assert(std::memcmp(f.body.data()+548,"$SHR",4)==0); // old code erases this tag
        assert(Rd32(f.body.data()+512+28)==0);
    }
    {
        Fixture f;f.splice();Wr32(f.body.data()+512+32,4090);
        assert(!WalkDeferredEvents(f.body.data(),f.body.size(),f.h(),f.out));
    }
    {
        Fixture f;f.splice();Wr32(f.body.data()+800+4,4094);
        assert(!WalkDeferredEvents(f.body.data(),f.body.size(),f.h(),f.out));
    }
    {
        Fixture f;f.splice();f.h()->events.count=0xFFFFFFFF;
        assert(!WalkDeferredEvents(f.body.data(),f.body.size(),f.h(),f.out));
    }
    {
        Fixture f;assert(!WalkDeferredEvents(f.body.data(),f.body.size(),f.h(),f.out));
    }
    if(argc==3){
        auto read=[](const char* p){std::ifstream s(p,std::ios::binary);assert(s);return std::vector<uint8_t>(std::istreambuf_iterator<char>(s),{});};
        auto model=read(argv[1]),skel=read(argv[2]);
        assert(std::memcmp(model.data(),"MD21",4)==0);
        uint32_t size=Rd32(model.data()+4);assert(size<=model.size()-8);
        auto* base=model.data()+8;auto* h=reinterpret_cast<fmt::M2Header*>(base);
        std::vector<uint8_t> tags;
        for(unsigned i=0;i<h->events.count;++i)tags.insert(tags.end(),base+h->events.offset+i*36,base+h->events.offset+i*36+4);
        Outcome out{};out.needsSkel=1;
        // Exercise the exact deferred event path against installed HD asset bytes.
        bool found=false;
        for(size_t p=0;p+8<=skel.size();){
            uint32_t n=Rd32(skel.data()+p+4);assert(n<=skel.size()-p-8);
            if(std::memcmp(skel.data()+p,"SKS1",4)==0){
                auto* payload=skel.data()+p+8;auto seq=*reinterpret_cast<fmt::M2Array*>(payload+8);
                assert(seq.offset+uint64_t(seq.count)*sizeof(fmt::M2Sequence)<=n);
                h->sequences={seq.count,uint32_t(reinterpret_cast<uintptr_t>(payload+seq.offset))};found=true;
            }
            p+=n+8;
        }
        assert(found);assert(WalkDeferredEvents(base,size,h,out));
        unsigned sheathKeys=0;
        for(unsigned i=0;i<h->events.count;++i){
            auto* e=reinterpret_cast<uint8_t*>(h->events.offset)+i*36;
            assert(std::memcmp(e,tags.data()+i*4,4)==0);
            if(std::memcmp(e,"$SHL",4) && std::memcmp(e,"$SHR",4))continue;
            auto* slots=reinterpret_cast<fmt::M2Array*>(Rd32(e+32));
            auto* seq=reinterpret_cast<fmt::M2Sequence*>(h->sequences.offset);
            for(unsigned j=0;j<Rd32(e+28);++j)if((seq[j].id==89 || seq[j].id==90) && slots[j].count){
                assert(seq[j].flags&0x20);
                auto* time=reinterpret_cast<uint8_t*>(slots[j].offset);
                assert(time>=base && time+4<=base+size);++sheathKeys;
                std::printf("event %.4s animation %u timestamp %u\n",e,seq[j].id,Rd32(time));
            }
        }
        assert(sheathKeys>0);std::printf("PASS: installed HD model retains %u sheath event keys\n",sheathKeys);
    }
    std::puts("PASS: deferred events, no adjacent overwrite, inline/external offsets, malformed bounds and missing skeleton");
}
