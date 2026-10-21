// 官方订阅额度适配：Claude / OpenAI Codex。
//
// 移植自 CC-Switch src-tauri/src/services/subscription.rs 中 get_subscription_quota
// 的 "claude" / "codex" 两条分支（第一层：只读取本机 CLI 已登录的 OAuth 凭据，
// 不实现登录与 token 刷新）。Windows 没有 macOS Keychain，因此只读凭据文件：
//   Claude : %USERPROFILE%\.claude\.credentials.json
//   Codex  : %USERPROFILE%\.codex\auth.json
//
// Rust 侧几个关键约定也一并移植：
//   * Claude 访问令牌过期但刷新令牌仍有效时为 RefreshPending（不算登录过期）；
//     无论 Expired 还是 RefreshPending 都先用旧 accessToken 试一次接口。
//   * Codex 仅 auth_mode == "chatgpt" 的 OAuth 登录才有用量数据；last_refresh
//     超过 8 天视为可能过期，同样先试一次接口。
//   * 用量窗口统一用「已用百分比」表示，展示时换算成剩余百分比。
//
// 输出：packageSummary 是各窗口剩余百分比摘要（如 L"5H 62%  7D 41%"），
//       extra 放重置时间、套餐名、超额用量、Credits 余额等，scopeLabel = L"套餐"，
//       accountBalance = false。失败返回 false 并填写可操作的 error。
//
// profile.baseUrl 在官方预设里是隐藏字段；本实现允许非空值时覆盖请求端点，
// 仅用于本地 mock 测试（官方 UI 不会填它）。

#include "OfficialSubscription.h"

#include "QuerySupport.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace aiapi {
namespace {

using query::HttpRequest;
using query::HttpResponse;
using query::JsonValue;
using query::Member;
using query::ParseDocument;
using query::ReadInt;
using query::ReadNumber;
using query::ReadString;
using query::SendHttp;
using query::WidenUtf8;

constexpr wchar_t kClaudeUsageEndpoint[] = L"https://api.anthropic.com/api/oauth/usage";
constexpr wchar_t kCodexUsageEndpoint[] = L"https://chatgpt.com/backend-api/wham/usage";
constexpr wchar_t kClaudeCredentialsRelative[] = L".claude\\.credentials.json";
constexpr wchar_t kCodexCredentialsRelative[] = L".codex\\auth.json";
constexpr DWORD kMaxCredentialFileBytes = 256 * 1024;
constexpr long long kCodexStaleSeconds = 8LL * 24 * 60 * 60;
constexpr int kRequestTimeoutMs = 15000;

// Claude 用量接口已知窗口，顺序即展示顺序（与 Rust KNOWN_TIERS 一致）。
constexpr const char* kClaudeKnownTiers[] = {
    "five_hour",
    "seven_day",
    "seven_day_fable",
    "seven_day_opus",
    "seven_day_sonnet",
};

// ── 通用工具 ─────────────────────────────────────────────

std::string TrimAscii(std::string text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) --end;
    return text.substr(begin, end - begin);
}

std::wstring TrimWide(std::wstring text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::iswspace(static_cast<wint_t>(text[begin])) != 0) ++begin;
    while (end > begin && std::iswspace(static_cast<wint_t>(text[end - 1])) != 0) --end;
    return text.substr(begin, end - begin);
}

std::wstring ReadEnvironment(const wchar_t* name) {
    DWORD capacity = GetEnvironmentVariableW(name, nullptr, 0);
    if (capacity == 0) return {};
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::wstring value(capacity, L'\0');
        const DWORD written = GetEnvironmentVariableW(name, value.data(), capacity);
        if (written == 0) {
            if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) return {};
            capacity = GetEnvironmentVariableW(name, nullptr, 0);
            if (capacity == 0) return {};
            continue;
        }
        if (written < capacity) {
            value.resize(written);
            return value;
        }
        capacity = written + 1;
    }
    return {};
}

// 凭据路径基于 USERPROFILE；这样测试时改环境变量就能指向假 profile。
std::wstring UserProfileDirectory() {
    const std::wstring profile = TrimWide(ReadEnvironment(L"USERPROFILE"));
    if (!profile.empty()) return profile;
    const std::wstring drive = TrimWide(ReadEnvironment(L"HOMEDRIVE"));
    const std::wstring path = TrimWide(ReadEnvironment(L"HOMEPATH"));
    if (!drive.empty() && !path.empty()) return drive + path;
    return {};
}

std::wstring AppendPath(const std::wstring& directory, const wchar_t* relative) {
    std::wstring result = directory;
    if (result.empty()) return result;
    if (result.back() != L'\\' && result.back() != L'/') result.push_back(L'\\');
    result += relative;
    return result;
}

std::wstring FormatWin32Error(DWORD code) {
    LPWSTR buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring message;
    if (length > 0 && buffer != nullptr) message.assign(buffer, length);
    if (buffer != nullptr) LocalFree(buffer);
    message = TrimWide(std::move(message));
    if (message.empty()) message = L"Windows 错误 " + std::to_wstring(code);
    return message;
}

class FileHandle {
public:
    explicit FileHandle(HANDLE handle = INVALID_HANDLE_VALUE) : handle_(handle) {}
    ~FileHandle() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    HANDLE get() const { return handle_; }

private:
    HANDLE handle_;
};

// 读取 UTF-8 文本文件。missing=true 表示路径不存在（对应 Rust 的 NotFound）。
bool ReadCredentialFile(const std::wstring& path, std::string& content, bool& missing,
    std::wstring& error) {
    content.clear();
    missing = false;
    error.clear();

    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ||
            code == ERROR_INVALID_NAME || code == ERROR_BAD_PATHNAME) {
            missing = true;
            return false;
        }
        error = FormatWin32Error(code);
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        missing = true;
        return false;
    }

    FileHandle handle(CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            missing = true;
            return false;
        }
        error = FormatWin32Error(code);
        return false;
    }

    char buffer[4096];
    for (;;) {
        DWORD bytesRead = 0;
        if (ReadFile(handle.get(), buffer, static_cast<DWORD>(sizeof(buffer)), &bytesRead, nullptr) == FALSE) {
            error = FormatWin32Error(GetLastError());
            return false;
        }
        if (bytesRead == 0) break;
        if (content.size() + bytesRead > kMaxCredentialFileBytes) {
            error = L"凭据文件超过大小限制";
            return false;
        }
        content.append(buffer, bytesRead);
    }
    return true;
}

bool ParseFixedDigits(const std::string& text, size_t& position, size_t count, int& value) {
    if (position + count > text.size()) return false;
    int result = 0;
    for (size_t index = 0; index < count; ++index) {
        const char digit = text[position + index];
        if (digit < '0' || digit > '9') return false;
        result = result * 10 + (digit - '0');
    }
    position += count;
    value = result;
    return true;
}

// Howard Hinnant 的 days_from_civil：把公历年月日换成自 1970-01-01 起的天数。
long long DaysFromCivil(long long year, unsigned int month, unsigned int day) {
    year -= month <= 2 ? 1 : 0;
    const long long era = (year >= 0 ? year : year - 399) / 400;
    const unsigned int yearOfEra = static_cast<unsigned int>(year - era * 400);
    const unsigned int dayOfYear =
        (153U * (month + (month > 2 ? static_cast<unsigned int>(-3) : 9U)) + 2U) / 5U + day - 1U;
    const unsigned int dayOfEra =
        yearOfEra * 365U + yearOfEra / 4U - yearOfEra / 100U + dayOfYear;
    return era * 146097LL + static_cast<long long>(dayOfEra) - 719468LL;
}

// 解析 ISO 8601 / RFC 3339 时间（Rust is_token_expired 使用 chrono 的两种格式）。
// 支持 "YYYY-MM-DDTHH:MM:SS[.fraction]"、'T'/'t'/空格分隔、Z 或 ±HH:MM 时区、
// 以及没有时区的 naive 形式。解析失败返回 false（Rust 同样「解析不了就不算过期」）。
bool ParseIsoTimestamp(std::string raw, long long& unixSeconds) {
    std::string text = TrimAscii(std::move(raw));
    if (text.empty()) return false;

    long long offsetSeconds = 0;
    if (text.back() == 'Z' || text.back() == 'z') {
        text.pop_back();
    } else {
        size_t offsetPosition = text.find('+');
        if (offsetPosition == std::string::npos) {
            const size_t minus = text.find_last_of('-');
            // 日期部分的 '-' 都在前 10 个字符内；更靠后的 '-' 才是时区偏移。
            if (minus != std::string::npos && minus >= 10) offsetPosition = minus;
        }
        if (offsetPosition != std::string::npos) {
            size_t position = offsetPosition + 1;
            int hours = 0;
            int minutes = 0;
            if (!ParseFixedDigits(text, position, 2, hours)) return false;
            if (position < text.size() && text[position] == ':') ++position;
            if (!ParseFixedDigits(text, position, 2, minutes)) return false;
            if (position != text.size()) return false;
            if (hours > 23 || minutes > 59) return false;
            offsetSeconds = static_cast<long long>(hours) * 3600 + static_cast<long long>(minutes) * 60;
            if (text[offsetPosition] == '-') offsetSeconds = -offsetSeconds;
            text.resize(offsetPosition);
        }
    }

    size_t position = 0;
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!ParseFixedDigits(text, position, 4, year)) return false;
    if (position >= text.size() || text[position] != '-') return false;
    ++position;
    if (!ParseFixedDigits(text, position, 2, month)) return false;
    if (position >= text.size() || text[position] != '-') return false;
    ++position;
    if (!ParseFixedDigits(text, position, 2, day)) return false;
    if (position >= text.size()) return false;
    const char separator = text[position];
    if (separator != 'T' && separator != 't' && separator != ' ') return false;
    ++position;
    if (!ParseFixedDigits(text, position, 2, hour)) return false;
    if (position >= text.size() || text[position] != ':') return false;
    ++position;
    if (!ParseFixedDigits(text, position, 2, minute)) return false;
    if (position >= text.size() || text[position] != ':') return false;
    ++position;
    if (!ParseFixedDigits(text, position, 2, second)) return false;
    if (position < text.size() && (text[position] == '.' || text[position] == ',')) {
        ++position;
        const size_t fractionStart = position;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9') ++position;
        if (position == fractionStart) return false;
    }
    if (position != text.size()) return false;
    if (year < 1 || year > 9999 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || minute > 59 || second > 59) {
        return false;
    }

    const long long days = DaysFromCivil(static_cast<long long>(year),
        static_cast<unsigned int>(month), static_cast<unsigned int>(day));
    unixSeconds = days * 86400LL + static_cast<long long>(hour) * 3600 +
        static_cast<long long>(minute) * 60 + second - offsetSeconds;
    return true;
}

// 兼容 Unix 时间戳（秒/毫秒）与 ISO 字符串，移植 is_token_expired 的类型判断。
bool ParseTimestampValue(const JsonValue* value, long long& unixSeconds) {
    if (value == nullptr) return false;
    if (value->kind == JsonValue::Kind::Number) {
        double seconds = 0.0;
        if (!value->Number(seconds) || !std::isfinite(seconds) || seconds <= 0.0) return false;
        // 毫秒级时间戳大于 1e12，与 Rust 的区分方式一致。
        if (seconds > 1.0e12) seconds /= 1000.0;
        unixSeconds = static_cast<long long>(seconds);
        return unixSeconds > 0;
    }
    std::string text;
    if (!ReadString(value, text)) return false;
    return ParseIsoTimestamp(text, unixSeconds);
}

bool IsTimestampExpired(const JsonValue* value, long long nowSeconds) {
    long long timestamp = 0;
    if (!ParseTimestampValue(value, timestamp)) return false;
    return timestamp < nowSeconds;
}

long long CurrentUnixSeconds() {
    const std::time_t now = std::time(nullptr);
    return now < 0 ? 0 : static_cast<long long>(now);
}

std::wstring FormatLocalDateTime(long long unixSeconds) {
    if (unixSeconds <= 0) return {};
    const std::time_t seconds = static_cast<std::time_t>(unixSeconds);
    std::tm local{};
    if (localtime_s(&local, &seconds) != 0) return {};
    wchar_t buffer[32]{};
    if (swprintf_s(buffer, std::size(buffer), L"%02d-%02d %02d:%02d",
        local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min) < 0) {
        return {};
    }
    return buffer;
}

double Clamp(double value, double low, double high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

std::wstring FormatPercent(double value) {
    const long percent = std::lround(Clamp(value, 0.0, 100.0));
    return std::to_wstring(percent) + L"%";
}

// 金额/数量：最多两位小数，去掉无意义的尾零（62500 -> "62500"，12.50 -> "12.5"）。
std::wstring FormatAmount(double value) {
    if (!std::isfinite(value)) return L"--";
    wchar_t buffer[64]{};
    if (swprintf_s(buffer, std::size(buffer), L"%.2f", value) < 0) return L"--";
    std::wstring text(buffer);
    while (text.size() > 1 && text.back() == L'0') text.pop_back();
    if (!text.empty() && text.back() == L'.') text.pop_back();
    return text;
}

void AppendPiece(std::wstring& extra, const std::wstring& piece) {
    if (piece.empty()) return;
    if (!extra.empty()) extra += L"；";
    extra += piece;
}

// 旧接头的端口覆盖（仅用于本地 mock 测试）。官方预设 UI 不会填写 baseUrl。
std::wstring ResolveEndpoint(const SiteProfile& profile, const wchar_t* defaultEndpoint) {
    const std::wstring override = TrimWide(profile.baseUrl);
    return override.empty() ? std::wstring(defaultEndpoint) : override;
}

// 只有本机回环地址才允许明文 HTTP（SendHttp 默认强制 HTTPS）。
bool IsLoopbackHttpUrl(const std::wstring& url) {
    std::wstring lower = url;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    const wchar_t* prefixes[] = { L"http://127.0.0.1", L"http://localhost", L"http://[::1]" };
    for (const wchar_t* prefix : prefixes) {
        const std::wstring prefixText(prefix);
        if (lower.rfind(prefixText, 0) != 0) continue;
        if (lower.size() == prefixText.size()) return true;
        const wchar_t next = lower[prefixText.size()];
        if (next == L':' || next == L'/') return true;
    }
    return false;
}

// ── 凭据状态与解析结果 ───────────────────────────────────

// 与 Rust CredentialStatus 对齐（Windows 只用到前四种）。
enum class CredentialState { Valid, Expired, RefreshPending, NotFound, ParseError };

struct ClaudeCredentials {
    CredentialState state = CredentialState::NotFound;
    std::wstring accessToken;
    std::string subscriptionType;   // claudeAiOauth.subscriptionType，可缺失
    std::wstring message;           // 解析/过期说明
};

struct CodexCredentials {
    CredentialState state = CredentialState::NotFound;
    std::wstring accessToken;
    std::wstring accountId;         // tokens.account_id，可为空
    std::wstring message;
};

// ── 额度窗口 ─────────────────────────────────────────────

struct QuotaWindow {
    std::string name;        // five_hour / seven_day / 30_day / ...
    double utilization = 0.0; // 已用百分比 0–100
    bool hasResetAt = false;
    long long resetAt = 0;    // Unix 秒
};

struct ClaudeExtraUsage {
    bool enabled = false;
    bool hasMonthlyLimit = false;
    double monthlyLimit = 0.0;
    bool hasUsedCredits = false;
    double usedCredits = 0.0;
    bool hasUtilization = false;
    double utilization = 0.0;
    std::string currency;
};

struct CodexCredits {
    bool hasBalance = false;
    double balance = 0.0;
};

struct ClaudeQuota {
    std::vector<QuotaWindow> windows;
    ClaudeExtraUsage extraUsage;
};

struct CodexQuota {
    std::vector<QuotaWindow> windows;
    CodexCredits credits;
    std::string planType;    // 接口里的 plan_type（plus/pro/...），可缺失
};

// 窗口短标签：统一 5H / 7D / 30D 这类写法。
std::wstring TierLabel(const std::string& name) {
    if (name == "five_hour") return L"5H";
    if (name == "seven_day") return L"7D";
    if (name == "seven_day_fable") return L"7D Fable";
    if (name == "seven_day_opus") return L"7D Opus";
    if (name == "seven_day_sonnet") return L"7D Sonnet";
    if (name == "thirty_day" || name == "30_day") return L"30D";
    if (name == "monthly") return L"月";
    if (name == "weekly_limit") return L"周";
    if (name == "credits") return L"Credits";
    if (name == "unknown") return L"用量";

    const size_t underscore = name.find('_');
    if (underscore != std::string::npos && underscore > 0) {
        const std::string number = name.substr(0, underscore);
        const std::string unit = name.substr(underscore + 1);
        const bool numeric = std::all_of(number.begin(), number.end(),
            [](char ch) { return ch >= '0' && ch <= '9'; });
        if (numeric && (unit == "hour" || unit == "hours")) return WidenUtf8(number) + L"H";
        if (numeric && (unit == "day" || unit == "days")) return WidenUtf8(number) + L"D";
    }
    return WidenUtf8(name);
}

size_t ClaudeTierOrder(const std::string& name) {
    for (size_t index = 0; index < std::size(kClaudeKnownTiers); ++index) {
        if (name == kClaudeKnownTiers[index]) return index;
    }
    return std::size(kClaudeKnownTiers);
}

// 移植 window_seconds_to_tier_name。
std::string WindowSecondsToTierName(long long seconds) {
    if (seconds == 18000) return "five_hour";
    if (seconds == 604800) return "seven_day";
    if (seconds == 2592000) return "30_day";
    if (seconds <= 0) return "unknown";
    const long long hours = seconds / 3600;
    if (hours >= 24) return std::to_string(hours / 24) + "_day";
    return std::to_string(hours) + "_hour";
}

std::wstring BuildPackageSummary(const std::vector<QuotaWindow>& windows) {
    std::wstring summary;
    for (const QuotaWindow& window : windows) {
        if (!std::isfinite(window.utilization)) continue;
        const double remaining = Clamp(100.0 - window.utilization, 0.0, 100.0);
        const std::wstring piece = TierLabel(window.name) + L" " + FormatPercent(remaining);
        if (!summary.empty()) summary += L"  ";
        summary += piece;
    }
    return summary;
}

std::wstring BuildResetSummary(const std::vector<QuotaWindow>& windows) {
    std::wstring summary;
    for (const QuotaWindow& window : windows) {
        if (!window.hasResetAt || window.resetAt <= 0) continue;
        const std::wstring local = FormatLocalDateTime(window.resetAt);
        if (local.empty()) continue;
        if (!summary.empty()) summary += L"；";
        summary += TierLabel(window.name) + L" 重置 " + local;
    }
    return summary;
}

// 输出约定：packageSummary + scopeLabel=套餐 + accountBalance=false。
void FillSubscriptionOutput(const std::vector<QuotaWindow>& windows, SiteBalance& output) {
    output.packageSummary = BuildPackageSummary(windows);
    output.scopeLabel = L"套餐";
    output.accountBalance = false;
}

// ── Claude 凭据解析（read_claude_credentials_from_file / parse_claude_credentials_json）──

ClaudeCredentials ParseClaudeCredentialsJson(const std::string& content) {
    ClaudeCredentials credentials;
    JsonValue root;
    if (!ParseDocument(content, root)) {
        credentials.state = CredentialState::ParseError;
        credentials.message = L"凭据文件不是有效的 JSON。";
        return credentials;
    }

    // 兼容两种 key 名，与 Rust 的 .get("claudeAiOauth").or(.get("claude.ai_oauth")) 一致。
    const JsonValue* entry = Member(&root, "claudeAiOauth");
    if (entry == nullptr || entry->kind != JsonValue::Kind::Object) {
        entry = Member(&root, "claude.ai_oauth");
    }
    if (entry == nullptr || entry->kind != JsonValue::Kind::Object) {
        credentials.state = CredentialState::ParseError;
        credentials.message = L"凭据文件里没有 claudeAiOauth 字段。";
        return credentials;
    }

    std::string accessToken;
    if (!ReadString(Member(entry, "accessToken"), accessToken) || accessToken.empty()) {
        credentials.state = CredentialState::ParseError;
        credentials.message = L"凭据文件缺少 accessToken（或为空）。";
        return credentials;
    }
    credentials.accessToken = WidenUtf8(accessToken);
    if (credentials.accessToken.empty()) {
        credentials.state = CredentialState::ParseError;
        credentials.message = L"accessToken 无法转换为文本。";
        return credentials;
    }
    ReadString(Member(entry, "subscriptionType"), credentials.subscriptionType);

    const JsonValue* expiresAt = Member(entry, "expiresAt");
    const long long now = CurrentUnixSeconds();
    if (expiresAt != nullptr && IsTimestampExpired(expiresAt, now)) {
        std::string refreshToken;
        const bool hasRefreshToken =
            ReadString(Member(entry, "refreshToken"), refreshToken) && !refreshToken.empty();
        const JsonValue* refreshExpiresAt = Member(entry, "refreshTokenExpiresAt");
        const bool refreshTokenExpired =
            refreshExpiresAt != nullptr && IsTimestampExpired(refreshExpiresAt, now);
        if (hasRefreshToken && !refreshTokenExpired) {
            // 访问令牌几小时一换，CLI 下次运行会自己续期：不算登录过期。
            credentials.state = CredentialState::RefreshPending;
            credentials.message = L"Claude 访问令牌已过期（刷新令牌仍有效）。";
        } else {
            credentials.state = CredentialState::Expired;
            credentials.message = refreshTokenExpired
                ? L"Claude 访问令牌和刷新令牌都已过期。"
                : L"Claude OAuth 访问令牌已过期。";
        }
        return credentials;
    }

    credentials.state = CredentialState::Valid;
    return credentials;
}

// ── Claude 用量响应解析（parse_claude_quota）─────────────

bool ParseClaudeQuota(const JsonValue& root, ClaudeQuota& quota) {
    quota = ClaudeQuota{};

    // 1. 已知顶层窗口。
    for (const char* tierName : kClaudeKnownTiers) {
        const JsonValue* window = Member(&root, tierName);
        if (window == nullptr || window->kind != JsonValue::Kind::Object) continue;
        double utilization = 0.0;
        if (!ReadNumber(Member(window, "utilization"), utilization) || !std::isfinite(utilization)) {
            continue;
        }
        QuotaWindow parsed;
        parsed.name = tierName;
        parsed.utilization = utilization;
        long long resetAt = 0;
        if (ParseTimestampValue(Member(window, "resets_at"), resetAt)) {
            parsed.hasResetAt = true;
            parsed.resetAt = resetAt;
        }
        quota.windows.push_back(std::move(parsed));
    }

    // 2. 未知顶层窗口（接口以后可能加新窗口类型）。
    for (const auto& member : root.object) {
        const std::string& key = member.first;
        if (key == "extra_usage" || key == "limits") continue;
        if (ClaudeTierOrder(key) < std::size(kClaudeKnownTiers)) continue;
        const JsonValue& value = member.second;
        if (value.kind != JsonValue::Kind::Object) continue;
        double utilization = 0.0;
        if (!ReadNumber(Member(&value, "utilization"), utilization) || !std::isfinite(utilization)) {
            continue;
        }
        QuotaWindow parsed;
        parsed.name = key;
        parsed.utilization = utilization;
        long long resetAt = 0;
        if (ParseTimestampValue(Member(&value, "resets_at"), resetAt)) {
            parsed.hasResetAt = true;
            parsed.resetAt = resetAt;
        }
        quota.windows.push_back(std::move(parsed));
    }

    // 3. 新版模型专属周限额：limits[] 里 kind=weekly_scoped + group=weekly，
    //    用 scope.model.display_name 映射到 seven_day_fable/opus/sonnet，覆盖同名旧窗口。
    //    与 Rust 一致：带 scope.surface 的子限额不并入整个模型的周限额；
    //    单个异常条目不影响其它条目；同名只取第一条。
    const JsonValue* limits = Member(&root, "limits");
    if (limits != nullptr && limits->kind == JsonValue::Kind::Array) {
        std::vector<std::string> scopedTiers;
        for (const JsonValue& limit : limits->array) {
            if (limit.kind != JsonValue::Kind::Object) continue;
            std::string kind;
            std::string group;
            if (!ReadString(Member(&limit, "kind"), kind) || kind != "weekly_scoped") continue;
            if (!ReadString(Member(&limit, "group"), group) || group != "weekly") continue;
            const JsonValue* scope = Member(&limit, "scope");
            const JsonValue* surface = Member(scope, "surface");
            if (surface != nullptr && surface->kind != JsonValue::Kind::Null) continue;
            const JsonValue* model = Member(scope, "model");
            std::string displayName;
            if (!ReadString(Member(model, "display_name"), displayName)) continue;

            std::string normalized = TrimAscii(std::move(displayName));
            std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            std::string tierName;
            if (normalized == "fable") tierName = "seven_day_fable";
            else if (normalized == "opus") tierName = "seven_day_opus";
            else if (normalized == "sonnet") tierName = "seven_day_sonnet";
            else continue;

            double percent = 0.0;
            if (!ReadNumber(Member(&limit, "percent"), percent) || !std::isfinite(percent) || percent < 0.0) {
                continue;
            }
            if (std::find(scopedTiers.begin(), scopedTiers.end(), tierName) != scopedTiers.end()) continue;
            scopedTiers.push_back(tierName);

            QuotaWindow parsed;
            parsed.name = tierName;
            parsed.utilization = percent;
            long long resetAt = 0;
            if (ParseTimestampValue(Member(&limit, "resets_at"), resetAt)) {
                parsed.hasResetAt = true;
                parsed.resetAt = resetAt;
            }
            const auto existing = std::find_if(quota.windows.begin(), quota.windows.end(),
                [&tierName](const QuotaWindow& window) { return window.name == tierName; });
            if (existing != quota.windows.end()) {
                *existing = std::move(parsed);
            } else {
                quota.windows.push_back(std::move(parsed));
            }
        }
    }

    // 4. 按已知窗口顺序排序（未知窗口保持原顺序）。
    std::stable_sort(quota.windows.begin(), quota.windows.end(),
        [](const QuotaWindow& left, const QuotaWindow& right) {
            return ClaudeTierOrder(left.name) < ClaudeTierOrder(right.name);
        });

    // 5. extra_usage（超额用量）。
    const JsonValue* extraUsage = Member(&root, "extra_usage");
    if (extraUsage != nullptr && extraUsage->kind == JsonValue::Kind::Object) {
        ClaudeExtraUsage parsed;
        query::ReadBool(Member(extraUsage, "is_enabled"), parsed.enabled);
        parsed.hasMonthlyLimit = ReadNumber(Member(extraUsage, "monthly_limit"), parsed.monthlyLimit);
        parsed.hasUsedCredits = ReadNumber(Member(extraUsage, "used_credits"), parsed.usedCredits);
        parsed.hasUtilization = ReadNumber(Member(extraUsage, "utilization"), parsed.utilization);
        ReadString(Member(extraUsage, "currency"), parsed.currency);
        quota.extraUsage = std::move(parsed);
    }

    return !quota.windows.empty();
}

// ── Codex 凭据解析（read_codex_credentials_from_file / parse_codex_credentials_json）──

bool IsCodexTokenStale(const std::string& lastRefresh) {
    long long timestamp = 0;
    if (!ParseIsoTimestamp(lastRefresh, timestamp)) return false;
    const long long now = CurrentUnixSeconds();
    if (now <= timestamp) return false;
    return now - timestamp > kCodexStaleSeconds;
}

CodexCredentials ParseCodexCredentialsJson(const std::string& content) {
    CodexCredentials credentials;
    JsonValue root;
    if (!ParseDocument(content, root)) {
        credentials.state = CredentialState::ParseError;
        credentials.message = L"Codex 凭据文件不是有效的 JSON。";
        return credentials;
    }

    // 仅 auth_mode == "chatgpt"（OAuth 登录）有用量数据；API key 模式没有。
    std::string authMode;
    if (!ReadString(Member(&root, "auth_mode"), authMode) || authMode != "chatgpt") {
        credentials.state = CredentialState::NotFound;
        if (authMode.empty()) {
            credentials.message = L"缺少 auth_mode 字段（不是 ChatGPT OAuth 登录）";
        } else {
            credentials.message = L"auth_mode 为 " + WidenUtf8(authMode) + L"，不是 ChatGPT OAuth 登录";
        }
        return credentials;
    }

    const JsonValue* tokens = Member(&root, "tokens");
    if (tokens == nullptr || tokens->kind != JsonValue::Kind::Object) {
        credentials.state = CredentialState::ParseError;
        credentials.message = L"Codex 凭据文件里没有 tokens 对象。";
        return credentials;
    }

    std::string accessToken;
    if (!ReadString(Member(tokens, "access_token"), accessToken) || accessToken.empty()) {
        credentials.state = CredentialState::ParseError;
        credentials.message = L"Codex 凭据缺少 access_token（或为空）。";
        return credentials;
    }
    credentials.accessToken = WidenUtf8(accessToken);
    if (credentials.accessToken.empty()) {
        credentials.state = CredentialState::ParseError;
        credentials.message = L"Codex access_token 无法转换为文本。";
        return credentials;
    }

    std::string accountId;
    if (ReadString(Member(tokens, "account_id"), accountId) && !accountId.empty()) {
        credentials.accountId = WidenUtf8(accountId);
    }

    std::string lastRefresh;
    if (ReadString(Member(&root, "last_refresh"), lastRefresh) && IsCodexTokenStale(lastRefresh)) {
        credentials.state = CredentialState::Expired;
        credentials.message = L"Codex 凭据距上次刷新已超过 8 天，可能已过期。";
        return credentials;
    }

    credentials.state = CredentialState::Valid;
    return credentials;
}

// ── Codex 用量响应解析（query_codex_usage 的解析部分）────

bool ParseCodexQuota(const JsonValue& root, CodexQuota& quota) {
    quota = CodexQuota{};

    const JsonValue* rateLimit = Member(&root, "rate_limit");
    if (rateLimit != nullptr && rateLimit->kind == JsonValue::Kind::Object) {
        const char* windowKeys[] = { "primary_window", "secondary_window" };
        for (const char* key : windowKeys) {
            const JsonValue* window = Member(rateLimit, key);
            if (window == nullptr || window->kind != JsonValue::Kind::Object) continue;
            double usedPercent = 0.0;
            if (!ReadNumber(Member(window, "used_percent"), usedPercent) || !std::isfinite(usedPercent)) {
                continue;
            }
            QuotaWindow parsed;
            long long windowSeconds = 0;
            parsed.name = ReadInt(Member(window, "limit_window_seconds"), windowSeconds)
                ? WindowSecondsToTierName(windowSeconds)
                : std::string("unknown");
            parsed.utilization = usedPercent;
            long long resetAt = 0;
            if (ReadInt(Member(window, "reset_at"), resetAt) && resetAt > 0) {
                parsed.hasResetAt = true;
                parsed.resetAt = resetAt;
            }
            quota.windows.push_back(std::move(parsed));
        }
    }

    // credits：{has_credits, unlimited, balance}，balance 实测是字符串。
    // 不限量、没有或为 0 时不显示（与 Rust 一致）。
    const JsonValue* credits = Member(&root, "credits");
    if (credits != nullptr && credits->kind == JsonValue::Kind::Object) {
        bool hasCredits = false;
        bool unlimited = false;
        query::ReadBool(Member(credits, "has_credits"), hasCredits);
        query::ReadBool(Member(credits, "unlimited"), unlimited);
        double balance = 0.0;
        if (hasCredits && !unlimited && ReadNumber(Member(credits, "balance"), balance) &&
            std::isfinite(balance) && balance > 0.0) {
            quota.credits.hasBalance = true;
            quota.credits.balance = balance;
        }
    }

    // 接口还带 plan_type（plus/pro/...），放进 extra 展示套餐名。
    ReadString(Member(&root, "plan_type"), quota.planType);

    return !quota.windows.empty();
}

// ── extra 文本 ───────────────────────────────────────────

// Claude CLI 凭据里的 subscriptionType；接口没给时也可以从凭据补上账号类型。
std::wstring ClaudePlanLabel(const std::string& subscriptionType) {
    if (subscriptionType.empty()) return {};
    std::string normalized = TrimAscii(subscriptionType);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (normalized == "max") return L"Claude Max";
    if (normalized == "pro") return L"Claude Pro";
    if (normalized == "free") return L"Claude Free";
    if (normalized == "team") return L"Claude Team";
    if (normalized == "enterprise") return L"Claude Enterprise";
    return L"Claude " + WidenUtf8(subscriptionType);
}

std::wstring CodexPlanLabel(const std::string& planType) {
    if (planType.empty()) return {};
    std::string normalized = TrimAscii(planType);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (normalized == "plus") return L"ChatGPT Plus";
    if (normalized == "pro") return L"ChatGPT Pro";
    if (normalized == "team") return L"ChatGPT Team";
    if (normalized == "enterprise") return L"ChatGPT Enterprise";
    if (normalized == "business") return L"ChatGPT Business";
    if (normalized == "free") return L"ChatGPT Free";
    if (normalized == "go") return L"ChatGPT Go";
    return L"ChatGPT " + WidenUtf8(planType);
}

std::wstring BuildClaudeExtra(const ClaudeCredentials& credentials, const ClaudeQuota& quota) {
    std::wstring extra;
    AppendPiece(extra, ClaudePlanLabel(credentials.subscriptionType));
    AppendPiece(extra, BuildResetSummary(quota.windows));

    const ClaudeExtraUsage& usage = quota.extraUsage;
    if (usage.enabled) {
        std::wstring text = L"超额额度已启用";
        if (usage.hasUtilization) {
            text += L"，已用 " + FormatPercent(usage.utilization);
        } else if (usage.hasMonthlyLimit && usage.hasUsedCredits) {
            text += L"，已用 " + FormatAmount(usage.usedCredits) + L" / " +
                FormatAmount(usage.monthlyLimit);
            if (!usage.currency.empty()) text += L" " + WidenUtf8(usage.currency);
        } else if (usage.hasMonthlyLimit) {
            text += L"，月限额 " + FormatAmount(usage.monthlyLimit);
            if (!usage.currency.empty()) text += L" " + WidenUtf8(usage.currency);
        }
        AppendPiece(extra, text);
    }
    return extra;
}

std::wstring BuildCodexExtra(const CodexQuota& quota) {
    std::wstring extra;
    AppendPiece(extra, CodexPlanLabel(quota.planType));
    AppendPiece(extra, BuildResetSummary(quota.windows));
    if (quota.credits.hasBalance) {
        AppendPiece(extra, L"Credits 余额 " + FormatAmount(quota.credits.balance));
    }
    return extra;
}

// ── HTTP 查询 ────────────────────────────────────────────

bool RequestClaudeQuota(const std::wstring& endpoint, const ClaudeCredentials& credentials,
    SiteBalance& output) {
    HttpRequest request;
    request.method = L"GET";
    request.headers.push_back(query::BearerHeader(credentials.accessToken));
    request.headers.emplace_back(L"anthropic-beta", L"oauth-2025-04-20");
    request.timeoutMs = kRequestTimeoutMs;
    request.allowInsecureHttp = IsLoopbackHttpUrl(endpoint);

    const HttpResponse response = SendHttp(endpoint, request);
    if (!response.success) {
        if (response.status == 401 || response.status == 403) {
            output.error = L"Claude 用量接口认证失败（HTTP " +
                std::to_wstring(response.status) +
                L"）。Claude Code 凭据可能已失效，请运行 claude 重新登录后重试。";
        } else if (response.status == 0) {
            output.error = L"无法连接 Claude 用量接口：" + response.error +
                L"请检查网络、代理或 VPN 后重试。";
        } else {
            output.error = L"Claude 用量接口请求失败：" + response.error;
        }
        return false;
    }

    JsonValue root;
    if (!ParseDocument(response.body, root)) {
        output.error = L"Claude 用量接口返回的数据不是有效 JSON，无法解析。";
        return false;
    }
    ClaudeQuota quota;
    if (!ParseClaudeQuota(root, quota)) {
        output.error = L"Claude 用量接口没有返回任何额度窗口；请确认账号有可用的套餐额度后重试。";
        return false;
    }
    FillSubscriptionOutput(quota.windows, output);
    output.extra = BuildClaudeExtra(credentials, quota);
    return true;
}

bool RequestCodexQuota(const std::wstring& endpoint, const CodexCredentials& credentials,
    SiteBalance& output) {
    HttpRequest request;
    request.method = L"GET";
    request.headers.push_back(query::BearerHeader(credentials.accessToken));
    request.headers.emplace_back(L"User-Agent", L"codex-cli");
    if (!credentials.accountId.empty()) {
        request.headers.emplace_back(L"ChatGPT-Account-Id", credentials.accountId);
    }
    request.timeoutMs = kRequestTimeoutMs;
    request.allowInsecureHttp = IsLoopbackHttpUrl(endpoint);

    const HttpResponse response = SendHttp(endpoint, request);
    if (!response.success) {
        if (response.status == 401 || response.status == 403) {
            output.error = L"Codex 用量接口认证失败（HTTP " +
                std::to_wstring(response.status) +
                L"）。Codex 凭据可能已失效，请运行 codex 登录（选择 ChatGPT 账号）后重试。";
        } else if (response.status == 0) {
            output.error = L"无法连接 Codex 用量接口：" + response.error +
                L"请检查网络、代理或 VPN 后重试。";
        } else {
            output.error = L"Codex 用量接口请求失败：" + response.error;
        }
        return false;
    }

    JsonValue root;
    if (!ParseDocument(response.body, root)) {
        output.error = L"Codex 用量接口返回的数据不是有效 JSON，无法解析。";
        return false;
    }
    CodexQuota quota;
    if (!ParseCodexQuota(root, quota)) {
        output.error = L"Codex 用量接口没有返回任何额度窗口；请确认账号有可用的套餐额度后重试。";
        return false;
    }
    FillSubscriptionOutput(quota.windows, output);
    output.extra = BuildCodexExtra(quota);
    return true;
}

// ── 入口分支 ─────────────────────────────────────────────

bool QueryClaude(const SiteProfile& profile, SiteBalance& output) {
    const std::wstring home = UserProfileDirectory();
    if (home.empty()) {
        output.error = L"无法确定用户目录（环境变量 USERPROFILE 为空），读不到 Claude Code 凭据。";
        return false;
    }
    const std::wstring path = AppendPath(home, kClaudeCredentialsRelative);

    std::string content;
    bool missing = false;
    std::wstring readError;
    if (!ReadCredentialFile(path, content, missing, readError)) {
        if (missing) {
            output.error = L"未找到 Claude Code 凭据文件：" + path +
                L"。请先在命令行运行 claude 并完成登录，然后重试。";
        } else {
            output.error = L"无法读取 Claude Code 凭据文件：" + path + L"（" + readError +
                L"）。请检查文件权限，或运行 claude 重新登录。";
        }
        return false;
    }

    const ClaudeCredentials credentials = ParseClaudeCredentialsJson(content);
    if (credentials.state == CredentialState::NotFound) {
        output.error = L"未找到 Claude Code 凭据文件：" + path +
            L"。请先在命令行运行 claude 并完成登录，然后重试。";
        return false;
    }
    if (credentials.state == CredentialState::ParseError) {
        output.error = L"Claude Code 凭据不可用：" + credentials.message +
            L"请运行 claude 重新登录后重试。";
        return false;
    }

    const std::wstring endpoint = ResolveEndpoint(profile, kClaudeUsageEndpoint);

    if (credentials.state == CredentialState::Valid) {
        return RequestClaudeQuota(endpoint, credentials, output);
    }

    // Expired / RefreshPending：与 Rust 一致，先用旧 accessToken 试一次接口，
    // token 可能实际上还没被服务端拒绝。
    if (RequestClaudeQuota(endpoint, credentials, output)) return true;
    const std::wstring apiError = output.error;
    output.error = credentials.message;
    output.error += credentials.state == CredentialState::RefreshPending
        ? L"请运行 claude（CLI 下次运行会自动续期），或重新登录后重试。"
        : L"请运行 claude 重新登录后重试。";
    if (!apiError.empty()) output.error += L"接口返回：" + apiError;
    return false;
}

bool QueryCodex(const SiteProfile& profile, SiteBalance& output) {
    const std::wstring home = UserProfileDirectory();
    if (home.empty()) {
        output.error = L"无法确定用户目录（环境变量 USERPROFILE 为空），读不到 Codex 凭据。";
        return false;
    }
    const std::wstring path = AppendPath(home, kCodexCredentialsRelative);

    std::string content;
    bool missing = false;
    std::wstring readError;
    if (!ReadCredentialFile(path, content, missing, readError)) {
        if (missing) {
            output.error = L"未找到 Codex 凭据文件：" + path +
                L"。请先在命令行运行 codex 登录（选择使用 ChatGPT 账号），然后重试。";
        } else {
            output.error = L"无法读取 Codex 凭据文件：" + path + L"（" + readError +
                L"）。请检查文件权限，或运行 codex 登录重新生成凭据。";
        }
        return false;
    }

    const CodexCredentials credentials = ParseCodexCredentialsJson(content);
    if (credentials.state == CredentialState::NotFound) {
        output.error = L"Codex 凭据不可用：" + credentials.message +
            L"。请在命令行运行 codex 登录，选择使用 ChatGPT 账号后重试。";
        return false;
    }
    if (credentials.state == CredentialState::ParseError) {
        output.error = L"Codex 凭据不可用：" + credentials.message +
            L"请运行 codex 登录重新生成凭据后重试。";
        return false;
    }

    const std::wstring endpoint = ResolveEndpoint(profile, kCodexUsageEndpoint);

    if (credentials.state == CredentialState::Valid) {
        return RequestCodexQuota(endpoint, credentials, output);
    }

    // Expired：同样先试一次接口。
    if (RequestCodexQuota(endpoint, credentials, output)) return true;
    const std::wstring apiError = output.error;
    output.error = credentials.message +
        L"请运行 codex 登录（选择 ChatGPT 账号）刷新凭据后重试。";
    if (!apiError.empty()) output.error += L"接口返回：" + apiError;
    return false;
}

} // namespace

bool IsOfficialSubscriptionProvider(SiteProvider provider) {
    return provider == SiteProvider::ClaudeOfficial || provider == SiteProvider::CodexOfficial;
}

bool QueryOfficialSubscription(const SiteProfile& profile, SiteBalance& output) {
    switch (profile.adapter) {
    case SiteProvider::ClaudeOfficial:
        return QueryClaude(profile, output);
    case SiteProvider::CodexOfficial:
        return QueryCodex(profile, output);
    default:
        output.error = L"该站点不是官方订阅类型（Claude / Codex）。";
        return false;
    }
}

} // namespace aiapi
