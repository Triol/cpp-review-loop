// Message buffer pool for the netstack.
#pragma once

#include <mutex>
#include <vector>

namespace net {

using MessageSink = void (*)(void* ctx);

class Pool {
public:
    void* Acquire();
    void Release(void* p);
    void OnMessage(MessageSink cb, void* ctx);
    void Drain();
private:
    std::vector<void*> items_;
    std::mutex mtx_;
};

}
