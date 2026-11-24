/* 启动浏览器.c — 双击启动 Nexus 专用浏览器 */
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmd, int show) {
    wchar_t dir[MAX_PATH];
    wchar_t script[MAX_PATH];
    wchar_t args[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    (void)hInst; (void)hPrev; (void)cmd; (void)show;
    if (n == 0) return 1;
    for (DWORD i = n; i > 0; i--) {
        if (dir[i - 1] == L'\\') { dir[i - 1] = 0; break; }
    }
    _snwprintf(script, MAX_PATH, L"%s\\浏览器\\Nexus浏览器.py", dir);
    _snwprintf(args, MAX_PATH * 2, L"\"%s\"", script);
    HINSTANCE h = ShellExecuteW(NULL, L"open", L"python.exe", args, dir, SW_SHOWNORMAL);
    if ((INT_PTR)h <= 32) {
        MessageBoxW(NULL,
            L"未找到 Python。\n请安装 Python 3.10+ 并加入 PATH 后重试。",
            L"Nexus 浏览器 - 启动失败", MB_ICONERROR | MB_OK);
    }
    return 0;
}
