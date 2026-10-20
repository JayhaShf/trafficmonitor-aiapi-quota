#pragma once

#include <string>
#include <vector>

namespace aiapi {

enum class SiteAdapter : unsigned int {
    AIAPI = 1,
    NewAPI = 2,
    Sub2API = 3,
    Custom = 4,
};

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
