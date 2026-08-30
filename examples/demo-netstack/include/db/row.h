#pragma once

#include <memory>

namespace db {

class Row {
public:
    virtual ~Row();
    virtual int Width() const = 0;
};

class Record {
public:
    virtual ~Record() = default;
    virtual int Width() const;
    int kind_;
};

std::unique_ptr<Record> MakeRecord(int kind);

}
