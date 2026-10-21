#pragma once

// 服务商预设注册表：界面用到的显示名称、默认地址、字段要求都在这里，
// 查询实现分别在 OfficialBalance.cpp / OfficialPlan.cpp / OfficialSubscription.cpp
// 以及 UsageApi.cpp 里的中转站适配器中。
//
// 数值顺序即选项下拉框顺序；adapter 的数值本身写进 sites.bin，见 UsageApi.h。

#include "UsageApi.h"

#include <string>

namespace aiapi {

namespace detail {

// AIAPI 的站点域名不在仓库里以明文出现：按字节保存，运行时还原。
// 这里只做“不显眼”处理，不是加密；目的是避免地址随公开源码扩散。
inline std::wstring DecodeAiapiHost() {
    static const unsigned char kEncoded[] = {
        0x39, 0x34, 0x74, 0x3B, 0x33, 0x3B, 0x2A, 0x33, 0x74, 0x38, 0x35, 0x2E
    };
    std::wstring host;
    host.reserve(sizeof(kEncoded) / sizeof(kEncoded[0]));
    for (unsigned char value : kEncoded) host.push_back(static_cast<wchar_t>(value ^ 0x5A));
    return host;
}

} // namespace detail

// AIAPI 专用站点的域名与根地址（运行期还原，源码里没有明文）。
inline const std::wstring& AIAPIHost() {
    static const std::wstring host = detail::DecodeAiapiHost();
    return host;
}

inline std::wstring AIAPIBaseUrl() {
    return L"https://" + AIAPIHost();
}

enum class ProviderCategory {
    Relay,                // 中转站与自定义：New API / Sub2API / AIAPI / 自定义接口
    OfficialBalance,      // 官方余额（API Key）
    OfficialPlan,         // 官方 Token Plan（API Key）
    OfficialSubscription, // 官方订阅（本机 CLI OAuth 凭据）
};

enum class FieldMode {
    Hidden,   // 不显示、不可编辑（值会被清空）
    Fixed,    // 显示但不可编辑（固定地址）
    Editable, // 可编辑
};

struct ProviderPreset {
    SiteProvider provider = SiteProvider::AIAPI;
    ProviderCategory category = ProviderCategory::Relay;
    const wchar_t* displayName = L"";   // 下拉框文本
    const wchar_t* shortName = L"";     // 列表/项目名里的简称
    const wchar_t* defaultBaseUrl = L"";
    FieldMode baseUrlMode = FieldMode::Editable;
    bool needsCredential = true;        // 需要用户填写 API Key/Token
    const wchar_t* credentialLabel = L"API Key";
    bool needsUserId = false;
    bool needsCustomScript = false;
    bool usesLocalCredentials = false;  // 读取本机 CLI OAuth 凭据
    const wchar_t* subscriptionCli = L""; // usesLocalCredentials 时对应的 CLI 名称
};

inline const ProviderPreset kProviderPresets[] = {
    // AIAPI 的地址在运行期由 AIAPIBaseUrl() 还原，源码里不写明文域名。
    { SiteProvider::AIAPI, ProviderCategory::Relay, L"AIAPI", L"AIAPI",
        L"", FieldMode::Fixed, true, L"API Key / 站点会话" },
    { SiteProvider::NewAPI, ProviderCategory::Relay, L"New API", L"New API",
        L"", FieldMode::Editable, true, L"System Access Token", true },
    { SiteProvider::Sub2API, ProviderCategory::Relay, L"Sub2API", L"Sub2API",
        L"", FieldMode::Editable, true, L"API Key（Bearer）" },
    { SiteProvider::DeepSeek, ProviderCategory::OfficialBalance, L"官方余额 · DeepSeek", L"DeepSeek",
        L"https://api.deepseek.com", FieldMode::Fixed, true, L"API Key" },
    { SiteProvider::StepFun, ProviderCategory::OfficialBalance, L"官方余额 · StepFun", L"StepFun",
        L"https://api.stepfun.com", FieldMode::Fixed, true, L"API Key" },
    { SiteProvider::SiliconFlowCN, ProviderCategory::OfficialBalance, L"官方余额 · SiliconFlow 国内", L"SiliconFlow",
        L"https://api.siliconflow.cn", FieldMode::Fixed, true, L"API Key" },
    { SiteProvider::SiliconFlowEN, ProviderCategory::OfficialBalance, L"官方余额 · SiliconFlow 国际", L"SiliconFlow",
        L"https://api.siliconflow.com", FieldMode::Fixed, true, L"API Key" },
    { SiteProvider::OpenRouter, ProviderCategory::OfficialBalance, L"官方余额 · OpenRouter", L"OpenRouter",
        L"https://openrouter.ai", FieldMode::Fixed, true, L"API Key" },
    { SiteProvider::NovitaAI, ProviderCategory::OfficialBalance, L"官方余额 · Novita AI", L"Novita AI",
        L"https://api.novita.ai", FieldMode::Fixed, true, L"API Key" },
    { SiteProvider::KimiPlan, ProviderCategory::OfficialPlan, L"Token Plan · Kimi", L"Kimi",
        L"https://api.kimi.com/coding", FieldMode::Fixed, true, L"API Key" },
    { SiteProvider::ZhipuPlan, ProviderCategory::OfficialPlan, L"Token Plan · 智谱 GLM", L"智谱 GLM",
        L"https://open.bigmodel.cn", FieldMode::Editable, true, L"API Key" },
    { SiteProvider::MiniMaxPlanCN, ProviderCategory::OfficialPlan, L"Token Plan · MiniMax 国内", L"MiniMax",
        L"https://api.minimaxi.com", FieldMode::Fixed, true, L"API Key" },
    { SiteProvider::MiniMaxPlanEN, ProviderCategory::OfficialPlan, L"Token Plan · MiniMax 国际", L"MiniMax",
        L"https://api.minimax.io", FieldMode::Fixed, true, L"API Key" },
    { SiteProvider::ZenMuxPlan, ProviderCategory::OfficialPlan, L"Token Plan · ZenMux", L"ZenMux",
        L"https://zenmux.ai", FieldMode::Editable, true, L"API Key" },
    { SiteProvider::OpenCodeGoPlan, ProviderCategory::OfficialPlan, L"Token Plan · OpenCode Go", L"OpenCode Go",
        L"https://opencode.ai/zen/go", FieldMode::Fixed, true, L"API Key" },
    { SiteProvider::CommandCodePlan, ProviderCategory::OfficialPlan, L"Token Plan · Command Code", L"Cmd Code",
        L"https://api.commandcode.ai/provider", FieldMode::Editable, true, L"API Key" },
    { SiteProvider::ClaudeOfficial, ProviderCategory::OfficialSubscription, L"官方订阅 · Claude", L"Claude",
        L"", FieldMode::Hidden, false, L"无需填写（本机凭据）", false, false, true,
        L"Claude Code" },
    { SiteProvider::CodexOfficial, ProviderCategory::OfficialSubscription, L"官方订阅 · OpenAI Codex", L"Codex",
        L"", FieldMode::Hidden, false, L"无需填写（本机凭据）", false, false, true,
        L"Codex" },
    { SiteProvider::Custom, ProviderCategory::Relay, L"自定义接口", L"自定义接口",
        L"", FieldMode::Editable, false, L"API Key（脚本占位符，可选）", false, true },
};

inline const ProviderPreset* ProviderPresetAt(size_t index) {
    constexpr size_t count = sizeof(kProviderPresets) / sizeof(kProviderPresets[0]);
    return index < count ? &kProviderPresets[index] : nullptr;
}

inline size_t ProviderPresetCount() {
    return sizeof(kProviderPresets) / sizeof(kProviderPresets[0]);
}

inline const ProviderPreset* FindProviderPreset(SiteProvider provider) {
    for (const auto& preset : kProviderPresets) {
        if (preset.provider == provider) return &preset;
    }
    return nullptr;
}

// 预设的默认地址：AIAPI 的地址不在源码里写死，由 AIAPIBaseUrl() 运行期还原。
inline std::wstring PresetBaseUrl(const ProviderPreset& preset) {
    if (preset.defaultBaseUrl && *preset.defaultBaseUrl) return preset.defaultBaseUrl;
    if (preset.provider == SiteProvider::AIAPI) return AIAPIBaseUrl();
    return std::wstring();
}

inline bool IsAiapiProvider(SiteProvider provider) {
    return provider == SiteProvider::AIAPI;
}

inline bool IsRelayProvider(SiteProvider provider) {
    return provider == SiteProvider::AIAPI || provider == SiteProvider::NewAPI ||
        provider == SiteProvider::Sub2API || provider == SiteProvider::Custom;
}

} // namespace aiapi
