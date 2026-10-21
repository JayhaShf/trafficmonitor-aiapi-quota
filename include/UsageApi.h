#pragma once

#include <string>
#include <vector>

namespace aiapi {

// 站点适配器（服务商预设）。数值写进 sites.bin 的 adapter 字段：
// 1-4 是旧版本已有取值，必须保持不变以兼容旧配置；新增服务商只追加在末尾。
enum class SiteProvider : unsigned int {
    // 中转站 / 自定义
    AIAPI = 1,
    NewAPI = 2,
    Sub2API = 3,
    Custom = 4,
    // 官方余额（API Key，对应 CC-Switch 的 balance 服务）
    DeepSeek = 5,
    StepFun = 6,
    SiliconFlowCN = 7,
    SiliconFlowEN = 8,
    OpenRouter = 9,
    NovitaAI = 10,
    // 官方 Token Plan（编程套餐，对应 CC-Switch 的 coding_plan 服务）
    KimiPlan = 11,
    ZhipuPlan = 12,
    MiniMaxPlanCN = 13,
    MiniMaxPlanEN = 14,
    ZenMuxPlan = 15,
    OpenCodeGoPlan = 16,
    CommandCodePlan = 17,
    // 官方订阅（读取本机 CLI 的 OAuth 凭据，对应 CC-Switch 的 subscription 服务）
    ClaudeOfficial = 18,
    CodexOfficial = 19,
};

// 旧代码里的名字，保持兼容。
using SiteAdapter = SiteProvider;

struct SiteProfile {
    unsigned int id = 0;
    SiteAdapter adapter = SiteAdapter::AIAPI;
    bool enabled = true;
    std::wstring name;
    std::wstring baseUrl;
    std::wstring credential;
    std::wstring userId;
    std::wstring customScript;
};

struct SiteBalance {
    unsigned int id = 0;
    SiteAdapter adapter = SiteAdapter::AIAPI;
    std::wstring name;
    bool success = false;
    bool accountBalance = false;
    // 本轮没有实际查询（失败退避未到期）：值保持上一次结果，界面不应显示为错误。
    bool skipped = false;
    double remaining = 0.0;
    double used = 0.0;
    double total = 0.0;
    bool hasUsed = false;
    bool hasTotal = false;
    std::string unit = "USD";
    std::wstring packageSummary;
    std::wstring detail;
    // 额度类别（如“余额/可用额/日限额/套餐”）。适配器给出时优先用它做显示标签，
    // 为空时界面按 accountBalance/packageSummary 推断。
    std::wstring scopeLabel;
    // tooltip 中追加的说明（Key 名称、今日消费/次数、限额用量等），可为空。
    std::wstring extra;
    std::wstring error;
};

SiteBalance FetchSiteBalance(const SiteProfile& profile);

} // namespace aiapi
