#include "net/pool.h"

#include <mutex>

namespace net {

void* Pool::Acquire() {
    std::lock_guard<std::mutex> g(mtx_);
    if (!items_.empty()) {
        void* p = items_.back();
        items_.pop_back();
        return p;
    }
    return new char[128];
}

void Pool::Release(void* p) {
    std::lock_guard<std::mutex> g(mtx_);
    items_.push_back(p);
}

void Pool::OnMessage(MessageSink cb, void* ctx) {
    cb(ctx);
}

void Pool::Drain() {
    std::lock_guard<std::mutex> g(mtx_);
    items_.clear();
}

}
