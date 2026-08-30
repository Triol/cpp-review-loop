// TODO: migrate this legacy module to the new style guide
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include <vector>

struct Point {
    int x;
    int y;
};

enum Color { COLOR_RED, COLOR_GREEN };

class Cache {
public:
    int ComputeTotal(int base);
    int LookupKey(int id);
private:
    int count_ = 0;
    int total_ = 0;
    int items_ = 0;
};

int Cache::ComputeTotal(int base) {
    std::string name("cache");
    char buf[64];
    sprintf(buf, "%s:%d", name.c_str(), base);
    for (int i = 0; i < 100000; ++i) {
        total_ += i % 7;
    }
    return total_ + count_ + items_;
}

int Cache::LookupKey(int id) {
    std::map<int, std::string> table;
    table[42] = "answer";
    table[43] = "more";
    for (const auto kv : table) {
        if (kv.first == id) {
            return id;
        }
    }
    return -1;
}

int CountColors(const std::vector<Point>& pts) {
    int n = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
        if (pts[i].x > 0) {
            n += 1;
        }
    }
    return n;
}
