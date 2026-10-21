// 对话框预览/截图工具：加载插件 DLL、显示选项对话框、抓图保存为 PNG。
//
// 用途：改界面时快速看效果，不必每次重启 TrafficMonitor 宿主。
//
// 构建（在 tools 目录）：
//   cl /nologo /std:c++17 /EHsc /utf-8 /DUNICODE /D_UNICODE dialog_preview.cpp ^
//      /I..\include /link user32.lib gdi32.lib ole32.lib windowscodecs.lib crypt32.lib ^
//      /OUT:dialog_preview.exe
//
// 用法：
//   dialog_preview.exe <插件 DLL> <输出 PNG> [--samples]
//
// --samples：先在临时目录写一份带 3 个站点的加密配置（全部指向 127.0.0.1，
//            不会访问真实服务），再用它初始化插件，这样列表/状态区有内容可看。
//
// 退出码：0 成功；2 加载或导出失败；3 窗口未出现；4 截图失败。

#include "PluginInterface.h"

#include <windows.h>
#include <commctrl.h>
#include <wincodec.h>
#include <objbase.h>

// 宿主 TrafficMonitor 的清单依赖 Common-Controls 6.0，对话框控件在宿主里是
// 主题化外观。预览工具也声明同样的依赖，截图才与真实宿主一致。
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
    "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include <cstdio>
#include <string>
#include <vector>

namespace {

const wchar_t* kDialogTitle = L"多站点 AI 余额设置";

// 诊断日志：控制台输出会和对话框线程交错，这里额外落一份文件。
void Log(const wchar_t* format, ...) {
    wchar_t buffer[1024]{};
    va_list args;
    va_start(args, format);
    vswprintf_s(buffer, format, args);
    va_end(args);
    fwprintf(stdout, L"%ls\n", buffer);
    fflush(stdout);

    wchar_t temp[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temp) == 0) return;
    const std::wstring path = std::wstring(temp) + L"aiapi-preview.log";
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"a, ccs=UTF-8") == 0 && file) {
        fwprintf(file, L"%ls\n", buffer);
        fclose(file);
    }
}

// ── 预览用的宿主桩 ───────────────────────────────────────────────────
// 只实现插件真正会调的两个方法：GetAPIVersion 与 GetPluginConfigDir。
class MockHost final : public ITrafficMonitor {
public:
    explicit MockHost(std::wstring configDir) : configDir_(std::move(configDir)) {}

    int GetAPIVersion() override { return 1; }
    const wchar_t* GetVersion() override { return L"1.86"; }
    double GetMonitorValue(MonitorItem) override { return 0.0; }
    const wchar_t* GetMonitorValueString(MonitorItem, int = false) override { return L"0"; }
    void ShowNotifyMessage(const wchar_t*) override {}
    unsigned short GetLanguageId() const override { return 0; }
    const wchar_t* GetPluginConfigDir() const override { return configDir_.c_str(); }
    int GetDPI(DPIType) const override { return 96; }
    unsigned int GetThemeColor() const override { return RGB(32, 32, 32); }
    const wchar_t* GetStringRes(const wchar_t*, const wchar_t*) override { return L""; }

private:
    std::wstring configDir_;
};

// ── 构造样例配置 ─────────────────────────────────────────────────────
// 格式与 Plugin::SaveProfiles 一致（小端 u32 + 长度前缀 UTF-8 字符串），
// 整体用 DPAPI 加密，只有当前用户能解开。

void AppendU32(std::vector<BYTE>& out, unsigned int value) {
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<BYTE>((value >> shift) & 0xff));
    }
}

void AppendString(std::vector<BYTE>& out, const std::wstring& value) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        utf8.data(), bytes, nullptr, nullptr);
    AppendU32(out, static_cast<unsigned int>(utf8.size()));
    out.insert(out.end(), utf8.begin(), utf8.end());
}

struct SampleSite {
    unsigned int id;
    unsigned int adapter;  // 0=AIAPI 1=NewAPI 2=Sub2API 3=Custom
    bool enabled;
    const wchar_t* name;
    const wchar_t* baseUrl;
    const wchar_t* credential;
    const wchar_t* userId;
};

// 把宿主真实配置复制到临时目录：同一用户 DPAPI 加密，插件能解开，
// 列表与状态区就有真实内容。预览只读，对话框不点“保存”不会写回。
bool CopyRealConfig(const std::wstring& targetDir) {
    wchar_t appData[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH) == 0) return false;

    const std::wstring source = std::wstring(appData) +
        L"\\TrafficMonitor\\Plugins\\AIAPIQuota\\sites.bin";
    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    CreateDirectoryW(targetDir.c_str(), nullptr);
    const std::wstring quotaDir = targetDir + L"\\AIAPIQuota";
    CreateDirectoryW(quotaDir.c_str(), nullptr);

    const std::wstring dest = quotaDir + L"\\sites.bin";
    if (!CopyFileW(source.c_str(), dest.c_str(), FALSE)) {
        Log(L"[preview] 复制真实配置失败（错误 %lu），改用样例。", GetLastError());
        return false;
    }
    Log(L"[preview] 已复制真实配置：%ls -> %ls", source.c_str(), dest.c_str());
    return true;
}

bool WriteSampleConfig(const std::wstring& directory) {
    // 全部指向本机回环地址：插件后台线程即使发起查询也会立即失败，不会外联。
    const SampleSite sites[] = {
        { 1001, 2, true,  L"AIAPI 主账号",  L"https://127.0.0.1:9", L"sk-preview-key-1", L"" },
        { 1002, 2, true,  L"New API 备用",  L"https://127.0.0.1:9", L"sk-preview-key-2", L"10001" },
        { 1003, 2, false, L"Sub2API 测试",  L"https://127.0.0.1:9", L"sk-preview-key-3", L"" },
    };

    std::vector<BYTE> plain;
    AppendU32(plain, 0x51414941);  // "AIQA"
    AppendU32(plain, 2);           // 版本
    AppendU32(plain, static_cast<unsigned int>(std::size(sites)));
    for (const auto& site : sites) {
        AppendU32(plain, site.id);
        AppendU32(plain, site.adapter);
        AppendU32(plain, site.enabled ? 1u : 0u);
        AppendString(plain, site.name);
        AppendString(plain, site.baseUrl);
        AppendString(plain, site.credential);
        AppendString(plain, site.userId);
        AppendString(plain, L"");  // customScript
    }

    DATA_BLOB input{ static_cast<DWORD>(plain.size()), plain.data() };
    DATA_BLOB encrypted{};
    if (!CryptProtectData(&input, L"preview", nullptr, nullptr, nullptr,
        CRYPTPROTECT_UI_FORBIDDEN, &encrypted)) {
        return false;
    }

    const std::wstring path = directory + L"\\sites.bin";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = false;
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ok = WriteFile(file, encrypted.pbData, encrypted.cbData, &written, nullptr) != FALSE &&
            written == encrypted.cbData;
        CloseHandle(file);
    }
    LocalFree(encrypted.pbData);
    SecureZeroMemory(plain.data(), plain.size());
    if (!ok) return false;

    // 回读自检：DPAPI 往返 + 头部魔数，确认插件侧能解开这份样例。
    HANDLE check = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (check == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(check, &size);
    std::vector<BYTE> blob(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    ReadFile(check, blob.data(), static_cast<DWORD>(blob.size()), &read, nullptr);
    CloseHandle(check);

    DATA_BLOB in{ static_cast<DWORD>(blob.size()), blob.data() };
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
        CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        wprintf(L"[preview] 自检失败：DPAPI 解不开刚写入的文件\n");
        return false;
    }
    unsigned int magic = static_cast<unsigned int>(out.pbData[0]) |
        (static_cast<unsigned int>(out.pbData[1]) << 8) |
        (static_cast<unsigned int>(out.pbData[2]) << 16) |
        (static_cast<unsigned int>(out.pbData[3]) << 24);
    wprintf(L"[preview] 自检：字节=%lu 魔数=0x%08X（期望 0x51414941）\n", out.cbData, magic);
    LocalFree(out.pbData);
    return magic == 0x51414941;
}

// ── 插件调用 ─────────────────────────────────────────────────────────

// ITMPlugin 虚表槽位（API 7 头文件顺序）。
constexpr int kVtShowOptions = 3;
constexpr int kVtOnInitialize = 14;  // GetAPIVersion..OnInitialize 之间的顺序见头文件

using ShowOptionsFn = int (*)(void*, void*);
using OnInitializeFn = void (*)(void*, ITrafficMonitor*);

struct ShowContext {
    HMODULE module = nullptr;
    std::wstring configDir;
    bool useSamples = false;
    bool finished = false;
};

void ShowDialogOnThread(ShowContext* context) {
    auto getInstance = reinterpret_cast<void* (*)()>(
        GetProcAddress(context->module, "TMPluginGetInstance"));
    if (!getInstance) { context->finished = true; return; }
    void* plugin = getInstance();
    if (!plugin) { context->finished = true; return; }

    MockHost host(context->configDir);
    if (context->useSamples && !context->configDir.empty()) {
        // 走官方加载路径：OnInitialize 会读配置目录并填充站点列表。
        // 优先复制宿主真实配置；没有时退回内置样例。
        CreateDirectoryW(context->configDir.c_str(), nullptr);
        if (!CopyRealConfig(context->configDir)) {
            const std::wstring quotaDir = context->configDir + L"\\AIAPIQuota";
            CreateDirectoryW(quotaDir.c_str(), nullptr);
            WriteSampleConfig(quotaDir);
        }
        auto initVtable = *reinterpret_cast<void***>(plugin);
        auto onInitialize = reinterpret_cast<OnInitializeFn>(initVtable[kVtOnInitialize]);
        onInitialize(plugin, &host);
    }

    auto vtable = *reinterpret_cast<void***>(plugin);
    auto showOptions = reinterpret_cast<ShowOptionsFn>(vtable[kVtShowOptions]);

    // 诊断：据 GetItem(0) 是否为空判断 OnInitialize 是否读到了配置。
    if (context->useSamples) {
        using GetItemFn = IPluginItem* (*)(void*, int);
        using TooltipFn = const wchar_t* (*)(void*);
        using ApiVersionFn = int (*)(void*);
        auto getItem = reinterpret_cast<GetItemFn>(vtable[1]);
        auto tooltip = reinterpret_cast<TooltipFn>(vtable[6]);
        auto apiVersion = reinterpret_cast<ApiVersionFn>(vtable[0]);
        wprintf(L"[preview] api=%d configDir=%ls\n", apiVersion(plugin), context->configDir.c_str());
        const wchar_t* tip = tooltip(plugin);
        wprintf(L"[preview] tooltip(len=%zu)=[%ls]\n", tip ? wcslen(tip) : 0u, tip ? tip : L"<null>");
        wprintf(L"[preview] GetItem(0)=%p GetItem(1)=%p GetItem(2)=%p\n",
            static_cast<void*>(getItem(plugin, 0)), static_cast<void*>(getItem(plugin, 1)),
            static_cast<void*>(getItem(plugin, 2)));
    }

    showOptions(plugin, nullptr);
    context->finished = true;
}

bool SaveHBitmapAsPng(HBITMAP bitmap, const std::wstring& path) {
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory)))) return false;

    bool ok = false;
    IWICBitmapEncoder* encoder = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    do {
        if (FAILED(factory->CreateStream(&stream))) break;
        if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) break;
        if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) break;
        if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) break;
        if (FAILED(encoder->CreateNewFrame(&frame, nullptr))) break;
        if (FAILED(frame->Initialize(nullptr))) break;

        BITMAP bm{};
        if (!GetObjectW(bitmap, sizeof(bm), &bm)) break;
        const UINT width = static_cast<UINT>(bm.bmWidth);
        const UINT height = static_cast<UINT>(bm.bmHeight);
        if (FAILED(frame->SetSize(width, height))) break;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(frame->SetPixelFormat(&format))) break;
        if (format != GUID_WICPixelFormat32bppBGRA) frame->SetPixelFormat(&format);

        const UINT stride = width * 4;
        std::vector<BYTE> pixels(static_cast<size_t>(stride) * height);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = static_cast<LONG>(width);
        info.bmiHeader.biHeight = -static_cast<LONG>(height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        HDC screen = GetDC(nullptr);
        HDC memory = CreateCompatibleDC(screen);
        HGDIOBJ old = SelectObject(memory, bitmap);
        const int lines = GetDIBits(memory, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS);
        SelectObject(memory, old);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        if (lines == 0) break;

        if (FAILED(frame->WritePixels(height, stride, static_cast<UINT>(pixels.size()), pixels.data()))) break;
        if (FAILED(frame->Commit())) break;
        if (FAILED(encoder->Commit())) break;
        ok = true;
    } while (false);

    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    factory->Release();
    return ok;
}

bool CaptureWindow(HWND window, const std::wstring& path) {
    RECT rect{};
    if (!GetWindowRect(window, &rect)) return false;
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return false;

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ old = SelectObject(memory, bitmap);

    RECT fill{ 0, 0, width, height };
    HBRUSH brush = CreateSolidBrush(RGB(32, 32, 32));
    FillRect(memory, &fill, brush);
    DeleteObject(brush);

    BOOL printed = PrintWindow(window, memory, 2);
    if (!printed) printed = PrintWindow(window, memory, 0);

    SelectObject(memory, old);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);

    if (!printed) { DeleteObject(bitmap); return false; }
    const bool saved = SaveHBitmapAsPng(bitmap, path);
    DeleteObject(bitmap);
    return saved;
}

std::wstring TempDirectory() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, buffer);
    if (length == 0) return {};
    std::wstring dir(buffer, length);
    if (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    dir += L"\\aiapi-dialog-preview";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 3) {
        wprintf(L"用法: dialog_preview.exe <插件 DLL> <输出 PNG> [--samples]\n");
        return 1;
    }

    const std::wstring dllPath = argv[1];
    const std::wstring output = argv[2];
    bool samples = false;
    bool customDialog = false;
    int saveRefreshChoice = -1;
    for (int i = 3; i < argc; ++i) {
        if (wcscmp(argv[i], L"--samples") == 0) samples = true;
        if (wcscmp(argv[i], L"--custom") == 0) { samples = true; customDialog = true; }
        if (wcscmp(argv[i], L"--save-refresh") == 0 && i + 1 < argc) {
            // 选中指定的刷新间隔并点“保存并关闭”，用来验证落盘。
            samples = true;
            saveRefreshChoice = _wtoi(argv[++i]);
        }
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        wprintf(L"COM 初始化失败\n");
        return 2;
    }

    const HMODULE module = LoadLibraryW(dllPath.c_str());
    if (!module) {
        wprintf(L"LoadLibrary 失败（错误 %lu）：%ls\n", GetLastError(), dllPath.c_str());
        wprintf(L"位数必须与本工具一致（x64 工具加载 x64 DLL）。\n");
        CoUninitialize();
        return 2;
    }
    if (!GetProcAddress(module, "TMPluginGetInstance")) {
        wprintf(L"未找到导出符号 TMPluginGetInstance\n");
        CoUninitialize();
        return 2;
    }

    ShowContext context{ module, samples ? TempDirectory() : std::wstring(), samples };
    HANDLE thread = CreateThread(nullptr, 0,
        [](LPVOID param) -> DWORD {
            ShowDialogOnThread(static_cast<ShowContext*>(param));
            return 0;
        }, &context, 0, nullptr);
    if (!thread) {
        wprintf(L"创建对话框线程失败\n");
        CoUninitialize();
        return 2;
    }

    HWND dialog = nullptr;
    for (int attempt = 0; attempt < 100; ++attempt) {
        Sleep(100);
        dialog = FindWindowW(L"#32770", kDialogTitle);
        if (dialog && IsWindowVisible(dialog)) break;
        if (context.finished) break;
    }

    if (!dialog || !IsWindowVisible(dialog)) {
        wprintf(L"没等到对话框窗口（标题：%ls）\n", kDialogTitle);
        CoUninitialize();
        return 3;
    }

    SetForegroundWindow(dialog);
    Sleep(700);

    // 选中第一个站点，让右侧字段与状态区有内容。
    // 列表控件是 ListView，设置选中状态时也会同步触发 LVN_ITEMCHANGED。
    HWND list = GetDlgItem(dialog, 1001);
    if (list) {
        if (SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) > 0) {
            LVITEMW item{};
            item.mask = LVIF_STATE;
            item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
            item.state = LVIS_SELECTED | LVIS_FOCUSED;
            item.iItem = 0;
            SendMessageW(list, LVM_SETITEMSTATE, 0, reinterpret_cast<LPARAM>(&item));
            Sleep(400);
        }
    }

    HWND target = dialog;
    if (customDialog) {
        // 新增站点 → 接口类型切到“自定义接口” → 点“自定义配置…”，
        // 这样第二层对话框才会出现（按钮只在自定义适配器下启用）。
        //
        // 注意：最后一步必须用 PostMessage。子对话框是模态的，SendMessage
        // 会一直等它关闭，主线程就再也走不到截图那一步。
        const HWND addButton = GetDlgItem(dialog, 1002);
        const HWND combo = GetDlgItem(dialog, 1004);
        if (addButton && combo) {
            SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(1002, BN_CLICKED),
                reinterpret_cast<LPARAM>(addButton));
            Sleep(250);
            // 接口类型下拉按服务商预设排列，“自定义接口”是最后一项；
            // 用文本查找，避免以后新增预设后写死索引失效。
            int customIndex = static_cast<int>(
                SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                    reinterpret_cast<LPARAM>(L"自定义接口")));
            if (customIndex < 0) {
                customIndex = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0)) - 1;
            }
            SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(customIndex), 0);
            SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(1004, CBN_SELCHANGE),
                reinterpret_cast<LPARAM>(combo));
            Sleep(250);
            PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(1013, BN_CLICKED),
                reinterpret_cast<LPARAM>(GetDlgItem(dialog, 1013)));

            for (int attempt = 0; attempt < 40; ++attempt) {
                Sleep(100);
                target = FindWindowW(L"#32770", L"自定义接口配置");
                if (target) break;
            }
            if (target) {
                SetForegroundWindow(target);
                Sleep(600);
            } else {
                Log(L"[preview] 没找到“自定义接口配置”对话框，退回主对话框");
                target = dialog;
            }
        }
    }

    const bool saved = CaptureWindow(target, output);

    // --save-refresh N：选中第 N 个刷新间隔并点“保存并关闭”，验证落盘。
    if (saveRefreshChoice >= 0 && target == dialog) {
        const HWND refreshCombo = GetDlgItem(dialog, 1024);
        if (refreshCombo) {
            SendMessageW(refreshCombo, CB_SETCURSEL,
                static_cast<WPARAM>(saveRefreshChoice), 0);
            const HWND okButton = GetDlgItem(dialog, IDOK);
            PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED),
                reinterpret_cast<LPARAM>(okButton));
            Sleep(800);
            wprintf(L"[preview] 已保存刷新间隔索引 %d\n", saveRefreshChoice);
        }
    } else {
        if (target != dialog) PostMessageW(target, WM_CLOSE, 0, 0);
        Sleep(150);
        PostMessageW(dialog, WM_CLOSE, 0, 0);
    }
    Sleep(300);

    wprintf(L"%ls：%ls\n", saved ? L"已保存" : L"截图失败", output.c_str());

    CoUninitialize();
    return saved ? 0 : 4;
}
