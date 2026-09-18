/*
 * win32-helloworld — 极简 Win32 窗口程序
 *
 * 目标：32 位 exe，兼容 Windows 7(32/64 位)，支持高分屏，也能在 Win11 上运行。
 *
 * 编译：
 *   i686-w64-mingw32-gcc -O2 -Wall -Wextra -mwindows -static -s -o hello.exe hello.c
 *
 * 兼容性要点（三条缺一不可）：
 *   1) WINVER / _WIN32_WINNT = 0x0601，只调用 Win7 就存在的 API。
 *   2) 所有 DPI 相关 API 都用 GetProcAddress 动态解析。静态导入会让 exe 在 Win7 上
 *      因“找不到过程入口点”而直接加载失败：
 *        user32!SetProcessDpiAwarenessContext  Win10 1703+  → Per-Monitor V2（Win11 走这条）
 *        shcore!SetProcessDpiAwareness         Win8.1+      → Per-Monitor
 *        user32!SetProcessDPIAware             Vista+/Win7  → 系统 DPI 感知（Win7 走这条）
 *        user32!GetDpiForWindow                Win10 1607+  → 取窗口 DPI；缺失时回退 GetDeviceCaps
 *   3) 只依赖 msvcrt.dll（Win7 系统自带），不依赖 UCRT（Win7 需另装 KB2999226）。
 *      -static 是为了不拖带 libgcc_s_dw2-1.dll / libwinpthread-1.dll。
 */
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#define UNICODE  /* 全程使用 ...W 版本 API，并让 IDC_*/MAKEINTRESOURCE 等宏走宽字符 */
#define _UNICODE
#include <windows.h>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0 /* Win8.1+ 才有；WINVER=0x0601 时头文件不定义 */
#endif

#define DPI_CTX_PER_MONITOR_AWARE_V2 ((HANDLE)-4) /* Win10 1703+ 的 DPI_AWARENESS_CONTEXT */
#define PROCESS_PER_MONITOR_DPI_AWARE 2           /* shcore 的 PROCESS_DPI_AWARENESS */
#define BASE_DPI 96

typedef BOOL(WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);
typedef HRESULT(WINAPI *PFN_SetProcessDpiAwareness)(int);
typedef BOOL(WINAPI *PFN_SetProcessDPIAware)(void);
typedef UINT(WINAPI *PFN_GetDpiForWindow)(HWND);

static HFONT g_font;

/* 从 user32.dll 动态取函数；Win7 上不存在则返回 NULL */
static FARPROC User32Proc(const char *name)
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    return u ? GetProcAddress(u, name) : NULL;
}

/* 尽力开启最高等级的 DPI 感知；老系统上逐级降级，最差情况什么都不做 */
static void EnableBestDpiAwareness(void)
{
    PFN_SetProcessDpiAwarenessContext v2 =
        (PFN_SetProcessDpiAwarenessContext)(void *)User32Proc("SetProcessDpiAwarenessContext");
    if (v2 && v2(DPI_CTX_PER_MONITOR_AWARE_V2))
        return; /* Win10 1703+ */

    {
        HMODULE shcore = LoadLibraryW(L"shcore.dll"); /* Win8.1+ */
        if (shcore) {
            PFN_SetProcessDpiAwareness set =
                (PFN_SetProcessDpiAwareness)(void *)GetProcAddress(shcore, "SetProcessDpiAwareness");
            if (set && set(PROCESS_PER_MONITOR_DPI_AWARE) == S_OK)
                return;
        }
    }

    {
        PFN_SetProcessDPIAware aware = (PFN_SetProcessDPIAware)(void *)User32Proc("SetProcessDPIAware");
        if (aware)
            aware(); /* Vista+ / Win7 */
    }
}

/* 取窗口（hwnd 为 NULL 时取屏幕）当前 DPI */
static UINT DpiOf(HWND hwnd)
{
    PFN_GetDpiForWindow get = (PFN_GetDpiForWindow)(void *)User32Proc("GetDpiForWindow");
    if (get) {
        UINT dpi = get(hwnd);
        if (dpi)
            return dpi;
    }

    {
        HDC dc = GetDC(hwnd); /* Win7 回退路径 */
        UINT dpi = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSY) : 0;
        if (dc)
            ReleaseDC(hwnd, dc);
        return dpi ? dpi : BASE_DPI;
    }
}

/* 取系统消息字体，并按当前 DPI 缩放（保留用户在系统里设的字号） */
static HFONT MakeFont(UINT dpi)
{
    NONCLIENTMETRICSW ncm;
    LOGFONTW lf;
    int pt;

    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        lf = ncm.lfMessageFont;
    } else {
        ZeroMemory(&lf, sizeof(lf));
        lf.lfHeight = -12;
        lstrcpynW(lf.lfFaceName, L"MS Shell Dlg", 32);
    }

    pt = MulDiv(-lf.lfHeight, 72, BASE_DPI); /* 逻辑高度 → 磅值 */
    if (pt < 1)
        pt = 9;
    lf.lfHeight = -MulDiv(pt, (int)dpi, 72); /* 再按目标 DPI 还原 */

    return CreateFontIndirectW(&lf);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_font = MakeFont(DpiOf(hwnd));
        return 0;

    case WM_DPICHANGED: { /* 拖到缩放比例不同的显示器时（Win8.1+） */
        const RECT *suggest = (const RECT *)lp;
        SetWindowPos(hwnd, NULL, suggest->left, suggest->top, suggest->right - suggest->left,
                     suggest->bottom - suggest->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_font)
            DeleteObject(g_font);
        g_font = MakeFont((UINT)LOWORD(wp)); /* wParam 低字 = 新 DPI */
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        HGDIOBJ old;

        GetClientRect(hwnd, &rc);
        old = SelectObject(dc, g_font ? g_font : GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, L"Hello, World!", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        if (g_font) {
            DeleteObject(g_font);
            g_font = NULL;
        }
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    WNDCLASSEXW wc;
    RECT rc;
    HWND hwnd;
    MSG msg;
    UINT dpi;

    (void)prev;
    (void)cmdline;

    EnableBestDpiAwareness(); /* 必须在创建任何窗口之前调用 */

    dpi = DpiOf(NULL); /* 主显示器 DPI：窗口初始尺寸也按它缩放 */

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"HelloWin32Class";
    if (!RegisterClassExW(&wc))
        return 1;

    rc.left = 0;
    rc.top = 0;
    rc.right = MulDiv(320, (int)dpi, BASE_DPI); /* 客户区 320x120 逻辑像素 */
    rc.bottom = MulDiv(120, (int)dpi, BASE_DPI);
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);

    hwnd = CreateWindowExW(0, wc.lpszClassName, L"Hello, World!", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                           CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, inst, NULL);
    if (!hwnd)
        return 2;

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}