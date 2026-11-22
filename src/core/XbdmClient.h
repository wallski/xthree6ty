#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace x360 {

using ProgressFn = std::function<void(size_t, size_t)>;

struct DirEntry {
    std::string name;
    uint64_t size = 0;
    bool isDirectory = false;
    uint32_t changeHi = 0;
    uint32_t changeLo = 0;
};

// One entry from the MODULES command.
struct ModuleInfo {
    std::string name;
    uint32_t base = 0;
    uint32_t size = 0;
};

// One entry from the WALKMEM command: a mapped virtual-address range.
struct MemRegion {
    uint32_t base = 0;
    uint32_t size = 0;
    uint32_t protect = 0;
};

class XbdmClient {
public:
    XbdmClient();
    ~XbdmClient();
    XbdmClient(const XbdmClient&) = delete;
    XbdmClient& operator=(const XbdmClient&) = delete;

    bool connect(const std::string& host, uint16_t port, std::string& error,
                 const std::string& localIp = {});
    bool connected() const;
    void close();

    std::vector<std::string> banner(std::string& error);
    std::vector<std::string> command(const std::string& cmd, std::string& error);
    bool listDir(const std::string& path, std::vector<DirEntry>& out, std::string& error);
    bool makeDir(const std::string& remote, std::string& error);
    bool rename(const std::string& from, const std::string& to, std::string& error);
    bool remove(const std::string& remote, bool isDirectory, std::string& error);
    bool deleteTree(const std::string& remoteDir, std::string& error, size_t* deleted = nullptr);
    bool download(const std::string& remote, const std::string& local, std::string& error,
                  uint64_t* bytes = nullptr, ProgressFn progress = {});
    bool screenshot(const std::string& localPng, std::string& error,
                    int* width = nullptr, int* height = nullptr, ProgressFn progress = {});
    bool downloadTree(const std::string& remoteDir, const std::string& localDir,
                      std::string& error, size_t* fileCount, uint64_t* totalBytes,
                      ProgressFn progress = {});
    bool upload(const std::string& local, const std::string& remote, std::string& error,
                uint64_t* bytes = nullptr, ProgressFn progress = {});

    // Fetch only the first `maxBytes` of a remote file. This build of XBDM has
    // no ranged GETFILE, so the file is streamed and the transfer is stopped
    // once enough has arrived; the underlying connection is closed afterwards
    // and this client is not reusable. Used to read XEX headers without pulling
    // multi-gigabyte game files.
    bool readFilePrefix(const std::string& remote, size_t maxBytes, std::vector<uint8_t>& out,
                        std::string& error);

    // Read/write target memory. Reads use GETMEMEX (raw binary); writes use
    // SETMEM (hex payload, 128 bytes per command, as the console requires).
    bool getMemory(uint32_t address, uint32_t length, std::vector<uint8_t>& out,
                   std::string& error);
    bool setMemory(uint32_t address, const std::vector<uint8_t>& data, std::string& error);

    // Target introspection, used by the trainer to pick scan ranges.
    bool listModules(std::vector<ModuleInfo>& out, std::string& error);
    bool listMemoryRegions(std::vector<MemRegion>& out, std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace x360
