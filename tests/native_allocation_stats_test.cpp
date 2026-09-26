#include "../src/load/NativeAllocationStats.hpp"
#include <cassert>
#include <cstdio>
int main(){
 wxl_memory::NativeAllocationStats<8> s;
 for(unsigned i=0;i<8;++i)assert(s.Add(16+i*128,100+i));
 assert(s.count==8 && s.bytes==828);
 assert(!s.Add(2048,50) && s.missed==1 && s.bytes==828);
 s.Remove(16);assert(s.count==7 && s.bytes==728);
 // Probe must traverse tombstones, and duplicate updates must not add entries.
 assert(s.Add(144,999));assert(s.count==7 && s.bytes==1626);
 assert(s.Add(2048,50));assert(s.count==8 && s.bytes==1676);
 s.Remove(99999);assert(s.bytes==1676);
 for(unsigned i=1;i<8;++i)s.Remove(16+i*128);
 s.Remove(2048);assert(s.count==0 && s.bytes==0);
 // All buckets are tombstones; reuse must still succeed.
 assert(s.Add(123456,42));s.Remove(123456);assert(s.count==0 && s.bytes==0);
 assert(!s.Add(0,10) && !s.Add(1,10));
 puts("PASS: collisions, overflow, tombstones, replacement, unknown free, reuse, byte accounting");
}
