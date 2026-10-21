#include "OfficialPlan.h"

#include "QuerySupport.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

// Token Plan（编程套餐）适配：CC-Switch services/coding_plan.rs 的 C++ 移植。
//
// 每个 provider 的「地址、请求头、请求体、响应字段名」都严格照着 Rust 实现来，
// 唯一额外允许的是 profile.baseUrl 非空时覆盖默认端点：官方预设 UI 会把默认
// 地址写进 baseUrl（见 ProviderRegistry.h），可变域名（智谱 / ZenMux /
// Command Code）也要靠它，同时本地 mock 测试把它指向 127.0.0.1。
//
// 输出约定见 OfficialPlan.h：packageSummary 是各窗口的「剩余百分比」摘要，
// extra 放重置时间 / 套餐名，scopeLabel = 套餐，accountBalance = false。

namespace aiapi {
namespace {

using query::HttpRequest;
using query::HttpResponse;
using query::JsonValue;

// ── 文本 / URL 工具 ──────────────────────────────────────────

bool IsAsciiSpace(wchar_t ch) {
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
}

std::wstring TrimAsciiSpace(const std::wstring& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && IsAsciiSpace(text[begin])) ++begin;
    while (end > begin && IsAsciiSpace(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

// 去掉首尾空白和结尾的 '/'，方便统一拼路径。
std::wstring TrimUrl(std::wstring url) {
    url = TrimAsciiSpace(url);
    while (!url.empty() && url.back() == L'/') url.pop_back();
    return TrimAsciiSpace(url);
}

std::wstring LowerAscii(std::wstring text) {
    for (wchar_t& ch : text) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return text;
}

bool ContainsNoCase(const std::wstring& text, const wchar_t* needle) {
    return LowerAscii(text).find(LowerAscii(needle)) != std::wstring::npos;
}

bool EndsWithNoCase(const std::wstring& text, const wchar_t* suffix) {
    const std::wstring loweredSuffix = LowerAscii(suffix);
    if (loweredSuffix.size() > text.size()) return false;
    return LowerAscii(text.substr(text.size() - loweredSuffix.size())) == loweredSuffix;
}

// ── 时间解析 / 格式化 ───────────────────────────────────────

// Howard Hinnant 的 civil_from_days：把公历日期换成 1970-01-01 起的天数。
long long DaysFromCivil(int year, unsigned int month, unsigned int day) {
    year -= month <= 2 ? 1 : 0;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned int yearOfEra = static_cast<unsigned int>(year - era * 400);
    const unsigned int dayOfYear =
        (153U * (month + (month > 2 ? static_cast<unsigned int>(-3) : 9U)) + 2U) / 5U + day - 1U;
    const unsigned int dayOfEra =
        yearOfEra * 365U + yearOfEra / 4U - yearOfEra / 100U + dayOfYear;
    return static_cast<long long>(era) * 146097LL + static_cast<long long>(dayOfEra) - 719468LL;
}

// 解析 ISO 8601 时间（如 "2026-10-21T18:30:00.000Z"、"2026-10-21T18:30:00+08:00"）
// 为 Unix 秒。末尾无时区标记时按 UTC 处理。
bool ParseIso8601Seconds(const std::string& text, long long& seconds) {
    const std::string value = text;  // 只读，避免调用方再传副本
    if (value.size() < 19) return false;
    const auto isDigit = [&value](size_t index) {
        return index < value.size() && value[index] >= '0' && value[index] <= '9';
    };
    const size_t digitPositions[] = { 0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18 };
    for (const size_t index : digitPositions) {
        if (!isDigit(index)) return false;
    }
    if (value[4] != '-' || value[7] != '-') return false;
    if (value[10] != 'T' && value[10] != 't' && value[10] != ' ') return false;
    if (value[13] != ':' || value[16] != ':') return false;

    const int year = (value[0] - '0') * 1000 + (value[1] - '0') * 100 + (value[2] - '0') * 10 + (value[3] - '0');
    const unsigned int month = static_cast<unsigned int>((value[5] - '0') * 10 + (value[6] - '0'));
    const unsigned int day = static_cast<unsigned int>((value[8] - '0') * 10 + (value[9] - '0'));
    const int hour = (value[11] - '0') * 10 + (value[12] - '0');
    const int minute = (value[14] - '0') * 10 + (value[15] - '0');
    const int second = (value[17] - '0') * 10 + (value[18] - '0');
    if (month == 0 || month > 12 || day == 0 || day > 31 || hour > 23 || minute > 59 || second > 60) return false;

    size_t position = 19;
    if (position < value.size() && value[position] == '.') {
        ++position;
        const size_t fractionStart = position;
        while (position < value.size() && isDigit(position)) ++position;
        if (position == fractionStart) return false;
    }

    long long offsetSeconds = 0;  // 本地时间 + 偏移 = UTC
    if (position < value.size()) {
        if (value[position] == 'Z' || value[position] == 'z') {
            ++position;
        } else if (value[position] == '+' || value[position] == '-') {
            const int sign = value[position] == '+' ? 1 : -1;
            ++position;
            if (position + 1 >= value.size() || !isDigit(position) || !isDigit(position + 1)) return false;
            const int offsetHour = (value[position] - '0') * 10 + (value[position + 1] - '0');
            position += 2;
            int offsetMinute = 0;
            if (position < value.size() && value[position] == ':') {
                ++position;
                if (position + 1 >= value.size() || !isDigit(position) || !isDigit(position + 1)) return false;
                offsetMinute = (value[position] - '0') * 10 + (value[position + 1] - '0');
                position += 2;
            } else if (position + 1 < value.size() && isDigit(position) && isDigit(position + 1)) {
                offsetMinute = (value[position] - '0') * 10 + (value[position + 1] - '0');
                position += 2;
            }
            if (offsetHour > 23 || offsetMinute > 59) return false;
            offsetSeconds = sign * (offsetHour * 3600 + offsetMinute * 60);
        } else {
            return false;
        }
    }
    if (position != value.size()) return false;

    seconds = DaysFromCivil(year, month, day) * 86400LL + hour * 3600LL + minute * 60LL + second - offsetSeconds;
    return true;
}

// 响应里的重置时间：兼容 ISO 8601 字符串与秒/毫秒时间戳（同 Rust extract_reset_time）。
bool ExtractResetMilliseconds(const JsonValue* value, long long& milliseconds) {
    if (!value) return false;
    if (value->kind == JsonValue::Kind::String) {
        long long seconds = 0;
        if (!ParseIso8601Seconds(value->text, seconds)) return false;
        milliseconds = seconds * 1000LL;
        return true;
    }
    long long number = 0;
    if (!query::ReadInt(value, number)) return false;
    if (number <= 0) return false;  // 0 / 负数表示没有重置时间
    milliseconds = number < 1000000000000LL ? number * 1000LL : number;
    return true;
}

// 转成本地时间的 "MM-DD HH:mm"；解析失败时返回空串（不展示重置时间）。
std::wstring FormatResetTime(long long milliseconds) {
    if (milliseconds <= 0) return {};
    const std::time_t seconds = static_cast<std::time_t>(milliseconds / 1000LL);
    if (seconds <= 0) return {};
    std::tm local{};
    if (localtime_s(&local, &seconds) != 0) return {};
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%02d-%02d %02d:%02d", local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min);
    return buffer;
}

// ── 数值 / 文本读取 ─────────────────────────────────────────

double ReadNumberOr(const JsonValue* value, double fallback) {
    double number = 0.0;
    return query::ReadNumber(value, number) ? number : fallback;
}

std::wstring WidenUtf8(const std::string& text) {
    return query::WidenUtf8(text);
}

std::wstring FormatUsd(double value) {
    if (!std::isfinite(value)) return L"$--";
    wchar_t buffer[48]{};
    swprintf_s(buffer, L"$%.2f", value);
    return buffer;
}

// 访问嵌套对象字段：Member 对空指针返回 nullptr。
const JsonValue* Field(const JsonValue* value, const char* name) {
    return query::Member(value, name);
}

// ── HTTP 公共封装 ───────────────────────────────────────────

HttpRequest MakeRequest(const std::wstring& apiKey) {
    HttpRequest request;
    request.headers.push_back(query::BearerHeader(TrimAsciiSpace(apiKey)));
    // 官方端点都是 HTTPS；打开该开关让本地 mock 测试能指向 127.0.0.1，
    // 同时兼容用户在可编辑字段（智谱 / ZenMux / Command Code）里填写的地址。
    request.allowInsecureHttp = true;
    request.timeoutMs = 15000;
    return request;
}

// 发送请求；失败时把可读错误写进 output。401/403 直接透传 SendHttp 的文案。
bool SendRequest(const std::wstring& url, const HttpRequest& request, const wchar_t* label,
    HttpResponse& response, SiteBalance& output) {
    response = query::SendHttp(url, request);
    if (response.success) return true;
    if (response.status == 401 || response.status == 403) {
        output.error = response.error;
    } else {
        output.error = std::wstring(label) + L"：" +
            (response.error.empty() ? L"请求失败。" : response.error);
    }
    return false;
}

bool ParseBody(const HttpResponse& response, const wchar_t* label, JsonValue& root, SiteBalance& output) {
    if (!query::ParseDocument(response.body, root)) {
        output.error = std::wstring(label) + L"返回了无法解析的 JSON。";
        return false;
    }
    return true;
}

// ── 套餐窗口与输出拼装 ──────────────────────────────────────

struct QuotaWindow {
    std::wstring label;        // 窗口标签：5H / 7D / 1M
    double utilization = 0.0;  // 已用百分比 0–100（下游再换算成剩余）
    std::wstring resetText;    // 已格式化的本地重置时间；空 = 不展示
    bool hasUsd = false;       // ZenMux 用美元口径展示已用/上限
    double usedUsd = 0.0;
    double limitUsd = 0.0;
};

double ClampPercent(double value) {
    if (!std::isfinite(value)) return 0.0;
    return std::min(100.0, std::max(0.0, value));
}

int RemainingPercent(const QuotaWindow& window) {
    const double remaining = ClampPercent(100.0 - window.utilization);
    return static_cast<int>(std::lround(remaining));
}

// 「5H 62%  7D 41%」；多窗口顺序由调用方固定为短窗口在前。
std::wstring BuildSummary(const std::vector<QuotaWindow>& windows) {
    std::wstring summary;
    for (const QuotaWindow& window : windows) {
        if (!summary.empty()) summary += L"  ";
        summary += window.label + L" " + std::to_wstring(RemainingPercent(window)) + L"%";
    }
    return summary;
}

// 「套餐 pro；5H 重置 10-21 18:30；7D 重置 10-24 00:00」
std::wstring BuildExtra(const std::vector<QuotaWindow>& windows, const std::wstring& planName) {
    std::vector<std::wstring> parts;
    if (!planName.empty()) parts.push_back(L"套餐 " + planName);
    for (const QuotaWindow& window : windows) {
        if (window.hasUsd) {
            parts.push_back(window.label + L" 已用 " + FormatUsd(window.usedUsd) + L" / " + FormatUsd(window.limitUsd));
        }
        if (!window.resetText.empty()) parts.push_back(window.label + L" 重置 " + window.resetText);
    }
    std::wstring extra;
    for (const std::wstring& part : parts) {
        if (!extra.empty()) extra += L"；";
        extra += part;
    }
    return extra;
}

bool FinishPlan(const std::vector<QuotaWindow>& windows, const std::wstring& planName, SiteBalance& output) {
    if (windows.empty()) return false;  // 调用方应先填好具体错误
    output.packageSummary = BuildSummary(windows);
    output.extra = BuildExtra(windows, planName);
    output.scopeLabel = L"套餐";
    output.accountBalance = false;
    return true;
}

// ── Kimi For Coding ─────────────────────────────────────────
//
// GET https://api.kimi.com/coding/v1/usages（Bearer）。
// limits[].detail（5 小时滚动窗口，limit/remaining/resetTime）+ usage（周限额）。
// 百分比口径：used = limit - remaining，utilization = used / limit * 100。
bool QueryKimi(const SiteProfile& profile, SiteBalance& output) {
    std::wstring base = TrimUrl(profile.baseUrl);
    if (base.empty()) base = L"https://api.kimi.com/coding";
    std::wstring url;
    if (EndsWithNoCase(base, L"/v1/usages")) {
        url = base;
    } else if (EndsWithNoCase(base, L"/v1")) {
        url = base + L"/usages";
    } else {
        url = base + L"/v1/usages";
    }

    HttpResponse response;
    if (!SendRequest(url, MakeRequest(profile.credential), L"Kimi For Coding 用量接口", response, output)) return false;
    JsonValue root;
    if (!ParseBody(response, L"Kimi For Coding 用量接口", root, output)) return false;

    std::vector<QuotaWindow> windows;
    bool sawLimits = false;
    bool sawUsage = false;

    const JsonValue* limits = root.Find("limits");
    if (limits && limits->kind == JsonValue::Kind::Array) {
        sawLimits = true;
        for (const JsonValue& item : limits->array) {
            const JsonValue* detail = item.Find("detail");
            if (!detail || detail->kind != JsonValue::Kind::Object) continue;
            const double limit = ReadNumberOr(detail->Find("limit"), 1.0);
            const double remaining = ReadNumberOr(detail->Find("remaining"), 0.0);
            const double used = std::max(0.0, limit - remaining);

            QuotaWindow window;
            window.label = L"5H";
            window.utilization = limit > 0.0 ? used / limit * 100.0 : 0.0;
            long long resetMs = 0;
            if (ExtractResetMilliseconds(detail->Find("resetTime"), resetMs)) window.resetText = FormatResetTime(resetMs);
            windows.push_back(window);
            break;  // 5 小时桶只取第一条 detail（Rust 侧实测最多一条）
        }
    }

    const JsonValue* usage = root.Find("usage");
    if (usage && usage->kind == JsonValue::Kind::Object) {
        sawUsage = true;
        const double limit = ReadNumberOr(usage->Find("limit"), 1.0);
        const double remaining = ReadNumberOr(usage->Find("remaining"), 0.0);
        const double used = std::max(0.0, limit - remaining);

        QuotaWindow window;
        window.label = L"7D";
        window.utilization = limit > 0.0 ? used / limit * 100.0 : 0.0;
        long long resetMs = 0;
        if (ExtractResetMilliseconds(usage->Find("resetTime"), resetMs)) window.resetText = FormatResetTime(resetMs);
        windows.push_back(window);
    }

    if (!sawLimits && !sawUsage) {
        output.error = L"Kimi 用量响应缺少 limits / usage 字段。";
        return false;
    }
    if (windows.empty()) {
        output.error = L"Kimi 用量响应中没有可用的额度窗口（limits[].detail.limit/remaining 或 usage.limit/remaining）。";
        return false;
    }
    return FinishPlan(windows, std::wstring(), output);
}

// ── 智谱 GLM（个人版）───────────────────────────────────────
//
// 端点 {open.bigmodel.cn|api.z.ai}/api/monitor/usage/quota/limit；
// 认证头 Authorization: <api key>（智谱不加 Bearer 前缀）。
// data.limits[] 里 TOKENS_LIMIT / CREDIT_LIMIT 的 percentage 是「已用百分比」；
// unit=3 表示 5 小时窗口，unit=6 表示每周窗口（Rust 推断规则）。
struct ZhipuWindowEntry {
    bool occupied = false;
    bool hasReset = false;
    long long resetMs = 0;
    double percentage = 0.0;
};

bool ZhipuBodyHasNoPlan(const JsonValue& root) {
    bool success = true;
    if (!query::ReadBool(root.Find("success"), success) || success) return false;

    std::string message;
    if (!query::ReadString(root.Find("msg"), message)) return false;
    std::string lowered = message;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::string compact;
    for (const char ch : lowered) {
        if (!std::isspace(static_cast<unsigned char>(ch)) && ch != '_' && ch != '-') compact.push_back(ch);
    }
    if (compact.find("codingplan") == std::string::npos) return false;
    static const char* const kNoPlanKeywords[] = {
        "不存在", "没有", "未开通", "not exist", "not found", "not have",
        "doesn't have", "no coding plan", "not subscribed",
    };
    for (const char* keyword : kNoPlanKeywords) {
        if (lowered.find(keyword) != std::string::npos) return true;
    }
    return false;
}

std::vector<QuotaWindow> ParseZhipuWindows(const JsonValue& data) {
    ZhipuWindowEntry fiveHour;
    ZhipuWindowEntry weekly;
    std::vector<ZhipuWindowEntry> unclassified;

    const JsonValue* limits = data.Find("limits");
    if (limits && limits->kind == JsonValue::Kind::Array) {
        for (const JsonValue& item : limits->array) {
            if (item.kind != JsonValue::Kind::Object) continue;
            std::string type;
            if (!query::ReadString(item.Find("type"), type)) continue;
            std::string loweredType = type;
            std::transform(loweredType.begin(), loweredType.end(), loweredType.begin(),
                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (loweredType != "tokens_limit" && loweredType != "credit_limit") continue;

            ZhipuWindowEntry entry;
            entry.percentage = ReadNumberOr(item.Find("percentage"), 0.0);
            long long resetMs = 0;
            if (ExtractResetMilliseconds(item.Find("nextResetTime"), resetMs)) {
                entry.hasReset = true;
                entry.resetMs = resetMs;
            }
            long long unit = 0;
            const bool hasUnit = query::ReadInt(item.Find("unit"), unit);
            if (hasUnit && unit == 3 && !fiveHour.occupied) {
                fiveHour = entry;
                fiveHour.occupied = true;
            } else if (hasUnit && unit == 6 && !weekly.occupied) {
                weekly = entry;
                weekly.occupied = true;
            } else {
                // unit 缺失 / 不认识 / 槽位已占：走重置时间启发式兜底
                unclassified.push_back(entry);
            }
        }
    }

    // 兜底排序同 Rust：没有 nextResetTime 的条目优先归 five_hour，其余按 reset 升序。
    std::sort(unclassified.begin(), unclassified.end(),
        [](const ZhipuWindowEntry& left, const ZhipuWindowEntry& right) {
            if (left.hasReset != right.hasReset) return !left.hasReset;
            return left.resetMs < right.resetMs;
        });
    for (const ZhipuWindowEntry& entry : unclassified) {
        if (!fiveHour.occupied) {
            fiveHour = entry;
            fiveHour.occupied = true;
        } else if (!weekly.occupied) {
            weekly = entry;
            weekly.occupied = true;
        }
    }

    std::vector<QuotaWindow> windows;
    const auto appendWindow = [&windows](const wchar_t* label, const ZhipuWindowEntry& entry) {
        QuotaWindow window;
        window.label = label;
        window.utilization = entry.percentage;
        if (entry.hasReset) window.resetText = FormatResetTime(entry.resetMs);
        windows.push_back(window);
    };
    if (fiveHour.occupied) appendWindow(L"5H", fiveHour);
    if (weekly.occupied) appendWindow(L"7D", weekly);
    return windows;
}

bool QueryZhipu(const SiteProfile& profile, SiteBalance& output) {
    const std::wstring base = TrimUrl(profile.baseUrl);
    std::wstring url;
    if (base.empty() || ContainsNoCase(base, L"bigmodel.cn")) {
        // 个人版默认国内站；Rust: 地址里出现 bigmodel.cn 一律走 open.bigmodel.cn。
        url = L"https://open.bigmodel.cn/api/monitor/usage/quota/limit";
    } else if (ContainsNoCase(base, L"z.ai")) {
        url = L"https://api.z.ai/api/monitor/usage/quota/limit";
    } else {
        // 自定义 / 本地 mock 地址：把 baseUrl 当站点根地址拼固定路径。
        url = base + L"/api/monitor/usage/quota/limit";
    }

    HttpRequest request;
    request.headers.push_back({ L"Authorization", TrimAsciiSpace(profile.credential) });  // 智谱不加 Bearer
    request.headers.push_back({ L"Content-Type", L"application/json" });
    request.headers.push_back({ L"Accept-Language", L"en-US,en" });
    request.allowInsecureHttp = true;
    request.timeoutMs = 15000;

    HttpResponse response;
    if (!SendRequest(url, request, L"智谱套餐接口", response, output)) return false;
    JsonValue root;
    if (!ParseBody(response, L"智谱套餐接口", root, output)) return false;

    bool success = true;
    if (query::ReadBool(root.Find("success"), success) && !success) {
        std::string message;
        query::ReadString(root.Find("msg"), message);
        const std::wstring messageWide = WidenUtf8(message);
        if (ZhipuBodyHasNoPlan(root)) {
            output.error = messageWide.empty()
                ? L"该智谱账户未开通 Coding Plan（按量付费账户）。"
                : L"该智谱账户未开通 Coding Plan（服务端返回：" + messageWide + L"）。";
        } else if (messageWide.empty()) {
            output.error = L"智谱套餐接口返回 success=false，且没有 msg 字段。";
        } else {
            output.error = L"智谱套餐接口返回错误：" + messageWide;
        }
        return false;
    }

    const JsonValue* data = root.Find("data");
    if (!data || data->kind != JsonValue::Kind::Object) {
        output.error = L"智谱套餐响应缺少 data 字段。";
        return false;
    }

    const std::vector<QuotaWindow> windows = ParseZhipuWindows(*data);
    if (windows.empty()) {
        output.error = L"智谱套餐响应缺少 limits[] 中 type=TOKENS_LIMIT 的额度条目。";
        return false;
    }

    std::wstring planName;
    std::string level;
    if (query::ReadString(data->Find("level"), level)) planName = WidenUtf8(level);
    return FinishPlan(windows, planName, output);
}

// ── MiniMax（国内 / 国际）──────────────────────────────────
//
// GET {api.minimaxi.com|api.minimax.io}/v1/api/openplatform/coding_plan/remains
// （Bearer）。model_remains 里只取 model_name == "general"：
// current_interval_remaining_percent 是 5h 的「剩余百分比」，
// current_weekly_status == 1 且有 current_weekly_remaining_percent 时才有周桶。
bool QueryMiniMax(const SiteProfile& profile, bool international, SiteBalance& output) {
    std::wstring base = TrimUrl(profile.baseUrl);
    if (base.empty()) base = international ? L"https://api.minimax.io" : L"https://api.minimaxi.com";
    std::wstring url;
    if (EndsWithNoCase(base, L"/remains")) {
        url = base;
    } else if (EndsWithNoCase(base, L"/v1")) {
        url = base + L"/api/openplatform/coding_plan/remains";
    } else {
        url = base + L"/v1/api/openplatform/coding_plan/remains";
    }

    HttpRequest request = MakeRequest(profile.credential);
    request.headers.push_back({ L"Content-Type", L"application/json" });

    HttpResponse response;
    if (!SendRequest(url, request, L"MiniMax 套餐接口", response, output)) return false;
    JsonValue root;
    if (!ParseBody(response, L"MiniMax 套餐接口", root, output)) return false;

    // 业务级错误：base_resp.status_code != 0（Rust 在字段缺失时按 -1 处理）。
    const JsonValue* baseResp = root.Find("base_resp");
    if (baseResp && baseResp->kind == JsonValue::Kind::Object) {
        long long statusCode = -1;
        query::ReadInt(baseResp->Find("status_code"), statusCode);
        if (statusCode != 0) {
            std::string message;
            query::ReadString(baseResp->Find("status_msg"), message);
            const std::wstring messageWide = WidenUtf8(message);
            output.error = L"MiniMax 套餐接口返回业务错误（code " + std::to_wstring(statusCode) + L"）：" +
                (messageWide.empty() ? L"未知原因。" : messageWide);
            return false;
        }
    }

    const JsonValue* modelRemains = root.Find("model_remains");
    if (!modelRemains || modelRemains->kind != JsonValue::Kind::Array) {
        output.error = L"MiniMax 套餐响应缺少 model_remains 字段。";
        return false;
    }

    const JsonValue* general = nullptr;
    for (const JsonValue& item : modelRemains->array) {
        std::string modelName;
        if (query::ReadString(item.Find("model_name"), modelName) && modelName == "general") {
            general = &item;
            break;
        }
    }
    if (!general) {
        output.error = L"MiniMax 套餐响应缺少 model_remains 中 model_name=general 的额度条目。";
        return false;
    }

    std::vector<QuotaWindow> windows;
    double intervalRemain = 0.0;
    if (query::ReadNumber(general->Find("current_interval_remaining_percent"), intervalRemain)) {
        QuotaWindow window;
        window.label = L"5H";
        window.utilization = 100.0 - intervalRemain;
        long long resetMs = 0;
        if (ExtractResetMilliseconds(general->Find("end_time"), resetMs)) window.resetText = FormatResetTime(resetMs);
        windows.push_back(window);
    }
    long long weeklyStatus = 0;
    double weeklyRemain = 0.0;
    if (query::ReadInt(general->Find("current_weekly_status"), weeklyStatus) && weeklyStatus == 1 &&
        query::ReadNumber(general->Find("current_weekly_remaining_percent"), weeklyRemain)) {
        QuotaWindow window;
        window.label = L"7D";
        window.utilization = 100.0 - weeklyRemain;
        long long resetMs = 0;
        if (ExtractResetMilliseconds(general->Find("weekly_end_time"), resetMs)) window.resetText = FormatResetTime(resetMs);
        windows.push_back(window);
    }
    if (windows.empty()) {
        output.error = L"MiniMax 套餐响应缺少 current_interval_remaining_percent / current_weekly_remaining_percent 字段。";
        return false;
    }
    return FinishPlan(windows, std::wstring(), output);
}

// ── ZenMux ─────────────────────────────────────────────────
//
// Rust query_zenmux 直接 GET base_url（Bearer），所以默认地址是 https://zenmux.ai，
// 用户也可以在可编辑的 baseUrl 里填完整用量接口地址。
// data.quota_5_hour / quota_7_day：usage_percentage 是 0–1 的小数（*100 为已用百分比），
// resets_at 为 ISO 字符串，used_value_usd / max_value_usd 为美元口径。
bool QueryZenMux(const SiteProfile& profile, SiteBalance& output) {
    std::wstring url = TrimUrl(profile.baseUrl);
    if (url.empty()) url = L"https://zenmux.ai";

    HttpResponse response;
    if (!SendRequest(url, MakeRequest(profile.credential), L"ZenMux 套餐接口", response, output)) return false;
    JsonValue root;
    if (!ParseBody(response, L"ZenMux 套餐接口", root, output)) return false;

    bool success = false;
    if (!query::ReadBool(root.Find("success"), success) || !success) {
        std::string message;
        query::ReadString(root.Find("message"), message);
        const std::wstring messageWide = WidenUtf8(message);
        output.error = L"ZenMux 套餐接口返回错误：" +
            (messageWide.empty() ? std::wstring(L"success 字段不是 true。") : messageWide);
        return false;
    }

    const JsonValue* data = root.Find("data");
    if (!data || data->kind != JsonValue::Kind::Object) {
        output.error = L"ZenMux 套餐响应缺少 data 字段。";
        return false;
    }

    std::vector<QuotaWindow> windows;
    const auto appendWindow = [&windows, data](const char* member, const wchar_t* label) {
        const JsonValue* quota = data->Find(member);
        if (!quota || quota->kind != JsonValue::Kind::Object) return;
        QuotaWindow window;
        window.label = label;
        window.utilization = ReadNumberOr(quota->Find("usage_percentage"), 0.0) * 100.0;
        long long resetMs = 0;
        if (ExtractResetMilliseconds(quota->Find("resets_at"), resetMs)) window.resetText = FormatResetTime(resetMs);
        double used = 0.0;
        double limit = 0.0;
        if (query::ReadNumber(quota->Find("used_value_usd"), used) &&
            query::ReadNumber(quota->Find("max_value_usd"), limit)) {
            window.hasUsd = true;
            window.usedUsd = used;
            window.limitUsd = limit;
        }
        windows.push_back(window);
    };
    appendWindow("quota_5_hour", L"5H");
    appendWindow("quota_7_day", L"7D");
    if (windows.empty()) {
        output.error = L"ZenMux 套餐响应缺少 data.quota_5_hour / data.quota_7_day 字段。";
        return false;
    }

    // 套餐名同 Rust：plan.tier (account_status)。
    std::wstring planName;
    std::string tier;
    if (query::ReadString(Field(Field(data, "plan"), "tier"), tier)) planName = WidenUtf8(tier);
    std::string accountStatus;
    if (query::ReadString(data->Find("account_status"), accountStatus)) {
        const std::wstring statusWide = WidenUtf8(accountStatus);
        if (!statusWide.empty()) {
            planName += planName.empty() ? statusWide : L" (" + statusWide + L")";
        }
    }
    return FinishPlan(windows, planName, output);
}

// ── OpenCode Go ────────────────────────────────────────────
//
// GET https://opencode.ai/zen/go/v1/usage（Bearer；注意不是 x-api-key）。
// usage.rolling / weekly / monthly 各带 percent（已用整数）与 resetsAt。
bool QueryOpenCodeGo(const SiteProfile& profile, SiteBalance& output) {
    std::wstring base = TrimUrl(profile.baseUrl);
    if (base.empty()) base = L"https://opencode.ai/zen/go";
    std::wstring url;
    if (EndsWithNoCase(base, L"/usage")) {
        url = base;
    } else if (EndsWithNoCase(base, L"/v1")) {
        url = base + L"/usage";
    } else {
        url = base + L"/v1/usage";
    }

    HttpResponse response;
    if (!SendRequest(url, MakeRequest(profile.credential), L"OpenCode Go 套餐接口", response, output)) return false;
    JsonValue root;
    if (!ParseBody(response, L"OpenCode Go 套餐接口", root, output)) return false;

    const JsonValue* usage = root.Find("usage");
    if (!usage || usage->kind != JsonValue::Kind::Object) {
        output.error = L"OpenCode Go 套餐响应缺少 usage 字段。";
        return false;
    }

    struct WindowSpec {
        const char* member;
        const wchar_t* label;
    };
    static const WindowSpec kWindows[] = {
        { "rolling", L"5H" },
        { "weekly", L"7D" },
        { "monthly", L"1M" },
    };

    std::vector<QuotaWindow> windows;
    for (const WindowSpec& spec : kWindows) {
        const JsonValue* window = usage->Find(spec.member);
        if (!window || window->kind != JsonValue::Kind::Object) continue;
        double percent = 0.0;
        if (!query::ReadNumber(window->Find("percent"), percent)) continue;  // 坏窗口跳过，不整体失败
        QuotaWindow quotaWindow;
        quotaWindow.label = spec.label;
        quotaWindow.utilization = percent;
        // percent == 0 时上游 resetsAt 是占位值（now+窗口时长），不展示倒计时。
        if (percent > 0.0) {
            long long resetMs = 0;
            if (ExtractResetMilliseconds(window->Find("resetsAt"), resetMs)) quotaWindow.resetText = FormatResetTime(resetMs);
        }
        windows.push_back(quotaWindow);
    }
    if (windows.empty()) {
        output.error = L"OpenCode Go 用量响应形态无法识别（usage.rolling/weekly/monthly 缺少可解析的 percent）。";
        return false;
    }
    return FinishPlan(windows, std::wstring(), output);
}

// ── Command Code ───────────────────────────────────────────
//
// 控制面固定在根域名（推理数据面才是 /provider、/provider/v1），故 baseUrl 为
// 预设的 https://api.commandcode.ai/provider 时取它的 origin：
//   GET {base}/alpha/whoami?limits=1
//   GET {base}/alpha/billing/credits[?orgId=...]
//   GET {base}/alpha/billing/subscriptions[?orgId=...]
//   GET {base}/alpha/usage/summary?since=<currentPeriodStart>&orgId=...
// 输出：windowLimits.fiveHour/weekly（used/cap 已用百分比）+ 月度
// totalCost / (totalCost + 剩余 credits)。
std::wstring UrlEncodeQueryValue(const std::wstring& value) {
    static const wchar_t kHex[] = L"0123456789ABCDEF";
    const std::string utf8 = query::NarrowUtf8(value);
    std::wstring encoded;
    for (const char raw : utf8) {
        const unsigned char ch = static_cast<unsigned char>(raw);
        const bool unreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '_' || ch == '~';
        if (unreserved) {
            encoded.push_back(static_cast<wchar_t>(ch));
        } else {
            encoded.push_back(L'%');
            encoded.push_back(kHex[ch >> 4]);
            encoded.push_back(kHex[ch & 0x0f]);
        }
    }
    return encoded;
}

std::wstring QueryParam(const wchar_t* name, const std::wstring& value) {
    return std::wstring(name) + L"=" + UrlEncodeQueryValue(value);
}

std::wstring UrlOrigin(const std::wstring& url) {
    const size_t schemeEnd = url.find(L"://");
    if (schemeEnd == std::wstring::npos) return url;
    const size_t pathStart = url.find(L'/', schemeEnd + 3);
    return pathStart == std::wstring::npos ? url : url.substr(0, pathStart);
}

std::wstring ResolveCommandCodeBase(const std::wstring& baseUrl) {
    const std::wstring base = TrimUrl(baseUrl);
    if (base.empty()) return L"https://api.commandcode.ai";
    if (ContainsNoCase(base, L"commandcode.ai")) return UrlOrigin(base);
    // 自定义 / 本地 mock 地址：当作控制面根地址直接使用。
    return base;
}

bool FetchCommandCodeJson(const std::wstring& url, const std::wstring& apiKey,
    JsonValue& root, SiteBalance& output) {
    HttpResponse response;
    if (!SendRequest(url, MakeRequest(apiKey), L"Command Code 接口", response, output)) return false;
    return ParseBody(response, L"Command Code 接口", root, output);
}

std::wstring CommandCodeQuerySuffix(const std::wstring& orgId) {
    if (orgId.empty()) return {};
    return L"?" + QueryParam(L"orgId", orgId);
}

bool ParseCommandCodeQuota(const JsonValue& creditsResponse, const JsonValue* subscriptionResponse,
    const JsonValue& summaryResponse, SiteBalance& output) {
    const JsonValue* creditRoot = creditsResponse.Find("credits");
    if (!creditRoot || creditRoot->kind != JsonValue::Kind::Object) {
        output.error = L"Command Code 额度响应缺少 credits 字段。";
        return false;
    }

    const double monthlyRemaining = std::max(0.0, ReadNumberOr(creditRoot->Find("monthlyCredits"), 0.0));
    const double purchasedRemaining = std::max(0.0, ReadNumberOr(creditRoot->Find("purchasedCredits"), 0.0));
    const double freeRemaining = std::max(0.0, ReadNumberOr(creditRoot->Find("freeCredits"), 0.0));
    const double totalRemaining = monthlyRemaining + purchasedRemaining + freeRemaining;

    double totalSpent = 0.0;
    if (!query::ReadNumber(summaryResponse.Find("totalCost"), totalSpent)) {
        output.error = L"Command Code 用量响应缺少 totalCost 字段。";
        return false;
    }
    totalSpent = std::max(0.0, totalSpent);
    const double totalPool = totalSpent + totalRemaining;

    const JsonValue* subscriptionData = subscriptionResponse ? subscriptionResponse->Find("data") : nullptr;
    long long periodEndMs = 0;
    const bool hasPeriodEnd = ExtractResetMilliseconds(Field(subscriptionData, "currentPeriodEnd"), periodEndMs);
    std::string planId;
    if (!query::ReadString(Field(subscriptionData, "planId"), planId) || planId.empty()) {
        query::ReadString(creditRoot->Find("planId"), planId);
    }

    std::vector<QuotaWindow> windows;
    // 实际响应把 windowLimits 放在 credits 同级（防御性兼容嵌套位置）。
    const JsonValue* windowLimits = creditsResponse.Find("windowLimits");
    if (!windowLimits) windowLimits = creditRoot->Find("windowLimits");
    if (windowLimits && windowLimits->kind == JsonValue::Kind::Object) {
        bool limited = false;
        if (query::ReadBool(windowLimits->Find("limited"), limited) && limited) {
            const auto appendWindow = [&windows, windowLimits](const char* member, const wchar_t* label) {
                const JsonValue* window = windowLimits->Find(member);
                double used = 0.0;
                double cap = 0.0;
                if (!window || !query::ReadNumber(window->Find("used"), used) ||
                    !query::ReadNumber(window->Find("cap"), cap) || cap <= 0.0) {
                    return;  // 坏窗口跳过
                }
                QuotaWindow quotaWindow;
                quotaWindow.label = label;
                quotaWindow.utilization = used / cap * 100.0;
                long long resetMs = 0;
                if (ExtractResetMilliseconds(window->Find("resetAt"), resetMs)) {
                    quotaWindow.resetText = FormatResetTime(resetMs);
                }
                windows.push_back(quotaWindow);
            };
            appendWindow("fiveHour", L"5H");
            appendWindow("weekly", L"7D");
        }
    }

    // 月度窗口始终存在：已用 = totalCost，池子 = totalCost + 剩余 credits。
    QuotaWindow monthlyWindow;
    monthlyWindow.label = L"1M";
    monthlyWindow.utilization = totalPool > 0.0 ? totalSpent / totalPool * 100.0 : 0.0;
    if (hasPeriodEnd) monthlyWindow.resetText = FormatResetTime(periodEndMs);
    windows.push_back(monthlyWindow);

    return FinishPlan(windows, WidenUtf8(planId), output);
}

bool QueryCommandCode(const SiteProfile& profile, SiteBalance& output) {
    const std::wstring base = ResolveCommandCodeBase(profile.baseUrl);
    const std::wstring apiKey = profile.credential;

    JsonValue whoami;
    if (!FetchCommandCodeJson(base + L"/alpha/whoami?limits=1", apiKey, whoami, output)) return false;

    std::string orgIdUtf8;
    query::ReadString(Field(whoami.Find("org"), "id"), orgIdUtf8);
    const std::wstring orgId = WidenUtf8(orgIdUtf8);
    const std::wstring orgSuffix = CommandCodeQuerySuffix(orgId);

    JsonValue credits;
    if (!FetchCommandCodeJson(base + L"/alpha/billing/credits" + orgSuffix, apiKey, credits, output)) return false;

    JsonValue subscription;
    if (!FetchCommandCodeJson(base + L"/alpha/billing/subscriptions" + orgSuffix, apiKey, subscription, output)) return false;

    std::string periodStart;
    query::ReadString(Field(subscription.Find("data"), "currentPeriodStart"), periodStart);
    std::wstring summaryQuery;
    if (!periodStart.empty()) summaryQuery += QueryParam(L"since", WidenUtf8(periodStart));
    if (!orgId.empty()) {
        if (!summaryQuery.empty()) summaryQuery += L"&";
        summaryQuery += QueryParam(L"orgId", orgId);
    }
    JsonValue summary;
    if (!FetchCommandCodeJson(base + L"/alpha/usage/summary" + (summaryQuery.empty() ? L"" : L"?" + summaryQuery),
        apiKey, summary, output)) {
        return false;
    }

    return ParseCommandCodeQuota(credits, &subscription, summary, output);
}

} // namespace

// ── 对外接口 ────────────────────────────────────────────────

bool IsOfficialPlanProvider(SiteProvider provider) {
    switch (provider) {
    case SiteProvider::KimiPlan:
    case SiteProvider::ZhipuPlan:
    case SiteProvider::MiniMaxPlanCN:
    case SiteProvider::MiniMaxPlanEN:
    case SiteProvider::ZenMuxPlan:
    case SiteProvider::OpenCodeGoPlan:
    case SiteProvider::CommandCodePlan:
        return true;
    default:
        return false;
    }
}

bool QueryOfficialPlan(const SiteProfile& profile, SiteBalance& output) {
    output.error.clear();
    if (!IsOfficialPlanProvider(profile.adapter)) {
        output.error = L"不支持的套餐适配器。";
        return false;
    }
    if (TrimAsciiSpace(profile.credential).empty()) {
        output.error = L"请填写 API Key。";
        return false;
    }

    switch (profile.adapter) {
    case SiteProvider::KimiPlan:
        return QueryKimi(profile, output);
    case SiteProvider::ZhipuPlan:
        return QueryZhipu(profile, output);
    case SiteProvider::MiniMaxPlanCN:
        return QueryMiniMax(profile, false, output);
    case SiteProvider::MiniMaxPlanEN:
        return QueryMiniMax(profile, true, output);
    case SiteProvider::ZenMuxPlan:
        return QueryZenMux(profile, output);
    case SiteProvider::OpenCodeGoPlan:
        return QueryOpenCodeGo(profile, output);
    case SiteProvider::CommandCodePlan:
        return QueryCommandCode(profile, output);
    default:
        output.error = L"不支持的套餐适配器。";
        return false;
    }
}

} // namespace aiapi
