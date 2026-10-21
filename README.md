# TrafficMonitor 多站点 AI 余额插件

TrafficMonitor 原生 C++ 插件，最多管理 8 个站点，内置 19 个服务商预设（中转站、官方余额、Token Plan、官方订阅、自定义接口）。所有站点、启用状态和凭据统一保存在一个当前 Windows 用户 DPAPI 加密的 `sites.bin` 中；不会为每个服务商各建配置文件。

## 站点适配器

站点类型下拉按服务商预设组织；官方服务商的查询逻辑移植自 CC-Switch 的余额 / 套餐 / 订阅三套服务（`services/balance.rs`、`coding_plan.rs`、`subscription.rs`）。

### 中转站与自定义

- **AIAPI**：API Key 查询 `GET /v1/usage`（`Authorization: Bearer <API Key>`、`Accept: application/json`）。剩余额度取 `data.available.amount`，缺省时退回 `data.limits.daily.remaining`；币种取 `data.currency`（缺省 CNY）。`data.available.limitedBy` 决定额度类别：`key_daily`/`key_weekly`/`key_monthly`/`key_total` 显示为日限额、周限额、月限额、总限额，`balance` 为钱包余额，`subscription` 为套餐。tooltip 附带 `data.key.name` 与 `data.today.cost/calls`，`data.limits.daily.limit > 0` 时再显示已用/总额。站点会话凭据（`site-session=` 或 `sts_`）则查询官网账户资产接口。
- **New API**：按 CC-Switch 模板 `GET /api/user/self`，使用 `Authorization: Bearer <System Access Token>`、`New-Api-User: <User ID>`、`User-Agent: cc-switch/1.0` 和 `Content-Type: application/json`。`quota` 与 `used_quota` 按模板假设的 `500000` 单位换算成 USD；`group` 作为套餐名。
- **Sub2API**：按 CC-Switch 模板 `GET /v1/usage`，使用 Bearer API Key 与 `User-Agent: cc-switch/1.0`。`remaining` 可能代表钱包、订阅组或单 Key 限额；只有明确的“钱包余额”分支才标作账户余额，其他显示为可用额。
- **自定义接口**：内置「通用 / New API / Sub2API」三种脚本模板（与 CC-Switch 模板一致），也可完全自定义。脚本使用 `{ request, extractor }` 格式，支持占位符 `{{baseUrl}}`、`{{apiKey}}`、`{{accessToken}}`、`{{userId}}`；提取器可返回 `remaining`（必填）、`unit`、`planName`、`used`、`total`、`isValid`、`invalidMessage` 和 `extra`。请求仅允许 HTTPS GET/POST；脚本由 Windows Chakra 引擎隔离执行（16 MiB / 5 秒，无文件、进程或网络对象）。“测试脚本”会发送真实请求，POST 可能修改远端数据。

### 官方余额（API Key）

| 服务商 | 接口 | 说明 |
|---|---|---|
| DeepSeek | `GET https://api.deepseek.com/user/balance` | 支持 CNY/USD 多钱包，优先显示非零钱包，其余钱包与赠金写进 tooltip |
| StepFun | `GET https://api.stepfun.com/v1/accounts` | 余额（CNY），现金/赠金写进 tooltip |
| SiliconFlow（国内/国际） | `GET https://api.siliconflow.cn|.com/v1/user/info` | `totalBalance` 为剩余，充值/赠送写进 tooltip |
| OpenRouter | `GET https://openrouter.ai/api/v1/credits` | 剩余 = 总额度 − 已用，显示已用/总额 |
| Novita AI | `GET https://api.novita.ai/v3/user/balance` | `availableBalance` / 10000 为 USD 余额 |

### 官方 Token Plan（API Key）

| 服务商 | 接口 | 窗口 |
|---|---|---|
| Kimi For Coding | `GET https://api.kimi.com/coding/v1/usages` | 5H + 7D |
| 智谱 GLM（个人版） | `GET <open.bigmodel.cn 或 api.z.ai>/api/monitor/usage/quota/limit` | 5H + 7D |
| MiniMax（国内/国际） | `GET https://api.minimaxi.com|.io/v1/api/openplatform/coding_plan/remains` | 5H + 周 |
| ZenMux | `GET <Base URL>`（Bearer） | 5H + 7D，USD 已用/上限 |
| OpenCode Go | `GET https://opencode.ai/zen/go/v1/usage` | 5H + 7D + 1M |
| Command Code | `https://api.commandcode.ai/provider` 控制面多个端点 | 5H + 7D + 1M |

套餐类统一显示为 `5H 62%  7D 41%` 的剩余百分比摘要，tooltip 附带重置时间与套餐名。未覆盖：火山方舟（需要 AK/SK HMAC 签名）、智谱团队版（需要组织/项目 ID）。

### 官方订阅（本机 CLI 凭据，无需填写 API Key）

- **Claude**：读取 `%USERPROFILE%\.claude\.credentials.json`（Claude Code 登录凭据），请求 `GET https://api.anthropic.com/api/oauth/usage`，展示 5H / 7D / 7D Opus 等窗口与超额用量。
- **OpenAI Codex**：读取 `%USERPROFILE%\.codex\auth.json`（仅 `auth_mode=chatgpt` 的 OAuth 登录），请求 `GET https://chatgpt.com/backend-api/wham/usage`，展示窗口百分比、Credits 与套餐类型。
- 只读取凭据，不实现登录与 token 刷新；未登录或凭据过期时会提示去对应 CLI 重新登录。未覆盖：Gemini、Grok。

### 显示规则

余额类显示金额（如 `¥12.34`），套餐/订阅类显示百分比摘要（如 `5H 62%  7D 41%`），站点标签分别为 `余额`、`套餐`；tooltip 每个启用站点一行，附重置时间、今日消费、Key 名称等补充信息。

## 启用控制

每个站点都有“启用查询”复选框。保存后，未勾选的站点不会被后台轮询，也不会注册余额显示项；重新勾选后恢复查询。失败时保留该站点上次成功值。

每条站点必须从服务商预设中明确选择类型（中转站、官方余额、Token Plan、官方订阅或自定义接口），插件不会按域名自动判断。切换服务商会清除旧认证凭据，避免把一家服务的 Key 误用到另一家。自定义接口需要先配置脚本；官方订阅无需填写地址与凭据，读取本机 CLI 的 OAuth 凭据。旧版 `api-key.bin` 迁移的项目固定为 AIAPI。保存时按服务商类型校验必填项：中转站需要地址（New API 另需数字 User ID）、官方余额/套餐需要该服务的 API Key、官方订阅不做凭据校验。

## 刷新策略

- **并行查询**：所有到期站点同时查询，最多 4 个并发。8 个站点不会再逐个排队——单站点超时不再拖慢其余站点。
- **失败退避**：某站点连续失败时，下一次尝试按 `间隔 × 2^(失败次数-1)` 推迟（15 秒间隔 → 15s、30s、60s…），上限 15 分钟；成功一次立刻恢复正常节奏。退避期间显示上次成功值，tooltip 标注“稍后刷新”，不会被误报成新一轮失败。保存配置会清空全部退避状态，改完凭据立即重试。
- **刷新间隔**：在选项对话框“刷新间隔”里选，15 秒到 15 分钟，默认 60 秒。间隔与站点配置加密保存在同一个 `sites.bin`（文件格式 v3；旧 v1/v2 配置可读，读取时按默认 60 秒处理）。

## 凭据安全

API Key、System Access Token、自定义脚本和站点信息统一使用 DPAPI 加密写入 `sites.bin`。AIAPI 会话凭据仅在本机配置，且只通过 HTTPS 发送到 `AIAPI 站点`。官方订阅不保存任何凭据，只读取 `%USERPROFILE%\.claude\.credentials.json` 与 `%USERPROFILE%\.codex\auth.json` 里的本机 OAuth token。不要把 Cookie、Access Token 或 API Key 发到聊天或截图；曾经公开的会话 Cookie 应先撤销。自定义请求必须使用 HTTPS，以免在网络中泄露凭据。

## 构建和安装

需要 Windows 10 或更高版本、Visual Studio 2022 C++ 桌面工具、CMake 和 Windows SDK。在 TrafficMonitor 正常退出后，用与其位数匹配的 DLL：

```powershell
# 64-bit TrafficMonitor
cmake -S . -B build -A x64
cmake --build build --config Release

# 32-bit TrafficMonitor
cmake -S . -B build-win32 -A Win32
cmake --build build-win32 --config Release
```

将生成的 `AIAPIQuota.dll` 放入安装目录的 `plugins` 文件夹后启动程序。首次增加站点时，在“接口类型”里选择服务商预设：中转站/官方余额/Token Plan 填对应 API Key 即可（地址为固定值时无需修改）；官方订阅无需填 Key，先在 Claude Code / Codex 里登录。然后勾选“启用查询”，并在 TrafficMonitor 的“显示项目”菜单启用该站点余额项目。

## 界面

插件对话框沿用 TrafficMonitor 本体的界面语言：9pt 微软雅黑、7 DLU 标准边距、`GROUPBOX` 分组、14 DLU 高按钮与 12 DLU 高输入框，全部使用系统原生控件，不接管绘制。

- **站点列表**：与宿主“插件管理”一致的 `ListView` 报表视图，列出名称、类型、状态三列；选中站点后在右侧编辑。
- **接口与凭据**：右侧表单按服务商预设动态调整——固定地址只读、可编辑地址预填默认值、官方订阅地址/凭据改为“使用本机 CLI 凭据”提示；New API 才显示 User ID，自定义接口才启用“自定义配置…”。
- **查询设置**：每站点“启用查询”复选框；全局刷新间隔下拉框（15 秒 ~ 15 分钟），并在页脚说明加密保存位置。
- **原生主题与 DPI**：不自定义颜色和字体，跟随系统浅色/深色控件主题与高对比度设置；对话框单位与字体由系统按当前 DPI 缩放。
- **自定义接口**：脚本编辑器使用等宽 Consolas，提供「通用 / New API / Sub2API」预设模板一键载入；占位符与隔离引擎限制在页脚说明。

### 界面预览工具

`tools/dialog_preview.cpp` 可以脱离宿主直接打开插件对话框并截图，改界面时不必反复重启 TrafficMonitor：

```powershell
# 构建（在 tools 目录；输出 exe 与宿主位数一致）
cl /nologo /std:c++17 /EHsc /utf-8 /DUNICODE /D_UNICODE /I..\include dialog_preview.cpp ^
   /link user32.lib gdi32.lib ole32.lib windowscodecs.lib crypt32.lib comctl32.lib /OUT:dialog_preview.exe

# 截图（--samples 会复制宿主真实配置到临时目录，带出真实站点列表）
.\dialog_preview.exe ..\build\Release\AIAPIQuota.dll shots\options.png --samples
.\dialog_preview.exe ..\build\Release\AIAPIQuota.dll shots\custom.png --custom
```

它通过 `TMPluginGetInstance` → `OnInitialize` → `ShowOptionsDialog` 走真实加载路径，截图用 `PrintWindow`。`--samples` 只读取配置，只有你在对话框里点“保存”才会写回。

## 许可证

本插件原创源代码和文档采用 [MIT License](LICENSE)。`include/PluginInterface.h` 是从 TrafficMonitor 官方仓库引入的文件，保留其版权头并单独遵循 [TrafficMonitor Anti-996 License 1.0](LICENSE-TRAFFICMONITOR)。

