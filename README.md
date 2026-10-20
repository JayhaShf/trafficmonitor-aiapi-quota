# TrafficMonitor 多站点 AI 余额插件

TrafficMonitor 原生 C++ 插件，最多管理 8 个站点。所有站点、启用状态和凭据统一保存在一个当前 Windows 用户 DPAPI 加密的 `sites.bin` 中；不会为 AIAPI、New API、Sub2API 各建配置文件。

## 站点适配器

- **AIAPI**：API Key 查询 `GET /v1/usage`（`Authorization: Bearer <API Key>`、`Accept: application/json`）。剩余额度取 `data.available.amount`，缺省时退回 `data.limits.daily.remaining`；币种取 `data.currency`（缺省 CNY）。`data.available.limitedBy` 决定额度类别：`key_daily`/`key_weekly`/`key_monthly`/`key_total` 显示为日限额、周限额、月限额、总限额，`balance` 为钱包余额，`subscription` 为套餐。tooltip 附带 `data.key.name` 与 `data.today.cost/calls`，`data.limits.daily.limit > 0` 时再显示已用/总额。已启用的 AIAPI 站点会话（凭据以 `site-session=` 或 `sts_` 开头）则查询官网账户资产 `GET /api/me/commerce/get-user-asset-summary`，按官网 Dashboard 的 CNY 格式显示账户钱包余额。
- **New API**：按 CC-Switch 模板 `GET /api/user/self`，使用 `Authorization: Bearer <System Access Token>`、`New-Api-User: <User ID>`、`User-Agent: cc-switch/1.0` 和 `Content-Type: application/json`。`quota` 与 `used_quota` 按模板假设的 `500000` 单位换算成 USD；若服务修改了 QuotaPerUnit，显示值也需按服务配置调整。
- **Sub2API**：按 CC-Switch 模板 `GET /v1/usage`，使用 Bearer API Key 与 `User-Agent: cc-switch/1.0`。`remaining` 可能代表钱包、订阅组或单 Key 限额；插件显示 `planName`，只有明确的“钱包余额”分支才标作账户余额，其他显示为可用额。
- **自定义接口**：用于 New API/Sub2API 预设未覆盖的服务。在站点类型选择“自定义接口”后点“自定义配置...”，编写 CC-Switch 格式的 `{ request, extractor }` 对象。
- **提示排版**：每个启用站点单独一行，只显示站点名称和余额类别，不重复适配器名或额度来源。例如：`AIAPI（余额）：¥8.75；Key default；今日 1.23 · 5 次`、`AIAPI（日限额）：¥6.50；已用 3.50/10.00`。

```javascript
({
  request: {
    url: "{{baseUrl}}/user/balance",
    method: "GET",
    headers: {
      Authorization: "Bearer {{apiKey}}",
      "User-Agent": "cc-switch/1.0"
    }
  },
  extractor: function(response) {
    return {
      isValid: response.is_active !== false,
      remaining: response.balance,
      unit: "USD"
    };
  }
})
```

自定义请求支持 HTTPS GET/POST；相对 URL 会拼接 Base URL。可用占位符：`{{baseUrl}}`、`{{apiKey}}`、`{{accessToken}}`、`{{userId}}`。提取器可返回 `remaining`（必填）、`unit`、`planName`、`used`、`total`、`isValid`、`invalidMessage` 和 `extra`。自定义脚本由 Windows Chakra JavaScript 引擎执行，限制内存和 5 秒运行时间，不提供文件、进程或网络对象。点“测试脚本”会发送配置的实际请求；若选 POST，请确认请求不会改动远端数据。

## 启用控制

每个站点都有“启用查询”复选框。保存后，未勾选的站点不会被后台轮询，也不会注册余额显示项；重新勾选后恢复查询。失败时保留该站点上次成功值。

每条站点必须明确选择 AIAPI、New API、Sub2API 或自定义接口，插件不会按域名自动判断类型。选择 Sub2API 后应填写站点根 URL（可带或不带 `/v1`）和该站点 API Key；无需填 User ID。切换接口类型会清除旧认证凭据，避免把 AIAPI Key 误用作 Sub2API Key。旧版 `api-key.bin` 迁移的项目固定为 AIAPI。

## 刷新策略

- **并行查询**：所有到期站点同时查询，最多 4 个并发。8 个站点不会再逐个排队——单站点超时不再拖慢其余站点。
- **失败退避**：某站点连续失败时，下一次尝试按 `间隔 × 2^(失败次数-1)` 推迟（15 秒间隔 → 15s、30s、60s…），上限 15 分钟；成功一次立刻恢复正常节奏。退避期间显示上次成功值，tooltip 标注“稍后刷新”，不会被误报成新一轮失败。保存配置会清空全部退避状态，改完凭据立即重试。
- **刷新间隔**：在选项对话框“刷新间隔”里选，15 秒到 15 分钟，默认 60 秒。间隔与站点配置加密保存在同一个 `sites.bin`（文件格式 v3；旧 v1/v2 配置可读，读取时按默认 60 秒处理）。

## 凭据安全

API Key、System Access Token、自定义脚本和站点信息统一使用 DPAPI 加密写入 `sites.bin`。AIAPI 会话凭据仅在本机配置，且只通过 HTTPS 发送到 `AIAPI 站点`。不要把 Cookie、Access Token 或 API Key 发到聊天或截图；曾经公开的会话 Cookie 应先撤销。自定义请求必须使用 HTTPS，以免在网络中泄露凭据。

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

将生成的 `AIAPIQuota.dll` 放入安装目录的 `plugins` 文件夹后启动程序。首次增加站点时，选择适配器、填写 Base URL 和凭据；New API 另需 User ID。然后勾选“启用查询”，并在 TrafficMonitor 的“显示项目”菜单启用该站点余额项目。

## 界面

选项对话框和“自定义接口配置”对话框使用 `src/UiTheme.h` + `src/UiControls.h` 提供的外观层：

- **跟随系统深浅色**：读取 `AppsUseLightTheme`，深色模式同时套用窗口深色标题栏、深色控件主题与自绘配色。设置环境变量 `AIAPIQUOTA_UI_THEME=light`（或 `dark`）可强制指定，便于对比。
- **DPI 自适应**：字体与所有留白按 `GetDpiForWindow` 缩放，125%/150% 下不糊、不裁切。
- **自绘控件**：站点列表（状态圆点 + 选中强调条 + 省略号截断）、按钮（主/次/危险三态 + 焦点框）、复选框与下拉框（深色下接管系统绘制）。
- **层级**：字段标签用次要文字色、说明文字放进圆角卡片、页脚有分隔线；主按钮使用强调色。

只改外观，不改交互：控件类型、控件 ID、消息处理与保存逻辑均保持原样。`BS_OWNERDRAW` 与 `BS_DEFPUSHBUTTON` 的样式位互斥，回车键的默认按钮语义由 `DM_SETDEFID` 补回。

### 界面预览工具

`tools/dialog_preview.cpp` 可以脱离宿主直接打开插件对话框并截图，改界面时不必反复重启 TrafficMonitor：

```powershell
# 构建（在 tools 目录；输出 exe 与宿主位数一致）
cl /nologo /std:c++17 /EHsc /utf-8 /DUNICODE /D_UNICODE /I..\include dialog_preview.cpp ^
   /link user32.lib gdi32.lib ole32.lib windowscodecs.lib crypt32.lib /OUT:dialog_preview.exe

# 截图（--samples 会复制宿主真实配置到临时目录，带出真实站点列表）
.\dialog_preview.exe ..\build\Release\AIAPIQuota.dll shots\opt-dark.png --samples
.\dialog_preview.exe ..\build\Release\AIAPIQuota.dll shots\custom-dark.png --custom
```

它通过 `TMPluginGetInstance` → `OnInitialize` → `ShowOptionsDialog` 走真实加载路径，截图用 `PrintWindow`。`--samples` 只读取配置，只有你在对话框里点“保存”才会写回。

## 许可证

本插件原创源代码和文档采用 [MIT License](LICENSE)。`include/PluginInterface.h` 是从 TrafficMonitor 官方仓库引入的文件，保留其版权头并单独遵循 [TrafficMonitor Anti-996 License 1.0](LICENSE-TRAFFICMONITOR)。

