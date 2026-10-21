#pragma once

// 官方订阅额度适配（CC-Switch services/subscription.rs 的移植）。
//
// 覆盖：Claude（Claude Code 本机 OAuth 凭据）/ OpenAI Codex（Codex CLI 本机
//       OAuth 凭据）。不需要用户填写 API Key：读取本机 CLI 已登录的凭据。
//
// 输出约定（写入 SiteBalance）：
//   success 由 FetchSiteBalance 统一置位，本函数成功时返回 true；
//   packageSummary = 各时间窗口的剩余百分比，例如 L"5H 62%  7D 41%"；
//   extra 放重置时间、账号类型等补充说明；scopeLabel = L"套餐"。
//   凭据缺失/过期时返回 false，error 要能提示用户去对应 CLI 登录。

#include "UsageApi.h"

namespace aiapi {

bool IsOfficialSubscriptionProvider(SiteProvider provider);
bool QueryOfficialSubscription(const SiteProfile& profile, SiteBalance& output);

} // namespace aiapi
