// 查询性能与退避行为基准工具。
//
// 构造 N 个指向本地 HTTP 服务的 Sub2API 站点，走真实插件加载路径
// （OnInitialize → 后台工作线程），测量从首次调用 DataRequired 到
// 所有站点拿到数值的耗时，用来验证并行查询是否真的生效。
//
// 构建（在 tools 目录）：
//   cl /nologo /std:c++17 /EHsc /utf-8 /DUNICODE /D_UNICODE /I..\include ^
//      query_bench.cpp /link user32.lib crypt32.lib /OUT:query_bench.exe
//
// 用法：
//   query_bench.exe <插件 DLL> <本地服务端口> [站点数] [单请求延迟毫秒]

#include "PluginInterface.h"

#include <windows.h>
#include <wincrypt.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

// 预览用宿主桩：只实现插件真正会调的 GetAPIVersion / GetPluginConfigDir。
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

void AppendU32(std::vector<BYTE>& out, unsigned int value) {
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<BYTE>((value >> shift) & 0xff));
    }
}

void AppendString(std::vector<BYTE>& out, const std::wstring& value) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(bytes), L'\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        utf8.data(), bytes, nullptr, nullptr);
    AppendU32(out, static_cast<unsigned int>(utf8.size()));
    out.insert(out.end(), utf8.begin(), utf8.end());
}

// 写一份 v3 配置：N 个 Sub2API 站点；前 deadSites 个指向死端口，用来观察失败退避。
bool WriteBenchConfig(const std::wstring& configDir, int port, int siteCount, int deadSites,
    int refreshSeconds) {
    CreateDirectoryW(configDir.c_str(), nullptr);
    const std::wstring quotaDir = configDir + L"\\AIAPIQuota";
    CreateDirectoryW(quotaDir.c_str(), nullptr);

    const std::wstring baseUrl = L"http://127.0.0.1:" + std::to_wstring(port);
    const std::wstring deadUrl = L"http://127.0.0.1:18099";  // 没有监听者，连接立即失败

    std::vector<BYTE> plain;
    AppendU32(plain, 0x51414941);
    AppendU32(plain, 3);            // v3：带刷新间隔
    AppendU32(plain, static_cast<unsigned int>(refreshSeconds));
    AppendU32(plain, static_cast<unsigned int>(siteCount));
    for (int i = 0; i < siteCount; ++i) {
        AppendU32(plain, static_cast<unsigned int>(1000 + i));
        AppendU32(plain, 3);        // Sub2API
        AppendU32(plain, 1);        // 启用
        AppendString(plain, L"bench-site-" + std::to_wstring(i));
        AppendString(plain, i < deadSites ? deadUrl : baseUrl);
        AppendString(plain, L"sk-bench-key");
        AppendString(plain, L"");
        AppendString(plain, L"");
    }

    DATA_BLOB input{ static_cast<DWORD>(plain.size()), plain.data() };
    DATA_BLOB encrypted{};
    if (!CryptProtectData(&input, L"bench", nullptr, nullptr, nullptr,
        CRYPTPROTECT_UI_FORBIDDEN, &encrypted)) {
        return false;
    }
    const std::wstring path = quotaDir + L"\\sites.bin";
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
    return ok;
}

std::wstring TempDirectory() {
    wchar_t buffer[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, buffer) == 0) return {};
    std::wstring dir(buffer);
    if (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    dir += L"\\aiapi-query-bench";
    return dir;
}

unsigned int ReadU32(const std::vector<BYTE>& data, size_t& offset) {
    if (offset + 4 > data.size()) return 0;
    const unsigned int value = static_cast<unsigned int>(data[offset]) |
        (static_cast<unsigned int>(data[offset + 1]) << 8) |
        (static_cast<unsigned int>(data[offset + 2]) << 16) |
        (static_cast<unsigned int>(data[offset + 3]) << 24);
    offset += 4;
    return value;
}

// 解密并打印配置头：用来确认界面保存的刷新间隔真的落盘了。
bool DumpConfig(const std::wstring& configDir) {
    const std::wstring path = configDir + L"\\AIAPIQuota\\sites.bin";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        wprintf(L"config not found: %ls\n", path.c_str());
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    std::vector<BYTE> blob(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    ReadFile(file, blob.data(), static_cast<DWORD>(blob.size()), &read, nullptr);
    CloseHandle(file);

    DATA_BLOB in{ static_cast<DWORD>(blob.size()), blob.data() };
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
        CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        wprintf(L"decrypt failed\n");
        return false;
    }
    std::vector<BYTE> plain(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);

    size_t offset = 0;
    const unsigned int magic = ReadU32(plain, offset);
    const unsigned int version = ReadU32(plain, offset);
    unsigned int refresh = 60;
    if (version >= 3) refresh = ReadU32(plain, offset);
    const unsigned int count = ReadU32(plain, offset);
    wprintf(L"magic=0x%08X version=%u refreshSeconds=%u sites=%u\n", magic, version, refresh, count);
    return magic == 0x51414941 && version == 3;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 3) {
        wprintf(L"usage: query_bench.exe <dll> <port> [sites=4] [deadSites=0] [refreshSeconds=60] [observeSeconds=0]\n");
        wprintf(L"       query_bench.exe --dump <configDir>\n");
        return 1;
    }
    if (wcscmp(argv[1], L"--dump") == 0) {
        return DumpConfig(argv[2]) ? 0 : 2;
    }
    const std::wstring dllPath = argv[1];
    const int port = _wtoi(argv[2]);
    const int siteCount = argc >= 4 ? _wtoi(argv[3]) : 4;
    const int deadSites = argc >= 5 ? _wtoi(argv[4]) : 0;
    const int refreshSeconds = argc >= 6 ? _wtoi(argv[5]) : 60;
    const int observeSeconds = argc >= 7 ? _wtoi(argv[6]) : 0;

    const std::wstring configDir = TempDirectory();
    DeleteFileW((configDir + L"\\AIAPIQuota\\sites.bin").c_str());
    if (!WriteBenchConfig(configDir, port, siteCount, deadSites, refreshSeconds)) {
        wprintf(L"write config failed\n");
        return 2;
    }

    const HMODULE module = LoadLibraryW(dllPath.c_str());
    if (!module) {
        wprintf(L"LoadLibrary 失败（错误 %lu）\n", GetLastError());
        return 2;
    }
    auto getInstance = reinterpret_cast<void* (*)()>(
        GetProcAddress(module, "TMPluginGetInstance"));
    if (!getInstance) {
        wprintf(L"缺少导出符号 TMPluginGetInstance\n");
        return 2;
    }
    void* plugin = getInstance();
    auto vtable = *reinterpret_cast<void***>(plugin);

    MockHost host(configDir);
    reinterpret_cast<void (*)(void*, ITrafficMonitor*)>(vtable[14])(plugin, &host);
    reinterpret_cast<void (*)(void*)>(vtable[2])(plugin);  // DataRequired

    using GetItemFn = IPluginItem* (*)(void*, int);
    auto getItem = reinterpret_cast<GetItemFn>(vtable[1]);
    using TooltipFn = const wchar_t* (*)(void*);
    auto tooltip = reinterpret_cast<TooltipFn>(vtable[6]);

    const auto started = std::chrono::steady_clock::now();

    if (observeSeconds > 0) {
        // 观察模式：按秒打印 tooltip，用来确认失败站点被退避而不是每轮都打。
        const auto stop = started + std::chrono::seconds(observeSeconds);
        std::wstring last;
        while (std::chrono::steady_clock::now() < stop) {
            const wchar_t* tip = tooltip(plugin);
            const std::wstring current = tip ? tip : L"";
            if (current != last) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started).count();
                wprintf(L"--- t=%lld ms ---\n%ls\n", static_cast<long long>(ms), current.c_str());
                last = current;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return 0;
    }

    std::vector<bool> ready(static_cast<size_t>(siteCount), false);
    const auto deadline = started + std::chrono::seconds(60);

    for (;;) {
        if (std::chrono::steady_clock::now() > deadline) {
            wprintf(L"TIMEOUT: %d/%d sites without value\n",
                static_cast<int>(std::count(ready.begin(), ready.end(), false)), siteCount);
            return 3;
        }
        int done = 0;
        for (int i = 0; i < siteCount; ++i) {
            IPluginItem* item = getItem(plugin, i);
            if (!item) continue;
            const wchar_t* value = item->GetItemValueText();
            if (value && wcscmp(value, L"--") != 0) {
                if (!ready[static_cast<size_t>(i)]) {
                    ready[static_cast<size_t>(i)] = true;
                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started).count();
                    wprintf(L"site %d ready at %lld ms, value=[%ls]\n", i,
                        static_cast<long long>(ms), value);
                }
            }
            if (ready[static_cast<size_t>(i)]) ++done;
        }
        if (done == siteCount) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    wprintf(L"ALL %d sites ready, total %lld ms\n", siteCount, static_cast<long long>(totalMs));

    // 顺带确认 tooltip 里没有把并行结果标成失败。
    wprintf(L"tooltip:\n%ls\n", tooltip(plugin));
    fflush(stdout);
    return 0;
}
