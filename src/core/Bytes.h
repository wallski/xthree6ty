#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace x360::bytes {

using Buffer = std::vector<uint8_t>;

inline void require(const Buffer& d, size_t offset, size_t length) {
    if (offset > d.size() || length > d.size() - offset) {
        throw std::out_of_range("read past end of buffer");
    }
}

inline uint16_t u16le(const Buffer& d, size_t o) {
    require(d, o, 2);
    return static_cast<uint16_t>(d[o] | (static_cast<uint16_t>(d[o + 1]) << 8));
}

inline uint16_t u16be(const Buffer& d, size_t o) {
    require(d, o, 2);
    return static_cast<uint16_t>((static_cast<uint16_t>(d[o]) << 8) | d[o + 1]);
}

inline uint32_t u32le(const Buffer& d, size_t o) {
    require(d, o, 4);
    return static_cast<uint32_t>(d[o]) |
           (static_cast<uint32_t>(d[o + 1]) << 8) |
           (static_cast<uint32_t>(d[o + 2]) << 16) |
           (static_cast<uint32_t>(d[o + 3]) << 24);
}

inline uint32_t u32be(const Buffer& d, size_t o) {
    require(d, o, 4);
    return (static_cast<uint32_t>(d[o]) << 24) |
           (static_cast<uint32_t>(d[o + 1]) << 16) |
           (static_cast<uint32_t>(d[o + 2]) << 8) |
           static_cast<uint32_t>(d[o + 3]);
}

inline std::string ascii(const Buffer& d, size_t o, size_t n) {
    require(d, o, n);
    std::string s;
    s.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t c = d[o + i];
        s.push_back(c >= 0x20 && c <= 0x7e ? static_cast<char>(c) : '.');
    }
    return s;
}

inline std::string utf16le(const Buffer& d, size_t o, size_t charCount) {
    require(d, o, charCount * 2);
    std::string out;
    out.reserve(charCount);
    for (size_t i = 0; i < charCount; ++i) {
        const uint16_t c = u16le(d, o + i * 2);
        if (c == 0) break;
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('?');
        }
    }
    return out;
}

inline std::string hex(uint64_t v, int width = 8) {
    static const char* digits = "0123456789ABCDEF";
    std::string s(width, '0');
    for (int i = width - 1; i >= 0; --i) {
        s[static_cast<size_t>(i)] = digits[v & 0xf];
        v >>= 4;
    }
    return "0x" + s;
}

inline std::string hexDump(const Buffer& d, size_t offset, size_t length) {
    require(d, offset, length);
    std::string out;
    char line[80];
    for (size_t i = 0; i < length; i += 16) {
        const size_t row = std::min<size_t>(16, length - i);
        int n = std::snprintf(line, sizeof(line), "%08zX  ", offset + i);
        out.append(line, static_cast<size_t>(n));
        for (size_t j = 0; j < 16; ++j) {
            if (j < row) {
                n = std::snprintf(line, sizeof(line), "%02X ", d[offset + i + j]);
            } else {
                n = std::snprintf(line, sizeof(line), "   ");
            }
            out.append(line, static_cast<size_t>(n));
        }
        out.push_back(' ');
        for (size_t j = 0; j < row; ++j) {
            const uint8_t c = d[offset + i + j];
            out.push_back(c >= 0x20 && c <= 0x7e ? static_cast<char>(c) : '.');
        }
        out.push_back('\n');
    }
    return out;
}

inline Buffer readFile(const std::string& path) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) {
        throw std::runtime_error("cannot open file: " + path);
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(f);
        throw std::runtime_error("cannot determine file size: " + path);
    }
    Buffer data(static_cast<size_t>(size));
    if (!data.empty() && std::fread(data.data(), 1, data.size(), f) != data.size()) {
        std::fclose(f);
        throw std::runtime_error("short read: " + path);
    }
    std::fclose(f);
    return data;
}

inline void writeFile(const std::string& path, const uint8_t* data, size_t size) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) {
        throw std::runtime_error("cannot create file: " + path);
    }
    if (size && std::fwrite(data, 1, size, f) != size) {
        std::fclose(f);
        throw std::runtime_error("short write: " + path);
    }
    std::fclose(f);
}

} // namespace x360::bytes
