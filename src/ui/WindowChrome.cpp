#include "ui/WindowChrome.h"

#include <QWidget>

#ifdef Q_OS_WIN
#  include <windows.h>

namespace {

using DwmSetWindowAttributeFn = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);

/// dwmapi 只在真正调用时才加载，避免多一个链接依赖
DwmSetWindowAttributeFn dwmSetWindowAttribute()
{
    static DwmSetWindowAttributeFn fn = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        if (HMODULE dwm = ::LoadLibraryW(L"dwmapi.dll")) {
            // 先转成 void* 再转函数指针，避免 GCC 的 -Wcast-function-type 警告
            fn = reinterpret_cast<DwmSetWindowAttributeFn>(
                reinterpret_cast<void *>(::GetProcAddress(dwm, "DwmSetWindowAttribute")));
        }
    }
    return fn;
}

} // namespace
#endif

namespace WindowChrome {

void apply(QWidget *window, bool dark, const QColor &captionColor, const QColor &textColor)
{
#ifdef Q_OS_WIN
    if (window == nullptr)
        return;

    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (hwnd == nullptr)
        return;

    const DwmSetWindowAttributeFn setAttribute = dwmSetWindowAttribute();
    if (setAttribute == nullptr)
        return;

    // DWMWA_USE_IMMERSIVE_DARK_MODE：Win10 1809 起可用，属性号老版本是 19，20 是新号，
    // 两个都设一遍，失败的那个系统会自己返回 E_INVALIDARG，不影响另一个。
    const DWORD darkFlag = dark ? 1u : 0u;
    setAttribute(hwnd, 20, &darkFlag, sizeof(darkFlag));
    setAttribute(hwnd, 19, &darkFlag, sizeof(darkFlag));

    // DWMWA_CAPTION_COLOR(35) / DWMWA_TEXT_COLOR(36)：Win11 22H2 起支持，
    // 其它版本会直接拒绝，保持系统默认（深/浅）配色即可。
    if (captionColor.isValid()) {
        const COLORREF caption =
            RGB(captionColor.red(), captionColor.green(), captionColor.blue());
        setAttribute(hwnd, 35, &caption, sizeof(caption));
    }
    if (textColor.isValid()) {
        const COLORREF text = RGB(textColor.red(), textColor.green(), textColor.blue());
        setAttribute(hwnd, 36, &text, sizeof(text));
    }
    // 部分系统改完配色要重绘非客户区才立刻生效
    ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
                       | SWP_FRAMECHANGED);
#else
    Q_UNUSED(window)
    Q_UNUSED(dark)
    Q_UNUSED(captionColor)
    Q_UNUSED(textColor)
#endif
}

} // namespace WindowChrome
