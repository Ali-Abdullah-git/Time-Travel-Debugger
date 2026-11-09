#pragma once

#include <string>
#include <cstdint>
#include <cstddef>

// Helper.h should contain the structs, functions, constants, etc. that are used more than one time in the project. For example, the structs that are used in both server and client, the constants that are used in all three files. 

struct FuncEntry
{
    std::string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    std::string targetFuncName;
};

class Crc32
{
    uint32_t state;
 
public:
    Crc32() : state(0xFFFFFFFF)
    {
    }
    void update(const void *data, size_t n)
    {
        const unsigned char *bytes = (const unsigned char *)data;
        for (size_t i = 0; i < n; i++) {
            state ^= bytes[i];
            for (int bit = 0; bit < 8; bit++) {
                if (state & 1)
                    state = (state >> 1) ^ 0xEDB88320;
                else
                    state = state >> 1;
            }
        }
    }
    uint32_t value() const
    {
        return state ^ 0xFFFFFFFF;
    }
};
 
inline void crcToBytes(uint32_t value, unsigned char out[4])
{
    out[0] = (unsigned char)(value >> 24);
    out[1] = (unsigned char)(value >> 16);
    out[2] = (unsigned char)(value >> 8);
    out[3] = (unsigned char)value;
}
 
// The receiver cannot know where the body ends until the stream ends, so it always
// holds back the last 4 bytes. feed() gives back the bytes that are surely part of the
// message. When the stream is over, the 4 bytes still held are the sender's CRC.
class TrailerChecker
{
    Crc32 crc;
    std::string pending;
 
public:
    std::string feed(const char *data, size_t n)
    {
        pending.append(data, n);
        if (pending.size() <= 4)
            return "";
        std::string released = pending.substr(0, pending.size() - 4);
        pending = pending.substr(pending.size() - 4);
        crc.update(released.data(), released.size());
        return released;
    }
    bool verify() const
    {
        if (pending.size() != 4)
            return false;
        unsigned char expected[4];
        crcToBytes(crc.value(), expected);
        return std::string((const char *)expected, 4) == pending;
    }
    const std::string &trailer() const
    {
        return pending;
    }
};

