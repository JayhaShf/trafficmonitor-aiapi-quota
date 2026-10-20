// 自绘控件层：站点列表、push button、状态圆点。
//
// 只接管“画”的部分，所有消息与状态仍归对话框逻辑所有：
//  - 列表用 LBS_OWNERDRAWFIXED / LBS_HASSTRINGS，字符串仍存在控件里，
//    业务代码继续用 LB_ADDSTRING / LB_GETCURSEL，不用改。
//  - 按钮用 BS_OWNERDRAW，点击仍发 BN_CLICKED / IDOK / IDCANCEL。
//    IDOK / IDCANCEL 的语义（EndDialog）由对话框自己处理，这里不碰。

#pragma once

#include "UiTheme.h"

#include <windows.h>

#include <string>

namespace ui {

// ── 站点列表 ─────────────────────────────────────────────────────────

// 从列表项字符串里拆出“是否启用”与显示名。
// 约定格式：L"[启用] 名称" / L"[停用] 名称"（由对话框写入）。
inline void SplitSiteLabel(const std::wstring& label, bool& enabled, std::wstring& name) {
    enabled = true;
    name = label;
    if (label.rfind(L"[启用] ", 0) == 0) {
        enabled = true;
        name = label.substr(5);
    } else if (label.rfind(L"[停用] ", 0) == 0) {
        enabled = false;
        name = label.substr(5);
    }
}

inline void DrawOwnerListBox(HWND window, const DRAWITEMSTRUCT& item) {
    State* state = GetState(window);
    if (!state || item.itemID == static_cast<UINT>(-1)) return;
    const Theme& theme = state->theme;
    const int dpi = state->dpi;
    const auto px = [&](int value) { return MulDiv(value, dpi, 96); };

    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;

    RECT row = item.rcItem;
    FillSolid(item.hDC, row, selected ? theme.accentSoft : theme.surface);

    // 选中态：左侧 2px 强调条
    if (selected) {
        RECT bar{ row.left, row.top, row.left + px(2), row.bottom };
        FillSolid(item.hDC, bar, theme.accent);
    }

    wchar_t buffer[256]{};
    const LRESULT length = SendMessageW(item.hwndItem, LB_GETTEXT, item.itemID,
        reinterpret_cast<LPARAM>(buffer));
    std::wstring label = length > 0 ? std::wstring(buffer, static_cast<size_t>(length)) : L"";

    bool enabled = true;
    std::wstring name;
    SplitSiteLabel(label, enabled, name);

    // 状态圆点
    const int radius = px(4);
    const int centerX = row.left + px(14);
    const int centerY = (row.top + row.bottom) / 2;
    const COLORREF dotColor = enabled ? theme.success : theme.textDisabled;
    HBRUSH dot = CreateSolidBrush(dotColor);
    HPEN dotPen = CreatePen(PS_SOLID, 1, dotColor);
    HGDIOBJ oldBrush = SelectObject(item.hDC, dot);
    HGDIOBJ oldPen = SelectObject(item.hDC, dotPen);
    Ellipse(item.hDC, centerX - radius, centerY - radius, centerX + radius, centerY + radius);
    SelectObject(item.hDC, oldPen);
    SelectObject(item.hDC, oldBrush);
    DeleteObject(dotPen);
    DeleteObject(dot);
    if (!enabled) {
        // 停用：圆点描边而非实心，弱化存在感
        HBRUSH inner = CreateSolidBrush(theme.surface);
        HGDIOBJ oldInner = SelectObject(item.hDC, inner);
        const int innerRadius = radius - px(2);
        Ellipse(item.hDC, centerX - innerRadius, centerY - innerRadius,
            centerX + innerRadius, centerY + innerRadius);
        SelectObject(item.hDC, oldInner);
        DeleteObject(inner);
    }

    // 名称（过长省略）
    RECT textRect{ centerX + px(12), row.top, row.right - px(8), row.bottom };
    const COLORREF textColor = disabled ? theme.textDisabled
        : (selected ? theme.textPrimary : (enabled ? theme.textPrimary : theme.textSecondary));
    DrawString(item.hDC, name, textRect, textColor, state->font,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

// 让列表框按状态圆点的高度取行高。返回 0 表示让系统自己算。
inline UINT MeasureListItem(HWND window, const MEASUREITEMSTRUCT& item) {
    const State* state = GetState(window);
    if (!state) return item.itemHeight;
    return item.itemHeight != 0 ? item.itemHeight : static_cast<UINT>(state->listItemHeight);
}

// ── 按钮 ─────────────────────────────────────────────────────────────

enum class ButtonKind {
    Secondary,  // 普通次要操作
    Primary,    // 默认动作（保存）
    Danger,     // 删除
};

inline ButtonKind ButtonKindFor(HWND window, int controlId, HWND control) {
    if (controlId == IDOK) return ButtonKind::Primary;
    if (controlId == 1003 /* IDC_SITE_REMOVE */) return ButtonKind::Danger;
    return ButtonKind::Secondary;
}

inline void DrawOwnerButton(HWND window, const DRAWITEMSTRUCT& item) {
    State* state = GetState(window);
    if (!state) return;
    const Theme& theme = state->theme;
    const int dpi = state->dpi;
    const auto px = [&](int value) { return MulDiv(value, dpi, 96); };

    const int controlId = GetDlgCtrlID(item.hwndItem);
    const ButtonKind kind = ButtonKindFor(window, controlId, item.hwndItem);
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;

    RECT rect = item.rcItem;
    COLORREF fill = theme.surface;
    COLORREF border = theme.borderStrong;
    COLORREF text = theme.textPrimary;

    switch (kind) {
    case ButtonKind::Primary:
        fill = disabled ? theme.surfaceAlt : theme.accent;
        border = fill;
        text = disabled ? theme.textDisabled : theme.accentText;
        break;
    case ButtonKind::Danger:
        text = disabled ? theme.textDisabled : theme.danger;
        break;
    case ButtonKind::Secondary:
        text = disabled ? theme.textDisabled : theme.textPrimary;
        break;
    }

    if (disabled) {
        fill = theme.surfaceAlt;
        border = theme.border;
    } else if (pressed && kind != ButtonKind::Primary) {
        fill = theme.accentSoft;
    } else if (pressed) {
        // 主按钮按下：整体压暗一档
        const int r = (GetRValue(fill) * 88) / 100;
        const int g = (GetGValue(fill) * 88) / 100;
        const int b = (GetBValue(fill) * 88) / 100;
        fill = RGB(r, g, b);
        border = fill;
    }

    FillRoundSolid(item.hDC, rect, px(5), fill, border);

    // 焦点框：内缩 2px 的圆角虚线，避免和实心描边叠在一起
    if (focused && !disabled) {
        RECT inner = rect;
        InflateRect(&inner, -px(2), -px(2));
        HPEN pen = CreatePen(PS_DOT, 1, kind == ButtonKind::Primary ? theme.accentText : theme.textPrimary);
        HGDIOBJ oldPen = SelectObject(item.hDC, pen);
        HGDIOBJ oldBrush = SelectObject(item.hDC, GetStockObject(NULL_BRUSH));
        RoundRect(item.hDC, inner.left, inner.top, inner.right, inner.bottom, px(4), px(4));
        SelectObject(item.hDC, oldBrush);
        SelectObject(item.hDC, oldPen);
        DeleteObject(pen);
    }

    wchar_t caption[128]{};
    GetWindowTextW(item.hwndItem, caption, static_cast<int>(std::size(caption)));
    DrawString(item.hDC, caption, rect, text, state->font, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
}

// ── 系统绘制的控件：自绘接管 ─────────────────────────────────────────
//
// 复选框和下拉箭头由 comctl32 按系统主题画，不认 WM_CTLCOLOR，
// 在深色底上会留白块。这里用子类化（不改控件样式、不改消息协议）
// 接管 WM_PAINT，只重画外观。

inline constexpr wchar_t kOrigProcProp[] = L"aiapi.ui.origProc";

inline void PaintChildBackground(HWND control, HDC dc) {
    // 用父对话框的底色铺一层，避免闪烁与残影。
    HWND parent = GetParent(control);
    const Theme& theme = ThemeOf(parent ? parent : control);
    RECT client{};
    GetClientRect(control, &client);
    FillSolid(dc, client, theme.window);
}

// 复选框：圆角方框 + 勾。状态仍由 BST_CHECKED 决定，点击行为交给默认过程。
inline LRESULT CALLBACK CheckboxProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto original = reinterpret_cast<WNDPROC>(GetPropW(window, kOrigProcProp));
    if (!original) return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        PaintChildBackground(window, dc);

        HWND parent = GetParent(window);
        const State* state = GetState(parent);
        const Theme theme = state ? state->theme : MakeTheme(SystemUsesDarkMode());
        const int dpi = state ? state->dpi : 96;
        const auto px = [&](int value) { return MulDiv(value, dpi, 96); };

        RECT client{};
        GetClientRect(window, &client);
        const bool enabled = IsWindowEnabled(window) != FALSE;
        const bool checked = SendMessageW(window, BM_GETCHECK, 0, 0) == BST_CHECKED;

        const int box = px(14);
        RECT boxRect{ client.left + px(1), (client.top + client.bottom - box) / 2,
                      client.left + px(1) + box, (client.top + client.bottom - box) / 2 + box };

        const COLORREF borderColor = enabled ? theme.borderStrong : theme.border;
        const COLORREF fillColor = checked
            ? (enabled ? theme.accent : theme.border)
            : theme.surface;
        FillRoundSolid(dc, boxRect, px(3), fillColor, borderColor);

        if (checked) {
            HPEN pen = CreatePen(PS_SOLID, px(2), enabled ? theme.accentText : theme.textDisabled);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            const int cx = (boxRect.left + boxRect.right) / 2;
            const int cy = (boxRect.top + boxRect.bottom) / 2;
            MoveToEx(dc, cx - px(4), cy, nullptr);
            LineTo(dc, cx - px(1), cy + px(3));
            LineTo(dc, cx + px(4), cy - px(3));
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        }

        wchar_t caption[128]{};
        GetWindowTextW(window, caption, static_cast<int>(std::size(caption)));
        RECT textRect{ boxRect.right + px(7), client.top, client.right, client.bottom };
        DrawString(dc, caption, textRect, enabled ? theme.textPrimary : theme.textDisabled,
            state ? state->font : nullptr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        // 键盘焦点：文字下画虚线下划线
        if (GetFocus() == window) {
            RECT focusRect = textRect;
            InflateRect(&focusRect, px(2), px(1));
            DrawFocusRect(dc, &focusRect);
        }

        EndPaint(window, &paint);
        return 0;
    }
    default:
        return CallWindowProcW(original, window, message, wParam, lParam);
    }
}

// 下拉框（CBS_DROPDOWNLIST）：底色 + 当前项 + 自绘箭头。
// 下拉列表本身走 WM_CTLCOLORLISTBOX，已由对话框统一配色。
inline LRESULT CALLBACK ComboProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto original = reinterpret_cast<WNDPROC>(GetPropW(window, kOrigProcProp));
    if (!original) return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);

        HWND parent = GetParent(window);
        const State* state = GetState(parent);
        const Theme theme = state ? state->theme : MakeTheme(SystemUsesDarkMode());
        const int dpi = state ? state->dpi : 96;
        const auto px = [&](int value) { return MulDiv(value, dpi, 96); };

        RECT client{};
        GetClientRect(window, &client);
        const bool enabled = IsWindowEnabled(window) != FALSE;

        FillRoundSolid(dc, client, px(4), theme.surface,
            enabled ? theme.borderStrong : theme.border);

        // 当前选中项文本
        const LRESULT selected = SendMessageW(window, CB_GETCURSEL, 0, 0);
        if (selected >= 0) {
            wchar_t text[256]{};
            SendMessageW(window, CB_GETLBTEXT, static_cast<WPARAM>(selected),
                reinterpret_cast<LPARAM>(text));
            RECT textRect{ client.left + px(8), client.top, client.right - px(26), client.bottom };
            DrawString(dc, text, textRect,
                enabled ? theme.textPrimary : theme.textDisabled,
                state ? state->font : nullptr,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }

        // 箭头
        const int cx = client.right - px(14);
        const int cy = (client.top + client.bottom) / 2;
        HPEN pen = CreatePen(PS_SOLID, px(1), enabled ? theme.textSecondary : theme.textDisabled);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        MoveToEx(dc, cx - px(4), cy - px(2), nullptr);
        LineTo(dc, cx, cy + px(3));
        LineTo(dc, cx + px(4), cy - px(2));
        SelectObject(dc, oldPen);
        DeleteObject(pen);

        // 焦点框
        if (GetFocus() == window) {
            RECT focusRect = client;
            InflateRect(&focusRect, -px(2), -px(2));
            DrawFocusRect(dc, &focusRect);
        }

        EndPaint(window, &paint);
        return 0;
    }
    default:
        return CallWindowProcW(original, window, message, wParam, lParam);
    }
}

inline void InstallPaintSubclass(HWND control, WNDPROC replacement) {
    if (!control) return;
    auto original = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(control, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(replacement)));
    if (original) SetPropW(control, kOrigProcProp, reinterpret_cast<HANDLE>(original));
    InvalidateRect(control, nullptr, TRUE);
}

// 深色模式下接管复选框 / 下拉框；浅色模式用系统原生绘制。
inline void InstallDarkPaint(HWND dialog, bool dark) {
    if (!dark) return;
    InstallPaintSubclass(GetDlgItem(dialog, kIdSiteEnabled), CheckboxProc);
    InstallPaintSubclass(GetDlgItem(dialog, kIdTypeCombo), ComboProc);
    InstallPaintSubclass(GetDlgItem(dialog, kIdRefreshCombo), ComboProc);
}

}  // namespace ui
