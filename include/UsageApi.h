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
    std::wstring error;
};

SiteBalance FetchSiteBalance(const SiteProfile& profile);

} // namespace aiapi
