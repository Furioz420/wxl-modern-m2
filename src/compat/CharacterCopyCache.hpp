#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <vector>
#include <list>
#include <memory>
#include <mutex>
#include <cstdint>
#include <cstring>
#pragma comment(lib, "bcrypt.lib")

namespace wxl_copy_cache {
using Key = std::array<uint8_t,32>;
using Bytes = std::vector<uint8_t>;
constexpr size_t kBudget = 64u * 1024u * 1024u;
constexpr size_t kMaxEntry = 16u * 1024u * 1024u;

// A failed native texture read is a cache miss, not a reason to publish a
// partially captured entry. Keep SEH inside a POD leaf, outside C++ ownership.
inline bool Copy(void* to,const void* from,size_t bytes) noexcept {
    __try { std::memcpy(to,from,bytes);return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
class Digest {
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    Bytes object_;
public:
    Digest() {
        ULONG size=0,received=0;
        if(BCryptOpenAlgorithmProvider(&algorithm_,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return;
        if(BCryptGetProperty(algorithm_,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&received,0)<0)return;
        try { object_.resize(size); }
        catch(...) { BCryptCloseAlgorithmProvider(algorithm_,0);algorithm_=nullptr;return; }
        if(BCryptCreateHash(algorithm_,&hash_,object_.data(),size,nullptr,0,0)<0)hash_=nullptr;
    }
    ~Digest(){if(hash_)BCryptDestroyHash(hash_);if(algorithm_)BCryptCloseAlgorithmProvider(algorithm_,0);}
    Digest(const Digest&)=delete;
    Digest& operator=(const Digest&)=delete;
    bool Add(const void* data,size_t bytes) {
        return hash_ && bytes<=0xffffffffu && BCryptHashData(hash_,static_cast<PUCHAR>(const_cast<void*>(data)),ULONG(bytes),0)>=0;
    }
    bool Finish(Key& key){return hash_ && BCryptFinishHash(hash_,key.data(),ULONG(key.size()),0)>=0;}
};

class Cache {
    struct Entry {Key key;std::shared_ptr<const Bytes> bytes;};
    std::list<Entry> entries_;
    std::mutex mutex_;
    size_t bytes_=0,budget_,limit_;
public:
    explicit Cache(size_t budget=kBudget,size_t limit=kMaxEntry):budget_(budget),limit_(limit){}
    std::shared_ptr<const Bytes> Find(const Key& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        for(auto it=entries_.begin();it!=entries_.end();++it)if(it->key==key){
            auto result=it->bytes;entries_.splice(entries_.begin(),entries_,it);return result;
        }
        return {};
    }
    void Store(const Key& key,std::shared_ptr<const Bytes> data) {
        if(!data || data->empty() || data->size()>limit_ || data->size()>budget_)return;
        std::lock_guard<std::mutex> lock(mutex_);
        for(const auto& entry:entries_)if(entry.key==key)return;
        // Allocate the list node before accounting so a failed allocation leaves
        // the existing cache consistent and the caller can simply draw uncached.
        entries_.push_front({key,data});bytes_+=data->size();
        while(bytes_>budget_ || entries_.size()>256){bytes_-=entries_.back().bytes->size();entries_.pop_back();}
    }
    size_t Size(){std::lock_guard<std::mutex> lock(mutex_);return bytes_;}
};
inline Cache& Local(){static Cache cache;return cache;}
}
