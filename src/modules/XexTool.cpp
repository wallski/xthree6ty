#include "modules/XexTool.h"

#include "core/Bytes.h"

#include <algorithm>
#include <filesystem>
#include <iostream>

namespace x360::modules {

namespace {

using bytes::Buffer;

struct XexHeader {
    uint32_t moduleFlags = 0;
    uint32_t imageFlags = 0;
    uint32_t headerSize = 0;
    uint32_t optionalCount = 0;
};

XexHeader parseHeader(const Buffer& d) {
    if (d.size() < 24 || bytes::ascii(d, 0, 4) != "XEX2") {
        throw std::runtime_error("not an XEX2 file (bad magic)");
    }
    XexHeader h;
    h.moduleFlags = bytes::u32be(d, 4);
    h.imageFlags = bytes::u32be(d, 8);
    h.headerSize = bytes::u32be(d, 16);
    h.optionalCount = bytes::u32be(d, 20);
    return h;
}

void printStrings(const Buffer& d, size_t maxStrings) {
    size_t shown = 0;
    std::string run;
    for (size_t i = 0; i < d.size() && shown < maxStrings; ++i) {
        const uint8_t c = d[i];
        if (c >= 0x20 && c <= 0x7e) {
            run.push_back(static_cast<char>(c));
        } else {
            if (run.size() >= 5) {
                std::cout << "  0x" << std::hex << (i - run.size()) << std::dec << "  " << run << '\n';
                ++shown;
            }
            run.clear();
        }
    }
}

int cmdInfo(const std::vector<std::string>& a) {
    if (a.size() < 2) {
        std::cerr << "usage: x360tool xex info <file.xex>\n";
        return 1;
    }
    const Buffer d = bytes::readFile(a[1]);
    const XexHeader h = parseHeader(d);

    std::cout << "Magic           XEX2\n";
    std::cout << "Module flags    " << h.moduleFlags << " (0x" << std::hex << h.moduleFlags << std::dec << ")\n";
    std::cout << "Image flags     0x" << std::hex << h.imageFlags << std::dec << '\n';
    std::cout << "Header size     " << h.headerSize << " (0x" << std::hex << h.headerSize << std::dec << ")\n";
    std::cout << "Optional headers " << h.optionalCount << '\n';
    std::cout << "File size       " << d.size() << '\n';

    const uint64_t optBytes = static_cast<uint64_t>(h.optionalCount) * 8;
    if (24 + optBytes <= d.size()) {
        std::cout << "\nOPTIONAL HEADERS (id, sub, raw value)\n";
        for (uint32_t i = 0; i < h.optionalCount; ++i) {
            const size_t off = 24 + static_cast<size_t>(i) * 8;
            const uint32_t key = bytes::u32be(d, off);
            const uint32_t value = bytes::u32be(d, off + 4);
            const uint32_t id = key >> 16;
            const uint32_t sub = key & 0xffff;
            std::cout << "  " << i << "\tid=" << std::hex << id << "\tsub=0x" << sub
                      << "\tvalue=0x" << value << std::dec;
            if (value > 0 && value < d.size()) {
                std::cout << "\t[offset 0x" << std::hex << value << std::dec << "]";
            }
            std::cout << '\n';
        }
    }

    const size_t sec = std::min<size_t>(h.headerSize, d.size());
    if (sec + 64 <= d.size()) {
        std::cout << "\nSECURITY INFO AREA (first 64 bytes at header size)\n";
        std::cout << bytes::hexDump(d, sec, 64);
    }
    return 0;
}

int cmdStrings(const std::vector<std::string>& a) {
    if (a.size() < 2) {
        std::cerr << "usage: x360tool xex strings <file.xex> [count]\n";
        return 1;
    }
    const Buffer d = bytes::readFile(a[1]);
    const size_t max = a.size() > 2 ? std::stoul(a[2]) : 60;
    printStrings(d, max);
    return 0;
}

int cmdDump(const std::vector<std::string>& a) {
    if (a.size() < 4) {
        std::cerr << "usage: x360tool xex dump <file.xex> <offset> <length>\n";
        return 1;
    }
    const Buffer d = bytes::readFile(a[1]);
    const size_t off = std::stoul(a[2], nullptr, 0);
    const size_t len = std::stoul(a[3], nullptr, 0);
    std::cout << bytes::hexDump(d, off, len);
    return 0;
}

} // namespace

void XexTool::printHelp() const {
    std::cout << "  xex info    <file.xex>            header + optional headers + security area\n"
                 "  xex strings <file.xex> [count]    printable strings (PE name, imports, ...)\n"
                 "  xex dump    <file.xex> <off> <len> hex dump\n";
}

int XexTool::run(const CommandContext& ctx) {
    if (ctx.args.empty()) {
        printHelp();
        return 0;
    }
    const std::string& cmd = ctx.args[0];
    try {
        if (cmd == "info") return cmdInfo(ctx.args);
        if (cmd == "strings") return cmdStrings(ctx.args);
        if (cmd == "dump") return cmdDump(ctx.args);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
    std::cerr << "unknown xex command: " << cmd << '\n';
    printHelp();
    return 1;
}

} // namespace x360::modules
