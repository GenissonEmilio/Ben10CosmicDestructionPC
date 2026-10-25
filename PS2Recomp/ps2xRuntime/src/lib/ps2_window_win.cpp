// GoW2-Port: Windows window tweaks that raylib/GLFW do not expose. F10 is a system key on Windows: releasing it opens the
// window's (empty) menu mode, which swallows its key-up and the next key the player presses. The F10 performance overlay
// (gs_host_present.cpp) needs a plain key, so the keyboard-activated menu (WM_SYSCOMMAND SC_KEYMENU with lParam 0) is
// dropped; Alt+Space and mouse access to the system menu keep working.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace
{
WNDPROC g_previousProc = nullptr;

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_SYSCOMMAND && (wParam & 0xFFF0u) == SC_KEYMENU && lParam == 0)
        return 0;
    return CallWindowProcW(g_previousProc, hwnd, message, wParam, lParam);
}
}

void ps2WindowDisableKeyMenu(void *windowHandle)
{
    HWND hwnd = static_cast<HWND>(windowHandle);
    if (hwnd == nullptr || g_previousProc != nullptr)
        return;
    g_previousProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&windowProc)));
}
#else
void ps2WindowDisableKeyMenu(void *) {}
#endif
