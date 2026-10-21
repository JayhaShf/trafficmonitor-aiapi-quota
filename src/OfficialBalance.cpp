// 官方余额适配（CC-Switch services/balance.rs 的移植）。
//
// 覆盖 5 家：DeepSeek / StepFun / SiliconFlow（国内、国际）/ OpenRouter /
// Novita AI。端点、认证头、金额字段和单位严格对应 Rust 实现：
//   DeepSeek      GET https://api.deepseek.com/user/balance
//   StepFun       GET https://api.stepfun.com/v1/accounts
//   SiliconFlowCN GET https://api.siliconflow.cn/v1/user/info
//   SiliconFlowEN GET https://api.siliconflow.com/v1/user/info
//   OpenRouter    GET https://openrouter.ai/api/v1/credits
//   NovitaAI      GET https://api.novita.ai/v3/user/balance
// 全部使用站点自己的 API Key：Authorization: Bearer <SiteProfile::credential>。
//
// 地址处理：官方服务商的地址在 UI 里是固定的（见 ProviderRegistry.h 的
// defaultBaseUrl），因此正常配置下 SiteProfile::baseUrl 就是该固定地址（如
// https://api.deepseek.com），这里在它后面追加接口路径；baseUrl 为空时退回内置
// 默认地址。为便于本地 mock HTTP 测试，baseUrl 非空时一律以它为地址基础，且当它
// 是 http:// 明文地址时允许明文请求——该开关仅用于测试：官方服务商在 UI 里不可编辑
// 地址，不会把凭据引到别的域名。
//
// 输出约定见 OfficialBalance.h：成功填 remaining/unit/scopeLabel/extra/detail，
// 不设置 output.success（外层 FetchSiteBalance 统一置位）；失败填 output.error 并
// 返回 false。

#include "OfficialBalance.h"
#include "QuerySupport.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cwctype>
#include <iomanip>
#include <initializer_list>
#include <locale>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace aiapi {
namespace {

using query::BearerHeader;
using query::HttpRequest;
using query::HttpResponse;
using query::JsonValue;
using query::Member;
using query::ParseDocument;
using query::ReadBool;
using query::ReadInt;
using query::ReadNumber;
using query::ReadString;
using query::SendHttp;

// balance.rs 里统一 15 秒超时。
constexpr int kRequestTimeoutMs = 15000;

constexpr wchar_t kDeepSeekUrl[] = L"https://api.deepseek.com/user/balance";
constexpr wchar_t kStepFunUrl[] = L"https://api.stepfun.com/v1/accounts";
constexpr wchar_t kSiliconFlowCnUrl[] = L"https://api.siliconflow.cn/v1/user/info";
constexpr wchar_t kSiliconFlowEnUrl[] = L"https://api.siliconflow.com/v1/user/info";
constexpr wchar_t kOpenRouterUrl[] = L"https://openrouter.ai/api/v1/credits";
constexpr wchar_t kNovitaUrl[] = L"https://api.novita.ai/v3/user/balance";

std::wstring ProviderLabel(SiteProvider provider) {
    switch (provider) {
    case SiteProvider::DeepSeek: return L"DeepSeek";
    case SiteProvider::StepFun: return L"StepFun";
    case SiteProvider::SiliconFlowCN: return L"SiliconFlow 国内站";
    case SiteProvider::SiliconFlowEN: return L"SiliconFlow 国际站";
    case SiteProvider::OpenRouter: return L"OpenRouter";
    case SiteProvider::NovitaAI: return L"Novita AI";
    default: return L"官方余额";
    }
}

std::wstring Trim(const std::wstring& value) {
    const size_t begin = value.find_first_not_of(L" \t\r\n");
    if (begin == std::wstring::npos) return std::wstring();
    const size_t end = value.find_last_not_of(L" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

bool StartsWithInsensitive(const std::wstring& value, const std::wstring& prefix) {
    if (value.size() < prefix.size()) return false;
    for (size_t index = 0; index < prefix.size(); ++index) {
        if (std::towlower(value[index]) != std::towlower(prefix[index])) return false;
    }
    return true;
}

bool EndsWithInsensitive(const std::wstring& value, const std::wstring& suffix) {
    if (value.size() < suffix.size()) return false;
    const size_t offset = value.size() - suffix.size();
    for (size_t index = 0; index < suffix.size(); ++index) {
        if (std::towlower(value[offset + index]) != std::towlower(suffix[index])) return false;
    }
    return true;
}

// profile.baseUrl 是 UI 预设的固定地址；为空时退回内置官方地址。测试可传完整端点 URL。
std::wstring BuildEndpoint(const SiteProfile& profile, const wchar_t* defaultUrl, const std::wstring& path) {
    std::wstring base = Trim(profile.baseUrl);
    if (base.empty()) return defaultUrl;
    while (!base.empty() && base.back() == L'/') base.pop_back();
    if (EndsWithInsensitive(base, path)) return base;
    return base + path;
}

// 发一次 GET 并带 Bearer 认证。allowInsecureHttp 只可能出现在测试把 baseUrl 指向
// 本机 http mock 的场景，官方固定地址始终是 HTTPS。
HttpResponse GetJson(const SiteProfile& profile, const std::wstring& credential,
    const wchar_t* defaultUrl, const std::wstring& path) {
    HttpRequest request;
    request.method = L"GET";
    request.headers.push_back(BearerHeader(credential));
    request.timeoutMs = kRequestTimeoutMs;
    const std::wstring url = BuildEndpoint(profile, defaultUrl, path);
    request.allowInsecureHttp = StartsWithInsensitive(url, L"http://");
    return SendHttp(url, request);
}

bool Fail(SiteBalance& output, std::wstring message) {
    output.error = std::move(message);
    return false;
}

// 公共的响应折叠：网络/HTTP 错误、认证错误、JSON 解析错误都转成可读中文，
// 成功时把解析后的 JSON 交给各服务商自己的字段映射。
bool QueryJson(const SiteProfile& profile, const std::wstring& credential,
    const wchar_t* defaultUrl, const std::wstring& path,
    SiteBalance& output, JsonValue& root) {
    const HttpResponse response = GetJson(profile, credential, defaultUrl, path);
    if (!response.success) {
        // SendHttp 对 401/403 给出的是通用“认证失败 (HTTP xxx)”，这里补上服务商名，
        // 并优先用响应体里的 message（如 invalid api key），便于定位是哪家 Key 失效。
        if (response.status == 401 || response.status == 403) {
            std::wstring reason;
            if (!query::ExtractJsonMessage(response.body, reason)) reason = L"API Key 无效或已过期。";
            Fail(output, ProviderLabel(profile.adapter) + L" 认证失败：" + reason);
            return false;
        }
        return Fail(output, response.error.empty()
            ? ProviderLabel(profile.adapter) + L" 请求失败。"
            : response.error);
    }
    if (!ParseDocument(response.body, root)) {
        return Fail(output, ProviderLabel(profile.adapter) + L" 响应解析失败（不是有效 JSON）。");
    }
    return true;
}

// 金额展示用（只用于 extra/detail 等说明文字，主数值保持双精度原值）。
std::wstring FormatAmount(double value) {
    if (!std::isfinite(value)) return L"—";
    const double magnitude = std::fabs(value);
    const int precision = (magnitude > 0.0 && magnitude < 0.01) ? 6 : 2;
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(precision) << value;
    return stream.str();
}

// 币种统一成大写（如 "cny" -> "CNY"），与 Rust 的字符串原样展示相比更稳。
std::string NormalizeUnit(const std::string& text) {
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return std::string();
    const size_t end = text.find_last_not_of(" \t\r\n");
    std::string unit = text.substr(begin, end - begin + 1);
    std::transform(unit.begin(), unit.end(), unit.begin(),
        [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return unit;
}

// ── DeepSeek ────────────────────────────────────────────────
// 响应：{ is_available, balance_infos: [{ currency, total_balance,
//         granted_balance, topped_up_balance }] }
// 同一账号可能同时列出 CNY / USD 钱包。与 Rust 一致：只要有钱包余额为正，就丢掉
// 空钱包（避免显示“余额已用完”）；主显示项按 CNY、USD、其它的顺序挑第一个有余额的。
bool QueryDeepSeek(const SiteProfile& profile, const std::wstring& credential, SiteBalance& output) {
    JsonValue root;
    if (!QueryJson(profile, credential, kDeepSeekUrl, L"/user/balance", output, root)) return false;

    bool available = true;
    ReadBool(Member(&root, "is_available"), available);
    if (!available) {
        return Fail(output, L"DeepSeek 账户余额不可用（is_available=false）：余额不足或账户受限。");
    }

    const JsonValue* infos = Member(&root, "balance_infos");
    if (!infos || infos->kind != JsonValue::Kind::Array) {
        return Fail(output, L"DeepSeek 响应缺少 balance_infos 字段。");
    }
    if (infos->array.empty()) {
        return Fail(output, L"DeepSeek 响应的 balance_infos 为空，无法读取钱包余额。");
    }

    struct Wallet {
        std::string unit;
        double total = 0.0;
        bool hasTotal = false;
        double granted = 0.0;
        bool hasGranted = false;
        double toppedUp = 0.0;
        bool hasToppedUp = false;
    };
    std::vector<Wallet> wallets;
    for (const JsonValue& item : infos->array) {
        if (item.kind != JsonValue::Kind::Object) continue;
        Wallet wallet;
        std::string currency;
        ReadString(Member(&item, "currency"), currency);
        wallet.unit = NormalizeUnit(currency);
        if (wallet.unit.empty()) wallet.unit = "CNY"; // 对齐 Rust 的 unwrap_or("CNY")
        wallet.hasTotal = ReadNumber(Member(&item, "total_balance"), wallet.total);
        wallet.hasGranted = ReadNumber(Member(&item, "granted_balance"), wallet.granted);
        wallet.hasToppedUp = ReadNumber(Member(&item, "topped_up_balance"), wallet.toppedUp);
        wallets.push_back(std::move(wallet));
    }
    if (wallets.empty()) {
        return Fail(output, L"DeepSeek 响应的 balance_infos 里没有钱包对象。");
    }
    if (std::none_of(wallets.begin(), wallets.end(),
            [](const Wallet& wallet) { return wallet.hasTotal; })) {
        return Fail(output, L"DeepSeek 响应缺少 total_balance 字段。");
    }

    const bool anyPositive = std::any_of(wallets.begin(), wallets.end(),
        [](const Wallet& wallet) { return wallet.hasTotal && wallet.total > 0.0; });
    std::vector<const Wallet*> candidates;
    for (const Wallet& wallet : wallets) {
        if (anyPositive && (!wallet.hasTotal || wallet.total <= 0.0)) continue;
        candidates.push_back(&wallet);
    }

    const Wallet* chosen = nullptr;
    for (const char* preferred : { "CNY", "USD" }) {
        for (const Wallet* wallet : candidates) {
            if (wallet->unit == preferred) { chosen = wallet; break; }
        }
        if (chosen) break;
    }
    if (!chosen) chosen = candidates.front();

    output.remaining = chosen->hasTotal ? chosen->total : 0.0;
    output.unit = chosen->unit;
    output.accountBalance = true;
    output.scopeLabel = L"余额";

    std::wstring extra;
    if (chosen->hasGranted || chosen->hasToppedUp) {
        extra = L"赠金 " + FormatAmount(chosen->granted) + L" · 充值 " + FormatAmount(chosen->toppedUp);
    }
    std::wstring otherWallets;
    for (const Wallet& wallet : wallets) {
        if (&wallet == chosen) continue;
        if (!otherWallets.empty()) otherWallets += L"、";
        otherWallets += query::WidenUtf8(wallet.unit) + L" " +
            FormatAmount(wallet.hasTotal ? wallet.total : 0.0);
    }
    if (!otherWallets.empty()) {
        if (!extra.empty()) extra += L"；";
        extra += L"其它钱包：" + otherWallets;
    }
    output.extra = extra;
    output.detail = L"DeepSeek /user/balance（钱包币种 " + query::WidenUtf8(output.unit) + L"）";
    return true;
}

// ── StepFun ─────────────────────────────────────────────────
// 响应：{ object, type, balance, total_cash_balance, total_voucher_balance }，单位 CNY。
bool QueryStepFun(const SiteProfile& profile, const std::wstring& credential, SiteBalance& output) {
    JsonValue root;
    if (!QueryJson(profile, credential, kStepFunUrl, L"/v1/accounts", output, root)) return false;

    double balance = 0.0;
    if (!ReadNumber(Member(&root, "balance"), balance)) {
        return Fail(output, L"StepFun 响应缺少 balance 字段。");
    }
    output.remaining = balance;
    output.unit = "CNY";
    output.accountBalance = true;
    output.scopeLabel = L"余额";

    std::wstring extra;
    double cash = 0.0;
    double voucher = 0.0;
    if (ReadNumber(Member(&root, "total_cash_balance"), cash)) {
        extra = L"现金余额 " + FormatAmount(cash);
    }
    if (ReadNumber(Member(&root, "total_voucher_balance"), voucher)) {
        if (!extra.empty()) extra += L" · ";
        extra += L"赠金 " + FormatAmount(voucher);
    }
    output.extra = extra;
    output.detail = L"StepFun /v1/accounts（单位 CNY）";
    return true;
}

// ── SiliconFlow ─────────────────────────────────────────────
// 响应：{ code, data: { balance, chargeBalance, totalBalance, status } }
// Rust 取 data.totalBalance 作为余额；国内站人民币、国际站美元。
bool QuerySiliconFlow(const SiteProfile& profile, const std::wstring& credential,
    bool international, SiteBalance& output) {
    const wchar_t* defaultUrl = international ? kSiliconFlowEnUrl : kSiliconFlowCnUrl;
    JsonValue root;
    if (!QueryJson(profile, credential, defaultUrl, L"/v1/user/info", output, root)) return false;

    const JsonValue* data = Member(&root, "data");
    if (!data || data->kind != JsonValue::Kind::Object) {
        std::wstring message = L"SiliconFlow 响应缺少 data 对象。";
        long long code = 0;
        if (ReadInt(Member(&root, "code"), code)) {
            message += L"（code=" + std::to_wstring(code) + L"）";
        }
        return Fail(output, std::move(message));
    }

    double totalBalance = 0.0;
    if (!ReadNumber(Member(data, "totalBalance"), totalBalance)) {
        return Fail(output, L"SiliconFlow 响应缺少 data.totalBalance 字段。");
    }
    output.remaining = totalBalance;
    output.unit = international ? "USD" : "CNY";
    output.accountBalance = true;
    output.scopeLabel = L"余额";

    std::wstring extra;
    double chargeBalance = 0.0;
    double giftBalance = 0.0;
    if (ReadNumber(Member(data, "chargeBalance"), chargeBalance)) {
        extra = L"充值 " + FormatAmount(chargeBalance);
    }
    if (ReadNumber(Member(data, "balance"), giftBalance)) {
        if (!extra.empty()) extra += L" · ";
        extra += L"赠送 " + FormatAmount(giftBalance);
    }
    output.extra = extra;
    output.detail = international ? L"SiliconFlow 国际站 /v1/user/info（单位 USD）"
                                  : L"SiliconFlow 国内站 /v1/user/info（单位 CNY）";
    return true;
}

// ── OpenRouter ──────────────────────────────────────────────
// 响应：{ data: { total_credits, total_usage } }；余额 = 总额 - 已用。
// /credits 报的是购买额度，不能用它判断 Key 是否过期（余额为负也只表示欠费）。
bool QueryOpenRouter(const SiteProfile& profile, const std::wstring& credential, SiteBalance& output) {
    JsonValue root;
    if (!QueryJson(profile, credential, kOpenRouterUrl, L"/api/v1/credits", output, root)) return false;

    const JsonValue* data = Member(&root, "data");
    const JsonValue* amounts = (data && data->kind == JsonValue::Kind::Object) ? data : &root;
    double totalCredits = 0.0;
    double totalUsage = 0.0;
    if (!ReadNumber(Member(amounts, "total_credits"), totalCredits) ||
        !ReadNumber(Member(amounts, "total_usage"), totalUsage)) {
        return Fail(output, L"OpenRouter 响应缺少 total_credits 或 total_usage 字段。");
    }
    const double remaining = totalCredits - totalUsage;
    if (!std::isfinite(remaining)) {
        return Fail(output, L"OpenRouter 余额计算结果无效。");
    }

    output.remaining = remaining;
    output.unit = "USD";
    output.accountBalance = true;
    output.scopeLabel = L"余额";
    output.used = totalUsage;
    output.hasUsed = true;
    output.total = totalCredits;
    output.hasTotal = true;
    output.extra = L"已用 " + FormatAmount(totalUsage) + L" · 总额 " + FormatAmount(totalCredits);
    output.detail = L"OpenRouter /api/v1/credits（单位 USD）";
    return true;
}

// ── Novita AI ───────────────────────────────────────────────
// 响应：{ availableBalance, cashBalance, creditLimit, outstandingInvoices }；
// 金额单位 0.0001 USD，需除以 10000。余额为负只表示欠费，不代表 Key 失效。
bool QueryNovita(const SiteProfile& profile, const std::wstring& credential, SiteBalance& output) {
    JsonValue root;
    if (!QueryJson(profile, credential, kNovitaUrl, L"/v3/user/balance", output, root)) return false;

    constexpr double kUnitToUsd = 10000.0;
    double rawAvailable = 0.0;
    if (!ReadNumber(Member(&root, "availableBalance"), rawAvailable)) {
        return Fail(output, L"Novita AI 响应缺少 availableBalance 字段。");
    }
    const double available = rawAvailable / kUnitToUsd;
    if (!std::isfinite(available)) {
        return Fail(output, L"Novita AI 余额数值无效。");
    }
    output.remaining = available;
    output.unit = "USD";
    output.accountBalance = true;
    output.scopeLabel = L"余额";

    std::wstring extra;
    double cash = 0.0;
    double creditLimit = 0.0;
    if (ReadNumber(Member(&root, "cashBalance"), cash)) {
        extra = L"现金余额 " + FormatAmount(cash / kUnitToUsd);
    }
    if (ReadNumber(Member(&root, "creditLimit"), creditLimit)) {
        if (!extra.empty()) extra += L" · ";
        extra += L"信用额度 " + FormatAmount(creditLimit / kUnitToUsd);
    }
    output.extra = extra;
    output.detail = L"Novita AI /v3/user/balance（金额单位 0.0001 USD）";
    return true;
}

} // namespace

bool IsOfficialBalanceProvider(SiteProvider provider) {
    switch (provider) {
    case SiteProvider::DeepSeek:
    case SiteProvider::StepFun:
    case SiteProvider::SiliconFlowCN:
    case SiteProvider::SiliconFlowEN:
    case SiteProvider::OpenRouter:
    case SiteProvider::NovitaAI:
        return true;
    default:
        return false;
    }
}

bool QueryOfficialBalance(const SiteProfile& profile, SiteBalance& output) {
    if (!IsOfficialBalanceProvider(profile.adapter)) {
        output.error = L"该站点类型不支持官方余额查询。";
        return false;
    }
    const std::wstring credential = Trim(profile.credential);
    if (credential.empty()) {
        output.error = ProviderLabel(profile.adapter) + L"：API Key 为空，无法查询余额。";
        return false;
    }
    switch (profile.adapter) {
    case SiteProvider::DeepSeek: return QueryDeepSeek(profile, credential, output);
    case SiteProvider::StepFun: return QueryStepFun(profile, credential, output);
    case SiteProvider::SiliconFlowCN: return QuerySiliconFlow(profile, credential, false, output);
    case SiteProvider::SiliconFlowEN: return QuerySiliconFlow(profile, credential, true, output);
    case SiteProvider::OpenRouter: return QueryOpenRouter(profile, credential, output);
    case SiteProvider::NovitaAI: return QueryNovita(profile, credential, output);
    default: break;
    }
    output.error = L"该站点类型不支持官方余额查询。";
    return false;
}

} // namespace aiapi
