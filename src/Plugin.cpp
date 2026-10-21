#include "PluginInterface.h"
#include "UsageApi.h"
#include "ProviderRegistry.h"
#include "resource.h"

#include <windows.h>
#include <commctrl.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "comctl32.lib")
#if defined(_MSC_VER) && defined(_M_IX86)
#pragma comment(linker, "/EXPORT:TMPluginGetInstance=_TMPluginGetInstance")
#endif

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance();

namespace {

constexpr auto kMaxBackoff = std::chrono::minutes(15);
constexpr int kDefaultRefreshSeconds = 60;
constexpr int kMinRefreshSeconds = 15;
constexpr int kMaxRefreshSeconds = 900;
// 同时进行的站点查询数上限。站点最多 8 个，再高没有收益，反而容易触发对端限流。
constexpr size_t kMaxParallelQueries = 4;
constexpr size_t kMaxSites = 8;
constexpr size_t kMaxCredentialChars = 4096;
constexpr size_t kMaxSiteNameChars = 48;
constexpr size_t kMaxBaseUrlChars = 512;
constexpr size_t kMaxCustomScriptChars = 16384;

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), count) != count) return {};
    return result;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), count, nullptr, nullptr) != count) return {};
    return result;
}

bool IsValidCredential(const std::wstring& credential) {
    if (credential.empty() || credential.size() > kMaxCredentialChars) return false;
    return std::all_of(credential.begin(), credential.end(), [](wchar_t c) {
        return c >= 0x21 && c <= 0x7e;
    });
}

bool IsValidBaseUrl(const std::wstring& value) {
    if (value.empty() || value.size() > kMaxBaseUrlChars) return false;
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUserNameLength = static_cast<DWORD>(-1);
    components.dwPasswordLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(value.c_str(), static_cast<DWORD>(value.size()), 0, &components)) return false;
    return (components.nScheme == INTERNET_SCHEME_HTTPS || components.nScheme == INTERNET_SCHEME_HTTP) &&
        components.dwHostNameLength > 0 && components.dwUserNameLength == 0 &&
        components.dwPasswordLength == 0 && components.dwExtraInfoLength == 0;
}

bool IsAIAPIBaseUrl(const std::wstring& value) {
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwUserNameLength = static_cast<DWORD>(-1);
    components.dwPasswordLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(value.c_str(), static_cast<DWORD>(value.size()), 0, &components)) return false;
    if (components.nScheme != INTERNET_SCHEME_HTTPS || components.dwUserNameLength != 0 ||
        components.dwPasswordLength != 0 || components.dwExtraInfoLength != 0 ||
        CompareStringOrdinal(components.lpszHostName, static_cast<int>(components.dwHostNameLength),
            L"AIAPI 站点", -1, TRUE) != CSTR_EQUAL) return false;
    if (components.dwUrlPathLength == 0) return true;
    return components.dwUrlPathLength == 1 && components.lpszUrlPath[0] == L'/';
}

bool IsNumericId(const std::wstring& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}

unsigned int NewSiteId() {
    static std::atomic<unsigned int> sequence{ 1 };
    unsigned int value = static_cast<unsigned int>(GetTickCount64()) ^
        (static_cast<unsigned int>(GetCurrentProcessId()) << 16) ^ sequence.fetch_add(1);
    return value == 0 ? sequence.fetch_add(1) : value;
}

std::wstring CurrencyPrefix(const std::string& currency) {
    if (currency == "CNY" || currency == "RMB" || currency == "¥") return L"¥";
    if (currency == "USD" || currency == "$") return L"$";
    if (currency == "HKD" || currency == "HK$") return L"HK$";
    const std::wstring wide = Utf8ToWide(currency);
    return wide.empty() ? L"" : wide + L" ";
}

std::wstring FormatMoney(double value, const std::string& currency) {
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << CurrencyPrefix(currency) << std::fixed << std::setprecision(2) << value;
    return stream.str();
}

std::wstring FormatCompactMoney(double value, const std::string& currency) {
    if (!std::isfinite(value) || value < 0.0) return L"--";
    const double magnitude = std::fabs(value);
    const std::wstring prefix = CurrencyPrefix(currency);
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    if (magnitude >= 9.99e17) {
        int exponent = static_cast<int>(std::floor(std::log10(std::max(1.0, magnitude))));
        double mantissa = value / std::pow(10.0, exponent);
        if (std::fabs(mantissa) >= 9.95) {
            mantissa /= 10.0;
            ++exponent;
        }
        stream << prefix << std::fixed << std::setprecision(1) << mantissa << L"e" << exponent;
        return stream.str();
    }

    static const wchar_t* suffixes[] = { L"", L"K", L"M", L"B", L"T", L"P" };
    double scaled = value;
    size_t suffix = 0;
    while (std::fabs(scaled) >= 1000.0 && suffix < std::size(suffixes) - 1) {
        scaled /= 1000.0;
        ++suffix;
    }
    stream << prefix << std::fixed << std::setprecision(2) << scaled << suffixes[suffix];
    return stream.str();
}

std::wstring LocalTimestamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t buffer[32]{};
    swprintf_s(buffer, std::size(buffer), L"%04u-%02u-%02u %02u:%02u:%02u",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    return buffer;
}

struct SiteValue {
    unsigned int id = 0;
    aiapi::SiteAdapter adapter = aiapi::SiteAdapter::Sub2API;
    bool accountBalance = false;
    std::wstring itemName;
    std::wstring itemId;
    std::wstring value = L"--";
    std::wstring fullValue = L"--";
    std::wstring packageSummary;
    // 额度类别（如“余额/套餐”），为空时按 packageSummary/accountBalance 推断。
    std::wstring label;
    // 适配器返回的补充信息（Key 名称、今日消费/次数等）。目前不参与显示，
    // 保留字段便于以后在详情面板里展示；tooltip 每行严格保持统一格式。
    std::wstring extra;
    std::wstring error;
    bool success = false;
};

struct Snapshot {
    std::vector<SiteValue> sites;
    std::wstring tooltip = L"请在插件选项中添加一个站点。";
};

class Plugin;
bool PinPluginModule();

class PluginItem final : public IPluginItem {
public:
    PluginItem(Plugin* owner, int itemIndex) : owner_(owner), itemIndex_(itemIndex) {}

    const wchar_t* GetItemName() const override;
    const wchar_t* GetItemId() const override;
    const wchar_t* GetItemLableText() const override;
    const wchar_t* GetItemValueText() const override;
    const wchar_t* GetItemValueSampleText() const override;

private:
    Plugin* owner_;
    int itemIndex_;
};

class Plugin final : public ITMPlugin {
public:
    Plugin() : items_{
        PluginItem(this, 0), PluginItem(this, 1), PluginItem(this, 2), PluginItem(this, 3),
        PluginItem(this, 4), PluginItem(this, 5), PluginItem(this, 6), PluginItem(this, 7)
    } {
        std::atomic_store(&snapshot_, std::make_shared<const Snapshot>());
    }

    IPluginItem* GetItem(int index) override {
        const auto snapshot = GetSnapshot();
        if (index < 0 || !snapshot || index >= static_cast<int>(snapshot->sites.size())) return nullptr;
        return &items_[static_cast<size_t>(index)];
    }

    void DataRequired() override {
        EnsureWorker();
    }

    OptionReturn ShowOptionsDialog(void* hParent) override {
        ConfigCopy current = GetConfig();
        DialogContext context{ this, std::move(current.sites), -1, false, current.refreshSeconds };
        const HMODULE module = ModuleHandle();
        if (!module) {
            ClearProfiles(context.sites);
            return OR_OPTION_UNCHANGED;
        }
        DialogBoxParamW(module, MAKEINTRESOURCEW(IDD_OPTIONS), static_cast<HWND>(hParent),
            OptionsDialogProc, reinterpret_cast<LPARAM>(&context));
        ClearProfiles(context.sites);
        return context.changed ? OR_OPTION_CHANGED : OR_OPTION_UNCHANGED;
    }

    const wchar_t* GetInfo(PluginInfoIndex index) override {
        switch (index) {
        case TMI_NAME: return L"多站点 AI 余额";
        case TMI_DESCRIPTION: return L"AIAPI/New API/Sub2API 中转站、自定义接口，以及 DeepSeek 等官方余额、Kimi 等 Token Plan、Claude/Codex 官方订阅";
        case TMI_AUTHOR: return L"TrafficMonitor AIAPI quota plugin";
        case TMI_COPYRIGHT: return L"Copyright (C) 2026";
        case TMI_VERSION: return L"3.3.0";
        case TMI_URL: return L"{AIAPI 站点}";
        default: return L"";
        }
    }

    const wchar_t* GetTooltipInfo() override {
        thread_local std::shared_ptr<const Snapshot> held;
        held = GetSnapshot();
        return held ? held->tooltip.c_str() : L"AIAPI 额度暂不可用。";
    }

    void OnInitialize(ITrafficMonitor* app) override {
        std::lock_guard<std::mutex> initLock(initMutex_);
        if (initialized_) return;
        initialized_ = true;

        if (!ResolveConfigPath(app)) {
            auto snapshot = std::make_shared<Snapshot>();
            snapshot->tooltip = L"无法访问 TrafficMonitor 插件配置目录，请检查目录权限。";
            Publish(std::move(snapshot));
            return;
        }

        std::vector<aiapi::SiteProfile> profiles;
        int refreshSeconds = kDefaultRefreshSeconds;
        if (!LoadConfig(profiles, refreshSeconds)) {
            auto snapshot = std::make_shared<Snapshot>();
            snapshot->tooltip = L"站点配置无法解密或格式无效，请在插件选项中重新设置。";
            Publish(std::move(snapshot));
            return;
        }
        {
            std::lock_guard<std::mutex> configLock(configMutex_);
            config_.sites = std::move(profiles);
            config_.refreshSeconds = ClampRefreshSeconds(refreshSeconds);
            ++config_.generation;
        }
        PublishLoading();
        EnsureWorker();
    }

    std::shared_ptr<const Snapshot> GetSnapshot() const {
        return std::atomic_load(&snapshot_);
    }

    std::vector<aiapi::SiteProfile> GetProfiles() const {
        std::lock_guard<std::mutex> lock(configMutex_);
        return config_.sites;
    }

    bool SetProfiles(std::vector<aiapi::SiteProfile> profiles) {
        return SetConfig(std::move(profiles), GetRefreshSeconds());
    }

    // 站点列表与刷新间隔一起保存：两者共用同一个加密文件，避免半更新状态。
    bool SetConfig(std::vector<aiapi::SiteProfile> profiles, int refreshSeconds) {
        refreshSeconds = ClampRefreshSeconds(refreshSeconds);
        if (!ValidateProfiles(profiles) || !SaveConfig(profiles, refreshSeconds)) {
            ClearProfiles(profiles);
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(configMutex_);
            ClearProfiles(config_.sites);
            config_.sites = std::move(profiles);
            ++config_.generation;
            config_.refreshSeconds = refreshSeconds;
            PublishLoadingFor(config_.sites);
        }
        {
            // 用户刚保存过配置（可能改了凭据或地址），退避一律作废，立即重试。
            std::lock_guard<std::mutex> backoffLock(backoffMutex_);
            backoff_.clear();
        }
        RequestRefresh();
        EnsureWorker();
        return true;
    }

    int GetRefreshSeconds() const {
        std::lock_guard<std::mutex> lock(configMutex_);
        return config_.refreshSeconds;
    }

    static int ClampRefreshSeconds(int seconds) {
        return std::clamp(seconds, kMinRefreshSeconds, kMaxRefreshSeconds);
    }

private:
    struct Config {
        std::vector<aiapi::SiteProfile> sites;
        unsigned long long generation = 0;
        int refreshSeconds = kDefaultRefreshSeconds;
    };

    struct ConfigCopy {
        std::vector<aiapi::SiteProfile> sites;
        unsigned long long generation = 0;
        int refreshSeconds = kDefaultRefreshSeconds;
        ~ConfigCopy() {
            for (auto& site : sites) {
                if (!site.credential.empty()) SecureZeroMemory(site.credential.data(), site.credential.size() * sizeof(wchar_t));
                if (!site.customScript.empty()) SecureZeroMemory(site.customScript.data(), site.customScript.size() * sizeof(wchar_t));
            }
        }
    };

    // 单站点失败退避状态：连续失败时把下一次尝试时间往后推。
    struct RetryState {
        int consecutiveFailures = 0;
        std::chrono::steady_clock::time_point nextAttempt{};
    };

    struct DialogContext {
        Plugin* plugin;
        std::vector<aiapi::SiteProfile> sites;
        int selectedIndex;
        bool changed;
        int refreshSeconds;
        // 代码里刷新列表时会触发 LVN_ITEMCHANGED，用它跳过这次通知。
        bool updatingList = false;
    };

    struct CustomDialogContext {
        aiapi::SiteProfile* site;
    };

    static void ClearProfile(aiapi::SiteProfile& site) {
        if (!site.credential.empty()) SecureZeroMemory(site.credential.data(), site.credential.size() * sizeof(wchar_t));
        if (!site.customScript.empty()) SecureZeroMemory(site.customScript.data(), site.customScript.size() * sizeof(wchar_t));
        site.credential.clear();
        site.customScript.clear();
    }

    static void ClearProfiles(std::vector<aiapi::SiteProfile>& sites) {
        for (auto& site : sites) ClearProfile(site);
        sites.clear();
    }

    // 空编辑框的提示文字（Windows 原生 cue banner）。
    static void SetEditCue(HWND window, int controlId, const wchar_t* text) {
        if (HWND edit = GetDlgItem(window, controlId)) {
            SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(text));
        }
    }

    static bool ReadText(HWND window, int controlId, size_t limit, std::wstring& value) {
        std::vector<wchar_t> buffer(limit + 1, L'\0');
        const int count = GetDlgItemTextW(window, controlId, buffer.data(), static_cast<int>(buffer.size()));
        if (count < 0 || static_cast<size_t>(count) > limit) {
            SecureZeroMemory(buffer.data(), buffer.size() * sizeof(wchar_t));
            return false;
        }
        value.assign(buffer.data(), static_cast<size_t>(count));
        SecureZeroMemory(buffer.data(), buffer.size() * sizeof(wchar_t));
        return true;
    }

    // 编辑框只把 CRLF 当换行：默认脚本在源码里是 LF，旧配置也可能只有 LF。
    static std::wstring WithWindowsLineEndings(const std::wstring& text) {
        if (text.find(L'\n') == std::wstring::npos) return text;
        std::wstring result;
        result.reserve(text.size() + 16);
        for (size_t i = 0; i < text.size(); ++i) {
            const wchar_t ch = text[i];
            if (ch == L'\n' && (i == 0 || text[i - 1] != L'\r')) result += L"\r\n";
            else result += ch;
        }
        return result;
    }

    // 刷新间隔：数字 + 单位（秒/分钟），范围 15 秒 ~ 15 分钟，可手动输入。
    static void LoadRefreshInterval(HWND window, int seconds) {
        seconds = ClampRefreshSeconds(seconds);
        const bool useMinutes = seconds >= 60 && seconds % 60 == 0;
        SetDlgItemInt(window, IDC_REFRESH_EDIT, useMinutes ? seconds / 60 : seconds, FALSE);
        SendDlgItemMessageW(window, IDC_REFRESH_UNIT, CB_SETCURSEL, useMinutes ? 1 : 0, 0);
    }

    static bool ReadRefreshInterval(HWND window, int& seconds, std::wstring& error) {
        BOOL translated = FALSE;
        const UINT value = GetDlgItemInt(window, IDC_REFRESH_EDIT, &translated, FALSE);
        const int unit = static_cast<int>(
            SendDlgItemMessageW(window, IDC_REFRESH_UNIT, CB_GETCURSEL, 0, 0));
        if (!translated || value == 0) {
            error = L"刷新间隔需要填写大于 0 的数字。";
            return false;
        }
        const int requested = unit == 1 ? static_cast<int>(value) * 60 : static_cast<int>(value);
        const int clamped = ClampRefreshSeconds(requested);
        if (clamped != requested) {
            error = L"刷新间隔需在 15 秒到 15 分钟之间。";
            return false;
        }
        seconds = requested;
        return true;
    }

    static int ProviderCategoryIndex(aiapi::ProviderCategory category) {
        switch (category) {
        case aiapi::ProviderCategory::Relay: return 0;
        case aiapi::ProviderCategory::OfficialBalance: return 1;
        case aiapi::ProviderCategory::OfficialPlan: return 2;
        case aiapi::ProviderCategory::OfficialSubscription: return 3;
        }
        return 0;
    }

    static aiapi::ProviderCategory ProviderCategoryFromIndex(int index) {
        switch (index) {
        case 1: return aiapi::ProviderCategory::OfficialBalance;
        case 2: return aiapi::ProviderCategory::OfficialPlan;
        case 3: return aiapi::ProviderCategory::OfficialSubscription;
        default: return aiapi::ProviderCategory::Relay;
        }
    }

    static const wchar_t* ProviderCategoryName(int index) {
        switch (index) {
        case 1: return L"官方余额";
        case 2: return L"Token Plan";
        case 3: return L"官方订阅";
        default: return L"中转站与自定义";
        }
    }

    static constexpr int kProviderCategoryCount = 4;

    // 服务商下拉只显示当前分类的预设；selected 不在该分类时选中第一项。
    static void PopulateProviderCombo(HWND window, aiapi::ProviderCategory category,
        aiapi::SiteProvider selected) {
        SendDlgItemMessageW(window, IDC_SITE_TYPE, CB_RESETCONTENT, 0, 0);
        int position = 0;
        int selectIndex = 0;
        for (size_t i = 0; i < aiapi::ProviderPresetCount(); ++i) {
            const auto* preset = aiapi::ProviderPresetAt(i);
            if (!preset || preset->category != category) continue;
            SendDlgItemMessageW(window, IDC_SITE_TYPE, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(preset->displayName));
            if (preset->provider == selected) selectIndex = position;
            ++position;
        }
        SendDlgItemMessageW(window, IDC_SITE_TYPE, CB_SETCURSEL,
            static_cast<WPARAM>(selectIndex), 0);
    }

    static aiapi::SiteProvider SelectedProvider(HWND window) {
        const int categoryIndex = static_cast<int>(
            SendDlgItemMessageW(window, IDC_SITE_CATEGORY, CB_GETCURSEL, 0, 0));
        const aiapi::ProviderCategory category = ProviderCategoryFromIndex(categoryIndex);
        const int providerIndex = static_cast<int>(
            SendDlgItemMessageW(window, IDC_SITE_TYPE, CB_GETCURSEL, 0, 0));
        int position = 0;
        for (size_t i = 0; i < aiapi::ProviderPresetCount(); ++i) {
            const auto* preset = aiapi::ProviderPresetAt(i);
            if (!preset || preset->category != category) continue;
            if (position == providerIndex) return preset->provider;
            ++position;
        }
        return aiapi::SiteProvider::AIAPI;
    }

    // 切换服务商后清空与旧服务商绑定的字段（凭据/User ID），并套用新预设的默认地址。
    static void ApplyProviderFields(HWND window, DialogContext* context,
        aiapi::SiteProvider previousProvider) {
        if (context->selectedIndex < 0 ||
            context->selectedIndex >= static_cast<int>(context->sites.size())) return;
        auto& site = context->sites[static_cast<size_t>(context->selectedIndex)];
        if (previousProvider == site.adapter) return;
        if (!site.credential.empty()) {
            SecureZeroMemory(site.credential.data(), site.credential.size() * sizeof(wchar_t));
        }
        site.credential.clear();
        site.userId.clear();
        const aiapi::ProviderPreset* preset = aiapi::FindProviderPreset(site.adapter);
        if (preset) {
            switch (preset->baseUrlMode) {
            case aiapi::FieldMode::Fixed:
            case aiapi::FieldMode::Editable:
                site.baseUrl = preset->defaultBaseUrl ? preset->defaultBaseUrl : L"";
                break;
            case aiapi::FieldMode::Hidden:
                site.baseUrl.clear();
                break;
            }
        }
        UpdateSiteList(window, context);
    }

    // 对话框居中：优先对齐可见的父窗口，否则退回屏幕工作区（宿主主窗口可能隐藏）。
    static void CenterDialog(HWND window) {
        RECT dialog{};
        if (!GetWindowRect(window, &dialog)) return;
        RECT anchor{};
        const HWND owner = GetParent(window);
        const bool useOwner = owner && IsWindowVisible(owner) && !IsIconic(owner) &&
            GetWindowRect(owner, &anchor) && MonitorFromRect(&anchor, MONITOR_DEFAULTTONULL) != nullptr;
        if (!useOwner) {
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &anchor, 0);
        }
        const int width = dialog.right - dialog.left;
        const int height = dialog.bottom - dialog.top;
        SetWindowPos(window, nullptr,
            anchor.left + ((anchor.right - anchor.left) - width) / 2,
            anchor.top + ((anchor.bottom - anchor.top) - height) / 2,
            0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    static void SetDetailEnabled(HWND window, bool enabled) {
        for (int id : { IDC_SITE_ENABLED, IDC_SITE_CATEGORY, IDC_SITE_TYPE, IDC_CUSTOM_CONFIG, IDC_SITE_NAME,
                        IDC_SITE_BASE_URL, IDC_SITE_BASE_URL_LABEL, IDC_SITE_CREDENTIAL,
                        IDC_SITE_CREDENTIAL_LABEL, IDC_SITE_USER_ID, IDC_SITE_USER_ID_LABEL }) {
            EnableWindow(GetDlgItem(window, id), enabled ? TRUE : FALSE);
        }
    }

    static void InsertListViewColumn(HWND list, int index, const wchar_t* text, int width) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(text);
        column.cx = width;
        column.iSubItem = index;
        SendMessageW(list, LVM_INSERTCOLUMNW, static_cast<WPARAM>(index),
            reinterpret_cast<LPARAM>(&column));
    }

    static void SetListViewSubItem(HWND list, int row, int column, const wchar_t* text) {
        LVITEMW item{};
        item.iItem = row;
        item.iSubItem = column;
        item.mask = LVIF_TEXT;
        item.pszText = const_cast<wchar_t*>(text);
        SendMessageW(list, LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&item));
    }

    // 与宿主“插件管理”一致：ListView 报表模式显示 名称 / 类型 / 状态。
    static void InitSiteListView(HWND window) {
        HWND list = GetDlgItem(window, IDC_SITE_LIST);
        if (!list) return;
        const LPARAM extended = LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP | LVS_EX_DOUBLEBUFFER;
        SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, static_cast<WPARAM>(extended), extended);
        RECT client{};
        GetClientRect(list, &client);
        const int total = client.right - client.left;
        const int nameWidth = total * 44 / 100;
        const int typeWidth = total * 34 / 100;
        const int statusWidth = std::max(28, total - nameWidth - typeWidth - 8);
        InsertListViewColumn(list, 0, L"名称", nameWidth);
        InsertListViewColumn(list, 1, L"类型", typeWidth);
        InsertListViewColumn(list, 2, L"状态", statusWidth);
    }

    static void SelectListViewRow(HWND list, int index) {
        if (index < 0) return;
        LVITEMW item{};
        item.mask = LVIF_STATE;
        item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        item.state = LVIS_SELECTED | LVIS_FOCUSED;
        item.iItem = index;
        SendMessageW(list, LVM_SETITEMSTATE, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&item));
        SendMessageW(list, LVM_ENSUREVISIBLE, static_cast<WPARAM>(index), FALSE);
    }

    static const wchar_t* AdapterDisplayName(aiapi::SiteAdapter adapter) {
        const auto* preset = aiapi::FindProviderPreset(adapter);
        return preset ? preset->shortName : L"未知";
    }

    static void UpdateAdapterFields(HWND window, aiapi::SiteAdapter adapter,
        bool hasCredential, bool hasCustomScript) {
        const aiapi::ProviderPreset* preset = aiapi::FindProviderPreset(adapter);
        if (!preset) return;
        const bool newApi = adapter == aiapi::SiteAdapter::NewAPI;
        const bool custom = adapter == aiapi::SiteAdapter::Custom;
        const bool needsCredential = preset->needsCredential;

        EnableWindow(GetDlgItem(window, IDC_SITE_BASE_URL),
            preset->baseUrlMode == aiapi::FieldMode::Editable ? TRUE : FALSE);
        EnableWindow(GetDlgItem(window, IDC_SITE_USER_ID), newApi ? TRUE : FALSE);
        EnableWindow(GetDlgItem(window, IDC_SITE_USER_ID_LABEL), newApi ? TRUE : FALSE);
        EnableWindow(GetDlgItem(window, IDC_CUSTOM_CONFIG), custom ? TRUE : FALSE);
        EnableWindow(GetDlgItem(window, IDC_SITE_CREDENTIAL), needsCredential || custom ? TRUE : FALSE);
        // 用不到的输入框直接隐藏，避免出现一排灰色空框；标签保留作为说明。
        ShowWindow(GetDlgItem(window, IDC_SITE_USER_ID), newApi ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(window, IDC_SITE_USER_ID_LABEL), newApi ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(window, IDC_SITE_CREDENTIAL),
            (needsCredential || custom) ? SW_SHOW : SW_HIDE);

        // 固定/隐藏地址的显示值统一由预设决定，避免旧配置里的残留值误导。
        if (preset->baseUrlMode == aiapi::FieldMode::Fixed && preset->defaultBaseUrl) {
            SetDlgItemTextW(window, IDC_SITE_BASE_URL, preset->defaultBaseUrl);
        } else if (preset->baseUrlMode == aiapi::FieldMode::Hidden) {
            SetDlgItemTextW(window, IDC_SITE_BASE_URL, L"（使用本机 CLI 凭据）");
        }

        std::wstring credentialLabel = preset->credentialLabel ? preset->credentialLabel : L"凭据";
        if (hasCredential && needsCredential) credentialLabel += L"（已保存）";
        SetDlgItemTextW(window, IDC_SITE_CREDENTIAL_LABEL, credentialLabel.c_str());

        switch (adapter) {
        case aiapi::SiteAdapter::AIAPI:
            SetDlgItemTextW(window, IDC_SITE_BASE_URL_LABEL, L"站点地址（固定）");
            break;
        case aiapi::SiteAdapter::NewAPI:
            SetDlgItemTextW(window, IDC_SITE_BASE_URL_LABEL, L"New API Base URL");
            break;
        case aiapi::SiteAdapter::Sub2API:
            SetDlgItemTextW(window, IDC_SITE_BASE_URL_LABEL, L"站点根 URL（可带 /v1）");
            break;
        case aiapi::SiteAdapter::Custom:
            SetDlgItemTextW(window, IDC_SITE_BASE_URL_LABEL, L"自定义 Base URL（相对 URL 前缀）");
            break;
        case aiapi::SiteAdapter::ClaudeOfficial:
        case aiapi::SiteAdapter::CodexOfficial:
            SetDlgItemTextW(window, IDC_SITE_BASE_URL_LABEL, L"服务地址（本机凭据）");
            break;
        case aiapi::SiteAdapter::ZhipuPlan:
            SetDlgItemTextW(window, IDC_SITE_BASE_URL_LABEL, L"智谱站点地址");
            break;
        case aiapi::SiteAdapter::ZenMuxPlan:
            SetDlgItemTextW(window, IDC_SITE_BASE_URL_LABEL, L"ZenMux Base URL");
            break;
        case aiapi::SiteAdapter::CommandCodePlan:
            SetDlgItemTextW(window, IDC_SITE_BASE_URL_LABEL, L"Command Code 地址");
            break;
        default:
            SetDlgItemTextW(window, IDC_SITE_BASE_URL_LABEL, L"服务地址（固定）");
            break;
        }

        const std::wstring providerName = preset->shortName ? preset->shortName : L"";
        switch (preset->category) {
        case aiapi::ProviderCategory::OfficialBalance:
            SetDlgItemTextW(window, IDC_STATUS, hasCredential
                ? (L"已保存 API Key，后台查询" + providerName + L"官方余额。").c_str()
                : (L"填写" + providerName + L"的 API Key 后查询官方余额。").c_str());
            break;
        case aiapi::ProviderCategory::OfficialPlan:
            SetDlgItemTextW(window, IDC_STATUS, hasCredential
                ? (L"已保存 API Key，后台查询" + providerName + L"套餐额度。").c_str()
                : (L"填写" + providerName + L"的 API Key 后查询套餐额度。").c_str());
            break;
        case aiapi::ProviderCategory::OfficialSubscription:
            SetDlgItemTextW(window, IDC_STATUS,
                (L"使用本机 " + std::wstring(preset->subscriptionCli ? preset->subscriptionCli : L"CLI") +
                    L" 的登录凭据，无需填写 Key。").c_str());
            break;
        case aiapi::ProviderCategory::Relay:
        default:
            switch (adapter) {
            case aiapi::SiteAdapter::AIAPI:
                SetDlgItemTextW(window, IDC_STATUS, hasCredential
                    ? L"已保存 AIAPI Key，后台按 GET /v1/usage 查询余额。"
                    : L"填写 AIAPI Key 后查询余额。");
                break;
            case aiapi::SiteAdapter::NewAPI:
                SetDlgItemTextW(window, IDC_STATUS, hasCredential
                    ? L"已保存 System Access Token，按用户 quota 查询余额。"
                    : L"填写 Base URL、System Access Token 和数字 User ID。");
                break;
            case aiapi::SiteAdapter::Sub2API:
                SetDlgItemTextW(window, IDC_STATUS, hasCredential
                    ? L"已保存 Sub2API API Key，请求 GET <Base URL>/v1/usage。"
                    : L"填写站点根 URL 和 API Key（可带 /v1），无需 User ID。");
                break;
            case aiapi::SiteAdapter::Custom:
                SetDlgItemTextW(window, IDC_STATUS, hasCustomScript
                    ? L"已配置 request / extractor，可点“自定义配置…”继续修改。"
                    : L"点右侧“自定义配置…”编写 request 和 extractor。");
                break;
            default:
                break;
            }
            break;
        }
        if (adapter == aiapi::SiteAdapter::ZhipuPlan) {
            wchar_t buffer[512]{};
            GetDlgItemTextW(window, IDC_STATUS, buffer, static_cast<int>(std::size(buffer)));
            std::wstring hint = buffer;
            hint += L"（地址可用 open.bigmodel.cn 或 api.z.ai）";
            SetDlgItemTextW(window, IDC_STATUS, hint.c_str());
        }
    }

    static void LoadSelectedSite(HWND window, DialogContext* context) {
        if (context->selectedIndex < 0 || context->selectedIndex >= static_cast<int>(context->sites.size())) {
            SetDetailEnabled(window, false);
            SetDlgItemTextW(window, IDC_SITE_NAME, L"");
            SetDlgItemTextW(window, IDC_SITE_BASE_URL, L"");
            SetDlgItemTextW(window, IDC_SITE_CREDENTIAL, L"");
            SetDlgItemTextW(window, IDC_SITE_USER_ID, L"");
            SendDlgItemMessageW(window, IDC_SITE_ENABLED, BM_SETCHECK, BST_UNCHECKED, 0);
            SendDlgItemMessageW(window, IDC_SITE_CATEGORY, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
            SendDlgItemMessageW(window, IDC_SITE_TYPE, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
            SetEditCue(window, IDC_SITE_NAME, L"例如：AIAPI 主账号");
            SetEditCue(window, IDC_SITE_BASE_URL, L"https://");
            SetEditCue(window, IDC_SITE_CREDENTIAL, L"粘贴 API Key / Token");
            SetEditCue(window, IDC_SITE_USER_ID, L"数字 User ID");
            return;
        }
        const auto& site = context->sites[static_cast<size_t>(context->selectedIndex)];
        const aiapi::ProviderPreset* preset = aiapi::FindProviderPreset(site.adapter);
        SetDetailEnabled(window, true);
        SetDlgItemTextW(window, IDC_SITE_NAME, site.name.c_str());
        std::wstring baseUrl = site.baseUrl;
        if (baseUrl.empty() && preset && preset->baseUrlMode == aiapi::FieldMode::Editable &&
            preset->defaultBaseUrl) {
            baseUrl = preset->defaultBaseUrl;
        }
        SetDlgItemTextW(window, IDC_SITE_BASE_URL, baseUrl.c_str());
        SetDlgItemTextW(window, IDC_SITE_CREDENTIAL, L"");
        SetDlgItemTextW(window, IDC_SITE_USER_ID, site.userId.c_str());
        SetEditCue(window, IDC_SITE_NAME, L"例如：AIAPI 主账号");
        SetEditCue(window, IDC_SITE_BASE_URL, L"https://");
        SetEditCue(window, IDC_SITE_CREDENTIAL,
            preset && !preset->needsCredential ? L"" :
            (site.credential.empty() ? L"粘贴 API Key / Token" : L"已保存，留空表示不修改"));
        SetEditCue(window, IDC_SITE_USER_ID, L"数字 User ID");
        const aiapi::ProviderCategory category = preset ? preset->category : aiapi::ProviderCategory::Relay;
        SendDlgItemMessageW(window, IDC_SITE_CATEGORY, CB_SETCURSEL,
            static_cast<WPARAM>(ProviderCategoryIndex(category)), 0);
        PopulateProviderCombo(window, category, site.adapter);
        SendDlgItemMessageW(window, IDC_SITE_ENABLED, BM_SETCHECK, site.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        UpdateAdapterFields(window, site.adapter, !site.credential.empty(), !site.customScript.empty());
    }

    static void UpdateSiteList(HWND window, DialogContext* context) {
        HWND list = GetDlgItem(window, IDC_SITE_LIST);
        if (!list) return;
        context->updatingList = true;
        SendMessageW(list, LVM_DELETEALLITEMS, 0, 0);
        for (size_t i = 0; i < context->sites.size(); ++i) {
            const auto& site = context->sites[i];
            const std::wstring name = site.name.empty() ? L"(未命名站点)" : site.name;
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(i);
            item.pszText = const_cast<wchar_t*>(name.c_str());
            const int row = static_cast<int>(
                SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item)));
            if (row < 0) continue;
            SetListViewSubItem(list, row, 1, AdapterDisplayName(site.adapter));
            SetListViewSubItem(list, row, 2, site.enabled ? L"启用" : L"停用");
        }
        if (context->sites.empty()) {
            context->selectedIndex = -1;
        } else if (context->selectedIndex < 0 ||
                   context->selectedIndex >= static_cast<int>(context->sites.size())) {
            context->selectedIndex = 0;
        }
        if (context->selectedIndex >= 0) SelectListViewRow(list, context->selectedIndex);
        context->updatingList = false;
        const std::wstring groupText = L"站点（" + std::to_wstring(context->sites.size()) + L"/" +
            std::to_wstring(kMaxSites) + L"）";
        SetDlgItemTextW(window, IDC_GROUP_SITES, groupText.c_str());
        LoadSelectedSite(window, context);
        EnableWindow(GetDlgItem(window, IDC_SITE_ADD), context->sites.size() < kMaxSites ? TRUE : FALSE);
        EnableWindow(GetDlgItem(window, IDC_SITE_REMOVE), context->selectedIndex >= 0 ? TRUE : FALSE);
        if (context->sites.empty()) {
            SetDlgItemTextW(window, IDC_STATUS,
                L"尚未添加站点。点击“新增站点”添加 AIAPI、New API、Sub2API 或自定义接口。");
        }
    }

    static bool StoreSelectedSite(HWND window, DialogContext* context) {
        if (context->selectedIndex < 0 || context->selectedIndex >= static_cast<int>(context->sites.size())) return true;
        auto& site = context->sites[static_cast<size_t>(context->selectedIndex)];
        std::wstring name, baseUrl, enteredCredential, userId;
        const bool readOkay = ReadText(window, IDC_SITE_NAME, kMaxSiteNameChars, name) &&
            ReadText(window, IDC_SITE_BASE_URL, kMaxBaseUrlChars, baseUrl) &&
            ReadText(window, IDC_SITE_CREDENTIAL, kMaxCredentialChars, enteredCredential) &&
            ReadText(window, IDC_SITE_USER_ID, 32, userId);
        if (!readOkay) {
            if (!enteredCredential.empty()) SecureZeroMemory(enteredCredential.data(), enteredCredential.size() * sizeof(wchar_t));
            return false;
        }
        site.name = std::move(name);
        site.baseUrl = std::move(baseUrl);
        site.userId = std::move(userId);
        site.adapter = SelectedProvider(window);
        const aiapi::ProviderPreset* preset = aiapi::FindProviderPreset(site.adapter);
        if (preset) {
            switch (preset->baseUrlMode) {
            case aiapi::FieldMode::Fixed:
                site.baseUrl = preset->defaultBaseUrl ? preset->defaultBaseUrl : L"";
                break;
            case aiapi::FieldMode::Hidden:
                site.baseUrl.clear();
                break;
            case aiapi::FieldMode::Editable:
                if (site.baseUrl.empty() && preset->defaultBaseUrl && *preset->defaultBaseUrl) {
                    site.baseUrl = preset->defaultBaseUrl;
                }
                break;
            }
        }
        site.enabled = SendDlgItemMessageW(window, IDC_SITE_ENABLED, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (!enteredCredential.empty()) {
            ClearProfile(site);
            site.credential = std::move(enteredCredential);
        }
        return true;
    }

    static bool ValidateProfiles(const std::vector<aiapi::SiteProfile>& sites, std::wstring* error = nullptr) {
        auto fail = [error](const std::wstring& message) {
            if (error) *error = message;
            return false;
        };
        if (sites.size() > kMaxSites) return fail(L"最多支持 8 个站点。");
        for (size_t i = 0; i < sites.size(); ++i) {
            const auto& site = sites[i];
            if (site.id == 0 || site.name.empty()) return fail(L"请填写站点显示名称。");
            if (site.name.size() > kMaxSiteNameChars) return fail(L"站点名称过长。");
            if (site.customScript.size() > kMaxCustomScriptChars) return fail(L"自定义提取器脚本超过 16 KiB 限制。");
            const aiapi::ProviderPreset* preset = aiapi::FindProviderPreset(site.adapter);
            if (!preset) return fail(L"未知接口类型。");
            const std::wstring providerName = preset->shortName ? preset->shortName : L"站点";

            if (site.adapter == aiapi::SiteAdapter::AIAPI && !IsAIAPIBaseUrl(site.baseUrl)) {
                return fail(L"AIAPI 是专用站点适配器，只能访问 {AIAPI 站点}。");            }
            if (!site.baseUrl.empty() && !IsValidBaseUrl(site.baseUrl)) {
                return fail(L"站点 Base URL 无效，请填写 HTTP(S) 地址。");
            }
            if (!site.credential.empty() && !IsValidCredential(site.credential)) {
                return fail(L"凭据格式无效或长度超过限制。");
            }

            if (site.enabled) {
                if (site.adapter == aiapi::SiteAdapter::Custom) {
                    if (site.customScript.empty()) return fail(L"已启用的自定义站点需要先配置 extractor 脚本。");
                } else if (preset->needsCredential) {
                    if (site.credential.empty()) {
                        return fail(providerName + L" 需要填写 API Key。");
                    }
                    if (site.adapter == aiapi::SiteAdapter::NewAPI) {
                        if (site.baseUrl.empty()) return fail(L"New API 必须填写 Base URL。");
                        if (!IsNumericId(site.userId)) return fail(L"New API User ID 必须为数字。");
                    }
                    if (site.adapter == aiapi::SiteAdapter::Sub2API && site.baseUrl.empty()) {
                        return fail(L"Sub2API 必须填写站点根 URL。");
                    }
                    if (site.adapter == aiapi::SiteAdapter::AIAPI && site.baseUrl.empty()) {
                        return fail(L"AIAPI 站点地址无效。");
                    }
                }
                // 官方订阅（usesLocalCredentials）无需地址和凭据，由适配器读取本机 CLI 凭据。
            }
            for (size_t j = 0; j < i; ++j) if (sites[j].id == site.id) return fail(L"站点内部 ID 重复，请重新新增该站点。");
        }
        return true;
    }

    // 自定义接口的预设模板（对应 CC-Switch 用量查询里的“通用 / New API / Sub2API”模板）。
    // 索引 0-2 是可载入的模板，索引 3 表示保留用户当前脚本。
    static constexpr int kCustomTemplateGeneric = 0;
    static constexpr int kCustomTemplateNewApi = 1;
    static constexpr int kCustomTemplateSub2Api = 2;
    static constexpr int kCustomTemplateKeep = 3;

    static const wchar_t* CustomTemplateName(int index) {
        switch (index) {
        case kCustomTemplateGeneric: return L"通用模板";
        case kCustomTemplateNewApi: return L"New API 模板";
        case kCustomTemplateSub2Api: return L"Sub2API 模板";
        default: return L"自定义脚本";
        }
    }

    static std::wstring CustomTemplateScript(int index) {
        switch (index) {
        case kCustomTemplateNewApi:
            return LR"CC(({
  request: {
    url: "{{baseUrl}}/api/user/self",
    method: "GET",
    headers: {
      "Content-Type": "application/json",
      "Authorization": "Bearer {{accessToken}}",
      "New-Api-User": "{{userId}}"
    }
  },
  extractor: function (response) {
    if (response.success && response.data) {
      return {
        planName: response.data.group || "默认套餐",
        remaining: response.data.quota / 500000,
        used: response.data.used_quota / 500000,
        total: (response.data.quota + response.data.used_quota) / 500000,
        unit: "USD"
      };
    }
    return { isValid: false, invalidMessage: response.message || "查询失败" };
  }
}))CC";
        case kCustomTemplateSub2Api:
            return LR"CC(({
  request: {
    url: "{{baseUrl}}".replace(/\/+$/, "").replace(/\/v1$/, "") + "/v1/usage",
    method: "GET",
    headers: {
      "Authorization": "Bearer {{apiKey}}",
      "User-Agent": "cc-switch/1.0"
    }
  },
  extractor: function (response) {
    var quota = response.quota || {};
    var result = {
      isValid: response.isValid !== false,
      unit: response.unit || quota.unit || "USD"
    };
    if (typeof response.planName === "string") result.planName = response.planName;
    if (typeof response.remaining === "number" && response.remaining >= 0) {
      result.remaining = response.remaining;
    }
    if (quota.limit > 0) {
      result.used = quota.used;
      result.total = quota.limit;
    }
    if (!result.isValid) {
      result.invalidMessage = response.status ? String(response.status) : "查询失败";
    }
    return result;
  }
}))CC";
        case kCustomTemplateGeneric:
        default:
            return LR"CC(({
  request: {
    url: "{{baseUrl}}/user/balance",
    method: "GET",
    headers: {
      "Authorization": "Bearer {{apiKey}}",
      "User-Agent": "cc-switch/1.0"
    }
  },
  extractor: function(response) {
    return {
      isValid: response.is_active !== false,
      remaining: response.balance,
      unit: "USD"
    };
  }
}))CC";
        }
    }

    static std::wstring DefaultCustomScript() {
        return CustomTemplateScript(kCustomTemplateGeneric);
    }

    static INT_PTR CALLBACK CustomConfigDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* context = reinterpret_cast<CustomDialogContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));

        switch (message) {
        case WM_INITDIALOG: {
            context = reinterpret_cast<CustomDialogContext*>(lParam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
            CenterDialog(window);
            const std::wstring script = WithWindowsLineEndings(context->site->customScript.empty()
                ? DefaultCustomScript() : context->site->customScript);
            SetDlgItemTextW(window, IDC_CUSTOM_SCRIPT, script.c_str());
            SendDlgItemMessageW(window, IDC_CUSTOM_SCRIPT, EM_LIMITTEXT,
                static_cast<WPARAM>(kMaxCustomScriptChars), 0);
            // 预设模板下拉：打开时按当前脚本内容回显，改动即载入对应模板。
            for (int i = 0; i <= kCustomTemplateKeep; ++i) {
                SendDlgItemMessageW(window, IDC_CUSTOM_TEMPLATE, CB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(CustomTemplateName(i)));
            }
            int matchedTemplate = kCustomTemplateKeep;
            for (int i = 0; i < kCustomTemplateKeep; ++i) {
                if (script == WithWindowsLineEndings(CustomTemplateScript(i))) {
                    matchedTemplate = i;
                    break;
                }
            }
            SendDlgItemMessageW(window, IDC_CUSTOM_TEMPLATE, CB_SETCURSEL,
                static_cast<WPARAM>(matchedTemplate), 0);
            SetDlgItemTextW(window, IDC_CUSTOM_STATUS,
                L"请求只允许 HTTPS GET/POST，自动禁用 Cookie 与重定向；脚本不能访问文件、进程或网络对象。");
            // 脚本编辑区用等宽字体；高度沿用对话框字体，DPI 缩放交给系统。
            // 返回 FALSE 并自己设置焦点，避免对话框管理器默认“全选第一个编辑框”。
            if (HWND editor = GetDlgItem(window, IDC_CUSTOM_SCRIPT)) {
                LOGFONTW font{};
                if (HFONT base = reinterpret_cast<HFONT>(SendMessageW(editor, WM_GETFONT, 0, 0))) {
                    GetObjectW(base, sizeof(font), &font);
                }
                if (font.lfHeight == 0) font.lfHeight = -MulDiv(9, GetDpiForWindow(window), 72);
                wcscpy_s(font.lfFaceName, L"Consolas");
                font.lfWeight = FW_NORMAL;
                font.lfItalic = FALSE;
                font.lfQuality = CLEARTYPE_QUALITY;
                if (HFONT mono = CreateFontIndirectW(&font)) {
                    SendMessageW(editor, WM_SETFONT, reinterpret_cast<WPARAM>(mono), TRUE);
                    SetPropW(window, L"aiapi.mono", reinterpret_cast<HANDLE>(mono));
                }
                SetFocus(editor);
                SendMessageW(editor, EM_SETSEL, 0, 0);
            }
            return FALSE;
        }
        case WM_DESTROY:
            if (auto* mono = reinterpret_cast<HFONT>(RemovePropW(window, L"aiapi.mono"))) {
                DeleteObject(mono);
            }
            return FALSE;
        case WM_CTLCOLORSTATIC: {
            const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
            if (id == IDC_CUSTOM_STATUS) {
                HDC dc = reinterpret_cast<HDC>(wParam);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
                return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
            }
            return FALSE;
        }
        default:
            break;
        }

        if (message != WM_COMMAND || !context) return FALSE;
        switch (LOWORD(wParam)) {
        case IDC_CUSTOM_TEMPLATE:
            if (HIWORD(wParam) != CBN_SELCHANGE) return FALSE;
            {
                const int index = static_cast<int>(
                    SendDlgItemMessageW(window, IDC_CUSTOM_TEMPLATE, CB_GETCURSEL, 0, 0));
                if (index < 0 || index >= kCustomTemplateKeep) return TRUE;
                if (MessageBoxW(window, L"载入选中的模板会覆盖当前脚本内容，继续？",
                    L"预设模板", MB_YESNO | MB_ICONQUESTION) != IDYES) {
                    return TRUE;
                }
                const std::wstring templateScript = WithWindowsLineEndings(CustomTemplateScript(index));
                SetDlgItemTextW(window, IDC_CUSTOM_SCRIPT, templateScript.c_str());
                SendDlgItemMessageW(window, IDC_CUSTOM_SCRIPT, EM_SETSEL, 0, 0);
            }
            return TRUE;
        case IDOK: {
            std::wstring script;
            if (!ReadText(window, IDC_CUSTOM_SCRIPT, kMaxCustomScriptChars, script) || script.empty()) {
                MessageBoxW(window, L"请输入完整的配置对象，包含 request 和 extractor。", L"自定义接口", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            if (!context->site->customScript.empty()) {
                SecureZeroMemory(context->site->customScript.data(), context->site->customScript.size() * sizeof(wchar_t));
            }
            context->site->customScript = std::move(script);
            EndDialog(window, IDOK);
            return TRUE;
        }
        case IDC_CUSTOM_TEST: {
            if (MessageBoxW(window, L"将立即发送自定义配置中的请求。若 method 为 POST，可能修改远端数据。继续测试？",
                L"测试自定义接口", MB_YESNO | MB_ICONWARNING) != IDYES) return TRUE;
            std::wstring script;
            if (!ReadText(window, IDC_CUSTOM_SCRIPT, kMaxCustomScriptChars, script) || script.empty()) {
                MessageBoxW(window, L"请先填写提取器脚本。", L"测试自定义接口", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            aiapi::SiteProfile draft = *context->site;
            if (!draft.customScript.empty()) SecureZeroMemory(draft.customScript.data(), draft.customScript.size() * sizeof(wchar_t));
            draft.customScript = std::move(script);
            draft.enabled = true;
            SetDlgItemTextW(window, IDC_CUSTOM_STATUS, L"正在请求并执行 extractor…");
            UpdateWindow(window);
            const aiapi::SiteBalance result = aiapi::FetchSiteBalance(draft);
            const std::wstring resultMessage = result.success
                ? L"查询成功：" + (result.packageSummary.empty()
                    ? FormatMoney(result.remaining, result.unit)
                    : result.packageSummary) +
                    (result.detail.empty() ? L"" : L"\r\n" + result.detail)
                : (result.error.empty() ? L"查询失败。" : result.error);
            SetDlgItemTextW(window, IDC_CUSTOM_STATUS, resultMessage.c_str());
            if (!draft.credential.empty()) SecureZeroMemory(draft.credential.data(), draft.credential.size() * sizeof(wchar_t));
            if (!draft.customScript.empty()) SecureZeroMemory(draft.customScript.data(), draft.customScript.size() * sizeof(wchar_t));
            MessageBoxW(window, resultMessage.c_str(), result.success ? L"自定义接口测试成功" : L"自定义接口测试失败",
                MB_OK | (result.success ? MB_ICONINFORMATION : MB_ICONWARNING));
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(window, IDCANCEL);
            return TRUE;
        default:
            return FALSE;
        }
    }

    static INT_PTR CALLBACK OptionsDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* context = reinterpret_cast<DialogContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));

        switch (message) {
        case WM_INITDIALOG: {
            context = reinterpret_cast<DialogContext*>(lParam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
            INITCOMMONCONTROLSEX commonControls{ sizeof(commonControls), ICC_LISTVIEW_CLASSES };
            InitCommonControlsEx(&commonControls);
            CenterDialog(window);

            for (int i = 0; i < kProviderCategoryCount; ++i) {
                SendDlgItemMessageW(window, IDC_SITE_CATEGORY, CB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(ProviderCategoryName(i)));
            }
            SendDlgItemMessageW(window, IDC_SITE_NAME, EM_LIMITTEXT,
                static_cast<WPARAM>(kMaxSiteNameChars), 0);
            SendDlgItemMessageW(window, IDC_SITE_BASE_URL, EM_LIMITTEXT,
                static_cast<WPARAM>(kMaxBaseUrlChars), 0);
            SendDlgItemMessageW(window, IDC_SITE_CREDENTIAL, EM_LIMITTEXT,
                static_cast<WPARAM>(kMaxCredentialChars), 0);
            SendDlgItemMessageW(window, IDC_SITE_USER_ID, EM_LIMITTEXT, 32, 0);
            // 刷新间隔：数字 + 单位，允许手动输入任意 15 秒 ~ 15 分钟的值。
            SendDlgItemMessageW(window, IDC_REFRESH_UNIT, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(L"秒"));
            SendDlgItemMessageW(window, IDC_REFRESH_UNIT, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(L"分钟"));
            SendDlgItemMessageW(window, IDC_REFRESH_EDIT, EM_LIMITTEXT, 4, 0);
            LoadRefreshInterval(window, context->refreshSeconds);
            InitSiteListView(window);
            UpdateSiteList(window, context);
            return TRUE;
        }
        case WM_CTLCOLORSTATIC: {
            const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
            if (id == IDC_STATUS || id == IDC_HINT || id == IDC_REFRESH_HINT) {
                HDC dc = reinterpret_cast<HDC>(wParam);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
                return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
            }
            return FALSE;
        }
        case WM_NOTIFY: {
            const auto* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header && header->idFrom == IDC_SITE_LIST && header->code == LVN_ITEMCHANGED &&
                context && !context->updatingList) {
                const auto* changed = reinterpret_cast<const NMLISTVIEW*>(header);
                if ((changed->uChanged & LVIF_STATE) && (changed->uNewState & LVIS_SELECTED) &&
                    !(changed->uOldState & LVIS_SELECTED) && changed->iItem >= 0) {
                    StoreSelectedSite(window, context);
                    context->selectedIndex = changed->iItem;
                    LoadSelectedSite(window, context);
                }
                return TRUE;
            }
            return FALSE;
        }
        default:
            break;
        }

        if (message != WM_COMMAND || !context) return FALSE;

        switch (LOWORD(wParam)) {
        case IDC_SITE_CATEGORY:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                const int categoryIndex = static_cast<int>(
                    SendDlgItemMessageW(window, IDC_SITE_CATEGORY, CB_GETCURSEL, 0, 0));
                const aiapi::ProviderCategory category = ProviderCategoryFromIndex(categoryIndex);
                if (context->selectedIndex >= 0 &&
                    context->selectedIndex < static_cast<int>(context->sites.size())) {
                    const aiapi::SiteProvider previousProvider =
                        context->sites[static_cast<size_t>(context->selectedIndex)].adapter;
                    if (!StoreSelectedSite(window, context)) return TRUE;
                    PopulateProviderCombo(window, category, static_cast<aiapi::SiteProvider>(0));
                    context->sites[static_cast<size_t>(context->selectedIndex)].adapter =
                        SelectedProvider(window);
                    ApplyProviderFields(window, context, previousProvider);
                } else {
                    PopulateProviderCombo(window, category, static_cast<aiapi::SiteProvider>(0));
                }
            }
            return TRUE;
        case IDC_SITE_TYPE:
            if (HIWORD(wParam) == CBN_SELCHANGE &&
                context->selectedIndex >= 0 && context->selectedIndex < static_cast<int>(context->sites.size())) {
                const aiapi::SiteProvider previousProvider =
                    context->sites[static_cast<size_t>(context->selectedIndex)].adapter;
                if (!StoreSelectedSite(window, context)) return TRUE;
                ApplyProviderFields(window, context, previousProvider);
            }
            return TRUE;
        case IDC_SITE_ADD:
            if (!StoreSelectedSite(window, context)) return TRUE;
            if (context->sites.size() >= kMaxSites) {
                MessageBoxW(window, L"最多支持 8 个站点。", L"多站点 AI 余额", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            {
                aiapi::SiteProfile site;
                site.id = NewSiteId();
                site.adapter = aiapi::SiteAdapter::AIAPI;
                site.name = L"AIAPI " + std::to_wstring(context->sites.size() + 1);
                site.baseUrl = L"{AIAPI 站点}";
                context->sites.push_back(std::move(site));
                context->selectedIndex = static_cast<int>(context->sites.size()) - 1;
                UpdateSiteList(window, context);
            }
            SetFocus(GetDlgItem(window, IDC_SITE_NAME));
            SendDlgItemMessageW(window, IDC_SITE_NAME, EM_SETSEL, 0, -1);
            return TRUE;
        case IDC_SITE_REMOVE:
            if (!StoreSelectedSite(window, context)) return TRUE;
            if (context->selectedIndex >= 0 && context->selectedIndex < static_cast<int>(context->sites.size())) {
                const std::wstring siteName = context->sites[static_cast<size_t>(context->selectedIndex)].name;
                const std::wstring prompt = L"确定删除站点“" + siteName + L"”吗？保存后生效。";
                if (MessageBoxW(window, prompt.c_str(), L"删除站点", MB_YESNO | MB_ICONQUESTION) != IDYES) {
                    return TRUE;
                }
                ClearProfile(context->sites[static_cast<size_t>(context->selectedIndex)]);
                context->sites.erase(context->sites.begin() + context->selectedIndex);
                if (context->selectedIndex >= static_cast<int>(context->sites.size())) --context->selectedIndex;
                UpdateSiteList(window, context);
            }
            return TRUE;
        case IDC_CUSTOM_CONFIG:
            if (!StoreSelectedSite(window, context)) return TRUE;
            if (context->selectedIndex < 0 || context->selectedIndex >= static_cast<int>(context->sites.size()) ||
                context->sites[static_cast<size_t>(context->selectedIndex)].adapter != aiapi::SiteAdapter::Custom) {
                MessageBoxW(window, L"请先将“服务商”选为“自定义接口”。", L"自定义接口", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            {
                CustomDialogContext customContext{ &context->sites[static_cast<size_t>(context->selectedIndex)] };
                const HMODULE module = ModuleHandle();
                if (module) {
                    DialogBoxParamW(module, MAKEINTRESOURCEW(IDD_CUSTOM_CONFIG), window,
                        CustomConfigDialogProc, reinterpret_cast<LPARAM>(&customContext));
                }
            }
            return TRUE;
        case IDC_REFRESH_NOW: {
            // 先保存当前编辑内容（含刷新间隔）再立刻唤醒后台线程，便于改完 Key 后马上验证。
            if (!StoreSelectedSite(window, context)) {
                MessageBoxW(window, L"站点字段过长，未保存。", L"多站点 AI 余额", MB_OK | MB_ICONERROR);
                return TRUE;
            }
            std::wstring validationError;
            if (!ValidateProfiles(context->sites, &validationError)) {
                MessageBoxW(window, validationError.c_str(), L"站点配置未保存", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            int refreshSeconds = kDefaultRefreshSeconds;
            std::wstring refreshError;
            if (!ReadRefreshInterval(window, refreshSeconds, refreshError)) {
                MessageBoxW(window, refreshError.c_str(), L"刷新间隔无效", MB_OK | MB_ICONINFORMATION);
                SetFocus(GetDlgItem(window, IDC_REFRESH_EDIT));
                return TRUE;
            }
            if (!context->plugin->SetConfig(context->sites, refreshSeconds)) {
                MessageBoxW(window, L"无法加密并保存站点配置。", L"多站点 AI 余额", MB_OK | MB_ICONERROR);
                return TRUE;
            }
            context->changed = true;
            context->plugin->RequestRefresh();
            SetDlgItemTextW(window, IDC_STATUS, L"配置已保存并请求立即刷新；关闭对话框后即可看到最新余额。");
            return TRUE;
        }
        case IDOK: {
            if (!StoreSelectedSite(window, context)) {
                MessageBoxW(window, L"站点字段过长，未保存。", L"多站点 AI 余额", MB_OK | MB_ICONERROR);
                return TRUE;
            }
            std::wstring validationError;
            if (!ValidateProfiles(context->sites, &validationError)) {
                MessageBoxW(window, validationError.c_str(), L"站点配置未保存", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            int refreshSeconds = kDefaultRefreshSeconds;
            std::wstring refreshError;
            if (!ReadRefreshInterval(window, refreshSeconds, refreshError)) {
                MessageBoxW(window, refreshError.c_str(), L"刷新间隔无效", MB_OK | MB_ICONINFORMATION);
                SetFocus(GetDlgItem(window, IDC_REFRESH_EDIT));
                return TRUE;
            }
            if (!context->plugin->SetConfig(context->sites, refreshSeconds)) {
                MessageBoxW(window, L"无法加密并保存站点配置。", L"多站点 AI 余额", MB_OK | MB_ICONERROR);
                return TRUE;
            }
            context->changed = true;
            EndDialog(window, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(window, IDCANCEL);
            return TRUE;
        default:
            return FALSE;
        }
    }

    static HMODULE ModuleHandle() {
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&TMPluginGetInstance), &module);
        return module;
    }

    ConfigCopy GetConfig() const {
        std::lock_guard<std::mutex> lock(configMutex_);
        ConfigCopy copy;
        copy.sites = config_.sites;
        copy.generation = config_.generation;
        copy.refreshSeconds = config_.refreshSeconds;
        return copy;
    }

    void Publish(std::shared_ptr<const Snapshot> snapshot) {
        std::atomic_store(&snapshot_, std::move(snapshot));
    }

    static std::wstring MakeItemId(unsigned int id) {
        return L"aisite" + std::to_wstring(id);
    }

    void PublishLoadingFor(const std::vector<aiapi::SiteProfile>& sites) {
        auto snapshot = std::make_shared<Snapshot>();
        std::vector<const aiapi::SiteProfile*> enabledSites;
        for (const auto& profile : sites) if (profile.enabled) enabledSites.push_back(&profile);
        if (enabledSites.empty()) {
            snapshot->tooltip = sites.empty()
                ? L"尚未添加站点。打开插件选项添加站点。"
                : L"没有启用的站点。勾选“启用查询”后才会查询并显示余额。";
        } else {
            snapshot->tooltip = L"正在读取站点余额…";
            for (const auto* profile : enabledSites) {
                SiteValue value;
                value.id = profile->id;
                value.adapter = profile->adapter;
                const aiapi::ProviderPreset* preset = aiapi::FindProviderPreset(profile->adapter);
                const bool packageProvider = preset &&
                    (preset->category == aiapi::ProviderCategory::OfficialPlan ||
                        preset->category == aiapi::ProviderCategory::OfficialSubscription);
                value.label = packageProvider ? L"套餐" : L"余额";
                value.accountBalance = !packageProvider;
                value.itemName = profile->name + L" " + value.label;
                value.itemId = MakeItemId(profile->id);
                snapshot->sites.push_back(std::move(value));
            }
        }
        Publish(std::move(snapshot));
    }

    void PublishLoading() {
        ConfigCopy config = GetConfig();
        PublishLoadingFor(config.sites);
    }

    void PublishFailure(const std::wstring& message) {
        auto previous = GetSnapshot();
        auto snapshot = std::make_shared<Snapshot>();
        if (previous) *snapshot = *previous;
        if (!snapshot->tooltip.empty()) snapshot->tooltip += L"\r\n\r\n";
        snapshot->tooltip += L"刷新失败：" + message + L"\r\n仍显示各站点上次成功值。";
        Publish(std::move(snapshot));
    }

    void PublishBalances(const std::vector<aiapi::SiteBalance>& results) {        auto previous = GetSnapshot();
        auto snapshot = std::make_shared<Snapshot>();
        std::wstring tip;
        bool anySuccess = false;
        bool anyFailure = false;
        bool anySkipped = false;
        for (const auto& result : results) {
            SiteValue value;
            value.id = result.id;
            value.adapter = result.adapter;
            value.accountBalance = result.accountBalance;
            value.packageSummary = result.packageSummary;
            const bool isPackage = !result.packageSummary.empty();
            const aiapi::ProviderPreset* preset = aiapi::FindProviderPreset(result.adapter);
            const bool packageProvider = preset &&
                (preset->category == aiapi::ProviderCategory::OfficialPlan ||
                    preset->category == aiapi::ProviderCategory::OfficialSubscription);
            // 显示格式统一：余额模式恒为「名称（余额）：金额」，套餐模式恒为
            // 「名称（套餐）：5H x% 7D x% 1M x%」，不再区分可用额/日限额等说法。
            const std::wstring category = (isPackage || packageProvider) ? L"套餐" : L"余额";
            value.label = category;
            value.extra = result.extra;
            value.itemName = result.name + L" " + category;
            value.itemId = MakeItemId(result.id);
            value.error = result.error;
            const SiteValue* oldValue = nullptr;
            if (previous) {
                for (const auto& oldSite : previous->sites) {
                    if (oldSite.id == result.id) { oldValue = &oldSite; break; }
                }
            }

            // skipped：这一轮还没到重试时间，沿用旧值且不当作错误。
            if (result.success) {
                value.success = true;
                if (!result.packageSummary.empty()) {
                    // 套餐类（Token Plan / 官方订阅）没有金额语义，显示百分比摘要。
                    // 统一成单空格分隔，避免不同服务商模板里的多空格造成排版差异。
                    std::wstring summary = result.packageSummary;
                    std::wstring collapsed;
                    collapsed.reserve(summary.size());
                    for (wchar_t ch : summary) {
                        if (ch == L' ' && !collapsed.empty() && collapsed.back() == L' ') continue;
                        collapsed.push_back(ch);
                    }
                    while (!collapsed.empty() && collapsed.back() == L' ') collapsed.pop_back();
                    value.value = collapsed;
                    value.fullValue = collapsed;
                } else {
                    value.value = FormatCompactMoney(result.remaining, result.unit);
                    value.fullValue = FormatMoney(result.remaining, result.unit);
                }
                anySuccess = true;
            } else if (result.skipped) {
                anySkipped = true;
                if (oldValue) {
                    value.success = oldValue->success;
                    value.accountBalance = oldValue->accountBalance;
                    value.packageSummary = oldValue->packageSummary;
                    value.label = oldValue->label;
                    value.extra = oldValue->extra;
                    value.itemName = oldValue->itemName;
                    value.value = oldValue->value;
                    value.fullValue = oldValue->fullValue;
                    if (value.success) anySuccess = true;
                }
            } else if (oldValue) {
                value.success = oldValue->success;
                value.accountBalance = oldValue->accountBalance;
                value.packageSummary = oldValue->packageSummary;
                value.label = oldValue->label;
                value.extra = oldValue->extra;
                value.itemName = oldValue->itemName;
                value.value = oldValue->value;
                value.fullValue = oldValue->fullValue;
                if (value.success) anySuccess = true;
            }
            if (!result.success && !result.skipped) anyFailure = true;

            // tooltip 每行固定为「名称（余额/套餐）：值」，失败行也使用同一模式。
            const std::wstring failCategory = packageProvider ? L"套餐" : L"余额";
            if (!tip.empty()) tip += L"\r\n";
            if (result.skipped) {
                // 退避中的站点：不显示错误，只说明数值是上次结果。
                if (oldValue && oldValue->success) {
                    const std::wstring oldCategory =
                        oldValue->packageSummary.empty() ? L"余额" : L"套餐";
                    tip += result.name + L"（" + oldCategory + L"）：" +
                        oldValue->fullValue + L"（稍后刷新）";
                } else {
                    tip += result.name + L"（" + failCategory + L"）：等待重试";
                }
            } else if (result.success) {
                tip += result.name + L"（" + category + L"）：" + value.fullValue;
            } else {
                tip += result.name + L"（" + failCategory + L"）：查询失败" +
                    (result.error.empty() ? L"" : L"：" + result.error);
                if (oldValue && oldValue->success) tip += L"（上次 " + oldValue->fullValue + L"）";
            }
            // 补充信息（Key 名称、今日花费/次数、重置时间等）不再拼进 tooltip：
            // 每行严格保持「名称（余额/套餐）：值」，余额与套餐两种模式的排版完全一致。
            snapshot->sites.push_back(std::move(value));
        }
        if (results.empty()) tip = L"没有启用的站点。请在插件选项中勾选“启用查询”。";
        else {
            if (!tip.empty()) tip += L"\r\n";
            tip += L"更新于 " + LocalTimestamp();
            if (!anySuccess && !anySkipped) tip += L"；当前没有站点查询成功";
            else if (anyFailure) tip += L"；失败站点已自动延长重试间隔";
        }
        snapshot->tooltip = std::move(tip);
        Publish(std::move(snapshot));
    }

    bool ResolveConfigPath(ITrafficMonitor* app) {
        std::wstring base;
        if (app && app->GetAPIVersion() >= 1) {
            const wchar_t* hostConfigDir = app->GetPluginConfigDir();
            if (hostConfigDir && *hostConfigDir) base = hostConfigDir;
        }
        if (base.empty()) {
            wchar_t appData[MAX_PATH]{};
            const DWORD length = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
            if (length == 0 || length >= MAX_PATH) return false;
            base = appData;
            base += L"\\TrafficMonitor\\Plugins";
        }
        try {
            const std::filesystem::path directory = std::filesystem::path(base) / L"AIAPIQuota";
            std::filesystem::create_directories(directory);
            configFile_ = directory / L"sites.bin";
            legacyConfigFile_ = directory / L"api-key.bin";
            return true;
        } catch (...) {
            return false;
        }
    }

    static void AppendU32(std::vector<BYTE>& output, unsigned int value) {
        for (unsigned int shift = 0; shift < 32; shift += 8) output.push_back(static_cast<BYTE>((value >> shift) & 0xff));
    }

    static bool AppendString(std::vector<BYTE>& output, const std::wstring& value) {
        std::string utf8 = WideToUtf8(value);
        if ((!value.empty() && utf8.empty()) || utf8.size() > 65536) {
            if (!utf8.empty()) SecureZeroMemory(utf8.data(), utf8.size());
            return false;
        }
        AppendU32(output, static_cast<unsigned int>(utf8.size()));
        output.insert(output.end(), utf8.begin(), utf8.end());
        if (!utf8.empty()) SecureZeroMemory(utf8.data(), utf8.size());
        return true;
    }

    static bool ReadU32(const BYTE* data, size_t size, size_t& offset, unsigned int& value) {
        if (size - offset < 4) return false;
        value = static_cast<unsigned int>(data[offset]) |
            (static_cast<unsigned int>(data[offset + 1]) << 8) |
            (static_cast<unsigned int>(data[offset + 2]) << 16) |
            (static_cast<unsigned int>(data[offset + 3]) << 24);
        offset += 4;
        return true;
    }

    static bool ReadString(const BYTE* data, size_t size, size_t& offset, std::wstring& value) {
        unsigned int length = 0;
        if (!ReadU32(data, size, offset, length) || length > 65536 || size - offset < length) return false;
        std::string utf8(reinterpret_cast<const char*>(data + offset), length);
        offset += length;
        value = Utf8ToWide(utf8);
        const bool okay = utf8.empty() || !value.empty();
        if (!utf8.empty()) SecureZeroMemory(utf8.data(), utf8.size());
        return okay;
    }

    static bool DecodeProfiles(const BYTE* data, size_t size, std::vector<aiapi::SiteProfile>& profiles,
        int& refreshSeconds) {
        size_t offset = 0;
        unsigned int magic = 0, version = 0, count = 0;
        if (!ReadU32(data, size, offset, magic) || magic != 0x51414941 ||
            !ReadU32(data, size, offset, version) || (version != 1 && version != 2 && version != 3)) {
            return false;
        }
        // v3 起头部顺序为 magic / version / refreshSeconds / count，
        // 与 SaveConfig 的写入顺序严格对应，不要调换。
        refreshSeconds = kDefaultRefreshSeconds;
        if (version >= 3) {
            unsigned int seconds = 0;
            if (!ReadU32(data, size, offset, seconds)) return false;
            refreshSeconds = ClampRefreshSeconds(static_cast<int>(seconds));
        }
        if (!ReadU32(data, size, offset, count) || count > kMaxSites) return false;
        std::vector<aiapi::SiteProfile> decoded;
        decoded.reserve(count);
        for (unsigned int i = 0; i < count; ++i) {
            unsigned int adapter = 0, enabled = 1;
            aiapi::SiteProfile site;
            if (!ReadU32(data, size, offset, site.id) || !ReadU32(data, size, offset, adapter) ||
                (version >= 2 && (!ReadU32(data, size, offset, enabled) || enabled > 1)) ||
                !ReadString(data, size, offset, site.name) || !ReadString(data, size, offset, site.baseUrl) ||
                !ReadString(data, size, offset, site.credential) || !ReadString(data, size, offset, site.userId) ||
                (version >= 2 && !ReadString(data, size, offset, site.customScript))) {
                ClearProfiles(decoded);
                return false;
            }
            site.adapter = static_cast<aiapi::SiteAdapter>(adapter);
            site.enabled = enabled != 0;
            decoded.push_back(std::move(site));
        }
        if (offset != size || !ValidateProfiles(decoded)) {
            ClearProfiles(decoded);
            return false;
        }
        profiles = std::move(decoded);
        return true;
    }

    bool ReadEncryptedFile(const std::filesystem::path& path, std::vector<BYTE>& encrypted, bool& exists) const {
        exists = false;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
        exists = true;
        LARGE_INTEGER size{};
        const bool sizeOkay = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= 1024 * 1024;
        if (!sizeOkay) { CloseHandle(file); return false; }
        encrypted.resize(static_cast<size_t>(size.QuadPart));
        DWORD bytesRead = 0;
        const bool readOkay = ReadFile(file, encrypted.data(), static_cast<DWORD>(encrypted.size()), &bytesRead, nullptr) &&
            bytesRead == encrypted.size();
        CloseHandle(file);
        return readOkay;
    }

    bool LoadConfig(std::vector<aiapi::SiteProfile>& profiles, int& refreshSeconds) {
        std::vector<BYTE> encrypted;
        bool exists = false;
        if (!ReadEncryptedFile(configFile_, encrypted, exists)) return false;
        if (exists) {
            DATA_BLOB input{ static_cast<DWORD>(encrypted.size()), encrypted.data() };
            DATA_BLOB output{};
            const bool decrypted = CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                CRYPTPROTECT_UI_FORBIDDEN, &output) != FALSE;
            SecureZeroMemory(encrypted.data(), encrypted.size());
            if (!decrypted) return false;
            const bool valid = DecodeProfiles(output.pbData, output.cbData, profiles, refreshSeconds);
            SecureZeroMemory(output.pbData, output.cbData);
            LocalFree(output.pbData);
            return valid;
        }

        bool legacyExists = false;
        std::vector<BYTE> legacy;
        refreshSeconds = kDefaultRefreshSeconds;
        if (!ReadEncryptedFile(legacyConfigFile_, legacy, legacyExists)) return false;
        if (!legacyExists) return true;
        DATA_BLOB input{ static_cast<DWORD>(legacy.size()), legacy.data() };
        DATA_BLOB output{};
        const bool decrypted = CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
            CRYPTPROTECT_UI_FORBIDDEN, &output) != FALSE;
        SecureZeroMemory(legacy.data(), legacy.size());
        if (!decrypted) return false;
        std::string utf8(reinterpret_cast<const char*>(output.pbData), output.cbData);
        std::wstring key = Utf8ToWide(utf8);
        SecureZeroMemory(utf8.data(), utf8.size());
        SecureZeroMemory(output.pbData, output.cbData);
        LocalFree(output.pbData);
        if (!IsValidCredential(key)) {
            if (!key.empty()) SecureZeroMemory(key.data(), key.size() * sizeof(wchar_t));
            return false;
        }
        aiapi::SiteProfile site;
        site.id = NewSiteId();
        site.adapter = aiapi::SiteAdapter::AIAPI;
        site.name = L"AIAPI";
        site.baseUrl = L"{AIAPI 站点}";
        site.credential = std::move(key);
        profiles.push_back(std::move(site));
        if (!SaveConfig(profiles, refreshSeconds)) { ClearProfiles(profiles); return false; }
        DeleteFileW(legacyConfigFile_.c_str());
        return true;
    }

    bool SaveConfig(const std::vector<aiapi::SiteProfile>& profiles, int refreshSeconds) {
        if (configFile_.empty() || profiles.size() > kMaxSites) return false;
        std::vector<BYTE> plain;
        AppendU32(plain, 0x51414941);
        // v3 在头部多一个刷新间隔字段；v1/v2 仍可读，方便回退旧版本。
        AppendU32(plain, 3);
        AppendU32(plain, static_cast<unsigned int>(ClampRefreshSeconds(refreshSeconds)));
        AppendU32(plain, static_cast<unsigned int>(profiles.size()));
        for (const auto& site : profiles) {
            AppendU32(plain, site.id);
            AppendU32(plain, static_cast<unsigned int>(site.adapter));
            AppendU32(plain, site.enabled ? 1u : 0u);
            if (!AppendString(plain, site.name) || !AppendString(plain, site.baseUrl) ||
                !AppendString(plain, site.credential) || !AppendString(plain, site.userId) ||
                !AppendString(plain, site.customScript)) {
                SecureZeroMemory(plain.data(), plain.size());
                return false;
            }
        }
        DATA_BLOB input{ static_cast<DWORD>(plain.size()), plain.data() };
        DATA_BLOB encrypted{};
        const bool protectedData = CryptProtectData(&input, L"TrafficMonitor site credentials", nullptr,
            nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &encrypted) != FALSE;
        SecureZeroMemory(plain.data(), plain.size());
        if (!protectedData) return false;

        bool written = false;
        try {
            std::filesystem::create_directories(configFile_.parent_path());
            std::filesystem::path temp = configFile_;
            temp += L".tmp";
            HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                DWORD bytesWritten = 0;
                written = WriteFile(file, encrypted.pbData, encrypted.cbData, &bytesWritten, nullptr) &&
                    bytesWritten == encrypted.cbData && FlushFileBuffers(file);
                CloseHandle(file);
                if (written) written = MoveFileExW(temp.c_str(), configFile_.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
                if (!written) DeleteFileW(temp.c_str());
            }
        } catch (...) {
            written = false;
        }
        SecureZeroMemory(encrypted.pbData, encrypted.cbData);
        LocalFree(encrypted.pbData);
        return written;
    }

    // The host API has no unload callback, so this worker and singleton live until process exit.
    void EnsureWorker() {
        if (!PinPluginModule()) {
            PublishFailure(L"无法保持插件模块有效，未启动后台刷新。");
            return;
        }
        bool expected = false;
        if (!workerStarted_.compare_exchange_strong(expected, true)) return;
        try {
            std::thread([this]() { WorkerLoop(); }).detach();
        } catch (...) {
            workerStarted_.store(false);
            PublishFailure(L"无法启动后台刷新线程。");
        }
    }

    void RequestRefresh() {
        {
            std::lock_guard<std::mutex> lock(wakeMutex_);
            refreshRequested_ = true;
        }
        wakeCondition_.notify_one();
    }

    // 等到 deadline 或收到手动刷新。返回后由调用方重新计算配置与到期站点。
    void WaitUntil(std::chrono::steady_clock::time_point deadline) {
        std::unique_lock<std::mutex> lock(wakeMutex_);
        if (refreshRequested_) {
            refreshRequested_ = false;
            return;
        }
        wakeCondition_.wait_until(lock, deadline, [this]() { return refreshRequested_; });
        refreshRequested_ = false;
    }

    // 并行查询指定站点。返回与 sites 等长的结果数组，未查询的位置保持默认。
    std::vector<aiapi::SiteBalance> QuerySitesParallel(
        const std::vector<aiapi::SiteProfile>& sites, const std::vector<size_t>& indices) {
        std::vector<aiapi::SiteBalance> results(indices.size());
        if (indices.empty()) return results;

        // 每个线程各自创建自己的 WinHTTP 会话与 Chakra 运行时，
        // 因此并发是安全的；上限压住，避免一次打出去太多请求。
        std::atomic<size_t> next{ 0 };
        auto worker = [&]() {
            for (;;) {
                const size_t slot = next.fetch_add(1);
                if (slot >= indices.size()) break;
                const auto& profile = sites[indices[slot]];
                try {
                    results[slot] = aiapi::FetchSiteBalance(profile);
                } catch (...) {
                    results[slot].id = profile.id;
                    results[slot].adapter = profile.adapter;
                    results[slot].name = profile.name;
                    results[slot].error = L"查询时发生错误。";
                }
            }
        };

        const size_t threadCount = std::min(indices.size(), kMaxParallelQueries);
        std::vector<std::thread> pool;
        pool.reserve(threadCount > 0 ? threadCount - 1 : 0);
        try {
            for (size_t i = 1; i < threadCount; ++i) pool.emplace_back(worker);
        } catch (...) {
            // 线程创建失败时退化成串行：worker 自己也干活，不影响正确性。
        }
        worker();
        for (auto& thread : pool) {
            if (thread.joinable()) thread.join();
        }
        return results;
    }

    // 失败退避：连续失败 n 次后按 interval * 2^(n-1) 推迟下一次，上限 15 分钟。
    std::chrono::seconds BackoffDelay(int consecutiveFailures, int intervalSeconds) const {
        const int shift = std::clamp(consecutiveFailures - 1, 0, 4);
        const auto delay = std::chrono::seconds(intervalSeconds) * (1 << shift);
        return std::min<std::chrono::seconds>(delay, kMaxBackoff);
    }

    void WorkerLoop() {
        for (;;) {
            ConfigCopy config = GetConfig();
            std::vector<aiapi::SiteProfile> enabledSites;
            for (const auto& site : config.sites) if (site.enabled) enabledSites.push_back(site);
            if (enabledSites.empty()) {
                ClearProfiles(config.sites);
                WaitUntil(std::chrono::steady_clock::now() +
                    std::chrono::seconds(config.refreshSeconds));
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            std::vector<size_t> dueIndices;
            {
                std::lock_guard<std::mutex> lock(backoffMutex_);
                // 清掉已删除站点的退避记录，避免 map 无限增长。
                for (auto iter = backoff_.begin(); iter != backoff_.end();) {
                    const bool stillExists = std::any_of(enabledSites.begin(), enabledSites.end(),
                        [&](const aiapi::SiteProfile& site) { return site.id == iter->first; });
                    iter = stillExists ? std::next(iter) : backoff_.erase(iter);
                }
                for (size_t i = 0; i < enabledSites.size(); ++i) {
                    const auto iter = backoff_.find(enabledSites[i].id);
                    if (iter == backoff_.end() || iter->second.nextAttempt <= now) dueIndices.push_back(i);
                }
            }

            if (!dueIndices.empty()) {
                const auto results = QuerySitesParallel(enabledSites, dueIndices);

                // 更新退避状态
                {
                    std::lock_guard<std::mutex> lock(backoffMutex_);
                    for (size_t slot = 0; slot < dueIndices.size(); ++slot) {
                        const unsigned int id = enabledSites[dueIndices[slot]].id;
                        if (results[slot].success) {
                            backoff_.erase(id);
                            continue;
                        }
                        auto& state = backoff_[id];
                        ++state.consecutiveFailures;
                        state.nextAttempt = std::chrono::steady_clock::now() +
                            BackoffDelay(state.consecutiveFailures, config.refreshSeconds);
                    }
                }

                // 组成本轮完整快照：到期站点用新结果，未到期站点标记 skipped 沿用旧值。
                std::vector<aiapi::SiteBalance> report(enabledSites.size());
                for (size_t i = 0; i < enabledSites.size(); ++i) {
                    report[i].id = enabledSites[i].id;
                    report[i].adapter = enabledSites[i].adapter;
                    report[i].name = enabledSites[i].name;
                    report[i].skipped = true;
                }
                for (size_t slot = 0; slot < dueIndices.size(); ++slot) {
                    report[dueIndices[slot]] = results[slot];
                }

                try {
                    std::lock_guard<std::mutex> lock(configMutex_);
                    if (config_.generation == config.generation) PublishBalances(report);
                } catch (...) {
                    std::lock_guard<std::mutex> lock(configMutex_);
                    if (config_.generation == config.generation) PublishFailure(L"后台站点刷新发生错误。");
                }
            }

            // 下一次醒来的时间：所有启用站点里最早到期的那个。
            // 注意要在 ClearProfiles(enabledSites) 之前算，否则向量已经被清空。
            auto deadline = now + std::chrono::seconds(config.refreshSeconds);
            {
                std::lock_guard<std::mutex> lock(backoffMutex_);
                for (const auto& site : enabledSites) {
                    const auto iter = backoff_.find(site.id);
                    if (iter != backoff_.end() && iter->second.nextAttempt < deadline) {
                        deadline = iter->second.nextAttempt;
                    }
                }
            }

            ClearProfiles(enabledSites);
            ClearProfiles(config.sites);

            const auto earliest = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            if (deadline < earliest) deadline = earliest;
            WaitUntil(deadline);
        }
    }

    mutable std::mutex configMutex_;
    Config config_;
    std::filesystem::path configFile_;
    std::filesystem::path legacyConfigFile_;
    std::mutex initMutex_;
    bool initialized_ = false;
    std::atomic<bool> workerStarted_{ false };
    std::mutex wakeMutex_;
    std::condition_variable wakeCondition_;
    bool refreshRequested_ = false;
    std::mutex backoffMutex_;
    std::map<unsigned int, RetryState> backoff_;
    std::shared_ptr<const Snapshot> snapshot_;
    PluginItem items_[8];
};

const wchar_t* PluginItem::GetItemName() const {
    thread_local std::shared_ptr<const Snapshot> held;
    held = owner_->GetSnapshot();
    if (!held || itemIndex_ < 0 || itemIndex_ >= static_cast<int>(held->sites.size())) return L"站点余额";
    return held->sites[static_cast<size_t>(itemIndex_)].itemName.c_str();
}

const wchar_t* PluginItem::GetItemId() const {
    thread_local std::shared_ptr<const Snapshot> held;
    held = owner_->GetSnapshot();
    if (!held || itemIndex_ < 0 || itemIndex_ >= static_cast<int>(held->sites.size())) return L"aisite0";
    return held->sites[static_cast<size_t>(itemIndex_)].itemId.c_str();
}

const wchar_t* PluginItem::GetItemLableText() const {
    thread_local std::shared_ptr<const Snapshot> held;
    held = owner_->GetSnapshot();
    if (!held || itemIndex_ < 0 || itemIndex_ >= static_cast<int>(held->sites.size())) return L"余额";
    const auto& site = held->sites[static_cast<size_t>(itemIndex_)];
    return site.packageSummary.empty() ? L"余额" : L"套餐";
}

const wchar_t* PluginItem::GetItemValueText() const {
    thread_local std::shared_ptr<const Snapshot> held[8];
    if (itemIndex_ < 0 || itemIndex_ >= static_cast<int>(std::size(held))) return L"--";
    const size_t index = static_cast<size_t>(itemIndex_);
    held[index] = owner_->GetSnapshot();
    if (!held[index] || index >= held[index]->sites.size()) return L"--";
    return held[index]->sites[index].value.c_str();
}

const wchar_t* PluginItem::GetItemValueSampleText() const {
    // 宿主用这串字符的宽度决定显示区域大小。金额类给货币前缀 + 两位小数 + K/M；
    // 套餐类（packageSummary 含百分比）给常见的窗口摘要形态。
    thread_local std::shared_ptr<const Snapshot> held;
    held = owner_->GetSnapshot();
    if (held && itemIndex_ >= 0 && itemIndex_ < static_cast<int>(held->sites.size()) &&
        !held->sites[static_cast<size_t>(itemIndex_)].packageSummary.empty()) {
        return L"5H 99%  7D 99%";
    }
    return L"USD 999.99M";
}

bool PinPluginModule() {
    // The worker is detached and the plugin ABI has no unload notification.
    static const bool pinned = []() {
        HMODULE module = nullptr;
        return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&TMPluginGetInstance), &module) != FALSE;
    }();
    return pinned;
}

} // namespace

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance() {
    PinPluginModule();
    static Plugin* plugin = new Plugin();
    return plugin;
}
