#include "modules/NandTool.h"

#include "core/Bytes.h"

#include <array>
#include <iostream>
#include <string>

namespace x360::modules {

namespace {

using bytes::Buffer;

uint32_t crc32(const uint8_t* data, size_t len) {
    static std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    }
    return crc ^ 0xffffffffu;
}

std::string sizeLabel(size_t n) {
    if (n == 16u * 1024u * 1024u) return "16 MiB (standard retail NAND)";
    if (n == 512u * 1024u * 1024u) return "512 MiB (large NAND)";
    if (n == 4u * 1024u * 1024u) return "4 MiB";
    return "unusual size";
}

int cmdInfo(const std::vector<std::string>& a) {
    if (a.size() < 2) {
        std::cerr << "usage: x360tool nand info <dump.bin>\n";
        return 1;
    }
    const Buffer d = bytes::readFile(a[1]);
    std::cout << "File            " << a[1] << '\n';
    std::cout << "Size            " << d.size() << " bytes  (" << sizeLabel(d.size()) << ")\n";
    std::cout << "CRC32           0x" << std::hex << crc32(d.data(), d.size()) << std::dec << '\n';

    const size_t block = 0x20000; // 128 KiB NAND block
    if (d.size() >= block) {
        size_t erased = 0;
        for (size_t off = 0; off + block <= d.size(); off += block) {
            bool allFF = true;
            for (size_t i = 0; i < block; i += 256) {
                if (d[off + i] != 0xff) { allFF = false; break; }
            }
            if (allFF) ++erased;
        }
        std::cout << "Blocks          " << (d.size() / block) << " x 128 KiB, ~" << erased << " look erased\n";
    }

    const std::string needles[] = {"XELL", "xell", "dashlaunch", "Aurora", "FSD", "XBOX", "CB", "CD", "CE"};
    std::cout << "\nMARKERS\n";
    for (const auto& needle : needles) {
        size_t count = 0;
        size_t first = std::string::npos;
        for (size_t i = 0; i + needle.size() <= d.size(); ++i) {
            bool hit = true;
            for (size_t j = 0; j < needle.size(); ++j) {
                if (d[i + j] != static_cast<uint8_t>(needle[j])) { hit = false; break; }
            }
            if (hit) {
                if (first == std::string::npos) first = i;
                ++count;
                if (count > 1000) break;
            }
        }
        if (count) {
            std::cout << "  " << needle << "  x" << count;
            if (first != std::string::npos) {
                std::cout << "  first @ 0x" << std::hex << first << std::dec;
            }
            std::cout << '\n';
        }
    }
    return 0;
}

int cmdCompare(const std::vector<std::string>& a) {
    if (a.size() < 3) {
        std::cerr << "usage: x360tool nand compare <flashdmp.bin> <updflash.bin>\n";
        return 1;
    }
    const Buffer x = bytes::readFile(a[1]);
    const Buffer y = bytes::readFile(a[2]);
    std::cout << "sizes: " << x.size() << " vs " << y.size() << '\n';
    const size_t n = std::min(x.size(), y.size());
    size_t diffs = 0;
    size_t first = std::string::npos;
    size_t last = 0;
    for (size_t i = 0; i < n; ++i) {
        if (x[i] != y[i]) {
            ++diffs;
            if (first == std::string::npos) first = i;
            last = i;
        }
    }
    std::cout << "differing bytes: " << diffs << " of " << n << '\n';
    if (first != std::string::npos) {
        std::cout << "first diff: 0x" << std::hex << first << "  last: 0x" << last << std::dec << '\n';
    }
    return 0;
}

int cmdStrings(const std::vector<std::string>& a) {
    if (a.size() < 2) {
        std::cerr << "usage: x360tool nand strings <dump.bin> [count]\n";
        return 1;
    }
    const Buffer d = bytes::readFile(a[1]);
    const size_t max = a.size() > 2 ? std::stoul(a[2]) : 60;
    size_t shown = 0;
    std::string run;
    for (size_t i = 0; i < d.size() && shown < max; ++i) {
        const uint8_t c = d[i];
        if (c >= 0x20 && c <= 0x7e) {
            run.push_back(static_cast<char>(c));
        } else {
            if (run.size() >= 6) {
                std::cout << "0x" << std::hex << (i - run.size()) << std::dec << "  " << run << '\n';
                ++shown;
            }
            run.clear();
        }
    }
    return 0;
}

int cmdDump(const std::vector<std::string>& a) {
    if (a.size() < 4) {
        std::cerr << "usage: x360tool nand dump <dump.bin> <offset> <length>\n";
        return 1;
    }
    const Buffer d = bytes::readFile(a[1]);
    std::cout << bytes::hexDump(d, std::stoul(a[2], nullptr, 0), std::stoul(a[3], nullptr, 0));
    return 0;
}

} // namespace

void NandTool::printHelp() const {
    std::cout << "  nand info    <dump.bin>                     size, CRC32, block map, markers\n"
                 "  nand compare <flashdmp.bin> <updflash.bin>  byte-level diff\n"
                 "  nand strings <dump.bin> [count]             printable strings\n"
                 "  nand dump    <dump.bin> <offset> <length>   hex dump\n";
}

int NandTool::run(const CommandContext& ctx) {
    if (ctx.args.empty()) {
        printHelp();
        return 0;
    }
    const std::string& cmd = ctx.args[0];
    try {
        if (cmd == "info") return cmdInfo(ctx.args);
        if (cmd == "compare") return cmdCompare(ctx.args);
        if (cmd == "strings") return cmdStrings(ctx.args);
        if (cmd == "dump") return cmdDump(ctx.args);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
    std::cerr << "unknown nand command: " << cmd << '\n';
    printHelp();
    return 1;
}

} // namespace x360::modules
