// 对话框视觉层：DPI 字体、深浅色主题、背景与分组绘制、列表项自绘。
//
// 设计目标：不改变控件类型与交互逻辑，把原生对话框从“系统默认灰”提升到
// 与深色模式一致的外观。
//
// 约束：
//  - 只用 GDI + 可选的 DWM 深色标题栏，不引入新依赖。
//  - 所有像素尺寸按 DPI 缩放；颜色跟随系统 AppsUseLightTheme。
//  - 字体沿用 .rc 里的 Segoe UI：DLU 布局是按它的度量算的，换字体会导致换行与裁切。

#pragma once

#include <windows.h>

#include <algorithm>
#include <string>

namespace ui {

// ── DPI ──────────────────────────────────────────────────────────────

inline int Dpi(HWND window) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static const auto getDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (getDpiForWindow && window) {
        const UINT dpi = getDpiForWindow(window);
        if (dpi != 0) return static_cast<int>(dpi);
    }
    HDC screen = GetDC(nullptr);
    const int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
    if (screen) ReleaseDC(nullptr, screen);
    return dpi == 0 ? 96 : dpi;
}

inline int Scale(HWND window, int value) {
    return MulDiv(value, Dpi(window), 96);
}

// ── 主题 ─────────────────────────────────────────────────────────────

struct Theme {
    bool dark = false;

    COLORREF window{};
    COLORREF surface{};
    COLORREF surfaceAlt{};
    COLORREF border{};
    COLORREF borderStrong{};
    COLORREF textPrimary{};
    COLORREF textSecondary{};
    COLORREF textDisabled{};
    COLORREF accent{};
    COLORREF accentSoft{};
    COLORREF accentText{};
    COLORREF danger{};
    COLORREF success{};
    COLORREF warn{};
};

inline bool SystemUsesDarkMode() {
    // 预览工具用环境变量强制主题，方便一次构建看到两种外观。
    wchar_t forced[16]{};
    if (GetEnvironmentVariableW(L"AIAPIQUOTA_UI_THEME", forced, 16) > 0) {
        if (_wcsicmp(forced, L"light") == 0) return false;
        if (_wcsicmp(forced, L"dark") == 0) return true;
    }

    DWORD value = 1;
    DWORD size = sizeof(value);
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return status == ERROR_SUCCESS && value == 0;
}

inline Theme MakeTheme(bool dark) {
    Theme theme{};
    theme.dark = dark;
    if (dark) {
        theme.window = RGB(32, 32, 32);
        theme.surface = RGB(45, 45, 45);
        theme.surfaceAlt = RGB(40, 40, 40);
        theme.border = RGB(58, 58, 58);
        theme.borderStrong = RGB(76, 76, 76);
        theme.textPrimary = RGB(238, 238, 238);
        theme.textSecondary = RGB(160, 160, 160);
        theme.textDisabled = RGB(110, 110, 110);
        theme.accent = RGB(76, 141, 255);
        theme.accentSoft = RGB(38, 56, 84);
        theme.accentText = RGB(255, 255, 255);
        theme.danger = RGB(240, 112, 112);
        theme.success = RGB(112, 208, 140);
        theme.warn = RGB(240, 190, 96);
    } else {
        theme.window = RGB(250, 250, 250);
        theme.surface = RGB(255, 255, 255);
        theme.surfaceAlt = RGB(244, 244, 246);
        theme.border = RGB(224, 224, 228);
        theme.borderStrong = RGB(200, 200, 206);
        theme.textPrimary = RGB(28, 28, 30);
        theme.textSecondary = RGB(106, 106, 112);
        theme.textDisabled = RGB(166, 166, 172);
        theme.accent = RGB(0, 95, 184);
        theme.accentSoft = RGB(226, 239, 252);
        theme.accentText = RGB(255, 255, 255);
        theme.danger = RGB(196, 43, 28);
        theme.success = RGB(16, 124, 65);
        theme.warn = RGB(157, 93, 0);
    }
    return theme;
}

// ── 每对话框状态 ─────────────────────────────────────────────────────

// 控制 ID（与 resources/resource.h 保持一致）。放在最前，供 Attach 与绘制层共用。
inline constexpr int kIdStatus = 1011;
inline constexpr int kIdCustomStatus = 1015;
inline constexpr int kIdHint = 1020;
inline constexpr int kIdList = 1001;
inline constexpr int kIdTypeCombo = 1004;
inline constexpr int kIdNameEdit = 1005;
inline constexpr int kIdBaseUrlEdit = 1006;
inline constexpr int kIdCredentialEdit = 1007;
inline constexpr int kIdUserIdEdit = 1008;
inline constexpr int kIdScriptEdit = 1014;
inline constexpr int kIdSiteEnabled = 1012;
inline constexpr int kIdRefreshCombo = 1024;
inline constexpr int kIdGroupSites = 1018;
inline constexpr int kIdLabelType = 1021;
inline constexpr int kIdLabelName = 1022;
inline constexpr int kIdBaseUrlLabel = 1017;
inline constexpr int kIdCredentialLabel = 1009;
inline constexpr int kIdUserIdLabel = 1010;

struct State {
    Theme theme;
    HFONT font = nullptr;
    HFONT fontBold = nullptr;
    HBRUSH brushWindow = nullptr;
    HBRUSH brushSurface = nullptr;
    HBRUSH brushSurfaceAlt = nullptr;
    int dpi = 96;
    int listItemHeight = 0;

    ~State() {
        if (font) DeleteObject(font);
        if (fontBold) DeleteObject(fontBold);
        if (brushWindow) DeleteObject(brushWindow);
        if (brushSurface) DeleteObject(brushSurface);
        if (brushSurfaceAlt) DeleteObject(brushSurfaceAlt);
    }
};

inline constexpr wchar_t kStateProp[] = L"aiapi.ui.state";

inline State* GetState(HWND window) {
    return reinterpret_cast<State*>(GetPropW(window, kStateProp));
}

inline const Theme& ThemeOf(HWND window) {
    static const Theme fallback = MakeTheme(false);
    const State* state = GetState(window);
    return state ? state->theme : fallback;
}

// ── 字体 ─────────────────────────────────────────────────────────────

inline HFONT CreateUiFont(int dpi, int pointSize, bool bold) {
    LOGFONTW font{};
    font.lfHeight = -MulDiv(pointSize, dpi, 72);
    font.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
    font.lfCharSet = DEFAULT_CHARSET;
    font.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(font.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&font);
}

// 交给 DWM 画深色标题栏。老系统没有该属性时静默失败。
inline void EnableDarkTitleBar(HWND window, bool dark) {
    using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    static const auto setAttribute = []() -> DwmSetWindowAttributeFn {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        return dwm ? reinterpret_cast<DwmSetWindowAttributeFn>(
            GetProcAddress(dwm, "DwmSetWindowAttribute")) : nullptr;
    }();
    if (!setAttribute) return;
    // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE（Win10 1903+），旧版用 19。
    const BOOL value = dark ? TRUE : FALSE;
    if (FAILED(setAttribute(window, 20, &value, sizeof(value)))) {
        setAttribute(window, 19, &value, sizeof(value));
    }
}

// 让这个窗口（及其子控件）允许使用深色主题。
// uxtheme 的 AllowDarkModeForWindow 是窗口级开关：只影响本窗口，
// 不会改变宿主进程的全局深色偏好，插件里用是安全的。
inline void AllowDarkModeForWindow(HWND window, bool dark) {
    using AllowFn = BOOL(WINAPI*)(HWND, BOOL);
    static const auto allow = []() -> AllowFn {
        HMODULE ux = LoadLibraryW(L"uxtheme.dll");
        if (!ux) return nullptr;
        // 序数 133，Win10 1809+ 起稳定存在；取不到就退化成“仅换主题名”。
        return reinterpret_cast<AllowFn>(GetProcAddress(ux, MAKEINTRESOURCEA(133)));
    }();
    if (allow && window) allow(window, dark ? TRUE : FALSE);
}

// 让滚动条、编辑框边框、下拉框、复选框跟着深色主题走。
// 控件类名决定用哪个主题名：Windows 内置的深色主题是按控件族分的，
// 混用会得到“深色底 + 亮边框”这类半成品。
inline void ApplyControlTheme(HWND control, bool dark) {
    using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    static const auto setWindowTheme = []() -> SetWindowThemeFn {
        HMODULE ux = LoadLibraryW(L"uxtheme.dll");
        return ux ? reinterpret_cast<SetWindowThemeFn>(
            GetProcAddress(ux, "SetWindowTheme")) : nullptr;
    }();
    if (!setWindowTheme || !control) return;
    if (!dark) {
        setWindowTheme(control, L"Explorer", nullptr);
        return;
    }

    AllowDarkModeForWindow(control, true);

    wchar_t className[64]{};
    GetClassNameW(control, className, static_cast<int>(std::size(className)));

    const wchar_t* themeName = L"DarkMode_Explorer";
    if (_wcsicmp(className, L"Edit") == 0 || _wcsicmp(className, L"ComboBox") == 0) {
        // 输入框与下拉框的深色边框由 CFD 系列主题提供；Explorer 会留下亮边。
        themeName = L"DarkMode_CFD";
    }
    setWindowTheme(control, themeName, nullptr);
}

// 列表与下拉框的深色边框不受 WM_CTLCOLOR 影响，需要显式换主题；
// 编辑框即使换了主题仍会画亮边界，这里改用无边框风格 + 自绘。
inline void StripEditBorder(HWND control) {
    if (!control) return;
    const LONG_PTR style = GetWindowLongPtrW(control, GWL_EXSTYLE);
    SetWindowLongPtrW(control, GWL_EXSTYLE, style & ~WS_EX_CLIENTEDGE);
    // 去掉 WS_BORDER：EDITTEXT 资源宏默认带它，深色下会画成亮白边框。
    const LONG_PTR frame = GetWindowLongPtrW(control, GWL_STYLE);
    SetWindowLongPtrW(control, GWL_STYLE, frame & ~WS_BORDER);
    SetWindowPos(control, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

// 状态说明的 static 只当数据容器：卡片和文字都由父窗口绘制（见 PaintCardAroundText）。
// 它自己没有可见内容，但文字会随适配器/选择变化，所以每次 WM_SETTEXT 都要
// 让父窗口重画卡片区域；否则界面停在旧文字上。
inline LRESULT CALLBACK StatusLabelProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    static constexpr wchar_t kOrig[] = L"aiapi.ui.statusOrig";
    auto original = reinterpret_cast<WNDPROC>(GetPropW(window, kOrig));
    if (!original) return DefWindowProcW(window, message, wParam, lParam);

    const LRESULT result = CallWindowProcW(original, window, message, wParam, lParam);
    if (message == WM_SETTEXT) {
        if (HWND parent = GetParent(window)) InvalidateRect(parent, nullptr, TRUE);
    }
    return result;
}

inline void InstallStatusLabel(HWND window, int controlId) {
    HWND label = GetDlgItem(window, controlId);
    if (!label) return;
    auto original = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(label, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(StatusLabelProc)));
    if (original) SetPropW(label, L"aiapi.ui.statusOrig", reinterpret_cast<HANDLE>(original));
    ShowWindow(label, SW_HIDE);
}

// 建立主题与字体，套到所有子控件。必须在 WM_INITDIALOG 最前面调用。
inline void Attach(HWND window) {
    auto* state = new State();
    state->dpi = Dpi(window);
    state->theme = MakeTheme(SystemUsesDarkMode());
    state->font = CreateUiFont(state->dpi, 9, false);
    state->fontBold = CreateUiFont(state->dpi, 9, true);
    state->brushWindow = CreateSolidBrush(state->theme.window);
    state->brushSurface = CreateSolidBrush(state->theme.surface);
    state->brushSurfaceAlt = CreateSolidBrush(state->theme.surfaceAlt);
    state->listItemHeight = MulDiv(30, state->dpi, 96);
    SetPropW(window, kStateProp, reinterpret_cast<HANDLE>(state));

    const bool dark = state->theme.dark;
    EnableDarkTitleBar(window, dark);
    AllowDarkModeForWindow(window, dark);
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);

    EnumChildWindows(window, [](HWND child, LPARAM param) -> BOOL {
        auto* s = reinterpret_cast<State*>(param);
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(s->font), TRUE);
        ApplyControlTheme(child, s->theme.dark);
        return TRUE;
    }, reinterpret_cast<LPARAM>(state));

    // 输入框在深色主题下仍会画亮边框，去掉系统边框后由 PaintBackground 统一描边。
    for (int id : { kIdNameEdit, kIdBaseUrlEdit, kIdCredentialEdit, kIdUserIdEdit, kIdScriptEdit, kIdList }) {
        StripEditBorder(GetDlgItem(window, id));
    }

    // 说明文字压在自绘圆角卡片上：控件本身隐藏，卡片与文字由父窗口绘制。
    InstallStatusLabel(window, kIdStatus);
    InstallStatusLabel(window, kIdCustomStatus);
}

inline void Detach(HWND window) {
    if (auto* state = reinterpret_cast<State*>(RemovePropW(window, kStateProp))) delete state;
}

// ── 绘制原语 ─────────────────────────────────────────────────────────

inline void FillSolid(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

inline void FillRoundSolid(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

inline void DrawString(HDC dc, const std::wstring& text, RECT rect, COLORREF color, HFONT font,
    UINT format = DT_LEFT | DT_SINGLELINE | DT_VCENTER) {
    HGDIOBJ previous = font ? SelectObject(dc, font) : nullptr;
    const int oldMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldColor = SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format);
    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldMode);
    if (previous) SelectObject(dc, previous);
}

// 子控件 → 对话框客户区坐标
inline RECT ChildRect(HWND window, HWND child) {
    RECT rect{};
    GetWindowRect(child, &rect);
    MapWindowPoints(HWND_DESKTOP, window, reinterpret_cast<POINT*>(&rect), 2);
    return rect;
}

// ── 对话框背景 ───────────────────────────────────────────────────────

inline void FrameRoundSolid(HDC dc, const RECT& rect, int radius, COLORREF color);


// 说明文字卡片：父窗口负责画圆角底和文字本身。
//
// 为什么不让 static 控件自己画字：控件背景是矩形，圆角卡片会被它自己的
// 底色切出方角；改成 NULL_BRUSH + WS_EX_TRANSPARENT 又会在父窗口重绘后
// 丢掉文字（浅色主题下实测）。所以把 static 当纯数据容器，绘制全在这里做。
inline void PaintCardAroundText(HWND window, HDC dc, HWND control, int padding) {
    if (!control) return;
    State* state = GetState(window);
    if (!state) return;
    const Theme& theme = state->theme;
    const int dpi = state->dpi;

    const int length = GetWindowTextLengthW(control);
    if (length <= 0) return;
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));

    RECT rect = ChildRect(window, control);
    const int width = rect.right - rect.left;

    HGDIOBJ previousFont = SelectObject(dc, state->font);
    RECT measured{ 0, 0, width, 0 };
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &measured,
        DT_LEFT | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    const int textHeight = std::max<int>(measured.bottom - measured.top, 0);

    RECT card{ rect.left - padding, rect.top - padding,
               rect.right + padding, rect.top + textHeight + padding };
    FillRoundSolid(dc, card, MulDiv(6, dpi, 96), theme.surfaceAlt, theme.border);

    RECT textRect{ rect.left, rect.top, rect.right, rect.top + textHeight };
    const int oldMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldColor = SetTextColor(dc, theme.textPrimary);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &textRect,
        DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldMode);
    if (previousFont) SelectObject(dc, previousFont);
}

inline void PaintBackground(HWND window, HDC dc) {
    const Theme& theme = ThemeOf(window);
    const int dpi = [&] {
        const State* state = GetState(window);
        return state ? state->dpi : 96;
    }();
    const auto px = [&](int value) { return MulDiv(value, dpi, 96); };

    RECT client{};
    GetClientRect(window, &client);
    FillSolid(dc, client, theme.window);

    // 输入类控件：去掉系统边框后，用自绘圆角描边统一观感。
    // 顺序在状态卡片之前，卡片才能压住重叠区域。
    for (int id : { kIdNameEdit, kIdBaseUrlEdit, kIdCredentialEdit, kIdUserIdEdit,
                    kIdList, kIdTypeCombo, kIdRefreshCombo, kIdScriptEdit }) {
        HWND control = GetDlgItem(window, id);
        if (!control || !IsWindowVisible(control)) continue;
        RECT rect = ChildRect(window, control);
        InflateRect(&rect, 1, 1);
        FrameRoundSolid(dc, rect, px(4), theme.borderStrong);
    }

    // 状态说明卡片
    if (HWND status = GetDlgItem(window, kIdStatus)) {
        PaintCardAroundText(window, dc, status, px(9));
    }

    // 自定义脚本对话框的状态区
    if (HWND status = GetDlgItem(window, kIdCustomStatus)) {
        PaintCardAroundText(window, dc, status, px(9));
    }

    // 页脚分隔线：锚在默认按钮上沿，保证不会穿过按钮本身。
    if (HWND ok = GetDlgItem(window, IDOK)) {
        const RECT okRect = ChildRect(window, ok);
        const int y = okRect.top - px(10);
        RECT line{ client.left + px(14), y, client.right - px(14), y + 1 };
        FillSolid(dc, line, theme.border);
    }
}

// 圆角描边（只画框，不填充内容区）。
inline void FrameRoundSolid(HDC dc, const RECT& rect, int radius, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

// 控件配色：统一处理 static / edit / listbox。
// 返回画刷供 Windows 填充控件背景；不认识的消息返回 nullptr。
inline HBRUSH ControlColor(HWND window, UINT message, HDC dc, HWND control) {
    State* state = GetState(window);
    if (!state) return nullptr;
    const Theme& theme = state->theme;

    switch (message) {
    case WM_CTLCOLORSTATIC: {
        SetBkMode(dc, TRANSPARENT);
        const bool enabled = control != nullptr && IsWindowEnabled(control) != FALSE;
        const int id = control ? GetDlgCtrlID(control) : 0;

        // 分组标题、字段标签、页脚提示：次要文字色，与输入内容拉开层级。
        const bool secondary = id == kIdGroupSites || id == kIdLabelType ||
            id == kIdLabelName || id == kIdBaseUrlLabel || id == kIdCredentialLabel ||
            id == kIdUserIdLabel || id == kIdHint;
        const bool card = id == kIdStatus || id == kIdCustomStatus;

        SetTextColor(dc, enabled ? (secondary ? theme.textSecondary : theme.textPrimary)
                                 : theme.textDisabled);
        if (card) {
            // 卡片由 PaintBackground 画圆角底；控件必须透明，否则矩形的控件背景
            // 会盖住卡片圆角，露出一块方角深色。
            return static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
        }
        return state->brushWindow;
    }
    case WM_CTLCOLOREDIT:
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, theme.surface);
        SetTextColor(dc, IsWindowEnabled(control) ? theme.textPrimary : theme.textDisabled);
        return state->brushSurface;
    case WM_CTLCOLORLISTBOX:
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, theme.surface);
        SetTextColor(dc, theme.textPrimary);
        return state->brushSurface;
    case WM_CTLCOLORDLG:
        return state->brushWindow;
    default:
        return nullptr;
    }
}

}  // namespace ui
