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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace x360
