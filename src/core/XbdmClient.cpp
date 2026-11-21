#include "core/XbdmClient.h"
#include "core/Bytes.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <cwchar>

#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <gdiplus.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdiplus.lib")

namespace x360 {

using namespace Gdiplus;

namespace {

bool waitSocket(SOCKET s, int seconds, bool forWrite) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    TIMEVAL tv{seconds, 0};
    const int r = ::select(0, forWrite ? nullptr : &set, forWrite ? &set : nullptr, nullptr, &tv);
    return r > 0 && FD_ISSET(s, &set);
}

std::string lastSockError() {
    const int code = ::WSAGetLastError();
    return "WSA " + std::to_string(code);
}

bool sameSubnet(const std::string& a, const std::string& b) {
    const auto da = a.rfind('.');
    const auto db = b.rfind('.');
    if (da == std::string::npos || db == std::string::npos) return false;
    return a.substr(0, da) == b.substr(0, db);
}

std::vector<std::string> localAddresses() {
    std::vector<std::string> result;
    char name[256]{};
    if (::gethostname(name, sizeof(name)) != 0) return result;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    if (::getaddrinfo(name, nullptr, &hints, &list) != 0) return result;
    for (auto* ai = list; ai; ai = ai->ai_next) {
        char buf[INET_ADDRSTRLEN]{};
        auto* sin = reinterpret_cast<sockaddr_in*>(ai->ai_addr);
        if (::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) {
            const std::string ip = buf;
            if (ip != "127.0.0.1" && std::find(result.begin(), result.end(), ip) == result.end()) {
                result.push_back(ip);
            }
        }
    }
    ::freeaddrinfo(list);
    return result;
}

std::wstring utf8ToWide(const std::string& text) {
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring result(static_cast<size_t>(size > 0 ? size - 1 : 0), L'\0');
    if (size > 0) ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), size - 1);
    return result;
}

bool pngEncoderClsid(CLSID* clsid) {
    UINT count = 0;
    UINT size = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &size) != Ok || count == 0) return false;
    std::vector<uint8_t> buffer(size);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    if (Gdiplus::GetImageEncoders(count, size, encoders) != Ok) return false;
    for (UINT i = 0; i < count; ++i) {
        if (std::wcscmp(encoders[i].MimeType, L"image/png") == 0) {
            *clsid = encoders[i].Clsid;
            return true;
        }
    }
    return false;
}

uint32_t hex32(const std::string& text, const std::string& key) {
    const auto at = text.find(key);
    if (at == std::string::npos) return 0;
    const auto start = at + key.size();
    if (start >= text.size() || text[start] != '0') return 0;
    if (start + 1 < text.size() && (text[start + 1] == 'x' || text[start + 1] == 'X')) {
        return static_cast<uint32_t>(std::strtoul(text.c_str() + start + 2, nullptr, 16));
    }
    return 0;
}

// Xenos (Xbox 360 GPU) 2D tiling: the front buffer is not stored row-by-row but
// in 32x32 macro-tiles split into 8x8 micro-tiles, with a swizzled address
// layout. This maps a pixel at (x, y) on a tiled surface `surfaceWidth` pixels
// wide back to its linear pixel index. Uses 32-bit (4-byte) pixels, so the byte
// offset is the returned index * 4. Equivalent to the official
// XGAddress2DTiledOffset().
uint32_t xenos2DTiledOffset(uint32_t x, uint32_t y, uint32_t surfaceWidth) {
    constexpr uint32_t logBpp = 2; // log2(bytes per pixel) for 32-bit formats
    const uint32_t alignedWidth = (surfaceWidth + 31u) & ~31u;
    const uint32_t macro = ((x >> 5) + (y >> 5) * (alignedWidth >> 5)) << (logBpp + 7);
    const uint32_t micro = (((x & 7u) + ((y & 6u) << 2)) << logBpp);
    const uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15u) +
                            ((y & 8u) << (3 + logBpp)) + ((y & 1u) << 4);
    return (((offset & ~511u) << 3) + ((offset & 448u) << 2) + (offset & 63u) +
            ((y & 16u) << 7) + (((((y & 8u) >> 2) + (x >> 3)) & 3u) << 6)) >> logBpp;
}

} // namespace

struct XbdmClient::Impl {
    SOCKET socket = INVALID_SOCKET;
    bool wsaStarted = false;
    std::string buffer;

    ~Impl() { close(); }

    void close() {
        if (socket != INVALID_SOCKET) {
            ::closesocket(socket);
            socket = INVALID_SOCKET;
        }
        if (wsaStarted) {
            ::WSACleanup();
            wsaStarted = false;
        }
        buffer.clear();
    }

    bool sendAll(const char* data, size_t len, std::string& error, const ProgressFn& progress = {}) {
        size_t done = 0;
        while (done < len) {
            const int n = ::send(socket, data + done, static_cast<int>(std::min<size_t>(len - done, 64 * 1024)), 0);
            if (n <= 0) {
                error = "send failed (" + lastSockError() + ")";
                return false;
            }
            done += static_cast<size_t>(n);
            if (progress) progress(done, len);
        }
        return true;
    }

    bool recvExact(uint8_t* dest, size_t length, std::string& error, const ProgressFn& progress = {}) {
        size_t done = 0;
        while (done < length) {
            if (!buffer.empty()) {
                const size_t take = std::min(buffer.size(), length - done);
                std::memcpy(dest + done, buffer.data(), take);
                buffer.erase(0, take);
                done += take;
                if (progress) progress(done, length);
                continue;
            }
            if (!waitSocket(socket, 10, false)) {
                error = "timeout receiving data";
                return false;
            }
            const int n = ::recv(socket, reinterpret_cast<char*>(dest + done),
                                 static_cast<int>(length - done), 0);
            if (n <= 0) {
                error = "connection closed";
                return false;
            }
            done += static_cast<size_t>(n);
            if (progress) progress(done, length);
        }
        return true;
    }

    bool nextLine(std::string& line, std::string& error, int timeoutSeconds) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
        while (true) {
            const size_t nl = buffer.find('\n');
            if (nl != std::string::npos) {
                line = buffer.substr(0, nl);
                buffer.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return true;
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0 || !waitSocket(socket, static_cast<int>(remaining), false)) {
                error = "no response from console";
                return false;
            }
            char chunk[8192];
            const int n = ::recv(socket, chunk, sizeof(chunk), 0);
            if (n <= 0) {
                error = "connection closed";
                return false;
            }
            buffer.append(chunk, static_cast<size_t>(n));
        }
    }

    // A 202 multiline response ends with a "." line, sometimes followed by a
    // final status line. Consume any immediately available trailing status so
    // it cannot desync the next command on this connection.
    void drainTrailingStatus() {
        if (!waitSocket(socket, 0, false)) return;
        char chunk[512];
        const int n = ::recv(socket, chunk, sizeof(chunk), 0);
        if (n > 0) buffer.append(chunk, static_cast<size_t>(n));
    }
};

XbdmClient::XbdmClient() : impl_(std::make_unique<Impl>()) {}
XbdmClient::~XbdmClient() = default;

bool XbdmClient::connect(const std::string& host, uint16_t port, std::string& error,
                         const std::string& localIp) {
    impl_->close();
    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        error = "WSAStartup failed";
        return false;
    }
    impl_->wsaStarted = true;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const std::string portStr = std::to_string(port);
    if (::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result) != 0) {
        error = "cannot resolve " + host;
        impl_->close();
        return false;
    }

    std::vector<std::string> candidates;
    if (!localIp.empty()) {
        candidates.push_back(localIp);
    } else {
        for (const auto& ip : localAddresses()) {
            if (sameSubnet(ip, host)) candidates.push_back(ip);
        }
    }
    if (candidates.empty()) candidates.push_back("0.0.0.0");

    std::string lastError;
    for (const auto& local : candidates) {
        SOCKET s = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (s == INVALID_SOCKET) {
            lastError = "socket() failed";
            continue;
        }
        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_port = 0;
        ::inet_pton(AF_INET, local.c_str(), &bindAddr.sin_addr);
        if (::bind(s, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
            ::closesocket(s);
            lastError = "bind to " + local + " failed";
            continue;
        }

        u_long nonblocking = 1;
        ::ioctlsocket(s, FIONBIO, &nonblocking);
        ::connect(s, result->ai_addr, static_cast<int>(result->ai_addrlen));
        if (!waitSocket(s, 2, true)) {
            ::closesocket(s);
            lastError = "timeout via local " + local;
            continue;
        }
        int soError = 0;
        int len = sizeof(soError);
        ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &len);
        if (soError != 0) {
            ::closesocket(s);
            lastError = "failed via local " + local + " (WSA " + std::to_string(soError) + ")";
            continue;
        }
        nonblocking = 0;
        ::ioctlsocket(s, FIONBIO, &nonblocking);
        impl_->socket = s;
        ::freeaddrinfo(result);
        return true;
    }

    ::freeaddrinfo(result);
    error = lastError.empty() ? "connection failed" : lastError;
    impl_->close();
    return false;
}

bool XbdmClient::connected() const {
    return impl_->socket != INVALID_SOCKET;
}

void XbdmClient::close() {
    impl_->close();
}

std::vector<std::string> XbdmClient::banner(std::string& error) {
    std::vector<std::string> lines;
    std::string line;
    if (!impl_->nextLine(line, error, 3)) return lines;
    lines.push_back(line);
    return lines;
}

std::vector<std::string> XbdmClient::command(const std::string& cmd, std::string& error) {
    std::vector<std::string> lines;
    const std::string wire = cmd + "\r\n";
    if (!impl_->sendAll(wire.data(), wire.size(), error)) return lines;

    bool multiline = false;
    for (int guard = 0; guard < 100000; ++guard) {
        std::string line;
        if (!impl_->nextLine(line, error, 5)) break;

        if (line.rfind("202-", 0) == 0) {
            multiline = true;
            lines.push_back(line);
            continue;
        }
        if (multiline && line == ".") {
            impl_->drainTrailingStatus();
            break;
        }
        lines.push_back(line);
        if (line.rfind("200-", 0) == 0 || line.rfind("2", 0) == 0 && line.size() >= 4 && line[3] == ' ') {
            break;
        }
        if (!line.empty() && line[0] == '4') break;
    }
    return lines;
}

bool XbdmClient::listDir(const std::string& path, std::vector<DirEntry>& out, std::string& error) {
    std::string normalized = path;
    if (!normalized.empty() && normalized.back() != '\\') normalized.push_back('\\');
    const auto lines = command("DIRLIST NAME=\"" + normalized + "\"", error);
    if (!error.empty()) return false;

    bool sawResponse = false;
    for (const auto& line : lines) {
        if (line.rfind("4", 0) == 0 && line.size() >= 3) {
            error = line;
            return false;
        }
        if (line.find("name=\"") == std::string::npos) continue;
        sawResponse = true;
        const auto nameStart = line.find("name=\"") + 6;
        const auto nameEnd = line.find('"', nameStart);
        if (nameEnd == std::string::npos) continue;

        DirEntry e;
        e.name = line.substr(nameStart, nameEnd - nameStart);
        const uint32_t lo = hex32(line, "sizelo=");
        const uint32_t hi = hex32(line, "sizehi=");
        e.size = (static_cast<uint64_t>(hi) << 32) | lo;
        e.isDirectory = line.find("directory") != std::string::npos;
        e.changeHi = hex32(line, "changehi=");
        e.changeLo = hex32(line, "changelo=");
        out.push_back(std::move(e));
    }

    if (!sawResponse && lines.empty()) {
        error = "empty directory response";
        return false;
    }
    return true;
}

bool XbdmClient::makeDir(const std::string& remote, std::string& error) {
    const auto lines = command("MKDIR NAME=\"" + remote + "\"", error);
    if (!error.empty()) return false;
    for (const auto& line : lines) {
        if (line.rfind("200-", 0) == 0) return true;
        // Re-creating an existing folder is fine; the console reports 410.
        if (line.find("already exists") != std::string::npos) return true;
        if (line.rfind("4", 0) == 0) {
            error = "console rejected MKDIR: " + line;
            return false;
        }
    }
    error = "no response to MKDIR";
    return false;
}

bool XbdmClient::rename(const std::string& from, const std::string& to, std::string& error) {
    const auto lines = command("RENAME NAME=\"" + from + "\" NEWNAME=\"" + to + "\"", error);
    if (!error.empty()) return false;
    for (const auto& line : lines) {
        if (line.rfind("200-", 0) == 0) return true;
        if (line.rfind("4", 0) == 0) {
            error = "console rejected RENAME: " + line;
            return false;
        }
    }
    error = "no response to RENAME";
    return false;
}

bool XbdmClient::remove(const std::string& remote, bool isDirectory, std::string& error) {
    std::string path = remote;
    // XBDM wants directory deletes without a trailing backslash, plus the DIR flag.
    while (!path.empty() && path.back() == '\\') path.pop_back();
    const std::string cmd = "DELETE NAME=\"" + path + "\"" + (isDirectory ? " DIR" : "");
    const auto lines = command(cmd, error);
    if (!error.empty()) return false;
    for (const auto& line : lines) {
        if (line.rfind("200-", 0) == 0) return true;
        if (line.rfind("4", 0) == 0) {
            error = "console rejected DELETE: " + line;
            return false;
        }
    }
    error = "no response to DELETE";
    return false;
}

bool XbdmClient::deleteTree(const std::string& remoteDir, std::string& error, size_t* deleted) {
    std::string dir = remoteDir;
    if (!dir.empty() && dir.back() != '\\') dir.push_back('\\');

    std::vector<DirEntry> entries;
    if (!listDir(dir, entries, error)) return false;

    size_t count = deleted ? *deleted : 0;
    for (const auto& e : entries) {
        if (e.isDirectory) {
            if (!deleteTree(dir + e.name, error, &count)) return false;
        } else {
            if (!remove(dir + e.name, false, error)) return false;
            ++count;
        }
    }
    // Remove the now-empty directory itself.
    if (!remove(remoteDir, true, error)) return false;
    ++count;
    if (deleted) *deleted = count;
    return true;
}

bool XbdmClient::download(const std::string& remote, const std::string& local, std::string& error,
                          uint64_t* bytes, ProgressFn progress) {
    const std::string wire = "GETFILE NAME=\"" + remote + "\"\r\n";
    if (!impl_->sendAll(wire.data(), wire.size(), error)) return false;

    std::string line;
    bool binary = false;
    for (int i = 0; i < 32; ++i) {
        if (!impl_->nextLine(line, error, 5)) return false;
        if (line.rfind("203-", 0) == 0) { binary = true; break; }
        if (!line.empty() && line[0] == '4') {
            error = "console rejected GETFILE: " + line;
            return false;
        }
    }
    if (!binary) {
        error = "no binary response from GETFILE";
        return false;
    }

    uint8_t sizeBytes[4]{};
    if (!impl_->recvExact(sizeBytes, 4, error)) return false;
    const uint64_t size = static_cast<uint64_t>(sizeBytes[0]) |
                          (static_cast<uint64_t>(sizeBytes[1]) << 8) |
                          (static_cast<uint64_t>(sizeBytes[2]) << 16) |
                          (static_cast<uint64_t>(sizeBytes[3]) << 24);

    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (size && !impl_->recvExact(data.data(), data.size(), error, progress)) return false;

    // GETFILE has no trailing status line, but drain any stray bytes defensively.
    impl_->drainTrailingStatus();
    bytes::writeFile(local, data.data(), data.size());
    if (bytes) *bytes = size;
    return true;
}

bool XbdmClient::downloadTree(const std::string& remoteDir, const std::string& localDir,
                              std::string& error, size_t* fileCount, uint64_t* totalBytes,
                              ProgressFn progress) {
    std::vector<DirEntry> entries;
    if (!listDir(remoteDir, entries, error)) return false;

    std::error_code ec;
    std::filesystem::create_directories(localDir, ec);

    size_t count = fileCount ? *fileCount : 0;
    uint64_t total = totalBytes ? *totalBytes : 0;

    for (const auto& e : entries) {
        const std::string remote = remoteDir + e.name;
        const auto local = std::filesystem::path(localDir) / e.name;
        if (e.isDirectory) {
            if (!downloadTree(remote + "\\", local.string(), error, &count, &total, progress)) return false;
        } else {
            uint64_t bytes = 0;
            if (!download(remote, local.string(), error, &bytes, progress)) return false;
            ++count;
            total += bytes;
        }
    }

    if (fileCount) *fileCount = count;
    if (totalBytes) *totalBytes = total;
    return true;
}

bool XbdmClient::upload(const std::string& local, const std::string& remote, std::string& error,
                        uint64_t* bytes, ProgressFn progress) {
    const auto data = bytes::readFile(local);
    char lengthHex[32]{};
    std::snprintf(lengthHex, sizeof(lengthHex), "%llx", static_cast<unsigned long long>(data.size()));
    const std::string wire = "SENDFILE NAME=\"" + remote + "\" LENGTH=0x" + lengthHex + "\"\r\n";
    if (!impl_->sendAll(wire.data(), wire.size(), error)) return false;

    bool ready = false;
    for (int i = 0; i < 16; ++i) {
        std::string line;
        if (!impl_->nextLine(line, error, 5)) return false;
        if (line.rfind("204-", 0) == 0) { ready = true; break; }
        if (!line.empty() && line[0] == '4') {
            error = "console rejected SENDFILE: " + line;
            return false;
        }
    }
    if (!ready) {
        error = "no binary-data prompt from SENDFILE";
        return false;
    }

    // The console does NOT want a length prefix on the data: LENGTH= already
    // told it how many bytes to expect. (Sending a 4-byte prefix writes those
    // bytes into the file and leaves 4 stray bytes to corrupt the next command.)
    if (!data.empty() && !impl_->sendAll(reinterpret_cast<const char*>(data.data()), data.size(), error, progress)) return false;

    // The console replies with a final status once the payload is written. Read
    // it so it cannot desync the next command on a reused connection.
    std::string status;
    if (!impl_->nextLine(status, error, 10)) return false;
    if (!status.empty() && status[0] == '4') {
        error = "console rejected SENDFILE: " + status;
        return false;
    }
    if (bytes) *bytes = data.size();
    return true;
}

bool XbdmClient::screenshot(const std::string& localPng, std::string& error,
                            int* width, int* height, ProgressFn progress) {
    const std::string wire = "SCREENSHOT\r\n";
    if (!impl_->sendAll(wire.data(), wire.size(), error)) return false;

    bool binary = false;
    for (int i = 0; i < 8; ++i) {
        std::string line;
        if (!impl_->nextLine(line, error, 5)) return false;
        if (line.rfind("203-", 0) == 0) { binary = true; break; }
        if (!line.empty() && line[0] == '4') {
            error = "console rejected SCREENSHOT: " + line;
            return false;
        }
    }
    if (!binary) {
        error = "no binary header from SCREENSHOT";
        return false;
    }

    std::string header;
    if (!impl_->nextLine(header, error, 5)) return false;

    const int w = static_cast<int>(hex32(header, "width="));
    const int h = static_cast<int>(hex32(header, "height="));
    const uint32_t pitch = hex32(header, "pitch=");
    const uint32_t frameBytes = hex32(header, "framebuffersize=");
    if (w <= 0 || h <= 0 || pitch == 0 || frameBytes == 0 || frameBytes > 256u * 1024u * 1024u) {
        error = "invalid screenshot header: " + header;
        return false;
    }

    // The tiling geometry is defined by the dump's own row stride (pitch), NOT
    // by sw/sh: those are the console's display size and can be larger than the
    // captured surface (e.g. a 1280x720 header with an 864x480 dump), which
    // would make the addressing run past the end of the buffer.
    uint32_t surfaceWidth = pitch / 4;
    if (surfaceWidth == 0) surfaceWidth = static_cast<uint32_t>(w);

    std::vector<uint8_t> raw(frameBytes);
    if (!impl_->recvExact(raw.data(), raw.size(), error, progress)) return false;
    impl_->drainTrailingStatus();

    // Undo the Xenos tiling to get a top-down linear image. The raw dump is
    // already in little-endian BGRA byte order, which is exactly what
    // PixelFormat32bppARGB expects, so the bytes can be copied straight across.
    std::vector<uint8_t> data(static_cast<size_t>(w) * h * 4, 0xff);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint32_t src = xenos2DTiledOffset(static_cast<uint32_t>(x),
                                                    static_cast<uint32_t>(y),
                                                    surfaceWidth) * 4u;
            if (src + 3u >= raw.size()) continue;
            const size_t dst = (static_cast<size_t>(y) * w + x) * 4;
            data[dst] = raw[src];
            data[dst + 1] = raw[src + 1];
            data[dst + 2] = raw[src + 2];
            data[dst + 3] = 0xff;
        }
    }

    ULONG_PTR token = 0;
    GdiplusStartupInput input{};
    if (GdiplusStartup(&token, &input, nullptr) != Ok) {
        error = "GDI+ initialization failed";
        return false;
    }

    bool ok = false;
    {
        Bitmap bitmap(w, h, PixelFormat32bppARGB);
        BitmapData locked{};
        Gdiplus::Rect rect(0, 0, w, h);
        if (bitmap.LockBits(&rect, ImageLockModeWrite, PixelFormat32bppARGB, &locked) == Ok) {
            const size_t rowBytes = static_cast<size_t>(w) * 4;
            for (int y = 0; y < h; ++y) {
                std::memcpy(static_cast<BYTE*>(locked.Scan0) + static_cast<size_t>(y) * locked.Stride,
                            data.data() + static_cast<size_t>(y) * rowBytes, rowBytes);
            }
            bitmap.UnlockBits(&locked);

            CLSID png{};
            if (pngEncoderClsid(&png)) {
                const std::wstring path = utf8ToWide(localPng);
                ok = bitmap.Save(path.c_str(), &png) == Ok;
            }
        }
        if (!ok) error = "failed to encode PNG";
    }
    GdiplusShutdown(token);

    if (ok) {
        if (width) *width = w;
        if (height) *height = h;
    }
    return ok;
}

} // namespace x360
