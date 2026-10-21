#pragma once

// 官方余额适配（CC-Switch services/balance.rs 的移植）。
//
// 覆盖：DeepSeek / StepFun / SiliconFlow（国内、国际）/ OpenRouter / Novita AI。
// 全部使用站点自己的 API Key（SiteProfile::credential）查询官方余额接口。
//
// 输出约定（写入 SiteBalance）：
//   success 由 FetchSiteBalance 统一置位，本函数成功时返回 true；
//   remaining = 余额金额；unit = 币种（USD/CNY）；accountBalance = true；
//   scopeLabel = L"余额"；extra 可放钱包细节（如赠金/充值余额、币种说明）。
//   失败时返回 false 并填写 error。

#include "UsageApi.h"

namespace aiapi {

bool IsOfficialBalanceProvider(SiteProvider provider);
bool QueryOfficialBalance(const SiteProfile& profile, SiteBalance& output);

} // namespace aiapi
