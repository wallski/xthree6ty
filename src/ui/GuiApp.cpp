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
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
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
enum class ConsoleTab { Console, Explorer };
enum class ViewMode { Icons, Details };

struct SavedConsole {
    std::string name;
    std::string ip;
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

    JobRunner jobs;
    TransferState transfer;
    ExplorerState explorer;

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
