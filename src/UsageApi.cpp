#include "UsageApi.h"
#include "OfficialBalance.h"
#include "OfficialPlan.h"
#include "OfficialSubscription.h"
#include "QuerySupport.h"

#include <windows.h>
#include <winhttp.h>
#include <jsrt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <iterator>
#include <locale>
#include <map>
#include <mutex>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace aiapi {
namespace {

using query::ExtractJsonMessage;
using query::JsonValue;
using query::Member;
using query::NarrowUtf8;
using query::ParseDocument;
using query::ReadBool;
using query::ReadInt;
using query::ReadNumber;
using query::ReadString;
using query::WidenUtf8;

constexpr size_t kMaxResponseBytes = 512 * 1024;
constexpr size_t kMaxCredentialChars = 4096;

class InternetHandle {
public:
    explicit InternetHandle(HINTERNET handle = nullptr) : handle_(handle) {}
    ~InternetHandle() { if (handle_) WinHttpCloseHandle(handle_); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    HINTERNET get() const { return handle_; }

private:
    HINTERNET handle_;
};

bool RunCustomJavaScript(const std::wstring& script, std::wstring& output, std::wstring& error) {
    constexpr size_t memoryLimit = 16 * 1024 * 1024;
    constexpr auto executionLimit = std::chrono::seconds(5);
    JsRuntimeHandle runtime = JS_INVALID_RUNTIME_HANDLE;
    JsContextRef context = JS_INVALID_REFERENCE;
    const JsRuntimeAttributes attributes = static_cast<JsRuntimeAttributes>(
        JsRuntimeAttributeDisableBackgroundWork | JsRuntimeAttributeAllowScriptInterrupt |
        JsRuntimeAttributeDisableNativeCodeGeneration | JsRuntimeAttributeDisableEval);
    JsErrorCode code = JsCreateRuntime(attributes, nullptr, &runtime);
    if (code != JsNoError) {
        error = L"无法创建 JavaScript 引擎。";
        return false;
    }

    bool runtimeReady = JsSetRuntimeMemoryLimit(runtime, memoryLimit) == JsNoError &&
        JsCreateContext(runtime, &context) == JsNoError &&
        JsSetCurrentContext(context) == JsNoError;
    if (!runtimeReady) {
        if (context != JS_INVALID_REFERENCE) JsSetCurrentContext(JS_INVALID_REFERENCE);
        JsDisposeRuntime(runtime);
        error = L"无法初始化 JavaScript 上下文。";
        return false;
    }

    std::mutex watchdogMutex;
    std::condition_variable watchdogCondition;
    bool finished = false;
    std::atomic<bool> timedOut{ false };
    std::thread watchdog([&]() {
        std::unique_lock<std::mutex> lock(watchdogMutex);
        if (!watchdogCondition.wait_for(lock, executionLimit, [&]() { return finished; })) {
            timedOut.store(true);
            lock.unlock();
            JsDisableRuntimeExecution(runtime);
        }
    });

    JsValueRef result = JS_INVALID_REFERENCE;
    code = JsRunScript(script.c_str(), 0, L"trafficmonitor-custom-extractor.js", &result);
    {
        std::lock_guard<std::mutex> lock(watchdogMutex);
        finished = true;
    }
    watchdogCondition.notify_one();
    watchdog.join();

    bool okay = code == JsNoError && !timedOut.load();
    if (okay) {
        JsValueRef resultString = JS_INVALID_REFERENCE;
        const wchar_t* text = nullptr;
        size_t length = 0;
        okay = JsConvertValueToString(result, &resultString) == JsNoError &&
            JsStringToPointer(resultString, &text, &length) == JsNoError;
        if (okay) output.assign(text, length);
    } else if (timedOut.load()) {
        error = L"自定义提取器运行超过 5 秒，已中断。";
    } else {
        JsValueRef exception = JS_INVALID_REFERENCE;
        JsValueRef exceptionString = JS_INVALID_REFERENCE;
        const wchar_t* text = nullptr;
        size_t length = 0;
        if (JsGetAndClearException(&exception) == JsNoError &&
            JsConvertValueToString(exception, &exceptionString) == JsNoError &&
            JsStringToPointer(exceptionString, &text, &length) == JsNoError) {
            error = L"自定义提取器错误：" + std::wstring(text, length);
        } else {
            error = L"自定义提取器执行失败。";
        }
    }

    JsSetCurrentContext(JS_INVALID_REFERENCE);
    JsDisposeRuntime(runtime);
    return okay;
}

std::wstring EscapeJsStringContents(const std::wstring& value) {
    std::wstring result;
    for (wchar_t ch : value) {
        switch (ch) {
        case L'\\': result += L"\\\\"; break;
        case L'"': result += L"\\\""; break;
        case L'\n': result += L"\\n"; break;
        case L'\r': result += L"\\r"; break;
        case L'\t': result += L"\\t"; break;
        default:
            if (ch < 0x20) {
                wchar_t escaped[7]{};
                swprintf_s(escaped, std::size(escaped), L"\\u%04X", static_cast<unsigned int>(ch));
                result += escaped;
            } else {
                result.push_back(ch);
            }
        }
    }
    return result;
}

std::wstring JsStringLiteral(const std::wstring& value) {
    return L"\"" + EscapeJsStringContents(value) + L"\"";
}

void ReplaceAll(std::wstring& value, const wchar_t* placeholder, const std::wstring& replacement) {
    const std::wstring key(placeholder);
    size_t position = 0;
    while ((position = value.find(key, position)) != std::wstring::npos) {
        value.replace(position, key.size(), replacement);
        position += replacement.size();
    }
}

std::wstring ExpandCustomTemplate(const SiteProfile& profile) {
    std::wstring script = profile.customScript;
    ReplaceAll(script, L"{{baseUrl}}", EscapeJsStringContents(profile.baseUrl));
    ReplaceAll(script, L"{{apiKey}}", EscapeJsStringContents(profile.credential));
    ReplaceAll(script, L"{{accessToken}}", EscapeJsStringContents(profile.credential));
    ReplaceAll(script, L"{{userId}}", EscapeJsStringContents(profile.userId));
    return script;
}

std::wstring TrimUrl(std::wstring value) {
    while (!value.empty() && iswspace(value.back())) value.pop_back();
    size_t first = 0;
    while (first < value.size() && iswspace(value[first])) ++first;
    value.erase(0, first);
    while (!value.empty() && value.back() == L'/') value.pop_back();
    return value;
}

bool EndsWithInsensitive(const std::wstring& value, const wchar_t* suffix) {
    const size_t suffixLength = std::wcslen(suffix);
    if (value.size() < suffixLength) return false;
    return CompareStringOrdinal(value.data() + value.size() - suffixLength,
        static_cast<int>(suffixLength), suffix, static_cast<int>(suffixLength), TRUE) == CSTR_EQUAL;
}

std::wstring AdapterUrl(const SiteProfile& profile, const wchar_t* endpoint) {
    std::wstring base = TrimUrl(profile.baseUrl);
    if (profile.adapter == SiteAdapter::NewAPI && EndsWithInsensitive(base, L"/api")) base.resize(base.size() - 4);
    if (profile.adapter == SiteAdapter::Sub2API && EndsWithInsensitive(base, L"/v1")) base.resize(base.size() - 3);
    if (profile.adapter == SiteAdapter::AIAPI && EndsWithInsensitive(base, L"/api")) base.resize(base.size() - 4);
    return base + endpoint;
}

class WipeWideStringOnExit {
public:
    explicit WipeWideStringOnExit(std::wstring& value) : value_(value) {}
    ~WipeWideStringOnExit() {
        if (!value_.empty()) SecureZeroMemory(value_.data(), value_.size() * sizeof(wchar_t));
    }
    WipeWideStringOnExit(const WipeWideStringOnExit&) = delete;
    WipeWideStringOnExit& operator=(const WipeWideStringOnExit&) = delete;
private:
    std::wstring& value_;
};

bool IsSiteSessionCredential(const std::wstring& credential) {
    return credential.rfind(L"site-session=", 0) == 0 || credential.rfind(L"sts_", 0) == 0;
}

bool NormalizeSiteSession(const std::wstring& credential, std::wstring& token) {
    token = credential;
    constexpr wchar_t prefix[] = L"site-session=";
    if (token.rfind(prefix, 0) == 0) token.erase(0, std::size(prefix) - 1);
    if (token.empty() || token.size() > kMaxCredentialChars) return false;
    return std::all_of(token.begin(), token.end(), [](wchar_t c) {
        return c >= 0x21 && c <= 0x7e && c != L';' && c != L'\r' && c != L'\n';
    });
}

struct HttpResult {
    bool success = false;
    unsigned int status = 0;
    std::string body;
    std::wstring error;
};

HttpResult GetJson(const std::wstring& url, const std::wstring& credential,
    const std::wstring& userId, bool siteSession, bool newApiTemplate = false,
    const std::wstring& method = L"GET", const std::wstring& requestBody = L"",
    const std::map<std::wstring, std::wstring>* customHeaders = nullptr, bool allowQuery = false) {
    HttpResult result;
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUserNameLength = static_cast<DWORD>(-1);
    components.dwPasswordLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components) ||
        (components.nScheme != INTERNET_SCHEME_HTTPS && components.nScheme != INTERNET_SCHEME_HTTP) ||
        components.dwHostNameLength == 0 || components.dwUserNameLength != 0 ||
        components.dwPasswordLength != 0 || (!allowQuery && components.dwExtraInfoLength != 0)) {
        result.error = L"请求地址无效；请使用不含用户凭据的 HTTP(S) URL。";
        return result;
    }
    if (customHeaders && components.nScheme != INTERNET_SCHEME_HTTPS) {
        result.error = L"自定义接口必须使用 HTTPS，避免在网络中泄露请求凭据。";
        return result;
    }
    if (method != L"GET" && method != L"POST") {
        result.error = L"自定义接口仅支持 GET 或 POST。";
        return result;
    }
    if (siteSession && (components.nScheme != INTERNET_SCHEME_HTTPS ||
        CompareStringOrdinal(components.lpszHostName, static_cast<int>(components.dwHostNameLength),
            L"AIAPI 站点", -1, TRUE) != CSTR_EQUAL)) {
        result.error = L"站点会话只允许通过 HTTPS 发送到 AIAPI 站点。";
        return result;
    }

    std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (path.empty()) path = L"/";
    if (allowQuery && components.dwExtraInfoLength > 0) path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    const DWORD requestFlags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;

    InternetHandle session(WinHttpOpen(L"TrafficMonitor-AIAPIQuota/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.get()) {
        result.error = L"无法初始化 HTTP 客户端。";
        return result;
    }
    WinHttpSetTimeouts(session.get(), 4000, 4000, 8000, 8000);

    InternetHandle connection(WinHttpConnect(session.get(), host.c_str(), components.nPort, 0));
    if (!connection.get()) {
        result.error = L"无法连接站点。";
        return result;
    }
    InternetHandle request(WinHttpOpenRequest(connection.get(), method.c_str(), path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, requestFlags));
    if (!request.get()) {
        result.error = L"无法创建 HTTP 请求。";
        return result;
    }

    DWORD disabledFeatures = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
    if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabledFeatures, sizeof(disabledFeatures))) {
        result.error = L"无法禁用重定向和 Cookie，已取消请求。";
        return result;
    }

    std::wstring headers = customHeaders ? L"" : L"Accept: application/json\r\nCache-Control: no-store\r\nUser-Agent: cc-switch/1.0\r\n";
    WipeWideStringOnExit wipeHeaders(headers);
    if (customHeaders) {
        for (const auto& [name, value] : *customHeaders) {
            if (name.empty() || name.find_first_of(L":\r\n") != std::wstring::npos ||
                value.find_first_of(L"\r\n") != std::wstring::npos) {
                result.error = L"自定义请求头格式无效。";
                return result;
            }
            headers += name + L": " + value + L"\r\n";
        }
    } else {
        if (newApiTemplate) headers += L"Content-Type: application/json\r\n";
        if (siteSession) {
            std::wstring token;
            WipeWideStringOnExit wipeToken(token);
            if (!NormalizeSiteSession(credential, token)) {
                result.error = L"site-session 格式无效。";
                return result;
            }
            headers += L"Cookie: site-session=" + token + L"\r\n";
            if (!token.empty()) SecureZeroMemory(token.data(), token.size() * sizeof(wchar_t));
            token.clear();
        } else if (!credential.empty()) {
            headers += L"Authorization: Bearer " + credential + L"\r\n";
        }
        if (!userId.empty()) headers += L"New-Api-User: " + userId + L"\r\n";
    }
    const BOOL headersAdded = headers.empty() || WinHttpAddRequestHeaders(request.get(), headers.c_str(),
        static_cast<DWORD>(headers.size()), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    SecureZeroMemory(headers.data(), headers.size() * sizeof(wchar_t));

    std::string bodyUtf8 = NarrowUtf8(requestBody);
    struct BodyWiper {
        std::string& value;
        ~BodyWiper() { if (!value.empty()) SecureZeroMemory(value.data(), value.size()); }
    } wipeBody{ bodyUtf8 };
    if ((!requestBody.empty() && bodyUtf8.empty()) || bodyUtf8.size() > MAXDWORD) {
        result.error = L"自定义请求 body 无法编码或过长。";
        return result;
    }
    const DWORD bodyLength = static_cast<DWORD>(bodyUtf8.size());
    const LPVOID bodyData = bodyLength == 0 ? WINHTTP_NO_REQUEST_DATA : bodyUtf8.data();
    if (!headersAdded ||
        !WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, bodyData, bodyLength,
            bodyLength, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        result.error = L"网络请求失败。";
        return result;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        result.error = L"无法读取 HTTP 状态。";
        return result;
    }
    result.status = statusCode;

    // 先读取响应体：认证失败和 4xx/5xx 的 JSON 里往往带有可直接展示的原因，
    // 例如 AIAPI 的 {"ok":false,"message":"API key is invalid"}。
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            result.error = L"读取响应失败。";
            return result;
        }
        if (available == 0) break;
        if (result.body.size() + available > kMaxResponseBytes) {
            result.error = L"响应超过大小限制。";
            return result;
        }
        std::vector<char> buffer(std::min<DWORD>(available, 8192));
        DWORD bytesRead = 0;
        if (!WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead)) {
            result.error = L"读取响应失败。";
            return result;
        }
        if (bytesRead == 0) break;
        result.body.append(buffer.data(), bytesRead);
    }

    std::wstring responseMessage;
    const bool hasMessage = ExtractJsonMessage(result.body, responseMessage);
    if (statusCode == 401 || statusCode == 403) {
        result.error = L"认证失败 (HTTP " + std::to_wstring(statusCode) + L")";
        result.error += hasMessage ? L"：" + responseMessage : L"。";
        return result;
    }
    if (statusCode < 200 || statusCode >= 300) {
        result.error = L"站点返回 HTTP " + std::to_wstring(statusCode);
        result.error += hasMessage ? L"：" + responseMessage : L"。";
        return result;
    }
    result.success = true;
    return result;
}

bool ParseAIAPI(const std::string& body, SiteBalance& output) {
    JsonValue root;
    if (!ParseDocument(body, root)) {
        output.error = L"AIAPI 返回的内容不是有效 JSON。";
        return false;
    }
    const JsonValue* ok = Member(&root, "ok");
    if (ok && ok->kind == JsonValue::Kind::Boolean && !ok->boolean) {
        output.error = L"AIAPI 账户汇总接口拒绝了请求。";
        return false;
    }

    const JsonValue* data = Member(&root, "data");
    if (!data) data = &root;
    const JsonValue* nestedData = Member(data, "data");
    if (nestedData) data = nestedData;
    const JsonValue* summary = Member(data, "assetSummary");
    if (!summary) summary = data;
    const JsonValue* readModel = Member(summary, "assetReadModel");
    if (!readModel) readModel = summary;

    double walletBalance = 0.0;
    bool found = ReadNumber(Member(readModel, "walletBalanceUsd"), walletBalance) ||
        ReadNumber(Member(summary, "walletBalanceUsd"), walletBalance) ||
        ReadNumber(Member(data, "walletBalanceUsd"), walletBalance);
    const JsonValue* wallet = Member(data, "wallet");
    if (!found) found = ReadNumber(Member(wallet, "balanceUsd"), walletBalance);
    if (!found || !std::isfinite(walletBalance) || walletBalance < 0.0) {
        output.error = L"AIAPI 会话响应中没有账户钱包余额字段，请检查站点接口版本。";
        return false;
    }
    output.remaining = walletBalance;
    output.accountBalance = true;

    // The public dashboard formats this read-model value with its CNY formatter directly.
    output.unit = "CNY";
    output.detail = L"官网账户余额（站点会话）";
    return true;
}

// /v1/usage 的 available.limitedBy → 显示类别。CC-Switch 脚本里用完整名称，
// TrafficMonitor 的显示项目标签要尽量短，所以这里给出短标签。
std::wstring AIAPIScopeLabel(const std::string& limitedBy) {
    if (limitedBy == "key_five_hour") return L"5 小时限额";
    if (limitedBy == "key_daily") return L"日限额";
    if (limitedBy == "key_weekly") return L"周限额";
    if (limitedBy == "key_monthly") return L"月限额";
    if (limitedBy == "key_total") return L"总限额";
    if (limitedBy == "balance") return L"余额";
    if (limitedBy == "subscription") return L"套餐";
    return L"";
}

// limitedBy → limits 里对应的窗口名，用于取该口径的 limit/used。
const char* AIAPILimitWindow(const std::string& limitedBy) {
    if (limitedBy == "key_five_hour") return "fiveHour";
    if (limitedBy == "key_daily") return "daily";
    if (limitedBy == "key_weekly") return "weekly";
    if (limitedBy == "key_monthly") return "monthly";
    if (limitedBy == "key_total") return "total";
    return "daily";
}

std::wstring FormatNumber(double value, int precision) {
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(precision) << value;
    return stream.str();
}

// 对应 CC-Switch 的 AIAPI 模板：GET /v1/usage，Authorization: Bearer <API Key>。
// 响应：{ ok, message, data: { currency, available{amount, limitedBy},
//         limits{daily{limit, remaining, used}}, today{cost, calls}, key{name} } }
bool ParseAIAPIKeyUsage(const std::string& body, SiteBalance& output) {
    JsonValue root;
    if (!ParseDocument(body, root)) {
        output.error = L"AIAPI /v1/usage 返回的内容不是有效 JSON。";
        return false;
    }
    const JsonValue* ok = Member(&root, "ok");
    if (ok && ok->kind == JsonValue::Kind::Boolean && !ok->boolean) {
        std::string message;
        if (ReadString(Member(&root, "message"), message) && !message.empty()) {
            output.error = WidenUtf8(message);
        } else {
            output.error = L"AIAPI 拒绝了此 API Key。";
        }
        return false;
    }
    const JsonValue* data = Member(&root, "data");
    if (!data || data->kind != JsonValue::Kind::Object) {
        output.error = L"AIAPI /v1/usage 响应缺少 data 对象。";
        return false;
    }

    output.unit = "CNY";
    std::string unit;
    if (ReadString(Member(data, "currency"), unit) && !unit.empty()) output.unit = unit;

    const JsonValue* available = Member(data, "available");
    const JsonValue* limits = Member(data, "limits");
    const JsonValue* today = Member(data, "today");
    const JsonValue* key = Member(data, "key");

    std::string limitedBy;
    ReadString(Member(available, "limitedBy"), limitedBy);
    output.scopeLabel = AIAPIScopeLabel(limitedBy);
    // limitedBy 为 null/缺失（线上钱包口径就是这样）时按钱包余额处理。
    if (output.scopeLabel.empty()) output.scopeLabel = L"余额";
    output.accountBalance = limitedBy.empty() || limitedBy == "balance";

    // available.amount 优先（0 也是有效值）；缺省时退回当前口径窗口的 remaining。
    const JsonValue* activeWindow = Member(limits, AIAPILimitWindow(limitedBy));
    double remaining = 0.0;
    if (!ReadNumber(Member(available, "amount"), remaining) &&
        !ReadNumber(Member(activeWindow, "remaining"), remaining)) {
        output.error = L"AIAPI /v1/usage 响应没有有效的 available.amount 或限额剩余。";
        return false;
    }
    if (!std::isfinite(remaining) || remaining < 0.0) {
        output.error = L"AIAPI /v1/usage 返回的剩余额度无效。";
        return false;
    }
    output.remaining = remaining;

    // 当前口径窗口的 limit > 0 时记录已用/总额，tooltip 里附“已用 x/y”。
    double windowLimit = 0.0;
    double windowUsed = 0.0;
    const bool hasWindowLimit = ReadNumber(Member(activeWindow, "limit"), windowLimit) &&
        std::isfinite(windowLimit) && windowLimit > 0.0;
    if (hasWindowLimit) {
        output.hasTotal = true;
        output.total = windowLimit;
        output.hasUsed = ReadNumber(Member(activeWindow, "used"), windowUsed) &&
            std::isfinite(windowUsed) && windowUsed >= 0.0;
        if (output.hasUsed) output.used = windowUsed;
    }

    // planName/extra：脚本里 planName 含类别，这里类别已单独作为标签，故只保留 Key 名称。
    std::wstring planName = L"账户";
    std::string keyName;
    if (ReadString(Member(key, "name"), keyName) && !keyName.empty()) {
        planName = L"Key " + WidenUtf8(keyName);
    }
    output.detail = planName;

    std::wstring extra = planName;
    double cost = 0.0;
    double calls = 0.0;
    const bool hasCost = ReadNumber(Member(today, "cost"), cost) && std::isfinite(cost);
    const bool hasCalls = ReadNumber(Member(today, "calls"), calls) && std::isfinite(calls) && calls >= 0.0;
    if (hasCost || hasCalls) {
        extra += L"；今日 ";
        if (hasCost) extra += FormatNumber(cost, 2);
        if (hasCalls) {
            if (hasCost) extra += L" · ";
            extra += FormatNumber(calls, 0) + L" 次";
        }
    }
    if (!limitedBy.empty() && !output.scopeLabel.empty() && limitedBy != "balance") {
        extra += L"；" + output.scopeLabel;
    }
    if (output.hasTotal && output.hasUsed) {
        extra += L"已用 " + FormatNumber(output.used, 2) + L"/" + FormatNumber(output.total, 2);
    }
    output.extra = extra;
    return true;
}

bool ParseNewAPI(const std::string& body, double quotaPerUnit, SiteBalance& output) {
    JsonValue root;
    if (!ParseDocument(body, root)) return false;
    const JsonValue* success = Member(&root, "success");
    const JsonValue* data = Member(&root, "data");
    if (!success || success->kind != JsonValue::Kind::Boolean || !success->boolean || !data) {
        std::string message;
        if (ReadString(Member(&root, "message"), message)) output.error = WidenUtf8(message);
        return false;
    }
    double quota = 0.0;
    if (!ReadNumber(Member(data, "quota"), quota) || quota < 0.0 || quotaPerUnit <= 0.0) return false;
    double usedQuota = 0.0;
    output.hasUsed = ReadNumber(Member(data, "used_quota"), usedQuota) && usedQuota >= 0.0;
    output.remaining = quota / quotaPerUnit;
    output.accountBalance = true;
    output.used = output.hasUsed ? usedQuota / quotaPerUnit : 0.0;
    output.total = output.hasUsed ? (quota + usedQuota) / quotaPerUnit : 0.0;
    output.hasTotal = output.hasUsed;
    std::string group;
    if (ReadString(Member(data, "group"), group)) output.detail = WidenUtf8(group);
    return true;
}

bool ParseSub2API(const std::string& body, SiteBalance& output) {
    JsonValue root;
    if (!ParseDocument(body, root)) return false;
    const JsonValue* valid = Member(&root, "isValid");
    if (valid && valid->kind == JsonValue::Kind::Boolean && !valid->boolean) {
        std::string status;
        if (ReadString(Member(&root, "status"), status)) output.error = WidenUtf8(status);
        else output.error = L"Sub2API 将此凭据标记为无效。";
        return false;
    }

    std::string unit;
    ReadString(Member(&root, "unit"), unit);
    const JsonValue* quota = Member(&root, "quota");
    if (unit.empty()) ReadString(Member(quota, "unit"), unit);
    if (!unit.empty()) output.unit = unit;

    double remaining = 0.0;
    if (!ReadNumber(Member(&root, "remaining"), remaining) || remaining < 0.0) return false;
    output.remaining = remaining;

    double limit = 0.0;
    double used = 0.0;
    const bool hasLimit = ReadNumber(Member(quota, "limit"), limit) && limit > 0.0;
    if (hasLimit) {
        output.hasTotal = true;
        output.total = limit;
        output.hasUsed = ReadNumber(Member(quota, "used"), used) && used >= 0.0;
        if (output.hasUsed) output.used = used;
    }
    std::string planName;
    if (ReadString(Member(&root, "planName"), planName)) {
        output.detail = WidenUtf8(planName);
        output.accountBalance = planName == "钱包余额" && !hasLimit;
        if (!planName.empty() && !output.accountBalance && !hasLimit) output.packageSummary = WidenUtf8(planName);
    }
    return true;
}

std::wstring EndpointError(const HttpResult& response, const wchar_t* endpoint);

bool ParseCustomResult(const std::string& body, SiteBalance& output) {
    JsonValue root;
    if (!ParseDocument(body, root)) return false;
    const JsonValue* valid = Member(&root, "isValid");
    if (valid && valid->kind == JsonValue::Kind::Boolean && !valid->boolean) {
        std::string message;
        if (ReadString(Member(&root, "invalidMessage"), message)) output.error = WidenUtf8(message);
        else output.error = L"自定义接口返回 isValid=false。";
        return false;
    }
    if (!ReadNumber(Member(&root, "remaining"), output.remaining) ||
        !std::isfinite(output.remaining) || output.remaining < 0.0) {
        output.error = L"自定义提取器必须返回非负数 remaining。";
        return false;
    }
    std::string value;
    if (ReadString(Member(&root, "unit"), value) && !value.empty()) output.unit = value;
    if (ReadString(Member(&root, "planName"), value)) output.detail = WidenUtf8(value);
    if (ReadString(Member(&root, "extra"), value) && !value.empty()) {
        if (!output.detail.empty()) output.detail += L"，";
        output.detail += WidenUtf8(value);
    }
    output.hasUsed = ReadNumber(Member(&root, "used"), output.used) && output.used >= 0.0;
    output.hasTotal = ReadNumber(Member(&root, "total"), output.total) && output.total >= 0.0;
    std::string extra;
    if (ReadString(Member(&root, "extra"), extra) && !extra.empty()) output.packageSummary = WidenUtf8(extra);
    else if (!output.detail.empty() && output.detail != L"钱包余额" && !(output.hasUsed && output.hasTotal)) {
        output.packageSummary = output.detail;
    }
    if (output.hasTotal && output.hasUsed) output.accountBalance = false;
    else if (output.detail == L"钱包余额") output.accountBalance = true;
    return true;
}

bool QueryCustomSite(const SiteProfile& profile, SiteBalance& output) {
    if (profile.customScript.empty() || profile.customScript.size() > 16384) {
        output.error = L"请在“自定义配置”中填写有效提取器脚本。";
        return false;
    }
    std::wstring script = ExpandCustomTemplate(profile);
    WipeWideStringOnExit wipeScript(script);
    const std::wstring requestScript = L"var __ccCfg = (" + script + L"); var __req = __ccCfg.request || {}; "
        L"if (__req.body && typeof __req.body === 'object') __req.body = JSON.stringify(__req.body); "
        L"JSON.stringify(__req);";
    std::wstring requestJson;
    if (!RunCustomJavaScript(requestScript, requestJson, output.error)) return false;
    std::string requestUtf8 = NarrowUtf8(requestJson);
    JsonValue request;
    const bool parsed = ParseDocument(requestUtf8, request);
    if (!requestUtf8.empty()) SecureZeroMemory(requestUtf8.data(), requestUtf8.size());
    if (!parsed) {
        output.error = L"自定义脚本没有返回有效 request 对象。";
        return false;
    }

    std::string urlUtf8, methodUtf8, bodyUtf8;
    if (!ReadString(Member(&request, "url"), urlUtf8) || urlUtf8.empty()) {
        output.error = L"自定义 request.url 不能为空。";
        return false;
    }
    ReadString(Member(&request, "method"), methodUtf8);
    if (methodUtf8.empty()) methodUtf8 = "GET";
    ReadString(Member(&request, "body"), bodyUtf8);
    std::wstring requestUrl = WidenUtf8(urlUtf8);
    const std::wstring baseUrl = TrimUrl(profile.baseUrl);
    if (requestUrl.rfind(L"https://", 0) != 0 && requestUrl.rfind(L"http://", 0) != 0) {
        if (baseUrl.empty()) {
            output.error = L"相对 request.url 需要填写 Base URL。";
            return false;
        }
        if (requestUrl.empty() || requestUrl.front() != L'/') requestUrl.insert(requestUrl.begin(), L'/');
        requestUrl = baseUrl + requestUrl;
    }

    std::map<std::wstring, std::wstring> headers;
    const JsonValue* requestHeaders = Member(&request, "headers");
    if (requestHeaders && requestHeaders->kind == JsonValue::Kind::Object) {
        for (const auto& header : requestHeaders->object) {
            if (header.second.kind != JsonValue::Kind::String) {
                output.error = L"自定义 request.headers 的值必须是字符串。";
                return false;
            }
            headers.emplace(WidenUtf8(header.first), WidenUtf8(header.second.text));
        }
    }
    std::wstring method = WidenUtf8(methodUtf8);
    std::transform(method.begin(), method.end(), method.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towupper(ch)); });
    const std::wstring bodyWide = WidenUtf8(bodyUtf8);
    const HttpResult response = GetJson(requestUrl, L"", L"", false, false,
        method, bodyWide, &headers, true);
    if (!response.success) {
        output.error = EndpointError(response, L"自定义接口");
        return false;
    }

    const std::wstring responseWide = WidenUtf8(response.body);
    const std::wstring extractorScript = L"(function(){ var __ccCfg = (" + script +
        L"); var response = JSON.parse(" + JsStringLiteral(responseWide) +
        L"); if (typeof __ccCfg.extractor !== 'function') throw new Error('extractor must be a function'); "
        L"return JSON.stringify(__ccCfg.extractor(response)); })();";
    std::wstring extracted;
    if (!RunCustomJavaScript(extractorScript, extracted, output.error)) return false;
    std::string extractedUtf8 = NarrowUtf8(extracted);
    const bool resultParsed = ParseCustomResult(extractedUtf8, output);
    if (!extractedUtf8.empty()) SecureZeroMemory(extractedUtf8.data(), extractedUtf8.size());
    if (!resultParsed) return false;
    output.adapter = SiteAdapter::Custom;
    return true;
}

std::wstring EndpointError(const HttpResult& response, const wchar_t* endpoint) {
    return std::wstring(endpoint) + L"：" + response.error;
}

} // namespace

SiteBalance FetchSiteBalance(const SiteProfile& profile) {
    SiteBalance output;
    output.id = profile.id;
    output.adapter = profile.adapter;
    output.name = profile.name;
    if (!profile.enabled) {
        output.error = L"此站点已停用。";
        return output;
    }
    if (profile.adapter == SiteAdapter::Custom) {
        if (!QueryCustomSite(profile, output)) return output;
        output.success = true;
        return output;
    }

    // 官方服务商（CC-Switch balance / coding_plan / subscription 三类服务的移植）。
    // 各自负责校验自己的必填项并给出可读错误。
    if (IsOfficialBalanceProvider(profile.adapter)) {
        if (!QueryOfficialBalance(profile, output)) {
            if (output.error.empty()) output.error = L"官方余额查询失败。";
        } else output.success = true;
        return output;
    }
    if (IsOfficialPlanProvider(profile.adapter)) {
        if (!QueryOfficialPlan(profile, output)) {
            if (output.error.empty()) output.error = L"套餐额度查询失败。";
        } else output.success = true;
        return output;
    }
    if (IsOfficialSubscriptionProvider(profile.adapter)) {
        if (!QueryOfficialSubscription(profile, output)) {
            if (output.error.empty()) output.error = L"官方订阅额度查询失败。";
        } else output.success = true;
        return output;
    }

    if (profile.baseUrl.empty() || profile.credential.empty()) {
        output.error = L"请在插件选项中填写站点地址和凭据。";
        return output;
    }
    if (profile.adapter == SiteAdapter::NewAPI && profile.userId.empty()) {
        output.error = L"New API 需要填写 User ID。";
        return output;
    }

    try {
        if (profile.adapter == SiteAdapter::AIAPI) {
            // 站点会话（Cookie）查询账户资产；普通 API Key 使用 CC-Switch 模板的
            // GET /v1/usage 接口，响应里的 available.limitedBy 决定额度类别。
            if (IsSiteSessionCredential(profile.credential)) {
                const auto response = GetJson(AdapterUrl(profile, L"/api/me/commerce/get-user-asset-summary"),
                    profile.credential, L"", true);
                if (!response.success) output.error = EndpointError(response, L"AIAPI 账户钱包接口");
                else if (!ParseAIAPI(response.body, output)) {
                    if (output.error.empty()) output.error = L"AIAPI 会话响应格式无效。";
                } else output.success = true;
                return output;
            }

            const auto response = GetJson(AdapterUrl(profile, L"/v1/usage"), profile.credential, L"", false);
            if (!response.success) output.error = EndpointError(response, L"AIAPI 用量接口");
            else if (!ParseAIAPIKeyUsage(response.body, output)) {
                if (output.error.empty()) output.error = L"AIAPI 用量响应格式无效。";
            } else output.success = true;
            return output;
        }

        if (profile.adapter == SiteAdapter::NewAPI) {
            constexpr double quotaPerUnit = 500000.0;
            const auto response = GetJson(AdapterUrl(profile, L"/api/user/self"),
                profile.credential, profile.userId, false, true);
            if (!response.success) output.error = EndpointError(response, L"New API 用户接口");
            else if (!ParseNewAPI(response.body, quotaPerUnit, output)) {
                if (output.error.empty()) output.error = L"New API 用户额度响应格式无效。";
            } else {
                output.unit = "USD";
                output.success = true;
            }
            return output;
        }

        if (profile.adapter == SiteAdapter::Sub2API) {
            const auto response = GetJson(AdapterUrl(profile, L"/v1/usage"), profile.credential, L"", false);
            if (!response.success) output.error = EndpointError(response, L"Sub2API 用量接口");
            else if (!ParseSub2API(response.body, output)) {
                if (output.error.empty()) output.error = L"Sub2API 响应未提供有效的剩余额度。";
            } else output.success = true;
            return output;
        }
    } catch (...) {
        output.error = L"处理站点响应时发生错误。";
    }
    if (output.error.empty()) output.error = L"不支持的站点适配器。";
    return output;
}

} // namespace aiapi
