#include "modules/XuizTool.h"

#include "core/Bytes.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>

namespace x360::modules {

namespace {

using bytes::Buffer;

struct Header {
    uint32_t version = 0;
    uint32_t declaredSize = 0;
    uint32_t flags = 0;
    uint32_t tableOffset = 0;
    uint16_t entryCount = 0;
    uint16_t field24 = 0;
};

struct Entry {
    size_t offset = 0;
    std::string name;
    uint8_t flags = 0;
    uint32_t meta1 = 0; // raw u32 LE from the 7-byte record trailer
    uint16_t meta2 = 0; // raw u16 BE from the 7-byte record trailer
};

Header parseHeader(const Buffer& d) {
    if (d.size() < 30 || bytes::ascii(d, 0, 4) != "XUIZ") {
        throw std::runtime_error("not an XUIZ/XZP file (bad magic)");
    }
    Header h;
    h.version = bytes::u32be(d, 4);
    h.declaredSize = bytes::u32be(d, 8);
    h.flags = bytes::u32be(d, 12);
    h.tableOffset = bytes::u32be(d, 16);
    h.entryCount = bytes::u16be(d, 20);
    h.field24 = bytes::u16be(d, 24);
    return h;
}

std::vector<Entry> parseEntries(const Buffer& d, const Header& h) {
    std::vector<Entry> out;
    size_t off = 30;
    for (uint16_t i = 0; i < h.entryCount; ++i) {
        if (off + 2 > d.size()) break;
        const uint16_t len = bytes::u16le(d, off);
        if (len == 0 || len > 1024) break;
        const size_t meta = off + 2 + static_cast<size_t>(len) * 2;
        if (meta + 7 > d.size()) break;

        Entry e;
        e.offset = off;
        e.name = bytes::utf16le(d, off + 2, len);
        e.flags = d[meta];
        e.meta1 = bytes::u32le(d, meta + 1);
        e.meta2 = bytes::u16be(d, meta + 5);
        out.push_back(std::move(e));
        off = meta + 7;
    }
    return out;
}

size_t findSignature(const Buffer& d, size_t start, const char* sig, size_t sigLen) {
    if (start >= d.size() || sigLen > d.size() - start) return std::string::npos;
    const auto end = d.end();
    auto it = std::search(d.begin() + static_cast<std::ptrdiff_t>(start), end,
                          reinterpret_cast<const uint8_t*>(sig),
                          reinterpret_cast<const uint8_t*>(sig) + sigLen);
    if (it == end) return std::string::npos;
    return static_cast<size_t>(std::distance(d.begin(), it));
}

size_t findXuib(const Buffer& d, const Header& h) {
    const size_t start = std::min<size_t>(h.tableOffset, d.size());
    return findSignature(d, start, "XUIB", 4);
}

void printHeader(const Header& h, const Buffer& d) {
    std::cout << "Magic          XUIZ\n";
    std::cout << "Version        " << h.version << '\n';
    std::cout << "Declared size  " << h.declaredSize << " (file is " << d.size() << ")\n";
    std::cout << "Flags          0x" << std::hex << h.flags << std::dec << '\n';
    std::cout << "Table offset   " << h.tableOffset << '\n';
    std::cout << "Entries        " << h.entryCount << '\n';
    std::cout << "Field 0x18     " << h.field24 << " (0x" << std::hex << h.field24 << std::dec << ")\n";
    const size_t xuib = findXuib(d, h);
    std::cout << "XUIB section   " << (xuib == std::string::npos ? "not found" : std::to_string(xuib)) << '\n';
}

int cmdInfo(const std::vector<std::string>& a) {
    if (a.size() < 2) {
        std::cerr << "usage: x360tool xuiz info <file.xzp>\n";
        return 1;
    }
    const Buffer d = bytes::readFile(a[1]);
    const Header h = parseHeader(d);
    printHeader(h, d);
    const auto entries = parseEntries(d, h);
    std::cout << "Parsed names   " << entries.size() << '\n';
    return 0;
}

int cmdList(const std::vector<std::string>& a) {
    if (a.size() < 2) {
        std::cerr << "usage: x360tool xuiz list <file.xzp> [name-filter]\n";
        return 1;
    }
    const Buffer d = bytes::readFile(a[1]);
    const Header h = parseHeader(d);
    const auto entries = parseEntries(d, h);
    const std::string filter = a.size() > 2 ? a[2] : std::string{};

    size_t shown = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        if (!filter.empty() && e.name.find(filter) == std::string::npos) continue;
        std::cout << std::dec << i << '\t' << e.meta1 << "\t0x" << std::hex << e.meta2
                  << std::dec << "\t" << e.name << '\n';
        ++shown;
    }
    std::cerr << shown << " of " << entries.size() << " entries\n";
    return 0;
}

int cmdExtract(const std::vector<std::string>& a) {
    if (a.size() < 3) {
        std::cerr << "usage: x360tool xuiz extract <file.xzp> <outdir>\n";
        return 1;
    }
    const Buffer d = bytes::readFile(a[1]);
    const Header h = parseHeader(d);
    const auto entries = parseEntries(d, h);

    std::filesystem::create_directories(a[2]);

    std::string names;
    for (const auto& e : entries) {
        names += e.name + "\t" + std::to_string(e.meta1) + "\n";
    }
    const auto namesPath = std::filesystem::path(a[2]) / "names.txt";
    bytes::writeFile(namesPath.string(), reinterpret_cast<const uint8_t*>(names.data()), names.size());

    const size_t xuib = findXuib(d, h);
    if (xuib != std::string::npos) {
        const auto sectionPath = std::filesystem::path(a[2]) / "section.xuib";
        bytes::writeFile(sectionPath.string(), d.data() + xuib, d.size() - xuib);
        std::cout << "wrote " << sectionPath.string() << " (" << (d.size() - xuib) << " bytes)\n";
    }
    std::cout << "wrote " << namesPath.string() << " (" << entries.size() << " names)\n";
    return 0;
}

int cmdCarve(const std::vector<std::string>& a) {
    if (a.size() < 3) {
        std::cerr << "usage: x360tool xuiz carve <file.xzp> <outdir>\n";
        return 1;
    }
    const Buffer d = bytes::readFile(a[1]);
    const Header h = parseHeader(d);
    size_t pos = findXuib(d, h);
    if (pos == std::string::npos) pos = 0;

    std::filesystem::create_directories(a[2]);
    const char pngSig[] = "\x89PNG\r\n\x1a\n";
    const char ddsSig[] = "DDS ";
    int written = 0;

    while (pos < d.size() && written < 500) {
        const size_t png = findSignature(d, pos, pngSig, 8);
        const size_t dds = findSignature(d, pos, ddsSig, 4);
        const bool usePng = png != std::string::npos && (dds == std::string::npos || png < dds);
        const size_t at = usePng ? png : dds;
        if (at == std::string::npos) break;

        size_t end = std::string::npos;
        std::string ext;
        if (usePng) {
            const size_t iend = findSignature(d, at + 8, "IEND", 4);
            if (iend == std::string::npos || iend + 8 > d.size()) {
                pos = at + 8;
                continue;
            }
            end = iend + 8;
            ext = ".png";
        } else {
            if (at + 8 > d.size()) break;
            const uint32_t size = bytes::u32le(d, at + 4);
            if (size == 0 || size > 64u * 1024u * 1024u || at + 4 + size > d.size()) {
                pos = at + 4;
                continue;
            }
            end = at + 4 + size;
            ext = ".dds";
        }

        const auto path = std::filesystem::path(a[2]) /
                          ("img_" + std::to_string(written) + ext);
        bytes::writeFile(path.string(), d.data() + at, end - at);
        ++written;
        pos = end;
    }
    std::cout << "carved " << written << " images into " << a[2] << '\n';
    return 0;
}

int cmdDump(const std::vector<std::string>& a) {
    if (a.size() < 4) {
        std::cerr << "usage: x360tool xuiz dump <file.xzp> <offset> <length>\n";
        return 1;
    }
    const Buffer d = bytes::readFile(a[1]);
    const size_t off = std::stoul(a[2], nullptr, 0);
    const size_t len = std::stoul(a[3], nullptr, 0);
    std::cout << bytes::hexDump(d, off, len);
    return 0;
}

} // namespace

void XuizTool::printHelp() const {
    std::cout << "  xuiz info   <file.xzp>                 header + section info\n"
                 "  xuiz list   <file.xzp> [filter]        list names (idx, raw1, raw2, name)\n"
                 "  xuiz extract <file.xzp> <outdir>       write names.txt + section.xuib\n"
                 "  xuiz carve  <file.xzp> <outdir>        carve PNG/DDS images from data section\n"
                 "  xuiz dump   <file.xzp> <off> <len>     hex dump\n";
}

int XuizTool::run(const CommandContext& ctx) {
    if (ctx.args.empty()) {
        printHelp();
        return 0;
    }
    const std::string& cmd = ctx.args[0];
    try {
        if (cmd == "info") return cmdInfo(ctx.args);
        if (cmd == "list") return cmdList(ctx.args);
        if (cmd == "extract") return cmdExtract(ctx.args);
        if (cmd == "carve") return cmdCarve(ctx.args);
        if (cmd == "dump") return cmdDump(ctx.args);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
    std::cerr << "unknown xuiz command: " << cmd << '\n';
    printHelp();
    return 1;
}

} // namespace x360::modules
