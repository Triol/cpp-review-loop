#include "db/row.h"

#include <memory>

namespace db {

int Record::Width() const { return 0; }

std::unique_ptr<Record> MakeRecord(int kind) {
    if (kind != 0 && kind != 1) {
        return nullptr;
    }
    auto r = std::make_unique<Record>();
    r->kind_ = kind;
    return r;
}

char* FindSlot(char* const* slots, int n) {
    for (int i = 0; i < n; ++i) {
        if (slots[i][0] == 'A') {
            return slots[i];
        }
    }
    return nullptr;
}

}
