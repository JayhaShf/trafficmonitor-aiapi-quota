#pragma once

// 官方 Token Plan（编程套餐）适配（CC-Switch services/coding_plan.rs 的移植）。
//
// 覆盖：Kimi For Coding / 智谱 GLM / MiniMax（国内、国际）/ ZenMux /
//       OpenCode Go / Command Code。
// 全部使用站点自己的 API Key（SiteProfile::credential）查询套餐额度。
//
// 输出约定（写入 SiteBalance）：
//   success 由 FetchSiteBalance 统一置位，本函数成功时返回 true；
//   packageSummary = 各时间窗口的剩余百分比，例如 L"5H 62%  7D 41%"（含 % 时
//     tooltip 直接展示该摘要）；extra 放重置时间、套餐名等补充说明；
//   scopeLabel = L"套餐"；accountBalance = false。
//   失败时返回 false 并填写 error。

#include "UsageApi.h"

namespace aiapi {

bool IsOfficialPlanProvider(SiteProvider provider);
bool QueryOfficialPlan(const SiteProfile& profile, SiteBalance& output);

} // namespace aiapi
