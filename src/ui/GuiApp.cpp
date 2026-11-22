#include "ui/GuiApp.h"

#include "core/Tool.h"
#include "core/XbdmClient.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <ole2.h>
#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace x360::ui {

namespace {

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_renderTarget = nullptr;
ImFont* g_titleFont = nullptr;
ImFont* g_monoFont = nullptr;

std::mutex g_dropMutex;
std::vector<std::string> g_droppedFiles;

constexpr float kStatusH = 30.0f;
constexpr float kHeaderH = 78.0f;
constexpr float kCardH = 96.0f;
constexpr float kCardGap = 12.0f;

enum class Screen { Home, Console, Tools };
enum class Tool { Xuiz, Xex, Nand };
enum class ConsoleTab { Console, Explorer, Trainer };
enum class ViewMode { Icons, Details };

struct SavedConsole {
    std::string name;
    std::string ip;
};

// ---------- trainer ----------

// A candidate address found by the memory scanner. `bits` / `number` /
// `display` describe the value read by the scan that produced this row, and the
// `prev*` copies keep the value from the scan before it so Changed / Increased /
// Decreased can compare two reads.
struct MemCandidate {
    uint32_t address = 0;
    uint64_t bits = 0;
    double number = 0.0;
    std::string display;
    uint64_t prevBits = 0;
    double prevNumber = 0.0;
    bool hasPrev = false;
    bool selected = false;
};

// One row in the address list: an address the user follows, with an optional
// value that is rewritten continuously while Freeze is ticked.
struct PinnedEntry {
    uint32_t address = 0;
    int type = 2;
    uint64_t bits = 0;
    double number = 0.0;
    std::string valueText;
    std::vector<uint8_t> bytes;
    bool freeze = false;
    char edit[64] = "";
};

struct FreezeTarget {
    uint32_t address = 0;
    std::vector<uint8_t> data;
};

// Re-writes every frozen address on a timer, on its own XBDM connection so it
// cannot interfere with the scanning connection.
struct FreezeState {
    std::thread worker;
    mutable std::mutex mutex;
    std::atomic<bool> stop{false};
    std::string ip;
    std::vector<FreezeTarget> targets;

    void ensureStarted() {
        if (!worker.joinable()) {
            stop.store(false);
            worker = std::thread([this] { run(); });
        }
    }
    void setTargets(const std::string& targetIp, std::vector<FreezeTarget> list) {
        ensureStarted();
        std::lock_guard<std::mutex> lock(mutex);
        ip = targetIp;
        targets = std::move(list);
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        targets.clear();
    }
    size_t count() const {
        std::lock_guard<std::mutex> lock(mutex);
        return targets.size();
    }

    void run() {
        XbdmClient client;
        std::string connectedIp;
        while (!stop.load()) {
            std::string targetIp;
            std::vector<FreezeTarget> list;
            {
                std::lock_guard<std::mutex> lock(mutex);
                targetIp = ip;
                list = targets;
            }
            if (!list.empty() && !targetIp.empty()) {
                if (!client.connected() || connectedIp != targetIp) {
                    client.close();
                    std::string err;
                    connectedIp = client.connect(targetIp, 730, err) ? targetIp : std::string();
                }
                if (client.connected()) {
                    for (const auto& t : list) {
                        std::string err;
                        if (!client.setMemory(t.address, t.data, err)) {
                            client.close();
                            connectedIp.clear();
                            break;
                        }
                    }
                }
            }
            for (int i = 0; i < 5 && !stop.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        client.close();
    }

    ~FreezeState() {
        stop.store(true);
        if (worker.joinable()) worker.join();
    }
};

// Everything the Trainer tab keeps: scan setup, results, the pinned address list
// and the memory viewer.
struct TrainerState {
    std::string target;
    std::vector<ModuleInfo> modules;
    std::vector<MemRegion> regions;
    bool refreshed = false;

    // Scanner setup.
    char startHex[32] = "82000000";
    char lengthHex[32] = "00800000";
    int valueType = 2; // index into kMemTypeNames (Int32)
    char value[64] = "100";
    char value2[64] = "0"; // upper bound for a Between first scan
    char delta[64] = "1";  // step for Increased by / Decreased by
    int firstMode = 0;     // index into kScanFirstNames
    int nextType = 0;      // index into kScanNextNames

    std::vector<MemCandidate> candidates;
    std::atomic<uint32_t> scannedBytes{0};
    std::atomic<uint32_t> totalBytes{0};
    std::atomic<int> liveMatches{0};
    bool hasScan = false;
    bool scanning = false;
    int selected = -1;
    std::string status;
    std::string error;
    char writeValue[64] = "";

    // Address list.
    std::vector<PinnedEntry> pins;
    char pinAddr[16] = "";
    std::string pinStatus;
    bool freezeDirty = false;
    std::string freezeIp;

    // Memory viewer.
    char viewerAddr[16] = "82000000";
    uint32_t viewerBase = 0x82000000;
    std::vector<uint8_t> viewerBytes;
    bool viewerLoaded = false;
    bool viewerReading = false;
    bool viewerTried = false;
    int viewerSelected = -1;
    char viewerWriteAddr[16] = "";
    char viewerWriteHex[128] = "";
    std::string viewerStatus;

    int subTab = 0;
    FreezeState freeze;
};

// ---------- OLE drag-out support ----------

class DroppedFileData : public IDataObject {
public:
    explicit DroppedFileData(std::vector<std::wstring> paths) : paths_(std::move(paths)) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *ppv = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG remaining = --refs_;
        if (remaining == 0) delete this;
        return remaining;
    }

    STDMETHODIMP GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (format->cfFormat != CF_HDROP || format->dwAspect != DVASPECT_CONTENT ||
            (format->tymed & TYMED_HGLOBAL) == 0) {
            return DV_E_FORMATETC;
        }
        size_t bytes = sizeof(DROPFILES);
        for (const auto& path : paths_) bytes += (path.size() + 1) * sizeof(WCHAR);
        bytes += sizeof(WCHAR);

        HGLOBAL handle = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!handle) return E_OUTOFMEMORY;
        auto* files = static_cast<DROPFILES*>(::GlobalLock(handle));
        files->pFiles = sizeof(DROPFILES);
        files->pt = {0, 0};
        files->fNC = FALSE;
        files->fWide = TRUE;
        auto* cursor = reinterpret_cast<WCHAR*>(reinterpret_cast<BYTE*>(files) + files->pFiles);
        for (const auto& path : paths_) {
            std::memcpy(cursor, path.c_str(), (path.size() + 1) * sizeof(WCHAR));
            cursor += path.size() + 1;
        }
        *cursor = L'\0';
        ::GlobalUnlock(handle);

        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = handle;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }
    STDMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    STDMETHODIMP QueryGetData(FORMATETC* format) override {
        if (format->cfFormat == CF_HDROP && (format->tymed & TYMED_HGLOBAL)) return S_OK;
        return DV_E_FORMATETC;
    }
    STDMETHODIMP GetCanonicalFormatEtc(FORMATETC* in, FORMATETC* out) override {
        *out = *in;
        return DATA_S_SAMEFORMATETC;
    }
    STDMETHODIMP SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    STDMETHODIMP EnumFormatEtc(DWORD, IEnumFORMATETC** result) override {
        *result = nullptr;
        return E_NOTIMPL;
    }
    STDMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    STDMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    STDMETHODIMP EnumDAdvise(IEnumSTATDATA** result) override {
        *result = nullptr;
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    ULONG refs_ = 1;
    std::vector<std::wstring> paths_;
};

class SimpleDropSource : public IDropSource {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *ppv = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG remaining = --refs_;
        if (remaining == 0) delete this;
        return remaining;
    }
    STDMETHODIMP QueryContinueDrag(BOOL escape, DWORD keys) override {
        if (escape) return DRAGDROP_S_CANCEL;
        if (!(keys & (MK_LBUTTON | MK_RBUTTON))) return DRAGDROP_S_DROP;
        return S_OK;
    }
    STDMETHODIMP GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
    ULONG refs_ = 1;
};

std::string wideToUtf8(const std::wstring& text) {
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size > 0 ? size - 1 : 0), '\0');
    if (size > 0) ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), size - 1, nullptr, nullptr);
    return result;
}

void doDragOut(const std::vector<std::string>& paths) {
    std::vector<std::wstring> wide;
    for (const auto& path : paths) {
        const int size = ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        std::wstring value(static_cast<size_t>(size > 0 ? size - 1 : 0), L'\0');
        if (size > 0) ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, value.data(), size - 1);
        wide.push_back(std::move(value));
    }
    auto* data = new DroppedFileData(std::move(wide));
    auto* source = new SimpleDropSource();
    DWORD effect = DROPEFFECT_COPY;
    ::DoDragDrop(data, source, DROPEFFECT_COPY, &effect);
    data->Release();
    source->Release();
}

// ---------- background jobs ----------

struct JobResult {
    std::string id;
    std::string text;
    std::vector<DirEntry> entries;
    std::string path;
    bool hasEntries = false;
    bool refresh = false;

    std::vector<MemCandidate> candidates;
    bool hasCandidates = false;
    std::vector<ModuleInfo> modules;
    std::vector<MemRegion> regions;
    std::vector<uint8_t> memBytes;
    uint32_t memBase = 0;
    bool hasMem = false;
};

struct Job {
    std::string id;
    std::function<JobResult()> fn;
};

class JobRunner {
public:
    static constexpr int kWorkers = 4;

    JobRunner() {
        for (int i = 0; i < kWorkers; ++i) workers_.emplace_back([this] { loop(); });
    }
    ~JobRunner() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
    }

    void submit(std::string id, std::function<JobResult()> fn) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push({std::move(id), std::move(fn)});
        }
        cv_.notify_one();
    }

    std::vector<JobResult> poll() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<JobResult> out;
        out.swap(results_);
        return out;
    }

    bool busy() const { return active_.load() > 0; }
    std::string busyId() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return busyId_;
    }

private:
    void loop() {
        while (true) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) break;
                job = std::move(queue_.front());
                queue_.pop();
                active_.fetch_add(1);
                busyId_ = job.id;
            }

            JobResult result;
            try {
                result = job.fn();
            } catch (const std::exception& e) {
                result.id = job.id;
                result.text = std::string("error: ") + e.what() + "\n";
            }
            if (result.id.empty()) result.id = job.id;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                results_.push_back(std::move(result));
                if (active_.fetch_sub(1) == 1) busyId_.clear();
            }
        }
    }

    std::vector<std::thread> workers_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<Job> queue_;
    std::vector<JobResult> results_;
    bool stop_ = false;
    std::atomic<int> active_{0};
    std::string busyId_;
};

// ---------- state ----------

struct ExplorerState {
    XbdmClient client;
    std::string clientIp;
    std::string path;
    char pathBuf[MAX_PATH]{};
    std::vector<DirEntry> entries;
    std::unordered_map<std::string, std::vector<DirEntry>> cache;
    std::vector<std::string> history;
    std::string status;
    std::string pendingStatus;
    std::string error;
    int selected = -1;
    int driveIndex = 0;
    ViewMode view = ViewMode::Icons;
    bool dragPending = false;

    bool openNewFolder = false;
    bool openRename = false;
    bool openDelete = false;
    char nameBuf[128]{};
    std::string renameRemote;
    std::string deleteRemote;
    std::string deleteName;
    bool deleteIsDir = false;
};

struct TransferState {
    mutable std::mutex mutex;
    float progress = 0.0f;
    std::string label;
    bool active = false;

    void begin(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex);
        progress = 0.0f;
        label = text;
        active = true;
    }
    void update(float value, const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex);
        progress = value;
        if (!text.empty()) label = text;
        active = true;
    }
    void end() {
        std::lock_guard<std::mutex> lock(mutex);
        active = false;
        progress = 0.0f;
    }
    void snapshot(float& value, std::string& text, bool& running) const {
        std::lock_guard<std::mutex> lock(mutex);
        value = progress;
        text = label;
        running = active;
    }
};

struct UiState {
    Screen screen = Screen::Home;
    Tool tool = Tool::Xuiz;
    ConsoleTab consoleTab = ConsoleTab::Console;
    bool showAbout = false;
    bool showHelp = false;
    bool done = false;
    int selected = -1;

    TransferState transfer;
    ExplorerState explorer;
    TrainerState trainer;
    // Declared after the job-scoped state so its worker threads are joined
    // (in ~JobRunner) before that state is torn down.
    JobRunner jobs;

    std::vector<SavedConsole> consoles;
    char newName[64]{};
    char newIp[64]{};

    std::string infoText;
    std::string logText;
    char remotePath[MAX_PATH]{};
    char localPath[MAX_PATH]{};
    char rawCommand[256]{};

    char xuizPath[MAX_PATH]{};
    char xexPath[MAX_PATH]{};
    char nandA[MAX_PATH]{};
    char nandB[MAX_PATH]{};
    std::string toolLog;
};

// ---------- D3D11 ----------

void createRenderTarget();

bool createDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate = {60, 1};
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc = {1, 0};
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level{};
    if (FAILED(::D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                               levels, 2, D3D11_SDK_VERSION, &sd, &g_swapChain,
                                               &g_device, &level, &g_context))) {
        return false;
    }
    createRenderTarget();
    return true;
}

void createRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (backBuffer) {
        g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTarget);
        backBuffer->Release();
    }
}

void cleanupRenderTarget() {
    if (g_renderTarget) { g_renderTarget->Release(); g_renderTarget = nullptr; }
}

void cleanupDeviceD3D() {
    cleanupRenderTarget();
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

LRESULT WINAPI wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;
    if (msg == WM_SIZE) {
        if (g_device && wParam != SIZE_MINIMIZED) {
            cleanupRenderTarget();
            g_swapChain->ResizeBuffers(0, static_cast<UINT>(LOWORD(lParam)),
                                       static_cast<UINT>(HIWORD(lParam)), DXGI_FORMAT_UNKNOWN, 0);
            createRenderTarget();
        }
        return 0;
    }
    if (msg == WM_GETMINMAXINFO) {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize = {800, 600};
        return 0;
    }
    if (msg == WM_DROPFILES) {
        const HDROP drop = reinterpret_cast<HDROP>(wParam);
        const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            const UINT chars = ::DragQueryFileW(drop, i, nullptr, 0);
            std::wstring wide(chars + 1, L'\0');
            ::DragQueryFileW(drop, i, wide.data(), chars + 1);
            const int size = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string utf8(static_cast<size_t>(size > 0 ? size - 1 : 0), '\0');
            if (size > 0) {
                ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, utf8.data(), size - 1, nullptr, nullptr);
            }
            std::lock_guard<std::mutex> lock(g_dropMutex);
            g_droppedFiles.push_back(std::move(utf8));
        }
        ::DragFinish(drop);
        return 0;
    }
    if (msg == WM_DESTROY) { ::PostQuitMessage(0); return 0; }
    return ::DefWindowProc(hwnd, msg, wParam, lParam);
}

// ---------- helpers ----------

std::string runCaptured(const std::string& module, std::vector<std::string> args) {
    static std::mutex captureMutex;
    std::lock_guard<std::mutex> lock(captureMutex);
    ITool* tool = Registry::instance().find(module);
    if (!tool) return "unknown module: " + module + "\n";
    std::ostringstream capture;
    auto* oldOut = std::cout.rdbuf(capture.rdbuf());
    auto* oldErr = std::cerr.rdbuf(capture.rdbuf());
    tool->run({std::move(args)});
    std::cout.rdbuf(oldOut);
    std::cerr.rdbuf(oldErr);
    return capture.str();
}

void runTextJob(UiState& s, const std::string& id, const std::string& module, std::vector<std::string> args) {
    s.jobs.submit(id, [module, args, id] {
        return JobResult{id, runCaptured(module, args), {}, {}, false};
    });
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

std::string formatSize(uint64_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + " KB";
    if (bytes < 1024ULL * 1024 * 1024) return std::to_string(bytes / (1024 * 1024)) + " MB";
    return std::to_string(bytes / (1024ULL * 1024 * 1024)) + " GB";
}

std::filesystem::path storagePath() {
    char buffer[MAX_PATH]{};
    ::GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path() / "consoles.ini";
}

std::filesystem::path downloadsFolder() {
    PWSTR path = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &path))) {
        std::filesystem::path result(path);
        ::CoTaskMemFree(path);
        return result;
    }
    return std::filesystem::path("downloads");
}

void loadConsoles(UiState& s) {
    std::ifstream in(storagePath());
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto bar = line.find('|');
        if (bar != std::string::npos) s.consoles.push_back({line.substr(0, bar), line.substr(bar + 1)});
    }
}

void saveConsoles(const UiState& s) {
    std::ofstream out(storagePath(), std::ios::trunc);
    for (const auto& c : s.consoles) out << c.name << '|' << c.ip << '\n';
}

bool fileExists(const char* path) {
    return ::GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

void loadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    if (fileExists("C:\\Windows\\Fonts\\segoeui.ttf"))
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f);
    else
        io.FontGlobalScale = 1.2f;
    if (fileExists("C:\\Windows\\Fonts\\segoeuib.ttf"))
        g_titleFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 26.0f);
    if (fileExists("C:\\Windows\\Fonts\\consola.ttf"))
        g_monoFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 14.0f);
}

void applyStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.ChildRounding = 0.0f;
    s.FrameRounding = 4.0f;
    s.GrabRounding = 4.0f;
    s.WindowPadding = ImVec2(16, 14);
    s.FramePadding = ImVec2(9, 6);
    s.ItemSpacing = ImVec2(8, 7);
    s.WindowBorderSize = 0.0f;
    s.ScrollbarSize = 9.0f;

    s.Colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.065f, 0.085f, 1.0f);
    s.Colors[ImGuiCol_ChildBg] = ImVec4(0.090f, 0.105f, 0.135f, 1.0f);
    s.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.075f, 0.085f, 0.110f, 1.0f);
    s.Colors[ImGuiCol_FrameBg] = ImVec4(0.130f, 0.150f, 0.190f, 1.0f);
    s.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.18f, 0.23f, 1.0f);
    s.Colors[ImGuiCol_Button] = ImVec4(0.32f, 0.72f, 0.36f, 1.0f);
    s.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.40f, 0.84f, 0.45f, 1.0f);
    s.Colors[ImGuiCol_ButtonActive] = ImVec4(0.26f, 0.60f, 0.30f, 1.0f);
    s.Colors[ImGuiCol_Header] = ImVec4(0.24f, 0.52f, 0.28f, 0.60f);
    s.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.32f, 0.68f, 0.36f, 0.80f);
    s.Colors[ImGuiCol_HeaderActive] = ImVec4(0.32f, 0.72f, 0.36f, 1.0f);
    s.Colors[ImGuiCol_Separator] = ImVec4(0.20f, 0.25f, 0.30f, 1.0f);
    s.Colors[ImGuiCol_Text] = ImVec4(0.90f, 0.93f, 0.92f, 1.0f);
    s.Colors[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.60f, 0.60f, 1.0f);
    s.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.25f, 0.45f, 0.28f, 1.0f);
    s.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.14f, 0.16f, 0.20f, 1.0f);
    s.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    s.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.025f);
    s.Colors[ImGuiCol_TableBorderStrong] = ImVec4(0.22f, 0.25f, 0.31f, 1.0f);
    s.Colors[ImGuiCol_TableBorderLight] = ImVec4(0.16f, 0.18f, 0.23f, 1.0f);
}

void sectionTitle(const char* text) {
    ImGui::TextDisabled(text);
    ImGui::Separator();
    ImGui::Spacing();
}

void field(const char* id, const char* caption, char* buffer, size_t size) {
    ImGui::TextUnformatted(caption);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText(id, buffer, size);
    ImGui::Spacing();
}

void outputPane(const char* id, const std::string& text, float height) {
    ImGui::BeginChild(id, ImVec2(0, height > 0 ? height : ImGui::GetContentRegionAvail().y), true,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (g_monoFont) ImGui::PushFont(g_monoFont);
    ImGui::TextUnformatted(text.empty() ? "No output yet." : text.c_str());
    ImGui::SetScrollHereY(1.0f);
    if (g_monoFont) ImGui::PopFont();
    ImGui::EndChild();
}

std::vector<std::pair<std::string, std::string>> parseInfo(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '2' || line == ".") continue;
        const auto eq = line.find('=');
        if (eq != std::string::npos)
            out.push_back({line.substr(0, eq), line.substr(eq + 1)});
        else
            out.push_back({"Info", line});
    }
    return out;
}

// ---------- explorer ----------

void explorerNavigate(UiState& s, const std::string& ip, std::string path, bool pushHistory) {
    if (path.empty()) path = "Hdd1:\\";
    if (path.back() != '\\') path.push_back('\\');

    if (pushHistory) s.explorer.history.push_back(s.explorer.path);
    s.explorer.path = path;
    std::snprintf(s.explorer.pathBuf, sizeof(s.explorer.pathBuf), "%s", path.c_str());
    s.explorer.selected = -1;
    s.explorer.error.clear();

    const auto key = lower(path);
    const auto cached = s.explorer.cache.find(key);
    if (cached != s.explorer.cache.end()) {
        s.explorer.entries = cached->second;
        s.explorer.status = "Cached - " + std::to_string(s.explorer.entries.size()) + " items";
    } else {
        s.explorer.entries.clear();
        s.explorer.status = "Loading " + path;
    }

    s.jobs.submit("explorer_list", [&s, ip, path] {
        JobResult r;
        r.id = "explorer_list";
        r.path = path;
        std::string err;
        XbdmClient client;
        if (!client.connect(ip, 730, err)) {
            r.text = err;
            return r;
        }
        std::vector<DirEntry> entries;
        if (!client.listDir(path, entries, err)) {
            r.text = err;
            return r;
        }
        r.entries = std::move(entries);
        r.hasEntries = true;
        return r;
    });
}

x360::ProgressFn transferProgress(TransferState& state, const std::string& label) {
    return [&state, label](size_t done, size_t total) {
        const float value = total ? static_cast<float>(done) / static_cast<float>(total) : 0.0f;
        state.update(value, label + "  " + std::to_string(static_cast<int>(value * 100.0f)) + "%");
    };
}

void drawTransfer(const UiState& s) {
    float value = 0.0f;
    std::string label;
    bool active = false;
    s.transfer.snapshot(value, label, active);
    if (!active) return;
    if (!label.empty()) ImGui::TextUnformatted(label.c_str());
    ImGui::ProgressBar(value, ImVec2(-1.0f, 0.0f));
}

void explorerDownload(UiState& s, const std::string& ip, const DirEntry& entry) {
    const std::string remote = s.explorer.path + entry.name;
    const auto folder = downloadsFolder();
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const auto local = (folder / entry.name).string();

    s.explorer.status = "Downloading " + entry.name + "...";
    s.transfer.begin("Downloading " + entry.name);
    s.jobs.submit("explorer_dl", [&s, ip, remote, local, name = entry.name] {
        JobResult r;
        r.id = "explorer_dl";
        std::string err;
        XbdmClient client;
        if (!client.connect(ip, 730, err)) {
            s.transfer.end();
            r.text = "Download failed: " + err;
            return r;
        }
        uint64_t bytes = 0;
        const bool ok = client.download(remote, local, err, &bytes,
                                        transferProgress(s.transfer, "Downloading " + name));
        s.transfer.end();
        r.text = ok ? "Downloaded " + name + " to " + local + " (" + formatSize(bytes) + ")"
                    : "Download failed: " + err;
        return r;
    });
}

void explorerDownloadToDownloads(UiState& s, const std::string& ip, const DirEntry& entry) {
    const std::string remote = s.explorer.path + entry.name;
    const auto destination = downloadsFolder() / entry.name;
    const bool isDir = entry.isDirectory;
    const std::string name = entry.name;

    s.explorer.status = "Downloading " + name + " to Downloads...";
    s.transfer.begin("Downloading " + name);
    s.jobs.submit("explorer_dl", [&s, ip, remote, destination, name, isDir] {
        JobResult r;
        r.id = "explorer_dl";
        std::string err;
        XbdmClient client;
        if (!client.connect(ip, 730, err)) {
            s.transfer.end();
            r.text = "Download failed: " + err;
            return r;
        }
        const std::string where = destination.string();
        if (isDir) {
            size_t files = 0;
            uint64_t total = 0;
            const bool ok = client.downloadTree(remote + "\\", where, err, &files, &total,
                                                transferProgress(s.transfer, "Downloading " + name));
            s.transfer.end();
            r.text = ok ? "Downloaded folder " + name + " to " + where + " (" + std::to_string(files) +
                          " files, " + formatSize(total) + ")"
                        : "Download failed: " + err;
        } else {
            uint64_t bytes = 0;
            const bool ok = client.download(remote, where, err, &bytes,
                                            transferProgress(s.transfer, "Downloading " + name));
            s.transfer.end();
            r.text = ok ? "Downloaded " + name + " to " + where + " (" + formatSize(bytes) + ")"
                        : "Download failed: " + err;
        }
        return r;
    });
}

void explorerUpload(UiState& s, const std::string& ip, const std::string& localPath,
                    const std::string& remoteRoot) {
    std::error_code ec;
    const bool isDir = std::filesystem::is_directory(localPath, ec);
    const std::string name = std::filesystem::path(localPath).filename().string();
    const std::string remote = remoteRoot + name + (isDir ? "\\" : "");

    s.explorer.status = "Uploading " + name + "...";
    s.transfer.begin("Uploading " + name);
    s.jobs.submit("explorer_upload", [&s, ip, localPath, remote, remoteRoot, name, isDir] {
        JobResult r;
        r.id = "explorer_upload";
        std::string err;
        XbdmClient client;
        if (!client.connect(ip, 730, err)) {
            s.transfer.end();
            r.text = "Upload failed: " + err;
            return r;
        }

        if (!isDir) {
            uint64_t bytes = 0;
            const bool ok = client.upload(localPath, remote, err, &bytes,
                                          transferProgress(s.transfer, "Uploading " + name));
            s.transfer.end();
            r.text = ok ? "Uploaded " + name + " (" + formatSize(bytes) + ")"
                        : "Upload failed: " + err;
            return r;
        }

        // SENDFILE cannot create directories, so build the whole folder tree on
        // the console before sending any files into it.
        std::string topDir = remote;
        while (!topDir.empty() && topDir.back() == '\\') topDir.pop_back();
        if (!client.makeDir(topDir, err)) {
            s.transfer.end();
            r.text = "Upload failed: " + err;
            return r;
        }

        std::error_code walkEc;
        std::vector<std::filesystem::path> localFiles;
        for (const auto& item : std::filesystem::recursive_directory_iterator(localPath, walkEc)) {
            if (item.is_directory()) {
                std::string relative = std::filesystem::relative(item.path(), localPath).generic_string();
                std::replace(relative.begin(), relative.end(), '/', '\\');
                if (!client.makeDir(topDir + "\\" + relative, err)) {
                    s.transfer.end();
                    r.text = "Upload failed: " + err;
                    return r;
                }
            } else if (item.is_regular_file()) {
                localFiles.push_back(item.path());
            }
        }

        size_t files = 0;
        uint64_t total = 0;
        for (const auto& file : localFiles) {
            std::string relative = std::filesystem::relative(file, localPath).generic_string();
            std::replace(relative.begin(), relative.end(), '/', '\\');
            uint64_t bytes = 0;
            const bool ok = client.upload(file.string(), topDir + "\\" + relative, err, &bytes,
                                          transferProgress(s.transfer, "Uploading " + relative));
            if (!ok) {
                s.transfer.end();
                r.text = "Upload failed for " + relative + ": " + err;
                return r;
            }
            ++files;
            total += bytes;
        }
        s.transfer.end();
        r.text = "Uploaded folder " + name + " (" + std::to_string(files) + " files, " +
                 formatSize(total) + ")";
        return r;
    });
}

void explorerCreateFolder(UiState& s, const std::string& ip, const std::string& name) {
    const std::string remote = s.explorer.path + name;
    s.explorer.status = "Creating folder " + name + "...";
    s.jobs.submit("explorer_mkdir", [&s, ip, remote, name] {
        JobResult r;
        r.id = "explorer_mkdir";
        std::string err;
        XbdmClient client;
        if (!client.connect(ip, 730, err)) {
            r.text = "Create folder failed: " + err;
            return r;
        }
        if (!client.makeDir(remote, err)) {
            r.text = "Create folder failed: " + err;
            return r;
        }
        r.text = "Created folder " + name;
        r.refresh = true;
        return r;
    });
}

void explorerRename(UiState& s, const std::string& ip, const std::string& oldRemote,
                    const std::string& newName) {
    const std::string newRemote = s.explorer.path + newName;
    s.explorer.status = "Renaming to " + newName + "...";
    s.jobs.submit("explorer_rename", [&s, ip, oldRemote, newRemote, newName] {
        JobResult r;
        r.id = "explorer_rename";
        std::string err;
        XbdmClient client;
        if (!client.connect(ip, 730, err)) {
            r.text = "Rename failed: " + err;
            return r;
        }
        if (!client.rename(oldRemote, newRemote, err)) {
            r.text = "Rename failed: " + err;
            return r;
        }
        r.text = "Renamed to " + newName;
        r.refresh = true;
        return r;
    });
}

void explorerDelete(UiState& s, const std::string& ip, const std::string& remote,
                    const std::string& name, bool isDir) {
    s.explorer.status = "Deleting " + name + "...";
    s.jobs.submit("explorer_delete", [&s, ip, remote, name, isDir] {
        JobResult r;
        r.id = "explorer_delete";
        std::string err;
        XbdmClient client;
        if (!client.connect(ip, 730, err)) {
            r.text = "Delete failed: " + err;
            return r;
        }
        bool ok = false;
        size_t removed = 0;
        if (isDir) {
            ok = client.deleteTree(remote, err, &removed);
        } else {
            ok = client.remove(remote, false, err);
            removed = 1;
        }
        if (!ok) {
            r.text = "Delete failed: " + err;
            return r;
        }
        r.text = "Deleted " + name + (isDir && removed > 1
                                          ? " (" + std::to_string(removed - 1) + " files)"
                                          : "");
        r.refresh = true;
        return r;
    });
}

void captureScreenshot(UiState& s, const std::string& ip) {
    char stamp[64]{};
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    if (!::localtime_s(&local, &now)) {
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    }
    const auto path = downloadsFolder() / (std::string("xbox-") + stamp + ".png");

    s.transfer.begin("Capturing screenshot");
    s.jobs.submit("screenshot", [&s, ip, path] {
        JobResult r;
        r.id = "screenshot";
        std::string err;
        XbdmClient client;
        if (!client.connect(ip, 730, err)) {
            s.transfer.end();
            r.text = "Screenshot failed: " + err;
            return r;
        }
        int w = 0;
        int h = 0;
        const bool ok = client.screenshot(path.string(), err, &w, &h,
                                          transferProgress(s.transfer, "Capturing screenshot"));
        s.transfer.end();
        r.text = ok ? "Screenshot saved to " + path.string() + " (" + std::to_string(w) + "x" +
                      std::to_string(h) + ")"
                    : "Screenshot failed: " + err;
        return r;
    });
}

void explorerUp(UiState& s, const std::string& ip) {
    std::string p = s.explorer.path;
    while (!p.empty() && p.back() == '\\') p.pop_back();
    const auto slash = p.rfind('\\');
    if (slash == std::string::npos) return;
    p = p.substr(0, slash + 1);
    explorerNavigate(s, ip, p, true);
}

std::string fileType(const DirEntry& e) {
    if (e.isDirectory) return "File folder";
    const auto dot = e.name.rfind('.');
    if (dot == std::string::npos) return "File";
    const std::string ext = lower(e.name.substr(dot + 1));
    if (ext == "xex") return "Xbox 360 executable";
    if (ext == "ini") return "Configuration settings";
    if (ext == "bin") return "Binary file";
    if (ext == "png" || ext == "jpg" || ext == "jpeg") return "Image";
    if (ext == "dds") return "DirectDraw surface";
    if (ext == "xur") return "XUI resource";
    if (ext == "xzp") return "XZP package";
    if (ext == "txt" || ext == "log") return "Text document";
    if (ext == "xml") return "XML document";
    if (ext == "cfg") return "Configuration file";
    return ext + " file";
}

std::string formatFileTime(uint32_t hi, uint32_t lo) {
    const uint64_t ft = (static_cast<uint64_t>(hi) << 32) | lo;
    if (ft == 0) return "-";
    constexpr uint64_t epochOffset = 116444736000000000ULL;
    if (ft < epochOffset) return "-";
    const uint64_t seconds = (ft - epochOffset) / 10000000ULL;
    const std::time_t t = static_cast<std::time_t>(seconds);
    std::tm local{};
    if (!::localtime_s(&local, &t)) {
        char buffer[32]{};
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local);
        return buffer;
    }
    return "-";
}

void drawFileIcon(const DirEntry& e) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    auto* dl = ImGui::GetWindowDrawList();
    if (e.isDirectory) {
        dl->AddRectFilled(p + ImVec2(0, 2), p + ImVec2(14, 12), IM_COL32(238, 186, 82, 255), 1.5f);
        dl->AddRectFilled(p + ImVec2(1, 0), p + ImVec2(7, 3), IM_COL32(238, 186, 82, 255), 1.0f);
    } else {
        dl->AddRectFilled(p + ImVec2(2, 0), p + ImVec2(12, 13), IM_COL32(178, 192, 212, 255), 1.0f);
        dl->AddTriangleFilled(p + ImVec2(12, 0), p + ImVec2(12, 4), p + ImVec2(8, 0),
                              IM_COL32(120, 138, 162, 255));
    }
    ImGui::Dummy(ImVec2(18, 14));
    ImGui::SameLine();
}

void drawBigFolder(ImDrawList* dl, ImVec2 p, float s) {
    const ImVec2 back = p + ImVec2(s * 0.04f, s * 0.26f);
    const ImVec2 backEnd = p + ImVec2(s * 0.96f, s * 0.92f);
    dl->AddRectFilled(back, backEnd, IM_COL32(118, 150, 58, 255), 3.0f);
    const ImVec2 tabA = p + ImVec2(s * 0.04f, s * 0.14f);
    const ImVec2 tabB = p + ImVec2(s * 0.46f, s * 0.32f);
    dl->AddRectFilled(tabA, tabB, IM_COL32(130, 165, 66, 255), 2.0f);
    const ImVec2 front = p + ImVec2(s * 0.02f, s * 0.30f);
    const ImVec2 frontEnd = p + ImVec2(s * 0.98f, s * 0.96f);
    dl->AddRectFilled(front, frontEnd, IM_COL32(172, 214, 78, 255), 3.0f);
    dl->AddRectFilled(front + ImVec2(2, 2), frontEnd - ImVec2(2, 2), IM_COL32(198, 232, 112, 70), 2.0f);
    // subtle concentric arcs like the Xbox folder art
    for (int i = 0; i < 3; ++i) {
        const float r = s * (0.16f + i * 0.10f);
        dl->AddCircle(front + ImVec2(s * 0.74f, s * 0.36f), r, IM_COL32(140, 190, 60, 90), 12, 1.2f);
    }
}

void drawBigFile(ImDrawList* dl, ImVec2 p, float s, const std::string& name) {
    const ImVec2 a = p + ImVec2(s * 0.20f, s * 0.06f);
    const ImVec2 b = p + ImVec2(s * 0.80f, s * 0.94f);
    dl->AddRectFilled(a, b, IM_COL32(222, 229, 238, 255), 2.0f);
    const ImVec2 fold = p + ImVec2(s * 0.58f, s * 0.06f);
    dl->AddTriangleFilled(fold, fold + ImVec2(s * 0.22f, 0), fold + ImVec2(0, s * 0.20f),
                          IM_COL32(150, 162, 178, 255));
    dl->AddRectFilled(a + ImVec2(s * 0.10f, s * 0.20f), b - ImVec2(s * 0.10f, s * 0.55f),
                      IM_COL32(165, 178, 195, 160), 1.0f);
    const auto dot = name.rfind('.');
    if (dot != std::string::npos) {
        const std::string ext = lower(name.substr(dot + 1));
        if (ext == "xex" || ext == "xur") {
            dl->AddCircleFilled(p + ImVec2(s * 0.66f, s * 0.70f), s * 0.13f, IM_COL32(96, 180, 72, 255), 16);
        }
    }
}

void sortEntries(ExplorerState& ex, ImGuiTableSortSpecs* specs) {
    if (!specs || !specs->SpecsDirty || specs->SpecsCount == 0) return;
    const int column = specs->Specs[0].ColumnIndex;
    const bool ascending = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
    std::stable_sort(ex.entries.begin(), ex.entries.end(), [column, ascending](const DirEntry& a, const DirEntry& b) {
        bool less = false;
        if (column == 1) less = a.size < b.size;
        else if (column == 2) less = fileType(a) < fileType(b);
        else if (column == 3) {
            const uint64_t ta = (static_cast<uint64_t>(a.changeHi) << 32) | a.changeLo;
            const uint64_t tb = (static_cast<uint64_t>(b.changeHi) << 32) | b.changeLo;
            less = ta < tb;
        } else less = lower(a.name) < lower(b.name);
        // Always keep folders first, regardless of sort direction.
        if (a.isDirectory != b.isDirectory) return a.isDirectory;
        return ascending ? less : !less;
    });
    specs->SpecsDirty = false;
}

void drawExplorerTab(UiState& s, const std::string& ip) {
    auto& ex = s.explorer;
    if (ex.path.empty()) explorerNavigate(s, ip, "Hdd1:\\", false);

    static const char* drives[] = {"Hdd1:\\", "HddX:\\", "GAME:\\", "HDD:\\", "D:\\", "SysExt:\\", "System:\\"};
    static int contextIndex = -1;
    float progressValue = 0.0f;
    std::string progressLabel;
    bool transferRunning = false;
    s.transfer.snapshot(progressValue, progressLabel, transferRunning);

    // Toolbar row
    if (ImGui::Button("< Back", ImVec2(88, 0)) && !ex.history.empty()) {
        const std::string prev = ex.history.back();
        ex.history.pop_back();
        explorerNavigate(s, ip, prev, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("^ Up", ImVec2(70, 0))) explorerUp(s, ip);
    ImGui::SameLine();
    if (ImGui::Button("Refresh", ImVec2(88, 0))) {
        ex.cache.erase(lower(ex.path));
        explorerNavigate(s, ip, ex.path, false);
    }
    ImGui::SameLine();
    if (ImGui::Button(ex.view == ViewMode::Icons ? "Details" : "Icons", ImVec2(80, 0))) {
        ex.view = ex.view == ViewMode::Icons ? ViewMode::Details : ViewMode::Icons;
    }

    const bool hasFile = ex.selected >= 0 && static_cast<size_t>(ex.selected) < ex.entries.size() &&
                         !ex.entries[static_cast<size_t>(ex.selected)].isDirectory;
    ImGui::SameLine();
    if (!hasFile) ImGui::BeginDisabled();
    if (ImGui::Button("Download", ImVec2(116, 0)) && hasFile) {
        explorerDownload(s, ip, ex.entries[static_cast<size_t>(ex.selected)]);
    }
    if (!hasFile) ImGui::EndDisabled();

    // Address bar: drive picker + editable path + Go. No clickable crumbs.
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::Combo("##drive", &ex.driveIndex, drives, IM_ARRAYSIZE(drives))) {
        explorerNavigate(s, ip, drives[ex.driveIndex], true);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-66.0f);
    const bool go = ImGui::InputText("##address", ex.pathBuf, sizeof(ex.pathBuf),
                                     ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (go || ImGui::Button("Go", ImVec2(60, 0))) {
        explorerNavigate(s, ip, ex.pathBuf, true);
    }
    ImGui::Separator();

    // Reserve room for the progress bar and the bottom status/error line so
    // neither can be clipped, then give the file area whatever is left.
    const float lineH = ImGui::GetTextLineHeight();
    const float frameH = ImGui::GetFrameHeight();
    const float itemSp = ImGui::GetStyle().ItemSpacing.y;
    float reserve = lineH + itemSp * 2.0f + 26.0f; // separator + status/error line
    if (transferRunning) reserve += lineH + frameH + itemSp * 4.0f; // label + progress bar
    const float fileAreaH = std::max(80.0f, ImGui::GetContentRegionAvail().y - reserve);

    ImGui::BeginChild("##explorer_files", ImVec2(0, fileAreaH), false, 0);

    if (ex.view == ViewMode::Icons) {
        const float cellW = 132.0f;
        const float cellH = 104.0f;
        const int cols = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / cellW));
        auto* dl = ImGui::GetWindowDrawList();
        for (size_t i = 0; i < ex.entries.size(); ++i) {
            if (i % static_cast<size_t>(cols) != 0) ImGui::SameLine(0.0f, 0.0f);
            const auto& e = ex.entries[i];
            const std::string id = "##item" + std::to_string(i);
            const ImVec2 start = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton(id.c_str(), ImVec2(cellW - 4, cellH - 4));
            const bool hovered = ImGui::IsItemHovered();
            const bool clicked = ImGui::IsItemClicked(0);
            const bool doubleClicked = clicked && ImGui::IsMouseDoubleClicked(0);
            const bool selected = ex.selected == static_cast<int>(i);

            if (selected || hovered) {
                dl->AddRectFilled(start, start + ImVec2(cellW - 6, cellH - 6),
                                  selected ? IM_COL32(70, 150, 90, 90) : IM_COL32(255, 255, 255, 18), 4.0f);
            }
            const float iconSize = 54.0f;
            const ImVec2 iconPos = start + ImVec2((cellW - iconSize) * 0.5f, 6.0f);
            if (e.isDirectory) drawBigFolder(dl, iconPos, iconSize);
            else drawBigFile(dl, iconPos, iconSize, e.name);

            const char* label = e.name.c_str();
            const ImVec2 textSize = ImGui::CalcTextSize(label);
            const float textX = start.x + (cellW - 6 - textSize.x) * 0.5f;
            dl->AddText(ImVec2(std::max(start.x + 2.0f, textX), start.y + iconSize + 10.0f),
                        selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(205, 214, 222, 255), label);

            if (clicked) ex.selected = static_cast<int>(i);
            if (ImGui::OpenPopupOnItemClick("##file_context", ImGuiPopupFlags_MouseButtonRight)) {
                ex.selected = static_cast<int>(i);
                contextIndex = static_cast<int>(i);
            }
            if (doubleClicked) {
                if (e.isDirectory) explorerNavigate(s, ip, ex.path + e.name, true);
                else explorerDownload(s, ip, e);
            }
        }
    } else {
        if (ImGui::BeginTable("explorer_table", 4,
                              ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable |
                              ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 100.0f, 1);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 190.0f, 2);
            ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 150.0f, 3);
            ImGui::TableHeadersRow();
            sortEntries(ex, ImGui::TableGetSortSpecs());

            for (size_t i = 0; i < ex.entries.size(); ++i) {
                const auto& e = ex.entries[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                drawFileIcon(e);
                const bool selected = ex.selected == static_cast<int>(i);
                if (ImGui::Selectable(e.name.c_str(), selected,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    ex.selected = static_cast<int>(i);
                }
                if (ImGui::OpenPopupOnItemClick("##file_context", ImGuiPopupFlags_MouseButtonRight)) {
                    ex.selected = static_cast<int>(i);
                    contextIndex = static_cast<int>(i);
                }
                if (ImGui::IsItemClicked(0) && ImGui::IsMouseDoubleClicked(0)) {
                    ex.selected = static_cast<int>(i);
                    if (e.isDirectory) explorerNavigate(s, ip, ex.path + e.name, true);
                    else explorerDownload(s, ip, e);
                }
                ImGui::TableNextColumn();
                if (!e.isDirectory) ImGui::TextDisabled("%s", formatSize(e.size).c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", fileType(e).c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", formatFileTime(e.changeHi, e.changeLo).c_str());
            }
            ImGui::EndTable();
        }
    }

    // Context menu for a right-clicked item. It must be handled in the same
    // window/ID scope as OpenPopupOnItemClick (i.e. inside this child).
    if (ImGui::BeginPopup("##file_context")) {
        if (contextIndex >= 0 && static_cast<size_t>(contextIndex) < ex.entries.size()) {
            const auto& e = ex.entries[static_cast<size_t>(contextIndex)];
            if (e.isDirectory && ImGui::MenuItem("Open folder")) {
                explorerNavigate(s, ip, ex.path + e.name, true);
            }
            if (ImGui::MenuItem(e.isDirectory ? "Download folder to Downloads" : "Download to Downloads")) {
                explorerDownloadToDownloads(s, ip, e);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Rename")) {
                ex.renameRemote = ex.path + e.name;
                std::snprintf(ex.nameBuf, sizeof(ex.nameBuf), "%s", e.name.c_str());
                ex.openRename = true;
            }
            if (ImGui::MenuItem("New folder")) {
                ex.openNewFolder = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete")) {
                ex.deleteRemote = ex.path + e.name;
                ex.deleteName = e.name;
                ex.deleteIsDir = e.isDirectory;
                ex.openDelete = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Copy path")) {
                const std::string full = ex.path + e.name;
                ::OpenClipboard(nullptr);
                ::EmptyClipboard();
                HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, full.size() + 1);
                if (mem) {
                    std::memcpy(::GlobalLock(mem), full.c_str(), full.size() + 1);
                    ::GlobalUnlock(mem);
                    ::SetClipboardData(CF_TEXT, mem);
                }
                ::CloseClipboard();
                ex.status = "Copied path: " + full;
            }
        }
        ImGui::EndPopup();
    }

    // Right-click on empty space: create a folder or refresh this directory.
    if (ImGui::BeginPopupContextWindow("##bg_context",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem("New folder")) {
            ex.openNewFolder = true;
        }
        if (ImGui::MenuItem("Refresh")) {
            ex.cache.erase(lower(ex.path));
            explorerNavigate(s, ip, ex.path, false);
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();

    if (transferRunning) {
        ImGui::Spacing();
        drawTransfer(s);
    }

    // New-folder / rename dialogs. Open and begin in the same scope so the
    // popup IDs match.
    if (ex.openNewFolder) {
        ex.openNewFolder = false;
        ex.nameBuf[0] = '\0';
        ImGui::OpenPopup("New folder");
    }
    if (ex.openRename) {
        ex.openRename = false;
        ImGui::OpenPopup("Rename");
    }
    if (ex.openDelete) {
        ex.openDelete = false;
        ImGui::OpenPopup("Delete");
    }
    if (ImGui::BeginPopupModal("New folder", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Create a folder in:");
        ImGui::TextDisabled("%s", ex.path.c_str());
        ImGui::Spacing();
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(260.0f);
        const bool enter = ImGui::InputText("##newfolder_name", ex.nameBuf, sizeof(ex.nameBuf),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        const bool create = ImGui::Button("Create", ImVec2(96, 0)) || enter;
        ImGui::SameLine();
        const bool cancel = ImGui::Button("Cancel", ImVec2(96, 0));
        if (create && ex.nameBuf[0]) {
            explorerCreateFolder(s, ip, ex.nameBuf);
            ImGui::CloseCurrentPopup();
        } else if (cancel) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Rename", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Rename to:");
        ImGui::Spacing();
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(260.0f);
        const bool enter = ImGui::InputText("##rename_name", ex.nameBuf, sizeof(ex.nameBuf),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        const bool rename = ImGui::Button("Rename", ImVec2(96, 0)) || enter;
        ImGui::SameLine();
        const bool cancel = ImGui::Button("Cancel", ImVec2(96, 0));
        if (rename && ex.nameBuf[0]) {
            explorerRename(s, ip, ex.renameRemote, ex.nameBuf);
            ImGui::CloseCurrentPopup();
        } else if (cancel) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(ex.deleteIsDir ? "Delete this folder and everything inside it?"
                                              : "Delete this file?");
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.30f, 1.0f), "%s", ex.deleteName.c_str());
        ImGui::TextDisabled("This cannot be undone.");
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.18f, 0.16f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.74f, 0.22f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.52f, 0.13f, 0.11f, 1.0f));
        const bool doDelete = ImGui::Button("Delete", ImVec2(96, 0));
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        const bool cancel = ImGui::Button("Cancel", ImVec2(96, 0));
        if (doDelete) {
            explorerDelete(s, ip, ex.deleteRemote, ex.deleteName, ex.deleteIsDir);
            ImGui::CloseCurrentPopup();
        } else if (cancel) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::Separator();
    if (!ex.error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f), "%s", ex.error.c_str());
    } else {
        std::string status = std::to_string(ex.entries.size()) + " items";
        if (!ex.status.empty()) status += "     " + ex.status;
        if (ex.selected >= 0 && static_cast<size_t>(ex.selected) < ex.entries.size()) {
            const auto& e = ex.entries[static_cast<size_t>(ex.selected)];
            status += "     Selected: " + e.name;
            if (!e.isDirectory) status += " (" + formatSize(e.size) + ")";
        }
        ImGui::TextDisabled("%s", status.c_str());
    }
    if (s.jobs.busy()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.40f, 0.84f, 0.45f, 1.0f), "     working...");
    }
}

// ---------- screens ----------

void drawHome(UiState& s, float height) {
    const float bodyY = kHeaderH;
    const float bodyH = height - bodyY;
    const float leftW = ImGui::GetContentRegionAvail().x * 0.60f - 8.0f;

    if (g_titleFont) ImGui::PushFont(g_titleFont);
    ImGui::TextUnformatted("X360 Toolkit");
    if (g_titleFont) ImGui::PopFont();
    ImGui::TextDisabled("Select a console to manage, or use an offline tool.");
    ImGui::SetCursorPosY(bodyY - 6.0f);

    ImGui::BeginChild("##home_left", ImVec2(leftW, bodyH), true, 0);
    sectionTitle("YOUR XBOXES");
    const float cardW = (ImGui::GetContentRegionAvail().x - kCardGap) * 0.5f;
    int drawn = 0;
    for (size_t i = 0; i < s.consoles.size() && drawn < 4; ++i) {
        const auto& c = s.consoles[i];
        const std::string label = c.name + "\n" + c.ip;
        if (ImGui::Button(label.c_str(), ImVec2(cardW, kCardH))) {
            s.selected = static_cast<int>(i);
            s.screen = Screen::Console;
            s.consoleTab = ConsoleTab::Console;
            s.logText.clear();
            s.infoText = "Loading...\n";
            runTextJob(s, "info", "console", {"info", c.ip});
        }
        if ((drawn & 1) == 0) ImGui::SameLine();
        ++drawn;
    }
    if (drawn & 1) ImGui::NewLine();
    if (s.consoles.empty()) ImGui::TextDisabled("No consoles saved yet.");
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##home_right", ImVec2(0, bodyH), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    sectionTitle("ADD XBOX");
    field("##new_name", "Name", s.newName, sizeof(s.newName));
    field("##new_ip", "IP address", s.newIp, sizeof(s.newIp));
    if (ImGui::Button("Add console", ImVec2(150, 0))) {
        if (s.newName[0] && s.newIp[0]) {
            s.consoles.push_back({s.newName, s.newIp});
            saveConsoles(s);
            s.newName[0] = '\0';
            s.newIp[0] = '\0';
        }
    }
    ImGui::Spacing();
    ImGui::Spacing();
    sectionTitle("OFFLINE TOOLS");
    if (ImGui::Button("XUIZ / XZP", ImVec2(120, 0))) { s.tool = Tool::Xuiz; s.screen = Screen::Tools; }
    ImGui::SameLine();
    if (ImGui::Button("XEX", ImVec2(80, 0))) { s.tool = Tool::Xex; s.screen = Screen::Tools; }
    ImGui::SameLine();
    if (ImGui::Button("NAND", ImVec2(80, 0))) { s.tool = Tool::Nand; s.screen = Screen::Tools; }
    ImGui::Spacing();
    ImGui::Spacing();
    sectionTitle("HELP");
    if (ImGui::Button("Command reference", ImVec2(180, 0))) s.showHelp = true;
    ImGui::EndChild();
}

void drawConsoleTab(UiState& s, const std::string& ip) {
    const float leftW = 320.0f;
    ImGui::BeginChild("##console_left", ImVec2(leftW, 0), true, 0);
    sectionTitle("CONSOLE");
    if (ImGui::Button("Refresh info", ImVec2(-1, 0))) runTextJob(s, "info", "console", {"info", ip});
    if (ImGui::Button("Probe", ImVec2(-1, 0))) runTextJob(s, "log", "console", {"probe", ip});
    if (ImGui::Button("Drives", ImVec2(-1, 0))) runTextJob(s, "log", "console", {"drives", ip});
    if (ImGui::Button("Screenshot", ImVec2(-1, 0))) captureScreenshot(s, ip);
    ImGui::Spacing();
    sectionTitle("INFO");
    if (s.infoText == "Loading...\n") {
        ImGui::TextDisabled("Loading...");
    } else if (s.infoText.empty()) {
        ImGui::TextDisabled("No info.");
    } else {
        for (const auto& kv : parseInfo(s.infoText)) {
            ImGui::TextDisabled("%s", kv.first.c_str());
            ImGui::TextWrapped("%s", kv.second.c_str());
            ImGui::Spacing();
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##console_right", ImVec2(0, 0), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    field("##remote", "Remote path", s.remotePath, sizeof(s.remotePath));
    field("##local", "Save as (local)", s.localPath, sizeof(s.localPath));
    if (ImGui::Button("Dir", ImVec2(90, 0))) runTextJob(s, "log", "console", {"dir", ip, s.remotePath});
    ImGui::SameLine();
    if (ImGui::Button("Download", ImVec2(110, 0))) runTextJob(s, "log", "console", {"get", ip, s.remotePath, s.localPath});
    ImGui::Spacing();
    field("##raw", "XBDM command", s.rawCommand, sizeof(s.rawCommand));
    if (ImGui::Button("Send command", ImVec2(140, 0))) runTextJob(s, "log", "console", {"cmd", ip, s.rawCommand});
    ImGui::Spacing();
    drawTransfer(s);
    sectionTitle("OUTPUT");
    outputPane("##console_log", s.logText, -1.0f);
    ImGui::EndChild();
}

// ---------- trainer ----------

enum MemTypeId {
    kMemI8 = 0,
    kMemI16,
    kMemI32,
    kMemI64,
    kMemU8,
    kMemU16,
    kMemU32,
    kMemU64,
    kMemF32,
    kMemF64,
};

const char* const kMemTypeNames[] = {"Int8", "Int16", "Int32", "Int64", "UInt8",
                                     "UInt16", "UInt32", "UInt64", "Float", "Double"};

// First-scan (and next-scan) predicates. A first scan walks the whole mapped
// range; a next scan re-reads the surviving addresses and filters them again.
enum FirstScanId { kFirstExact = 0, kFirstUnknown, kFirstBigger, kFirstSmaller, kFirstBetween };
enum NextScanId {
    kNextExact = 0,
    kNextBigger,
    kNextSmaller,
    kNextBetween,
    kNextIncreased,
    kNextDecreased,
    kNextIncreasedBy,
    kNextDecreasedBy,
    kNextChanged,
    kNextUnchanged
};

const char* const kScanFirstNames[] = {"Exact", "Unknown", "Bigger", "Smaller", "Between"};
const char* const kScanNextNames[] = {"Exact",      "Bigger",    "Smaller",     "Between",
                                      "Increased",  "Decreased", "Increased by", "Decreased by",
                                      "Changed",    "Unchanged"};

int memTypeSize(int t) {
    switch (t) {
    case kMemI8:
    case kMemU8: return 1;
    case kMemI16:
    case kMemU16: return 2;
    case kMemI32:
    case kMemU32:
    case kMemF32: return 4;
    case kMemI64:
    case kMemU64:
    case kMemF64: return 8;
    default: return 4;
    }
}

bool memTypeIsSigned(int t) {
    return t == kMemI8 || t == kMemI16 || t == kMemI32 || t == kMemI64;
}

double memNumber(int t, const uint8_t* p) {
    switch (t) {
    case kMemI8: return static_cast<int8_t>(p[0]);
    case kMemU8: return p[0];
    case kMemI16: { int16_t v; std::memcpy(&v, p, 2); return v; }
    case kMemU16: { uint16_t v; std::memcpy(&v, p, 2); return v; }
    case kMemI32: { int32_t v; std::memcpy(&v, p, 4); return v; }
    case kMemU32: { uint32_t v; std::memcpy(&v, p, 4); return v; }
    case kMemI64: { int64_t v; std::memcpy(&v, p, 8); return static_cast<double>(v); }
    case kMemU64: { uint64_t v; std::memcpy(&v, p, 8); return static_cast<double>(v); }
    case kMemF32: { float v; std::memcpy(&v, p, 4); return v; }
    case kMemF64: { double v; std::memcpy(&v, p, 8); return v; }
    default: return 0.0;
    }
}

std::string memDisplay(int t, const uint8_t* p) {
    char buf[64]{};
    if (t == kMemF32 || t == kMemF64) std::snprintf(buf, sizeof(buf), "%.6g", memNumber(t, p));
    else if (t == kMemU64) std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(memNumber(t, p)));
    else if (t == kMemI64) std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(memNumber(t, p)));
    else std::snprintf(buf, sizeof(buf), "%g", memNumber(t, p));
    return buf;
}

bool parseHexU32(const char* text, uint32_t& out) {
    if (!text || !*text) return false;
    std::string s(text);
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (s.compare(i, 2, "0x") == 0 || s.compare(i, 2, "0X") == 0) i += 2;
    if (i >= s.size()) return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s.c_str() + i, &end, 16);
    if (end == s.c_str() + i) return false;
    out = static_cast<uint32_t>(v);
    return true;
}

bool parseScanValue(int t, const std::string& text, std::vector<uint8_t>& bytes, double& number) {
    std::string s = text;
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    if (s.empty()) return false;

    const size_t n = static_cast<size_t>(memTypeSize(t));
    bytes.assign(n, 0);

    if (t == kMemF32) {
        const float f = static_cast<float>(std::strtod(s.c_str(), nullptr));
        std::memcpy(bytes.data(), &f, 4);
        number = f;
        return true;
    }
    if (t == kMemF64) {
        const double d = std::strtod(s.c_str(), nullptr);
        std::memcpy(bytes.data(), &d, 8);
        number = d;
        return true;
    }

    const bool neg = !s.empty() && s[0] == '-';
    const char* p = s.c_str() + (neg ? 1 : 0);
    int base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    const unsigned long long mag = std::strtoull(p, nullptr, base);

    if (memTypeIsSigned(t)) {
        const long long sv = neg ? -static_cast<long long>(mag) : static_cast<long long>(mag);
        const unsigned long long uv = static_cast<unsigned long long>(sv);
        for (size_t i = 0; i < n; ++i) bytes[i] = static_cast<uint8_t>((uv >> (8 * i)) & 0xff);
        number = static_cast<double>(sv);
    } else {
        for (size_t i = 0; i < n; ++i) bytes[i] = static_cast<uint8_t>((mag >> (8 * i)) & 0xff);
        number = static_cast<double>(mag);
    }
    return true;
}

// Walks every mapped byte in [start, start+length), reading it in chunks. For
// each chunk the callback gets the chunk address, a buffer that may extend a
// few bytes past `newLen` (so values straddling a chunk boundary are still
// visible), the buffer size, and `newLen`. Returning false stops the walk.
void forEachMappedChunk(const std::vector<MemRegion>& regions, uint32_t start, uint32_t length,
                        size_t overlap, XbdmClient& client, std::string& error, bool& aborted,
                        const std::function<bool(uint32_t, const uint8_t*, size_t, size_t)>& fn) {
    aborted = false;
    if (length == 0) return;

    struct Range { uint32_t base; uint32_t size; };
    std::vector<Range> ranges;
    const uint64_t windowEnd = static_cast<uint64_t>(start) + length;
    if (regions.empty()) {
        ranges.push_back({start, length});
    } else {
        for (const auto& r : regions) {
            const uint64_t rb = r.base;
            const uint64_t re = static_cast<uint64_t>(r.base) + r.size;
            const uint64_t a = std::max<uint64_t>(rb, start);
            const uint64_t b = std::min<uint64_t>(re, windowEnd);
            if (b > a) ranges.push_back({static_cast<uint32_t>(a), static_cast<uint32_t>(b - a)});
        }
    }

    constexpr uint32_t kChunk = 0x40000;
    for (const auto& rg : ranges) {
        uint32_t off = 0;
        while (off < rg.size) {
            const uint32_t newLen = std::min<uint32_t>(kChunk, rg.size - off);
            const uint32_t extra =
                std::min<uint32_t>(static_cast<uint32_t>(overlap), rg.size - off - newLen);
            std::vector<uint8_t> data;
            const uint32_t addr = rg.base + off;
            if (!client.getMemory(addr, newLen + extra, data, error)) {
                aborted = true;
                return;
            }
            if (data.size() < newLen) {
                error = "short memory read";
                aborted = true;
                return;
            }
            if (!fn(addr, data.data(), data.size(), newLen)) return;
            off += newLen;
        }
    }
}

// Total bytes of mapped memory that a scan of [start, start+length) will read;
// used for the progress bar.
uint32_t mappedBytesInWindow(const std::vector<MemRegion>& regions, uint32_t start, uint32_t length) {
    if (length == 0) return 0;
    const uint64_t windowEnd = static_cast<uint64_t>(start) + length;
    if (regions.empty()) return length;
    uint64_t total = 0;
    for (const auto& r : regions) {
        const uint64_t a = std::max<uint64_t>(r.base, start);
        const uint64_t b = std::min<uint64_t>(static_cast<uint64_t>(r.base) + r.size, windowEnd);
        if (b > a) total += (b - a);
    }
    return total > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(total);
}

void trainerRefreshTarget(UiState& s, const std::string& ip) {
    s.trainer.status = "Reading target...";
    s.trainer.error.clear();
    s.jobs.submit("trainer_refresh", [&s, ip] {
        JobResult r;
        r.id = "trainer_refresh";
        XbdmClient client;
        std::string err;
        if (!client.connect(ip, 730, err)) {
            r.text = "Trainer: " + err;
            return r;
        }
        std::string name;
        const auto info = client.command("XBEINFO RUNNING", err);
        for (const auto& line : info) {
            const auto at = line.find("name=\"");
            if (at == std::string::npos) continue;
            const auto end = line.find('"', at + 6);
            if (end != std::string::npos) {
                name = line.substr(at + 6, end - (at + 6));
                break;
            }
        }
        r.path = name;
        std::vector<ModuleInfo> mods;
        std::string modErr;
        client.listModules(mods, modErr);
        std::vector<MemRegion> regs;
        std::string regErr;
        client.listMemoryRegions(regs, regErr);
        client.close();
        r.modules = std::move(mods);
        r.regions = std::move(regs);
        return r;
    });
}

// Little-endian bytes -> uint64, used for exact bit compares.
uint64_t readBits(const uint8_t* p, size_t n) {
    uint64_t bits = 0;
    for (size_t i = 0; i < n && i < 8; ++i) bits |= static_cast<uint64_t>(p[i]) << (8 * i);
    return bits;
}

std::vector<uint8_t> bytesFromBits(uint64_t bits, int type) {
    const size_t n = static_cast<size_t>(memTypeSize(type));
    std::vector<uint8_t> bytes(n, 0);
    for (size_t i = 0; i < n; ++i) bytes[i] = static_cast<uint8_t>((bits >> (8 * i)) & 0xff);
    return bytes;
}

std::string displayFromBits(int type, uint64_t bits) {
    const std::vector<uint8_t> bytes = bytesFromBits(bits, type);
    return memDisplay(type, bytes.data());
}

std::string hex8(uint32_t v) {
    char buf[16]{};
    std::snprintf(buf, sizeof(buf), "%08X", v);
    return buf;
}

// Reads whitespace/comma separated hex bytes ("90 90 12" or "909012").
bool parseHexBytes(const std::string& text, std::vector<uint8_t>& out) {
    out.clear();
    int hi = -1;
    for (char ch : text) {
        int v;
        if (ch >= '0' && ch <= '9') v = ch - '0';
        else if (ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
        else continue;
        if (hi < 0) hi = v;
        else { out.push_back(static_cast<uint8_t>((hi << 4) | v)); hi = -1; }
    }
    return hi < 0 && !out.empty();
}

// Result rows the user is acting on: every ticked row, or the single highlighted
// row when nothing is ticked.
std::vector<size_t> trainerChosen(const TrainerState& tr) {
    std::vector<size_t> out;
    for (size_t i = 0; i < tr.candidates.size(); ++i)
        if (tr.candidates[i].selected) out.push_back(i);
    if (out.empty() && tr.selected >= 0 && static_cast<size_t>(tr.selected) < tr.candidates.size())
        out.push_back(static_cast<size_t>(tr.selected));
    return out;
}

// First scan: walks the whole mapped range and keeps addresses that satisfy the
// chosen predicate. "Unknown" keeps every addressed value so it can be narrowed
// afterwards (the usual way to find a health bar).
void trainerFirstScan(UiState& s, const std::string& ip, int mode) {
    if (s.trainer.scanning) return;
    uint32_t start = 0;
    uint32_t length = 0;
    if (!parseHexU32(s.trainer.startHex, start) || !parseHexU32(s.trainer.lengthHex, length)) {
        s.trainer.error = "Start and Length must be hex values.";
        return;
    }
    if (length == 0) {
        s.trainer.error = "Length must be greater than zero.";
        return;
    }
    const int type = s.trainer.valueType;
    s.trainer.firstMode = mode;

    std::vector<uint8_t> needle;
    double lo = 0.0;
    double hi = 0.0;
    if (mode != kFirstUnknown) {
        if (!parseScanValue(type, s.trainer.value, needle, lo)) {
            s.trainer.error = "Enter a value to scan for.";
            return;
        }
        if (mode == kFirstBetween) {
            std::vector<uint8_t> n2;
            if (!parseScanValue(type, s.trainer.value2, n2, hi)) {
                s.trainer.error = "Enter the upper bound for the Between scan.";
                return;
            }
        }
    }
    const uint64_t needleBits = needle.empty() ? 0 : readBits(needle.data(), needle.size());
    const std::vector<MemRegion> regions = s.trainer.regions;

    s.trainer.scanning = true;
    s.trainer.error.clear();
    s.trainer.status = (mode == kFirstUnknown) ? "Snapshotting memory..." : "Scanning...";
    s.trainer.scannedBytes.store(0);
    s.trainer.liveMatches.store(0);
    s.trainer.totalBytes.store(mappedBytesInWindow(regions, start, length));
    s.jobs.submit("trainer_scan", [&s, ip, start, length, type, needleBits, lo, hi, mode, regions] {
        JobResult r;
        r.id = "trainer_scan";
        XbdmClient client;
        std::string err;
        if (!client.connect(ip, 730, err)) {
            r.text = "Trainer: " + err;
            return r;
        }
        const size_t tsize = static_cast<size_t>(memTypeSize(type));
        const size_t step = (mode == kFirstUnknown) ? (tsize ? tsize : 1) : 1;
        const size_t cap = (mode == kFirstUnknown) ? 3000000u : 1000000u;
        std::vector<MemCandidate> out;
        if (mode == kFirstUnknown) {
            const uint64_t est = mappedBytesInWindow(regions, start, length) / step;
            out.reserve(static_cast<size_t>(std::min<uint64_t>(est + 16, cap)));
        } else {
            out.reserve(1u << 16);
        }
        bool truncated = false;
        bool aborted = false;
        forEachMappedChunk(regions, start, length, tsize ? tsize - 1 : 0, client, err, aborted,
                           [&](uint32_t base, const uint8_t* data, size_t dataSize, size_t newLen) {
                               s.trainer.scannedBytes.fetch_add(static_cast<uint32_t>(newLen));
                               for (size_t o = 0; o + tsize <= dataSize && o < newLen; o += step) {
                                   const uint8_t* p = data + o;
                                   const uint64_t bits = readBits(p, tsize);
                                   const double num = memNumber(type, p);
                                   bool keep = false;
                                   switch (mode) {
                                   case kFirstUnknown: keep = true; break;
                                   case kFirstExact: keep = bits == needleBits; break;
                                   case kFirstBigger: keep = num > lo; break;
                                   case kFirstSmaller: keep = num < lo; break;
                                   case kFirstBetween: keep = num >= lo && num <= hi; break;
                                   }
                                   if (!keep) continue;
                                   if (out.size() >= cap) { truncated = true; return false; }
                                   MemCandidate c;
                                   c.address = base + static_cast<uint32_t>(o);
                                   c.bits = bits;
                                   c.number = num;
                                   c.display = memDisplay(type, p);
                                   out.push_back(std::move(c));
                                   s.trainer.liveMatches.fetch_add(1);
                               }
                               return true;
                           });
        client.close();
        if (aborted) {
            r.text = "Scan stopped: " + err;
            return r;
        }
        char msg[160]{};
        if (mode == kFirstUnknown) {
            std::snprintf(msg, sizeof(msg),
                          "%zu snapshot(s)%s. Change the value in game, then pick Decreased or Changed and press Next scan.",
                          out.size(), truncated ? " (cap reached - narrow the range)" : "");
        } else {
            std::snprintf(msg, sizeof(msg), "%zu match(es)%s.", out.size(),
                          truncated ? " (cap reached - narrow the range)" : "");
        }
        r.text = msg;
        r.candidates = std::move(out);
        r.hasCandidates = true;
        return r;
    });
}

// Next scan: re-reads the surviving addresses and keeps those that match the
// chosen predicate against the value captured by the previous scan.
void trainerNextScan(UiState& s, const std::string& ip) {
    if (s.trainer.scanning) return;
    if (!s.trainer.hasScan || s.trainer.candidates.empty()) {
        s.trainer.error = "Run a first scan before a next scan.";
        return;
    }
    const int type = s.trainer.valueType;
    const int mode = s.trainer.nextType;
    std::vector<uint8_t> needle;
    double lo = 0.0;
    double hi = 0.0;
    double delta = 0.0;

    const bool needsValue =
        mode == kNextExact || mode == kNextBigger || mode == kNextSmaller || mode == kNextBetween;
    if (needsValue) {
        if (!parseScanValue(type, s.trainer.value, needle, lo)) {
            s.trainer.error = "Enter a value to compare against.";
            return;
        }
        if (mode == kNextBetween) {
            std::vector<uint8_t> n2;
            if (!parseScanValue(type, s.trainer.value2, n2, hi)) {
                s.trainer.error = "Enter the upper bound for the Between scan.";
                return;
            }
        }
    }
    if (mode == kNextIncreasedBy || mode == kNextDecreasedBy) {
        const std::string d = s.trainer.delta;
        if (d.empty()) {
            s.trainer.error = "Enter the step for the Increased by / Decreased by scan.";
            return;
        }
        delta = std::strtod(d.c_str(), nullptr);
    }
    const uint64_t needleBits = needle.empty() ? 0 : readBits(needle.data(), needle.size());
    const std::vector<MemRegion> regions = s.trainer.regions;

    // A lightweight snapshot of the surviving rows: the scan job only needs the
    // address and the previous value, so this avoids copying thousands of value
    // strings while the job runs.
    struct CandsLite {
        uint32_t address;
        uint64_t bits;
        double number;
    };
    std::vector<CandsLite> cands;
    cands.reserve(s.trainer.candidates.size());
    for (const auto& c : s.trainer.candidates) cands.push_back({c.address, c.bits, c.number});

    s.trainer.scanning = true;
    s.trainer.error.clear();
    s.trainer.status = "Filtering...";
    s.trainer.scannedBytes.store(0);
    s.trainer.liveMatches.store(0);
    {
        const size_t tsize = static_cast<size_t>(memTypeSize(type));
        uint32_t mn = cands.front().address;
        uint32_t mx = cands.front().address;
        for (const auto& c : cands) {
            mn = std::min(mn, c.address);
            mx = std::max(mx, c.address);
        }
        s.trainer.totalBytes.store(mappedBytesInWindow(
            regions, mn, static_cast<uint32_t>(static_cast<uint64_t>(mx) + tsize - mn)));
    }
    s.jobs.submit("trainer_narrow",
                  [&s, ip, type, mode, needleBits, lo, hi, delta, regions, cands = std::move(cands)]() mutable {
                      JobResult r;
                      r.id = "trainer_narrow";
                      XbdmClient client;
                      std::string err;
                      if (!client.connect(ip, 730, err)) {
                          r.text = "Trainer: " + err;
                          return r;
                      }
                      std::sort(cands.begin(), cands.end(),
                                [](const CandsLite& a, const CandsLite& b) {
                                    return a.address < b.address;
                                });
                      const size_t tsize = static_cast<size_t>(memTypeSize(type));
                      const size_t cap = 3000000u;
                      std::vector<MemCandidate> out;
                      size_t idx = 0;
                      bool aborted = false;
                      bool truncated = false;

                      // Read only windows that actually hold candidates. As the
                      // result set thins out this avoids re-reading megabytes of
                      // memory that cannot match.
                      constexpr uint32_t kChunk = 0x40000;
                      auto regionEndFor = [&regions](uint32_t addr) -> uint64_t {
                          if (regions.empty()) return static_cast<uint64_t>(addr) + 0x40000;
                          for (const auto& rg : regions) {
                              const uint64_t b = rg.base;
                              const uint64_t e = b + rg.size;
                              if (addr >= b && addr < e) return e;
                          }
                          return 0;
                      };

                      while (idx < cands.size()) {
                          if (out.size() >= cap) { truncated = true; break; }
                          const uint32_t start = cands[idx].address;
                          const uint64_t regionEnd = regionEndFor(start);
                          if (regionEnd <= start) { ++idx; continue; }
                          const uint32_t len = static_cast<uint32_t>(
                              std::min<uint64_t>(kChunk, regionEnd - start));
                          if (len < tsize) { ++idx; continue; }
                          const uint32_t extra = static_cast<uint32_t>(
                              std::min<uint64_t>(tsize - 1, regionEnd - start - len));
                          std::vector<uint8_t> data;
                          if (!client.getMemory(start, len + extra, data, err)) {
                              aborted = true;
                              break;
                          }
                          if (data.size() < len + extra) {
                              aborted = true;
                              err = "short memory read";
                              break;
                          }
                          s.trainer.scannedBytes.fetch_add(len);
                          const uint64_t chunkEnd = static_cast<uint64_t>(start) + len;
                          while (idx < cands.size() && cands[idx].address < chunkEnd) {
                              const size_t o = cands[idx].address - start;
                              if (o + tsize > data.size()) { ++idx; continue; }
                              const uint8_t* p = data.data() + o;
                              const uint64_t bits = readBits(p, tsize);
                              const double num = memNumber(type, p);
                              const double prev = cands[idx].number;
                              const uint64_t prevBits = cands[idx].bits;
                              bool keep = false;
                              switch (mode) {
                              case kNextExact: keep = bits == needleBits; break;
                              case kNextBigger: keep = num > lo; break;
                              case kNextSmaller: keep = num < lo; break;
                              case kNextBetween: keep = num >= lo && num <= hi; break;
                              case kNextIncreased: keep = num > prev; break;
                              case kNextDecreased: keep = num < prev; break;
                              case kNextIncreasedBy: keep = num == prev + delta; break;
                              case kNextDecreasedBy: keep = num == prev - delta; break;
                              case kNextChanged: keep = bits != prevBits; break;
                              case kNextUnchanged: keep = bits == prevBits; break;
                              }
                              if (keep) {
                                  if (out.size() >= cap) { truncated = true; break; }
                                  MemCandidate c;
                                  c.address = cands[idx].address;
                                  c.prevBits = prevBits;
                                  c.prevNumber = prev;
                                  c.hasPrev = true;
                                  c.bits = bits;
                                  c.number = num;
                                  c.display = memDisplay(type, p);
                                  out.push_back(std::move(c));
                                  s.trainer.liveMatches.fetch_add(1);
                              }
                              ++idx;
                          }
                      }
                      client.close();
                      if (aborted) {
                          r.text = "Filter stopped: " + err;
                          return r;
                      }
                      char msg[96]{};
                      std::snprintf(msg, sizeof(msg), "%zu match(es)%s.", out.size(),
                                    truncated ? " (cap reached)" : "");
                      r.text = msg;
                      r.candidates = std::move(out);
                      r.hasCandidates = true;
                      return r;
                  });
}

// One-shot writes of a batch of addresses on a single connection.
void trainerSubmitWrite(UiState& s, const std::string& ip, const std::string& jobId,
                        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> writes) {
    s.jobs.submit(jobId, [&s, ip, jobId, writes = std::move(writes)]() mutable {
        JobResult r;
        r.id = jobId;
        XbdmClient client;
        std::string err;
        if (!client.connect(ip, 730, err)) {
            r.text = "Write failed: " + err;
            return r;
        }
        int ok = 0;
        for (auto& w : writes) {
            std::string e;
            if (client.setMemory(w.first, w.second, e)) ++ok;
        }
        client.close();
        char msg[96]{};
        std::snprintf(msg, sizeof(msg), "Wrote %d of %zu address(es).", ok, writes.size());
        r.text = msg;
        return r;
    });
}

void trainerPokeSelected(UiState& s, const std::string& ip) {
    const std::vector<size_t> chosen = trainerChosen(s.trainer);
    if (chosen.empty()) {
        s.trainer.error = "Tick one or more results, or click a row, then Poke.";
        return;
    }
    std::vector<uint8_t> bytes;
    double number = 0.0;
    const std::string text = s.trainer.writeValue[0] ? s.trainer.writeValue : s.trainer.value;
    if (!parseScanValue(s.trainer.valueType, text, bytes, number)) {
        s.trainer.error = "Enter a value to write.";
        return;
    }
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> writes;
    writes.reserve(chosen.size());
    for (size_t i : chosen) writes.emplace_back(s.trainer.candidates[i].address, bytes);
    s.trainer.status = "Writing...";
    s.trainer.error.clear();
    trainerSubmitWrite(s, ip, "trainer_write", std::move(writes));
}

void trainerAddSelectedToPins(UiState& s) {
    const std::vector<size_t> chosen = trainerChosen(s.trainer);
    if (chosen.empty()) {
        s.trainer.error = "Tick one or more results, or click a row, then add them.";
        return;
    }
    int added = 0;
    for (size_t i : chosen) {
        const MemCandidate& c = s.trainer.candidates[i];
        bool exists = false;
        for (const auto& p : s.trainer.pins)
            if (p.address == c.address) { exists = true; break; }
        if (exists) continue;
        PinnedEntry p;
        p.address = c.address;
        p.type = s.trainer.valueType;
        p.bits = c.bits;
        p.number = c.number;
        p.valueText = c.display;
        p.bytes = bytesFromBits(c.bits, p.type);
        p.freeze = false;
        std::snprintf(p.edit, sizeof(p.edit), "%s", c.display.c_str());
        s.trainer.pins.push_back(std::move(p));
        ++added;
    }
    if (added == 0) s.trainer.pinStatus = "Those address(es) are already listed.";
    else s.trainer.pinStatus = "Added " + std::to_string(added) + " address(es) to the list.";
    s.trainer.error.clear();
}

void trainerPinAddManual(UiState& s) {
    uint32_t addr = 0;
    if (!parseHexU32(s.trainer.pinAddr, addr)) {
        s.trainer.pinStatus = "Enter a hex address to add.";
        return;
    }
    std::vector<uint8_t> bytes;
    double number = 0.0;
    if (!parseScanValue(s.trainer.valueType, s.trainer.value, bytes, number)) {
        s.trainer.pinStatus = "Enter a value for the new address.";
        return;
    }
    PinnedEntry p;
    p.address = addr;
    p.type = s.trainer.valueType;
    p.bits = readBits(bytes.data(), bytes.size());
    p.number = number;
    p.valueText = memDisplay(p.type, bytes.data());
    p.bytes = bytes;
    p.freeze = false;
    std::snprintf(p.edit, sizeof(p.edit), "%s", p.valueText.c_str());
    s.trainer.pins.push_back(std::move(p));
    s.trainer.pinStatus = "Added 0x" + hex8(addr) + ".";
}

void trainerPinRemove(UiState& s, size_t index) {
    if (index >= s.trainer.pins.size()) return;
    s.trainer.pins.erase(s.trainer.pins.begin() + static_cast<long>(index));
    s.trainer.freezeDirty = true;
}

void trainerPinPoke(UiState& s, const std::string& ip, size_t index) {
    if (index >= s.trainer.pins.size()) return;
    const PinnedEntry& p = s.trainer.pins[index];
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> writes;
    writes.emplace_back(p.address, p.bytes);
    s.trainer.status = "Writing...";
    s.trainer.error.clear();
    trainerSubmitWrite(s, ip, "trainer_write", std::move(writes));
}

std::filesystem::path trainerPinsPath() {
    char buffer[MAX_PATH]{};
    ::GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path() / "trainer_pins.txt";
}

void trainerSavePins(UiState& s) {
    std::ofstream out(trainerPinsPath());
    if (!out) {
        s.trainer.pinStatus = "Could not write trainer_pins.txt.";
        return;
    }
    out << "# X360 Toolkit trainer address list\n";
    out << "# address type value freeze\n";
    for (const auto& p : s.trainer.pins) {
        out << hex8(p.address) << " " << p.type << " " << p.valueText << " " << (p.freeze ? 1 : 0)
            << "\n";
    }
    s.trainer.pinStatus = "Saved " + std::to_string(s.trainer.pins.size()) + " address(es).";
}

void loadTrainerPins(UiState& s) {
    std::ifstream in(trainerPinsPath());
    if (!in) return;
    s.trainer.pins.clear();
    std::string line;
    int loaded = 0;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string addrText, valueText;
        int type = 2;
        int freeze = 0;
        if (!(ss >> addrText >> type >> valueText >> freeze)) continue;
        uint32_t addr = 0;
        if (!parseHexU32(addrText.c_str(), addr)) continue;
        if (type < 0 || type >= IM_ARRAYSIZE(kMemTypeNames)) type = 2;
        std::vector<uint8_t> bytes;
        double number = 0.0;
        if (!parseScanValue(type, valueText, bytes, number)) continue;
        PinnedEntry p;
        p.address = addr;
        p.type = type;
        p.bits = readBits(bytes.data(), bytes.size());
        p.number = number;
        p.valueText = memDisplay(type, bytes.data());
        p.bytes = bytes;
        p.freeze = freeze != 0;
        std::snprintf(p.edit, sizeof(p.edit), "%s", p.valueText.c_str());
        s.trainer.pins.push_back(std::move(p));
        ++loaded;
    }
    if (loaded > 0) s.trainer.pinStatus = "Loaded " + std::to_string(loaded) + " address(es).";
    s.trainer.freezeDirty = true;
}

// Pushes the frozen subset of the address list to the freeze worker.
void trainerRebuildFreeze(UiState& s, const std::string& ip) {
    std::vector<FreezeTarget> list;
    for (const auto& p : s.trainer.pins)
        if (p.freeze) list.push_back({p.address, p.bytes});
    s.trainer.freeze.setTargets(ip, std::move(list));
    s.trainer.freezeDirty = false;
    s.trainer.freezeIp = ip;
}

// Reads a page of memory for the hex viewer.
void trainerView(UiState& s, const std::string& ip, uint32_t base) {
    s.trainer.viewerBase = base;
    std::snprintf(s.trainer.viewerAddr, sizeof(s.trainer.viewerAddr), "%08X", base);
    s.trainer.viewerReading = true;
    s.trainer.viewerStatus = "Reading...";
    s.jobs.submit("trainer_mem", [&s, ip, base] {
        JobResult r;
        r.id = "trainer_mem";
        XbdmClient client;
        std::string err;
        if (!client.connect(ip, 730, err)) {
            r.text = "Trainer: " + err;
            return r;
        }
        static const uint32_t sizes[] = {0x200, 0x100, 0x80, 0x40, 0x20, 0x10};
        std::vector<uint8_t> data;
        std::string lastErr = "read failed";
        for (uint32_t want : sizes) {
            if (client.getMemory(base, want, data, lastErr)) break;
            data.clear();
        }
        client.close();
        if (data.empty()) {
            r.text = "Read failed: " + lastErr;
            return r;
        }
        r.memBytes = std::move(data);
        r.memBase = base;
        r.hasMem = true;
        return r;
    });
}

void drawTrainerFooter(UiState& s) {
    if (!s.trainer.error.empty()) ImGui::TextWrapped("%s", s.trainer.error.c_str());
    else ImGui::TextDisabled("%s", s.trainer.status.c_str());
}

// ----- Scanner sub-tab -----
void drawTrainerScanner(UiState& s, const std::string& ip) {
    auto& tr = s.trainer;

    if (ImGui::Button("Refresh target", ImVec2(120, 0))) trainerRefreshTarget(s, ip);
    ImGui::SameLine();
    ImGui::TextDisabled("Target: %s", tr.target.empty() ? "(none)" : tr.target.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("| %zu module(s), %zu region(s)", tr.modules.size(), tr.regions.size());

    ImGui::SetNextItemWidth(104.0f);
    ImGui::InputText("Start##scan", tr.startHex, sizeof(tr.startHex));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(104.0f);
    ImGui::InputText("Length##scan", tr.lengthHex, sizeof(tr.lengthHex));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(96.0f);
    ImGui::Combo("Type##scan", &tr.valueType, kMemTypeNames, IM_ARRAYSIZE(kMemTypeNames));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::InputText("Value##scan", tr.value, sizeof(tr.value));
    if (tr.firstMode == kFirstBetween) {
        ImGui::SameLine();
        ImGui::TextDisabled("max");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::InputText("##value2", tr.value2, sizeof(tr.value2));
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(240.0f);
    static int moduleIndex = -1;
    const std::string preview = (moduleIndex >= 0 && static_cast<size_t>(moduleIndex) < tr.modules.size())
                                    ? tr.modules[static_cast<size_t>(moduleIndex)].name
                                    : "Use module range...";
    if (ImGui::BeginCombo("##module", preview.c_str())) {
        for (size_t i = 0; i < tr.modules.size(); ++i) {
            const auto& m = tr.modules[i];
            char label[160]{};
            std::snprintf(label, sizeof(label), "%s  (0x%08X + 0x%X)", m.name.c_str(), m.base, m.size);
            if (ImGui::Selectable(label, moduleIndex == static_cast<int>(i))) {
                moduleIndex = static_cast<int>(i);
                std::snprintf(tr.startHex, sizeof(tr.startHex), "%08X", m.base);
                std::snprintf(tr.lengthHex, sizeof(tr.lengthHex), "%X", m.size);
            }
        }
        ImGui::EndCombo();
    }

    ImGui::BeginDisabled(tr.scanning);
    ImGui::SetNextItemWidth(110.0f);
    ImGui::Combo("##firstmode", &tr.firstMode, kScanFirstNames, IM_ARRAYSIZE(kScanFirstNames));
    ImGui::SameLine();
    if (ImGui::Button("First scan", ImVec2(90, 0))) trainerFirstScan(s, ip, tr.firstMode);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (tr.scanning) ImGui::TextDisabled("%d match(es) so far", tr.liveMatches.load());
    else ImGui::TextDisabled("%zu match(es)", tr.candidates.size());

    ImGui::BeginDisabled(!tr.hasScan || tr.candidates.empty() || tr.scanning);
    ImGui::SetNextItemWidth(120.0f);
    ImGui::Combo("Next##next", &tr.nextType, kScanNextNames, IM_ARRAYSIZE(kScanNextNames));
    ImGui::SameLine();
    if (tr.nextType == kNextIncreasedBy || tr.nextType == kNextDecreasedBy) {
        ImGui::SetNextItemWidth(70.0f);
        ImGui::InputText("Step", tr.delta, sizeof(tr.delta));
        ImGui::SameLine();
    }
    if (ImGui::Button("Next scan", ImVec2(100, 0))) trainerNextScan(s, ip);
    ImGui::EndDisabled();

    ImGui::BeginDisabled(tr.candidates.empty());
    if (ImGui::Button("Select all", ImVec2(84, 0)))
        for (auto& c : tr.candidates) c.selected = true;
    ImGui::SameLine();
    if (ImGui::Button("Select none", ImVec2(90, 0)))
        for (auto& c : tr.candidates) c.selected = false;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputTextWithHint("##write", "value to poke", tr.writeValue, sizeof(tr.writeValue));
    ImGui::SameLine();
    ImGui::BeginDisabled(tr.candidates.empty());
    if (ImGui::Button("Poke", ImVec2(70, 0))) trainerPokeSelected(s, ip);
    ImGui::SameLine();
    if (ImGui::Button("Add to list", ImVec2(96, 0))) trainerAddSelectedToPins(s);
    ImGui::EndDisabled();

    if (tr.scanning) {
        const uint32_t total = tr.totalBytes.load();
        const uint32_t done = tr.scannedBytes.load();
        const float frac = total ? std::min(1.0f, static_cast<float>(done) / static_cast<float>(total)) : 0.0f;
        ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f));
    }

    const float lineH = ImGui::GetTextLineHeight();
    const float itemSp = ImGui::GetStyle().ItemSpacing.y;
    const float progressH = tr.scanning ? ImGui::GetFrameHeight() + itemSp : 0.0f;
    const float tableH = std::max(80.0f, ImGui::GetContentRegionAvail().y - lineH - itemSp * 2.0f - 6.0f - progressH);

    ImGui::BeginChild("##trainer_results", ImVec2(0, tableH), true, 0);
    if (ImGui::BeginTable("##trainer_table", 4,
                          ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("##sel", ImGuiTableColumnFlags_WidthFixed, 26.0f);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Previous", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(tr.candidates.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                auto& c = tr.candidates[static_cast<size_t>(row)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(row);
                ImGui::Checkbox("##sel", &c.selected);
                ImGui::PopID();
                ImGui::TableNextColumn();
                if (ImGui::Selectable(hex8(c.address).c_str(), tr.selected == row)) {
                    tr.selected = row;
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(c.display.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(c.hasPrev ? displayFromBits(tr.valueType, c.prevBits).c_str() : "-");
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    drawTrainerFooter(s);
}

// ----- Address list sub-tab -----
void drawTrainerPins(UiState& s, const std::string& ip) {
    auto& tr = s.trainer;

    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputTextWithHint("##pin_addr", "address", tr.pinAddr, sizeof(tr.pinAddr));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(96.0f);
    ImGui::Combo("##pin_type", &tr.valueType, kMemTypeNames, IM_ARRAYSIZE(kMemTypeNames));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::InputTextWithHint("##pin_value", "value", tr.value, sizeof(tr.value));
    ImGui::SameLine();
    if (ImGui::Button("Add", ImVec2(60, 0))) trainerPinAddManual(s);
    ImGui::SameLine();
    if (ImGui::Button("Save##pins", ImVec2(70, 0))) trainerSavePins(s);
    ImGui::SameLine();
    if (ImGui::Button("Load##pins", ImVec2(70, 0))) loadTrainerPins(s);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu address(es) | %zu frozen", tr.pins.size(), tr.freeze.count());

    const float lineH = ImGui::GetTextLineHeight();
    const float itemSp = ImGui::GetStyle().ItemSpacing.y;
    const float tableH = std::max(80.0f, ImGui::GetContentRegionAvail().y - lineH - itemSp * 2.0f - 24.0f);

    ImGui::BeginChild("##pin_list", ImVec2(0, tableH), true, 0);
    int removeIndex = -1;
    int pokeIndex = -1;
    if (ImGui::BeginTable("##pin_table", 5,
                          ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Freeze", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < tr.pins.size(); ++i) {
            auto& p = tr.pins[i];
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(i));

            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##freeze", &p.freeze)) tr.freezeDirty = true;

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(hex8(p.address).c_str());

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kMemTypeNames[p.type]);

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##val", p.edit, sizeof(p.edit));
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                std::vector<uint8_t> bytes;
                double number = 0.0;
                if (parseScanValue(p.type, p.edit, bytes, number)) {
                    p.bytes = bytes;
                    p.bits = readBits(bytes.data(), bytes.size());
                    p.number = number;
                    p.valueText = p.edit;
                    tr.freezeDirty = true;
                }
            }

            ImGui::TableNextColumn();
            if (ImGui::Button("Poke")) pokeIndex = static_cast<int>(i);
            ImGui::SameLine();
            if (ImGui::Button("Remove")) removeIndex = static_cast<int>(i);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    if (pokeIndex >= 0) trainerPinPoke(s, ip, static_cast<size_t>(pokeIndex));
    if (removeIndex >= 0) trainerPinRemove(s, static_cast<size_t>(removeIndex));

    if (!tr.pinStatus.empty()) ImGui::TextDisabled("%s", tr.pinStatus.c_str());
    else drawTrainerFooter(s);
}

// ----- Memory viewer sub-tab -----
void drawTrainerViewer(UiState& s, const std::string& ip) {
    auto& tr = s.trainer;

    if (!tr.viewerLoaded && !tr.viewerReading && !tr.viewerTried) {
        tr.viewerTried = true;
        uint32_t base = tr.viewerBase;
        parseHexU32(tr.viewerAddr, base);
        trainerView(s, ip, base);
    }

    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputText("Address##view", tr.viewerAddr, sizeof(tr.viewerAddr));
    ImGui::SameLine();
    if (ImGui::Button("Go", ImVec2(50, 0))) {
        uint32_t base = 0;
        if (parseHexU32(tr.viewerAddr, base)) trainerView(s, ip, base);
        else tr.viewerStatus = "Enter a hex address.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Prev", ImVec2(60, 0))) trainerView(s, ip, tr.viewerBase - 0x100);
    ImGui::SameLine();
    if (ImGui::Button("Next", ImVec2(60, 0))) trainerView(s, ip, tr.viewerBase + 0x100);
    ImGui::SameLine();
    if (ImGui::Button("Refresh", ImVec2(80, 0))) trainerView(s, ip, tr.viewerBase);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", tr.viewerStatus.c_str());

    const float lineH = ImGui::GetTextLineHeight();
    const float itemSp = ImGui::GetStyle().ItemSpacing.y;
    const float hexH = std::max(120.0f, ImGui::GetContentRegionAvail().y - lineH * 3.0f - itemSp * 5.0f - 40.0f);

    ImGui::BeginChild("##hex", ImVec2(0, hexH), true, 0);
    if (tr.viewerBytes.empty()) {
        ImGui::TextDisabled("No data.");
    } else {
        const int cols = 16;
        const int rows = static_cast<int>((tr.viewerBytes.size() + cols - 1) / cols);
        for (int r = 0; r < rows; ++r) {
            const uint32_t rowAddr = tr.viewerBase + static_cast<uint32_t>(r * cols);
            ImGui::Text("%08X", rowAddr);
            for (int c = 0; c < cols; ++c) {
                const size_t idx = static_cast<size_t>(r * cols + c);
                ImGui::SameLine();
                ImGui::PushID(static_cast<int>(idx));
                if (idx < tr.viewerBytes.size()) {
                    char b[4]{};
                    std::snprintf(b, sizeof(b), "%02X", tr.viewerBytes[idx]);
                    if (ImGui::Selectable(b, tr.viewerSelected == static_cast<int>(idx), 0,
                                          ImVec2(24.0f, 0.0f))) {
                        tr.viewerSelected = static_cast<int>(idx);
                        std::snprintf(tr.viewerWriteAddr, sizeof(tr.viewerWriteAddr), "%08X",
                                      rowAddr + static_cast<uint32_t>(c));
                    }
                } else {
                    ImGui::TextDisabled("  ");
                }
                ImGui::PopID();
            }
            ImGui::SameLine();
            std::string ascii;
            for (int c = 0; c < cols; ++c) {
                const size_t idx = static_cast<size_t>(r * cols + c);
                if (idx >= tr.viewerBytes.size()) break;
                const uint8_t b = tr.viewerBytes[idx];
                ascii.push_back((b >= 32 && b < 127) ? static_cast<char>(b) : '.');
            }
            ImGui::TextDisabled("%s", ascii.c_str());
        }
    }
    ImGui::EndChild();

    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputTextWithHint("##waddr", "write address", tr.viewerWriteAddr, sizeof(tr.viewerWriteAddr));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##whex", "hex bytes (e.g. 63 00 00 00)", tr.viewerWriteHex,
                             sizeof(tr.viewerWriteHex));
    ImGui::SameLine();
    if (ImGui::Button("Write", ImVec2(70, 0))) {
        uint32_t addr = 0;
        std::vector<uint8_t> bytes;
        if (!parseHexU32(tr.viewerWriteAddr, addr)) {
            tr.viewerStatus = "Enter a hex write address.";
        } else if (!parseHexBytes(tr.viewerWriteHex, bytes)) {
            tr.viewerStatus = "Enter hex bytes, for example 63 00 00 00.";
        } else {
            tr.viewerStatus = "Writing...";
            trainerSubmitWrite(s, ip, "trainer_vwrite", {{addr, bytes}});
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(tr.viewerSelected < 0 ||
                         static_cast<size_t>(tr.viewerSelected) + 4 > tr.viewerBytes.size());
    if (ImGui::Button("Follow dword", ImVec2(110, 0))) {
        const size_t idx = static_cast<size_t>(tr.viewerSelected);
        const uint32_t target = static_cast<uint32_t>(tr.viewerBytes[idx]) |
                                (static_cast<uint32_t>(tr.viewerBytes[idx + 1]) << 8) |
                                (static_cast<uint32_t>(tr.viewerBytes[idx + 2]) << 16) |
                                (static_cast<uint32_t>(tr.viewerBytes[idx + 3]) << 24);
        trainerView(s, ip, target);
    }
    ImGui::EndDisabled();

    if (!tr.viewerStatus.empty()) ImGui::TextDisabled("%s", tr.viewerStatus.c_str());
}

void drawTrainerTab(UiState& s, const std::string& ip) {
    auto& tr = s.trainer;

    // On first open, read the target's module and memory map so scans know which
    // ranges are actually mapped.
    if (!tr.refreshed && tr.regions.empty() && !tr.scanning) {
        tr.refreshed = true;
        trainerRefreshTarget(s, ip);
    }
    if (tr.freezeDirty && !ip.empty()) trainerRebuildFreeze(s, ip);
    else if (!ip.empty() && tr.freezeIp != ip) trainerRebuildFreeze(s, ip);

    if (ImGui::BeginTabBar("##trainer_sub")) {
        if (ImGui::BeginTabItem("Scanner")) {
            tr.subTab = 0;
            drawTrainerScanner(s, ip);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Address list")) {
            tr.subTab = 1;
            drawTrainerPins(s, ip);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Memory viewer")) {
            tr.subTab = 2;
            drawTrainerViewer(s, ip);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void drawConsoleScreen(UiState& s, float height) {
    if (s.selected < 0 || static_cast<size_t>(s.selected) >= s.consoles.size()) {
        s.screen = Screen::Home;
        return;
    }
    const auto& console = s.consoles[static_cast<size_t>(s.selected)];

    if (ImGui::Button("< Back", ImVec2(90, 0))) s.screen = Screen::Home;
    ImGui::SameLine();
    if (g_titleFont) ImGui::PushFont(g_titleFont);
    ImGui::TextUnformatted(console.name.c_str());
    if (g_titleFont) ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", console.ip.c_str());
    ImGui::Separator();
    (void)height;

    if (ImGui::BeginTabBar("##console_tabs")) {
        if (ImGui::BeginTabItem("Console")) {
            drawConsoleTab(s, console.ip);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("File Explorer")) {
            drawExplorerTab(s, console.ip);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Trainer")) {
            drawTrainerTab(s, console.ip);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void drawTools(UiState& s, float height) {
    if (ImGui::Button("< Back", ImVec2(90, 0))) s.screen = Screen::Home;
    ImGui::SameLine();
    if (g_titleFont) ImGui::PushFont(g_titleFont);
    ImGui::TextUnformatted("Offline tools");
    if (g_titleFont) ImGui::PopFont();
    ImGui::Separator();

    const float bodyH = height - ImGui::GetCursorPosY();
    ImGui::BeginChild("##tools_nav", ImVec2(150, bodyH), true, ImGuiWindowFlags_NoScrollbar);
    if (ImGui::Selectable("XUIZ / XZP", s.tool == Tool::Xuiz)) s.tool = Tool::Xuiz;
    if (ImGui::Selectable("XEX", s.tool == Tool::Xex)) s.tool = Tool::Xex;
    if (ImGui::Selectable("NAND", s.tool == Tool::Nand)) s.tool = Tool::Nand;
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##tools_content", ImVec2(0, bodyH), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    switch (s.tool) {
    case Tool::Xuiz:
        field("##xuiz", "XZP file", s.xuizPath, sizeof(s.xuizPath));
        if (ImGui::Button("Info", ImVec2(90, 0))) runTextJob(s, "tool", "xuiz", {"info", s.xuizPath});
        ImGui::SameLine();
        if (ImGui::Button("List", ImVec2(90, 0))) runTextJob(s, "tool", "xuiz", {"list", s.xuizPath});
        ImGui::SameLine();
        if (ImGui::Button("Extract", ImVec2(90, 0))) runTextJob(s, "tool", "xuiz", {"extract", s.xuizPath, "xuiz_out"});
        ImGui::SameLine();
        if (ImGui::Button("Carve", ImVec2(90, 0))) runTextJob(s, "tool", "xuiz", {"carve", s.xuizPath, "xuiz_out"});
        break;
    case Tool::Xex:
        field("##xex", "XEX file", s.xexPath, sizeof(s.xexPath));
        if (ImGui::Button("Info", ImVec2(90, 0))) runTextJob(s, "tool", "xex", {"info", s.xexPath});
        ImGui::SameLine();
        if (ImGui::Button("Strings", ImVec2(90, 0))) runTextJob(s, "tool", "xex", {"strings", s.xexPath, "80"});
        break;
    case Tool::Nand:
        field("##nand_a", "Dump A", s.nandA, sizeof(s.nandA));
        field("##nand_b", "Dump B", s.nandB, sizeof(s.nandB));
        if (ImGui::Button("Info", ImVec2(90, 0))) runTextJob(s, "tool", "nand", {"info", s.nandA});
        ImGui::SameLine();
        if (ImGui::Button("Compare", ImVec2(90, 0))) runTextJob(s, "tool", "nand", {"compare", s.nandA, s.nandB});
        ImGui::SameLine();
        if (ImGui::Button("Strings", ImVec2(90, 0))) runTextJob(s, "tool", "nand", {"strings", s.nandA, "80"});
        break;
    }
    ImGui::Spacing();
    sectionTitle("OUTPUT");
    outputPane("##tools_log", s.toolLog, -1.0f);
    ImGui::EndChild();
}

void drawHelpWindow(UiState& s) {
    if (!s.showHelp) return;
    ImGui::Begin("Command Reference", &s.showHelp, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
    ImGui::TextUnformatted("X360 Toolkit - actions & XBDM commands");
    ImGui::Separator();

    sectionTitle("CONSOLE TAB");
    ImGui::BulletText("Refresh info - sends SYSTEMINFO and shows console details");
    ImGui::BulletText("Probe - connects on port 730 and reads the 201- connected banner");
    ImGui::BulletText("Drives - sends DRIVELIST");
    ImGui::BulletText("Dir - sends DIRLIST NAME=\"<remote path>\"");
    ImGui::BulletText("Download - sends GETFILE NAME=\"<remote path>\" and saves locally");
    ImGui::BulletText("Send command - sends any raw XBDM command below");

    sectionTitle("FILE EXPLORER TAB");
    ImGui::BulletText("Double-click a folder to open it; double-click a file to download it");
    ImGui::BulletText("Right-click an item: Open, Download, Rename, New folder, Delete, Copy path");
    ImGui::BulletText("Right-click empty space: New folder or Refresh");
    ImGui::BulletText("Drag files or folders from Windows into the Explorer to upload them");
    ImGui::BulletText("Screenshot - captures the console framebuffer to a PNG in Downloads");
    ImGui::BulletText("Back, Up, Refresh and breadcrumb buttons navigate instantly from the cache");
    ImGui::BulletText("Icons / Details switches between thumbnail grid and sortable table");

    sectionTitle("TRAINER TAB");
    ImGui::BulletText("Refresh target - reads XBEINFO RUNNING, MODULES and WALKMEM");
    ImGui::BulletText("Scanner: set a start address + length (hex) and a value type");
    ImGui::BulletText("First scan: Exact, Unknown, Bigger, Smaller or Between");
    ImGui::BulletText("Unknown snapshots everything (use it when a value is a bar, not a number)");
    ImGui::BulletText("Only mapped memory inside the window is read, in 256 KB chunks");
    ImGui::BulletText("Next scan: Exact, Bigger, Smaller, Between, Increased, Decreased,");
    ImGui::BulletText("           Increased by, Decreased by, Changed and Unchanged");
    ImGui::BulletText("For a health bar: Unknown, take damage, then Next scan -> Decreased");
    ImGui::BulletText("Tick results (or click one) and Poke to write the value once, never automatically");
    ImGui::BulletText("Add selected puts results in the Address list, which can save/load and freeze");
    ImGui::BulletText("Address list: tick Freeze to re-write that address every half second");
    ImGui::BulletText("Memory viewer: Go to an address, click a byte, write hex bytes or follow a dword");
    ImGui::BulletText("Writing to a running title can crash it - poke only addresses you understand");

    sectionTitle("RAW XBDM COMMANDS");
    ImGui::BulletText("SYSTEMINFO - console type, platform, kernel versions");
    ImGui::BulletText("DRIVELIST - list mounted drives");
    ImGui::BulletText("DIRLIST NAME=\"Hdd1:\\\" - list a directory");
    ImGui::BulletText("GETFILE NAME=\"Hdd1:\\file.bin\" - download a file (binary response)");
    ImGui::BulletText("SENDFILE NAME=\"Hdd1:\\file.bin\" LENGTH=0x<size> - upload raw bytes");
    ImGui::BulletText("MKDIR NAME=\"Hdd1:\\folder\" - create a folder");
    ImGui::BulletText("RENAME NAME=\"old\" NEWNAME=\"new\" - rename a file or folder");
    ImGui::BulletText("DELETE NAME=\"Hdd1:\\file.bin\" - delete a file");
    ImGui::BulletText("DELETE NAME=\"Hdd1:\\folder\" DIR - delete an empty folder");
    ImGui::BulletText("XBEINFO RUNNING - currently running title");
    ImGui::BulletText("MODULES - loaded modules with base address and size");
    ImGui::BulletText("WALKMEM - mapped virtual address ranges");
    ImGui::BulletText("GETMEMEX ADDR=0x<addr> LENGTH=0x<len> - read memory (binary)");
    ImGui::BulletText("SETMEM ADDR=0x<addr> DATA=<hex> - write up to 128 bytes");
    ImGui::BulletText("REBOOT - reboot the console");

    sectionTitle("OFFLINE TOOLS");
    ImGui::BulletText("XUIZ / XZP - info, list names, extract, carve images");
    ImGui::BulletText("XEX - header info and embedded strings");
    ImGui::BulletText("NAND - read-only dump info, compare and strings");

    ImGui::Spacing();
    if (ImGui::Button("Close", ImVec2(120, 0))) s.showHelp = false;
    ImGui::End();
}

void drawUi(UiState& s) {
    {
        std::vector<std::string> dropped;
        {
            std::lock_guard<std::mutex> lock(g_dropMutex);
            dropped.swap(g_droppedFiles);
        }
        if (!dropped.empty() && s.selected >= 0 &&
            static_cast<size_t>(s.selected) < s.consoles.size()) {
            const std::string ip = s.consoles[static_cast<size_t>(s.selected)].ip;
            const std::string root = s.explorer.path.empty() ? std::string("Hdd1:\\") : s.explorer.path;
            for (const auto& local : dropped) {
                explorerUpload(s, ip, local, root);
            }
        }
    }

    const std::string currentIp =
        (s.selected >= 0 && static_cast<size_t>(s.selected) < s.consoles.size())
            ? s.consoles[static_cast<size_t>(s.selected)].ip
            : std::string();

    for (auto& result : s.jobs.poll()) {
        if (result.id == "info") s.infoText = result.text;
        else if (result.id == "log") s.logText = result.text;
        else if (result.id == "tool") s.toolLog = result.text;
        else if (result.id == "explorer_list") {
            if (result.hasEntries) {
                auto& ex = s.explorer;
                ex.entries = std::move(result.entries);
                ex.cache[lower(result.path)] = ex.entries;
                ex.error.clear();
                ex.status = ex.pendingStatus;
                ex.pendingStatus.clear();
            } else {
                s.explorer.error = result.text;
                s.explorer.status.clear();
                s.explorer.pendingStatus.clear();
            }
        } else if (result.id == "explorer_dl" || result.id == "explorer_upload") {
            s.explorer.status = result.text;
        } else if (result.id == "explorer_mkdir" || result.id == "explorer_rename" ||
                   result.id == "explorer_delete") {
            s.explorer.status = result.text;
            if (result.refresh) {
                // Re-list the current folder so the new/renamed item shows up,
                // keeping the confirmation text visible after the re-list.
                s.explorer.pendingStatus = result.text;
                const std::string ip =
                    (s.selected >= 0 && static_cast<size_t>(s.selected) < s.consoles.size())
                        ? s.consoles[static_cast<size_t>(s.selected)].ip
                        : std::string();
                if (!ip.empty()) {
                    s.explorer.cache.erase(lower(s.explorer.path));
                    explorerNavigate(s, ip, s.explorer.path, false);
                }
            }
        } else if (result.id == "screenshot") {
            s.logText = result.text;
        } else if (result.id == "trainer_refresh") {
            if (!result.text.empty()) {
                s.trainer.error = result.text;
                s.trainer.status.clear();
            } else {
                s.trainer.target = result.path;
                s.trainer.modules = std::move(result.modules);
                s.trainer.regions = std::move(result.regions);
                s.trainer.error.clear();
                s.trainer.status = "Read " + std::to_string(s.trainer.modules.size()) + " module(s) and " +
                                   std::to_string(s.trainer.regions.size()) + " memory region(s).";
            }
        } else if (result.id == "trainer_scan" || result.id == "trainer_narrow") {
            s.trainer.scanning = false;
            if (result.hasCandidates) {
                s.trainer.candidates = std::move(result.candidates);
                s.trainer.hasScan = true;
                s.trainer.selected = -1;
                s.trainer.error.clear();
                s.trainer.status = result.text;
            } else {
                s.trainer.status.clear();
                s.trainer.error = result.text;
            }
        } else if (result.id == "trainer_write") {
            s.trainer.error.clear();
            s.trainer.status = result.text;
            if (result.text.rfind("Write failed", 0) == 0) s.trainer.error = result.text;
        } else if (result.id == "trainer_vwrite") {
            if (result.text.rfind("Write failed", 0) == 0) {
                s.trainer.viewerStatus = result.text;
            } else {
                s.trainer.viewerStatus = result.text;
                const std::string ip =
                    (s.selected >= 0 && static_cast<size_t>(s.selected) < s.consoles.size())
                        ? s.consoles[static_cast<size_t>(s.selected)].ip
                        : std::string();
                if (!ip.empty()) trainerView(s, ip, s.trainer.viewerBase);
            }
        } else if (result.id == "trainer_mem") {
            s.trainer.viewerReading = false;
            if (result.hasMem) {
                s.trainer.viewerBytes = std::move(result.memBytes);
                s.trainer.viewerBase = result.memBase;
                s.trainer.viewerLoaded = true;
                s.trainer.viewerSelected = -1;
                char msg[64]{};
                std::snprintf(msg, sizeof(msg), "0x%08X, %zu bytes", result.memBase,
                              s.trainer.viewerBytes.size());
                s.trainer.viewerStatus = msg;
            } else {
                s.trainer.viewerStatus = result.text;
            }
        }
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(display);
    ImGui::Begin("Main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoScrollWithMouse);

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Home")) s.screen = Screen::Home;
            if (ImGui::MenuItem("Exit")) s.done = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Command reference")) s.showHelp = true;
            if (ImGui::MenuItem("About")) s.showAbout = true;
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    const float contentH = ImGui::GetContentRegionAvail().y - kStatusH - spacing;

    ImGui::BeginChild("##screen", ImVec2(0, contentH), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                      ImGuiWindowFlags_NoBackground);
    switch (s.screen) {
    case Screen::Home: drawHome(s, contentH - 4.0f); break;
    case Screen::Console: drawConsoleScreen(s, contentH - 4.0f); break;
    case Screen::Tools: drawTools(s, contentH - 4.0f); break;
    }
    ImGui::EndChild();

    // Dedicated bottom status bar so error/status text is never clipped.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 5.0f));
    ImGui::BeginChild("##statusbar", ImVec2(0, kStatusH), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (s.jobs.busy()) ImGui::TextDisabled("Working...");
    else ImGui::TextDisabled("X360 Toolkit 0.1     XBDM :730");
    ImGui::EndChild();
    ImGui::PopStyleVar();

    drawHelpWindow(s);

    if (s.showAbout) {
        ImGui::Begin("About", &s.showAbout, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
        ImGui::TextUnformatted("X360 Toolkit 0.1");
        ImGui::Separator();
        ImGui::TextUnformatted("Console control over XBDM, plus XZP, XEX and NAND tools.");
        ImGui::TextUnformatted("Trainer: scans and edits a running title's memory over XBDM.");
        if (ImGui::Button("Close", ImVec2(100, 0))) s.showAbout = false;
        ImGui::End();
    }

    ImGui::End();
}

} // namespace

int runGui() {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT oleResult = ::OleInitialize(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = ::GetModuleHandle(nullptr);
    wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"X360ToolkitClass";
    ::RegisterClassExW(&wc);

    RECT work{};
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int workW = work.right - work.left;
    const int workH = work.bottom - work.top;
    const int width = std::min(1200, workW - 40);
    const int height = std::min(760, workH - 40);
    const int x = work.left + (workW - width) / 2;
    const int y = work.top + (workH - height) / 2;

    HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"X360 Toolkit",
                                  WS_OVERLAPPEDWINDOW | WS_VISIBLE, x, y, width, height,
                                  nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 1;
    if (!createDeviceD3D(hwnd)) {
        cleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    ::DragAcceptFiles(hwnd, TRUE);
    ::ShowWindow(hwnd, SW_SHOWNORMAL);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigDebugHighlightIdConflicts = false;
    loadFonts();
    applyStyle();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    UiState state;
    loadConsoles(state);
    loadTrainerPins(state);

    while (!state.done) {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) state.done = true;
        }
        if (state.done) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        drawUi(state);
        ImGui::Render();

        const float clear[4] = {0.055f, 0.065f, 0.085f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_renderTarget, nullptr);
        g_context->ClearRenderTargetView(g_renderTarget, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (SUCCEEDED(oleResult)) ::OleUninitialize();
    cleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

} // namespace x360::ui
