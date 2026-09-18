/* ============================================================================
 * simplehyperv.c — 极简 Hyper-V 图形前端（Win32 原生 UI + 子进程调用 PowerShell）
 *
 * 原版是 C# WinForms，用进程内 PowerShell SDK（System.Management.Automation）
 * 拿结构化 PSObject。这里不链接任何 .NET/PowerShell 组件，改成把脚本交给
 * powershell.exe 子进程跑，抓 stdout 文本。代价是失去结构化输出，收益是零依赖。
 *
 * 六个必须记住的坑（都已在代码里处理）：
 *   1. -EncodedCommand (Base64/UTF-16LE) 传脚本 —— 彻底绕开命令行引号地狱，
 *      且脚本里的中文不会因为代码页被破坏。
 *   2. -OutputFormat Text 必须加。不加的话 PowerShell 5.1 会把 stderr 序列化成
 *      CLIXML（"#< CLIXML ..." XML 垃圾）灌进日志；加了才是纯文本。
 *   3. 脚本第一句必须是 [Console]::OutputEncoding=UTF8，否则管道里拿到的是
 *      OEM 代码页（中文系统 936），VM 名含中文就乱码。
 *   4. 用户输入一律用单引号包裹并把 ' 翻倍，绝不拼双引号字符串 —— 否则名字里
 *      带引号就能破坏脚本结构。
 *   5. 每个脚本包在 try/catch 里，失败输出 "ERROR: ..." 并以退出码 1 结束，
 *      这样 C 侧既能显示原因也能判成败。
 *   6. 需要的返回值（VM 的 Name/State/Id）在 PS 里主动拼成 tab 分隔的一行，
 *      不要去解析 Get-VM 的默认表格 —— 默认表格会按控制台宽度截断并加 "..."。
 *
 * 目标平台：Win10/11 x64（Hyper-V 只在这里有）。Hyper-V 的 PowerShell 模块只有
 * 64 位版，所以本程序必须是 x64 —— 32 位进程加载不了该模块。
 * ========================================================================== */

#define WINVER       0x0601
#define _WIN32_WINNT 0x0601
/* 注意：链接时用了 -municode，mingw 的 specs 已经带了 -DUNICODE，这里加保护避免重定义 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

/* ---------------------------------------------------------------- 资源 / 常量 */

#define IDI_APP          101
#define IDR_HVINTEGRATE  102

#define MODE_VM      0
#define MODE_SWITCH  1
#define MODE_NAT     2

#define IDC_COMBO    2001
#define IDC_PROGRESS 2002
#define IDC_LIST     2003
#define IDC_LOG      2004
#define IDC_REFRESH  2010
#define IDC_START    2011
#define IDC_STOP     2012
#define IDC_CONNECT  2013
#define IDC_NEW      2014
#define IDC_DELETE   2015
#define IDC_MORE     2016

#define IDM_MGR       3001
#define IDM_HVSET     3002
#define IDM_VSSET     3003
#define IDM_EDITDISK  3004
#define IDM_CREATEVM  3005
#define IDM_OPTVHD    3006
#define IDM_NETCONN   3007
#define IDM_MSTSC     3008
#define IDM_TASKSCHD  3009
#define IDM_OPENDIR   3010
#define IDM_EXIT      3011
#define IDM_SHOWLOGS  3020
#define IDM_TRAYOPT   3021
#define IDM_AUTOSTART 3022
#define IDM_ELEVATE   3023
#define IDM_RESETDEF  3024
#define IDM_ABOUT     3030
#define IDM_VMINFO    3040
#define IDM_VMSET     3041
#define IDM_NESTED    3042
#define IDM_VMDELETE  3043
#define IDM_TRAYSHOW  3050
#define IDM_TRAYEXIT  3051

#define WM_APP_DONE    (WM_APP + 1)
#define WM_APP_TRAY    (WM_APP + 2)
#define WM_APP_RESTORE (WM_APP + 3)

#define SHV_CLASS      L"SimpleHyperVWin32Class"
#define SHV_POPUPCLASS L"SimpleHyperVWin32Popup"
#define SHV_TITLE      L"Simple Hyper-V (Win32)"
#define SHV_REGKEY     L"Software\\SimpleHyperVWin32"
#define SHV_MUTEX      L"Local\\SimpleHyperVWin32Mutex"
#define SHV_TASKNAME   L"SimpleHyperVWin32"

/* 任务类型：跑完命令之后干什么 */
#define RA_LOG     0   /* 只写日志 */
#define RA_LIST    1   /* 用输出填充列表 */
#define RA_REFRESH 2   /* 写日志后自动刷新列表 */

/* ------------------------------------------------------------------- 全局状态 */

static HINSTANCE g_inst;
static HWND  g_hwnd, g_combo, g_progress, g_list, g_log;
static HFONT g_font, g_fontFixed;
static HICON g_iconBig, g_iconSmall;
static HBRUSH g_whiteBrush;

static int  g_mode        = MODE_VM;
static int  g_showLogs    = 0;
static int  g_closeToTray = 0;
static int  g_autoStartup = 0;
static BOOL g_busy        = 0;
static BOOL g_exiting     = 0;
static BOOL g_trayAdded   = 0;
static BOOL g_settingsWarned = 0;

typedef struct { wchar_t *name, *state, *id; } Item;
static Item *g_items;
static int   g_nItems;

/* 前置声明：Log 在下面几个"底层"函数里被用到 */
static void LogAppend(const wchar_t *text);
static void LogText(const wchar_t *text);
static void Log(const wchar_t *fmt, ...);
static void UpdateButtons(void);
static void LayoutAll(HWND hwnd);
int SHVPopup(HWND owner, const wchar_t *title,
             const wchar_t *l1, const wchar_t *i1,
             const wchar_t *l2, const wchar_t *i2,
             wchar_t *o1, size_t c1, wchar_t *o2, size_t c2);

/* ------------------------------------------------------- 动态解析的 DPI 相关 API */
/* 一律 GetProcAddress，绝不静态导入：静态导入 Win8.1+/Win10+ 的 DPI API 会让
 * exe 在老系统上直接"找不到入口点"加载失败，而不是给出一条能看懂的错误。 */

typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);
typedef HRESULT (WINAPI *PFN_SetProcessDpiAwareness)(int);
typedef BOOL (WINAPI *PFN_SetProcessDPIAware)(void);
typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);

static HMODULE g_user32, g_shcore;

static void EnableBestDpiAwareness(void)
{
    PFN_SetProcessDpiAwarenessContext setCtx;
    PFN_SetProcessDpiAwareness        setAware;
    PFN_SetProcessDPIAware            setOld;

    g_user32 = GetModuleHandleW(L"user32.dll");
    g_shcore = LoadLibraryW(L"shcore.dll");   /* Win7 上没有，失败是正常的 */

    /* Win10 1703+：Per-Monitor V2，(HANDLE)-4 */
    if (g_user32) {
        setCtx = (PFN_SetProcessDpiAwarenessContext)
                 (void *)GetProcAddress(g_user32, "SetProcessDpiAwarenessContext");
        if (setCtx && setCtx((HANDLE)-4)) return;
    }
    /* Win8.1+：PROCESS_PER_MONITOR_DPI_AWARE = 2 */
    if (g_shcore) {
        setAware = (PFN_SetProcessDpiAwareness)
                   (void *)GetProcAddress(g_shcore, "SetProcessDpiAwareness");
        if (setAware && SUCCEEDED(setAware(2))) return;
    }
    /* Vista/Win7 */
    if (g_user32) {
        setOld = (PFN_SetProcessDPIAware)
                 (void *)GetProcAddress(g_user32, "SetProcessDPIAware");
        if (setOld) setOld();
    }
}

static UINT DpiOf(HWND hwnd)
{
    PFN_GetDpiForWindow getDpi;
    if (g_user32 && hwnd) {
        getDpi = (PFN_GetDpiForWindow)
                 (void *)GetProcAddress(g_user32, "GetDpiForWindow");
        if (getDpi) {
            UINT d = getDpi(hwnd);
            if (d) return d;
        }
    }
    {   /* Win7 回退：屏幕 DPI */
        HDC dc = GetDC(NULL);
        int d = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
        if (dc) ReleaseDC(NULL, dc);
        return d > 0 ? (UINT)d : 96;
    }
}

/* ------------------------------------------------------------------ 小工具函数 */

static wchar_t *W(const char *utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    wchar_t *s = (wchar_t *)malloc((n > 0 ? n : 1) * sizeof(wchar_t));
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8, -1, s, n);
    else s[0] = 0;
    return s;
}

static char *U8(const wchar_t *s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char *o = (char *)malloc(n > 0 ? n : 1);
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s, -1, o, n, NULL, NULL);
    else o[0] = 0;
    return o;
}

/* 调试日志：GUI 子系统程序没有控制台，想看内部状态只能落到文件。
 * 设了环境变量 SHV_DBG（值随意）就把关键状态追加到 <程序目录>\_shv_dbg.txt。 */
static void Dbg(const wchar_t *fmt, ...)
{
    wchar_t path[MAX_PATH], buf[2048];
    va_list ap;
    FILE *f;

    if (GetEnvironmentVariableW(L"SHV_DBG", NULL, 0) == 0) return;

    GetModuleFileNameW(NULL, path, MAX_PATH);
    {
        wchar_t *s = wcsrchr(path, L'\\');
        if (s) *s = 0;
    }
    wcscat(path, L"\\_shv_dbg.txt");

    va_start(ap, fmt);
    _vsnwprintf(buf, 2047, fmt, ap);
    va_end(ap);
    buf[2047] = 0;

    f = _wfopen(path, L"ab");
    if (!f) return;
    {
        wchar_t line[2200];
        char *u8;
        _snwprintf(line, 2199, L"%ls\r\n", buf);
        line[2199] = 0;
        u8 = U8(line);
        fwrite(u8, 1, strlen(u8), f);
        free(u8);
    }
    fclose(f);
}

/* 可增长的宽字符串缓冲 */
typedef struct { wchar_t *p; size_t len, cap; } WBuf;

static void WInit(WBuf *b)
{
    b->cap = 256; b->len = 0;
    b->p = (wchar_t *)malloc(b->cap * sizeof(wchar_t));
    b->p[0] = 0;
}
static void WNeed(WBuf *b, size_t extra)
{
    if (b->len + extra + 1 > b->cap) {
        while (b->len + extra + 1 > b->cap) b->cap *= 2;
        b->p = (wchar_t *)realloc(b->p, b->cap * sizeof(wchar_t));
    }
}
static void WCat(WBuf *b, const wchar_t *s)
{
    size_t n = wcslen(s);
    WNeed(b, n);
    wmemcpy(b->p + b->len, s, n + 1);
    b->len += n;
}
/* 把用户输入拼成 PowerShell 单引号字符串，内部的 ' 翻倍 —— 唯一的转义入口 */
static void WCatQ(WBuf *b, const wchar_t *s)
{
    WNeed(b, wcslen(s) * 2 + 3);
    b->p[b->len++] = L'\'';
    for (; *s; s++) {
        if (*s == L'\'') b->p[b->len++] = L'\'';
        b->p[b->len++] = *s;
    }
    b->p[b->len++] = L'\'';
    b->p[b->len] = 0;
}
static void WFree(WBuf *b) { free(b->p); b->p = NULL; }

/* UTF-16LE -> Base64，用于 powershell.exe -EncodedCommand */
static wchar_t *Base64Of(const wchar_t *s)
{
    static const char *T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *p = (const unsigned char *)s;
    size_t bytes = wcslen(s) * 2, i = 0, j = 0;
    size_t outLen = ((bytes + 2) / 3) * 4;
    wchar_t *o = (wchar_t *)malloc((outLen + 1) * sizeof(wchar_t));

    while (i + 2 < bytes) {
        unsigned v = (p[i] << 16) | (p[i + 1] << 8) | p[i + 2];
        i += 3;
        o[j++] = (wchar_t)T[(v >> 18) & 63];
        o[j++] = (wchar_t)T[(v >> 12) & 63];
        o[j++] = (wchar_t)T[(v >> 6) & 63];
        o[j++] = (wchar_t)T[v & 63];
    }
    if (i < bytes) {
        unsigned v = p[i] << 16;
        int rest = (int)(bytes - i);
        if (rest == 2) v |= p[i + 1] << 8;
        o[j++] = (wchar_t)T[(v >> 18) & 63];
        o[j++] = (wchar_t)T[(v >> 12) & 63];
        o[j++] = (rest == 2) ? (wchar_t)T[(v >> 6) & 63] : L'=';
        o[j++] = L'=';
    }
    o[j] = 0;
    return o;
}

/* ------------------------------------------------------------------ 设置持久化 */

static void SettingsLoad(void)
{
    HKEY k;
    DWORD v, cb, type;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SHV_REGKEY, 0, KEY_READ, &k) != ERROR_SUCCESS) {
        Dbg(L"SettingsLoad  : 注册表键不存在，用默认值 showLogs=%d tray=%d auto=%d",
            g_showLogs, g_closeToTray, g_autoStartup);
        return;
    }
    cb = sizeof(v); type = REG_DWORD;
    if (RegQueryValueExW(k, L"ShowLogs", NULL, &type, (BYTE *)&v, &cb) == ERROR_SUCCESS) g_showLogs = (int)v;
    cb = sizeof(v); type = REG_DWORD;
    if (RegQueryValueExW(k, L"CloseToTray", NULL, &type, (BYTE *)&v, &cb) == ERROR_SUCCESS) g_closeToTray = (int)v;
    cb = sizeof(v); type = REG_DWORD;
    if (RegQueryValueExW(k, L"AutoStartup", NULL, &type, (BYTE *)&v, &cb) == ERROR_SUCCESS) g_autoStartup = (int)v;
    RegCloseKey(k);
    Dbg(L"SettingsLoad  : showLogs=%d tray=%d auto=%d", g_showLogs, g_closeToTray, g_autoStartup);
}

static void SettingsSave(void)
{
    HKEY k;
    DWORD d;
    LSTATUS rc;
    Dbg(L"SettingsSave  : showLogs=%d tray=%d auto=%d", g_showLogs, g_closeToTray, g_autoStartup);
    rc = RegCreateKeyExW(HKEY_CURRENT_USER, SHV_REGKEY, 0, NULL, 0,
                         KEY_WRITE, NULL, &k, NULL);
    if (rc != ERROR_SUCCESS) {
        Dbg(L"SettingsSave  : RegCreateKeyEx 失败 rc=%ld", (long)rc);
        if (!g_settingsWarned) {
            /* 组策略锁死或权限受限时会走到这里；只在界面上说一次，别刷屏 */
            g_settingsWarned = TRUE;
            Log(L"警告：设置无法保存（注册表写入被拒绝，错误 %ld）。"
                L"本次运行内的选项仍然有效。", (long)rc);
        }
        return;
    }
    d = (DWORD)g_showLogs;
    rc = RegSetValueExW(k, L"ShowLogs", 0, REG_DWORD, (BYTE *)&d, sizeof(d));
    if (rc != ERROR_SUCCESS) Dbg(L"SettingsSave  : 写 ShowLogs 失败 rc=%ld", (long)rc);
    d = (DWORD)g_closeToTray;
    rc = RegSetValueExW(k, L"CloseToTray", 0, REG_DWORD, (BYTE *)&d, sizeof(d));
    if (rc != ERROR_SUCCESS) Dbg(L"SettingsSave  : 写 CloseToTray 失败 rc=%ld", (long)rc);
    d = (DWORD)g_autoStartup;
    rc = RegSetValueExW(k, L"AutoStartup", 0, REG_DWORD, (BYTE *)&d, sizeof(d));
    if (rc != ERROR_SUCCESS) Dbg(L"SettingsSave  : 写 AutoStartup 失败 rc=%ld", (long)rc);
    /* 隐藏到托盘时不记位置，免得下次启动窗口落在看不见的地方 */
    if (IsWindowVisible(g_hwnd) || IsIconic(g_hwnd)) {
        WINDOWPLACEMENT wp;
        DWORD x, y, cx, cy;
        wp.length = sizeof(wp);
        if (GetWindowPlacement(g_hwnd, &wp)) {
            x  = (DWORD)wp.rcNormalPosition.left;
            y  = (DWORD)wp.rcNormalPosition.top;
            cx = (DWORD)(wp.rcNormalPosition.right - wp.rcNormalPosition.left);   /* 存宽高，不存 right/bottom */
            cy = (DWORD)(wp.rcNormalPosition.bottom - wp.rcNormalPosition.top);
            RegSetValueExW(k, L"X",  0, REG_DWORD, (BYTE *)&x,  sizeof(DWORD));
            RegSetValueExW(k, L"Y",  0, REG_DWORD, (BYTE *)&y,  sizeof(DWORD));
            RegSetValueExW(k, L"CX", 0, REG_DWORD, (BYTE *)&cx, sizeof(DWORD));
            RegSetValueExW(k, L"CY", 0, REG_DWORD, (BYTE *)&cy, sizeof(DWORD));
        }
    }
    RegCloseKey(k);
}

static BOOL SettingsLoadRect(RECT *r)
{
    HKEY k;
    DWORD x = 0, y = 0, cx = 0, cy = 0, cb = sizeof(DWORD), type = REG_DWORD;
    BOOL ok = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SHV_REGKEY, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return FALSE;
    if (RegQueryValueExW(k, L"X",  NULL, &type, (BYTE *)&x,  &cb) == ERROR_SUCCESS &&
        RegQueryValueExW(k, L"Y",  NULL, &type, (BYTE *)&y,  &cb) == ERROR_SUCCESS &&
        RegQueryValueExW(k, L"CX", NULL, &type, (BYTE *)&cx, &cb) == ERROR_SUCCESS &&
        RegQueryValueExW(k, L"CY", NULL, &type, (BYTE *)&cy, &cb) == ERROR_SUCCESS &&
        cx >= 400 && cy >= 300) {
        r->left = (LONG)x; r->top = (LONG)y;
        r->right = (LONG)cx; r->bottom = (LONG)cy;   /* right/bottom 在这里是宽高 */
        ok = TRUE;
    }
    RegCloseKey(k);
    return ok;
}

/* ------------------------------------------------------------ 进程 / 外壳调用 */

static void LaunchExe(const wchar_t *exe, const wchar_t *args, BOOL asAdmin)
{
    SHELLEXECUTEINFOW sei;
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb       = asAdmin ? L"runas" : L"open";
    sei.lpFile       = exe;
    sei.lpParameters = (args && *args) ? args : NULL;
    sei.nShow        = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        DWORD e = GetLastError();
        if (e == ERROR_CANCELLED) Log(L"启动被取消（UAC）：%ls", exe);
        else                      Log(L"启动失败 (错误 %lu)：%ls", (unsigned long)e, exe);
        return;
    }
    if (sei.hProcess) CloseHandle(sei.hProcess);
}

/* 跑一个隐藏进程并等它结束，用来调 schtasks.exe 之类的命令行工具 */
static void RunHiddenWait(const wchar_t *cmdline)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 0;
    wchar_t *buf = _wcsdup(cmdline);

    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, buf, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        Log(L"无法执行：%ls（错误 %lu）", cmdline, (unsigned long)GetLastError());
        free(buf);
        return;
    }
    WaitForSingleObject(pi.hProcess, 30000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    Log(L"$ %ls  -> 退出码 %lu", cmdline, (unsigned long)code);
    free(buf);
}

static BOOL IsAdmin(void)
{
    BOOL isAdmin = FALSE;
    PSID admins = NULL;
    SID_IDENTIFIER_AUTHORITY nt = { SECURITY_NT_AUTHORITY };
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admins)) {
        CheckTokenMembership(NULL, admins, &isAdmin);
        FreeSid(admins);
    }
    return isAdmin;
}

/* ------------------------------------------------------------ PowerShell 执行层 */

static BOOL PsExePath(wchar_t *out, size_t cch)
{
    wchar_t sysroot[MAX_PATH];
    if (GetEnvironmentVariableW(L"SystemRoot", sysroot, MAX_PATH)) {
        _snwprintf(out, cch - 1,
                   L"%ls\\System32\\WindowsPowerShell\\v1.0\\powershell.exe", sysroot);
        out[cch - 1] = 0;
        if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return TRUE;
    }
    /* 兜底：交给 PATH 解析（注意 Hyper-V 模块只在 Windows PowerShell 5.1 里保证可用） */
    wcscpy(out, L"powershell.exe");
    return TRUE;
}

/* 把脚本主体包进统一的 try/catch 外壳 */
static wchar_t *PsWrap(const wchar_t *body)
{
    static const wchar_t *pre =
        L"[Console]::OutputEncoding=[Text.Encoding]::UTF8;"
        L"$ProgressPreference='SilentlyContinue';"
        L"$ErrorActionPreference='Stop';"
        L"try{";
    static const wchar_t *post =
        L"}catch{'ERROR: '+$_.Exception.Message;exit 1}";
    size_t n = wcslen(pre) + wcslen(body) + wcslen(post) + 1;
    wchar_t *s = (wchar_t *)malloc(n * sizeof(wchar_t));
    wcscpy(s, pre); wcscat(s, body); wcscat(s, post);
    return s;
}

/* 脚本没能通过语法解析时，PowerShell 把诊断写成 CLIXML（而且是 OEM 代码页），
 * 此时 -OutputFormat Text 和脚本里的 [Console]::OutputEncoding 都还没生效。
 * 这是唯一会漏出 XML 垃圾的路径，识别出来换成人话。 */
static BOOL IsClixml(const wchar_t *s)
{
    if (!s) return FALSE;
    while (*s == L' ' || *s == L'\t' || *s == L'\r' || *s == L'\n') s++;
    return wcsncmp(s, L"#< CLIXML", 9) == 0;
}

/* 同步执行一段脚本主体，返回 UTF-8 转换后的输出（调用方 free） */
static BOOL PsRunSync(const wchar_t *body, wchar_t **outText, DWORD *outExit)
{
    wchar_t psExe[MAX_PATH * 2];
    wchar_t *script, *b64, *cmd;
    size_t cmdLen;
    SECURITY_ATTRIBUTES sa;
    HANDLE rd = NULL, wr = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    char *raw = NULL;
    size_t len = 0, cap = 0;
    DWORD code = 0;

    *outText = NULL;
    *outExit = 0;

    PsExePath(psExe, MAX_PATH * 2);
    script = PsWrap(body);
    b64 = Base64Of(script);

    cmdLen = wcslen(psExe) + wcslen(b64) + 160;
    cmd = (wchar_t *)malloc(cmdLen * sizeof(wchar_t));
    _snwprintf(cmd, cmdLen - 1,
               L"\"%ls\" -NoProfile -NonInteractive -ExecutionPolicy Bypass "
               L"-OutputFormat Text -EncodedCommand %ls",
               psExe, b64);
    cmd[cmdLen - 1] = 0;

    sa.nLength = sizeof(sa); sa.lpSecurityDescriptor = NULL; sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        Log(L"CreatePipe 失败");
        goto done;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError  = wr;      /* 合并到同一根管道；-OutputFormat Text 保证 stderr 是纯文本 */
    si.hStdInput  = NULL;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(psExe, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        Log(L"CreateProcess 失败（错误 %lu）：%ls", (unsigned long)GetLastError(), psExe);
        CloseHandle(rd); CloseHandle(wr);
        goto done;
    }
    CloseHandle(wr); wr = NULL;

    for (;;) {
        DWORD got = 0;
        if (len + 8192 > cap) {
            cap = cap ? cap * 2 : 16384;
            raw = (char *)realloc(raw, cap);
        }
        if (!ReadFile(rd, raw + len, 8192, &got, NULL) || got == 0) break;
        len += got;
    }
    CloseHandle(rd); rd = NULL;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    raw = (char *)realloc(raw, len + 1);
    raw[len] = 0;

    {   /* 去掉可能的 UTF-8 BOM */
        char *p = raw;
        if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
            (unsigned char)p[2] == 0xBF) p += 3;
        *outText = W(p);
    }
    if (IsClixml(*outText)) {
        free(*outText);
        *outText = _wcsdup(L"PowerShell 未能解析这段脚本（语法错误），CLIXML 诊断已丢弃。\r\n"
                           L"多半是脚本里的引号或括号被破坏 —— 检查 >>> 那一行。");
        if (code == 0) code = 1;
    }
    *outExit = code;

done:
    if (wr) CloseHandle(wr);
    if (rd) CloseHandle(rd);
    free(raw);
    free(cmd);
    free(b64);
    free(script);
    return *outText != NULL;
}

/* 后台执行：每条命令一个工作线程，做完 PostMessage 回 UI 线程 */
typedef struct { wchar_t *body; int action; } Job;
typedef struct { wchar_t *text; DWORD exitCode; int action; } DoneMsg;

static DWORD WINAPI JobThread(LPVOID param)
{
    Job *j = (Job *)param;
    wchar_t *text = NULL;
    DWORD code = 0;
    DoneMsg *d;

    if (!PsRunSync(j->body, &text, &code)) {
        text = _wcsdup(L"启动 powershell.exe 失败");
        code = 0xFFFFFFFFu;
    }
    d = (DoneMsg *)malloc(sizeof(DoneMsg));
    d->text = text; d->exitCode = code; d->action = j->action;
    PostMessageW(g_hwnd, WM_APP_DONE, 0, (LPARAM)d);

    free(j->body);
    free(j);
    return 0;
}

/* ---------------------------------------------------------------- 日志 / 列表 */

/* 归一化换行：EDIT 控件只认 CRLF，单独的 LF 会显示成方块 */
static void LogNormalize(wchar_t *s)
{
    wchar_t *r = s, *w = s;
    while (*r) {
        if (*r == L'\r' && r[1] == L'\n') { *w++ = L'\r'; *w++ = L'\n'; r += 2; }
        else if (*r == L'\n')             { *w++ = L'\r'; *w++ = L'\n'; r += 1; }
        else                              { *w++ = *r++; }
    }
    *w = 0;
}

#define LOG_LIMIT 400000

static void LogAppend(const wchar_t *text)
{
    int len;
    if (!g_log) return;
    len = GetWindowTextLengthW(g_log);
    if (len > LOG_LIMIT) {          /* 超限就砍掉前半段，避免 EDIT 越来越大 */
        SendMessageW(g_log, EM_SETSEL, 0, len / 2);
        SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"");
        len = GetWindowTextLengthW(g_log);
    }
    SendMessageW(g_log, EM_SETSEL, len, len);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)text);
    Dbg(L"%ls", text);   /* SHV_DBG 时把日志窗口内容也镜像到调试文件 */
}

static void LogText(const wchar_t *text)
{
    wchar_t *copy = _wcsdup(text);
    LogNormalize(copy);
    LogAppend(copy);
    if (copy[0] && copy[wcslen(copy) - 1] != L'\n') LogAppend(L"\r\n");
    free(copy);
}

static void Log(const wchar_t *fmt, ...)
{
    wchar_t buf[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 4095, fmt, ap);
    va_end(ap);
    buf[4095] = 0;
    LogText(buf);
    LogAppend(L"\r\n");
}

static void FreeItems(void)
{
    int i;
    for (i = 0; i < g_nItems; i++) {
        free(g_items[i].name);
        free(g_items[i].state);
        free(g_items[i].id);
    }
    free(g_items);
    g_items = NULL;
    g_nItems = 0;
}

static Item *SelItem(void)
{
    int sel = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR || sel < 0 || sel >= g_nItems) return NULL;
    return &g_items[sel];
}

static void UpdateButtons(void);

/* 把一行输出拆成字段（VM 行是 Name<TAB>State<TAB>Id）。line 会被就地改写。
 * 单独抽出来是为了能被 --selftest 直接断言。 */
static void ParseListLine(wchar_t *line, int mode,
                          wchar_t **name, wchar_t **state, wchar_t **id)
{
    *name = NULL; *state = NULL; *id = NULL;
    if (mode == MODE_VM) {
        wchar_t *t1 = wcschr(line, L'\t');
        if (t1) {
            wchar_t *t2;
            *t1 = 0;
            t2 = wcschr(t1 + 1, L'\t');
            if (t2) { *t2 = 0; *id = _wcsdup(t2 + 1); }
            *name = _wcsdup(line);
            *state = _wcsdup(t1 + 1);
        } else {
            *name = _wcsdup(line);   /* 没有 tab 就整行当名字，不让界面变空 */
        }
    } else {
        *name = _wcsdup(line);
    }
}

/* 解析 PS 输出填列表：VM 行是 Name<TAB>State<TAB>Id，其它模式只有 Name */
static void PopulateList(const wchar_t *text)
{
    const wchar_t *p = text;
    int prevSel = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    wchar_t line[2048];

    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    FreeItems();
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);

    while (*p) {
        size_t n = 0;
        const wchar_t *e = p;
        while (*e && *e != L'\n' && n < 2047) { line[n++] = *e++; }
        line[n] = 0;
        while (n > 0 && (line[n - 1] == L'\r' || line[n - 1] == L' ')) line[--n] = 0;
        p = (*e == L'\n') ? e + 1 : e;

        if (n == 0) continue;

        g_items = (Item *)realloc(g_items, (g_nItems + 1) * sizeof(Item));
        g_items[g_nItems].name = NULL;
        g_items[g_nItems].state = NULL;
        g_items[g_nItems].id = NULL;

        ParseListLine(line, g_mode,
                      &g_items[g_nItems].name,
                      &g_items[g_nItems].state,
                      &g_items[g_nItems].id);

        {
            wchar_t disp[2200];
            if (g_items[g_nItems].state)
                _snwprintf(disp, 2199, L"%ls [%ls]",
                           g_items[g_nItems].name, g_items[g_nItems].state);
            else
                _snwprintf(disp, 2199, L"%ls", g_items[g_nItems].name);
            disp[2199] = 0;
            SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)disp);
            SendMessageW(g_list, LB_SETITEMDATA, (WPARAM)g_nItems, (LPARAM)g_nItems);
        }
        g_nItems++;
    }

    if (prevSel >= 0 && prevSel < g_nItems)
        SendMessageW(g_list, LB_SETCURSEL, (WPARAM)prevSel, 0);
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, NULL, TRUE);
    UpdateButtons();
}

/* --------------------------------------------------------------- 脚本主体构造 */

static void ScriptRefresh(WBuf *b)
{
    if (g_mode == MODE_VM)
        WCat(b, L"Get-VM | ForEach-Object { $_.Name + \"`t\" + $_.State + \"`t\" + $_.Id }");
    else if (g_mode == MODE_SWITCH)
        WCat(b, L"Get-VMSwitch | ForEach-Object { $_.Name }");
    else
        WCat(b, L"Get-NetNat | ForEach-Object { $_.Name }");
}

/* 把 "192.168.56.1/24" 拆成 IP 和前缀长度 */
static void SplitCidr(const wchar_t *in, wchar_t *ip, size_t cchIp, wchar_t *prefix, size_t cchPre)
{
    const wchar_t *slash = wcschr(in, L'/');
    size_t n;
    if (slash) {
        n = (size_t)(slash - in);
        if (n >= cchIp) n = cchIp - 1;
        wmemcpy(ip, in, n); ip[n] = 0;
        _snwprintf(prefix, cchPre - 1, L"%ls", slash + 1);
    } else {
        _snwprintf(ip, cchIp - 1, L"%ls", in);
        _snwprintf(prefix, cchPre - 1, L"24");
    }
    ip[cchIp - 1] = 0;
    prefix[cchPre - 1] = 0;
    {   /* 前缀必须是数字，否则退回 24 */
        const wchar_t *q = prefix;
        if (!*q) { wcscpy(prefix, L"24"); }
        for (; *q; q++) if (*q < L'0' || *q > L'9') { wcscpy(prefix, L"24"); break; }
    }
}

/* ---------------------------------------------------------------- 异步执行入口 */

static void UpdateButtons(void);

static void SetBusyUI(BOOL busy)
{
    ShowWindow(g_progress, busy ? SW_SHOW : SW_HIDE);
    SendMessageW(g_progress, PBM_SETMARQUEE, (WPARAM)busy, busy ? 30 : 0);
    UpdateButtons();
}

static void PsRunAsync(const wchar_t *body, int action)
{
    Job *j;
    HANDLE h;

    if (g_busy) { Log(L"[忙] 上一条命令还没结束。"); return; }

    j = (Job *)malloc(sizeof(Job));
    j->body = _wcsdup(body);
    j->action = action;

    g_busy = TRUE;
    SetBusyUI(TRUE);
    Log(L">>> %ls", body);

    h = CreateThread(NULL, 0, JobThread, j, 0, NULL);
    if (!h) {
        Log(L"CreateThread 失败");
        free(j->body); free(j);
        g_busy = FALSE;
        SetBusyUI(FALSE);
        return;
    }
    CloseHandle(h);
}

static void StartRefresh(void)
{
    WBuf b;
    WInit(&b);
    ScriptRefresh(&b);
    PsRunAsync(b.p, RA_LIST);
    WFree(&b);
}

/* -------------------------------------------------------------------- 各项动作 */

static void ShowLogPane(BOOL on)
{
    HMENU m = GetMenu(g_hwnd);
    g_showLogs = on ? 1 : 0;
    if (m) CheckMenuItem(m, IDM_SHOWLOGS, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
    ShowWindow(g_log, on ? SW_SHOW : SW_HIDE);
    {   /* 触发布局重算 */
        RECT rc; GetClientRect(g_hwnd, &rc);
        SendMessageW(g_hwnd, WM_SIZE, SIZE_RESTORED,
                     MAKELPARAM(rc.right, rc.bottom));
    }
    SettingsSave();
}

static wchar_t *HvintegratePath(void);
static void EnsureHvintegrate(void);

static void ActConnect(void)
{
    Item *it = SelItem();
    wchar_t localPath[MAX_PATH], sysPath[MAX_PATH], args[1200];
    wchar_t sysroot[MAX_PATH];
    wchar_t exeDir[MAX_PATH];

    if (!it) return;

    GetModuleFileNameW(NULL, exeDir, MAX_PATH);
    {   /* 去掉文件名，留目录 */
        wchar_t *s = wcsrchr(exeDir, L'\\');
        if (s) *s = 0;
    }
    _snwprintf(localPath, MAX_PATH - 1, L"%ls\\vmconnect.exe", exeDir);
    localPath[MAX_PATH - 1] = 0;

    _snwprintf(args, 1199, L"localhost \"%ls\"", it->name);
    args[1199] = 0;

    if (GetFileAttributesW(localPath) != INVALID_FILE_ATTRIBUTES) {
        LaunchExe(localPath, args, FALSE);
        return;
    }

    Log(L"提示：为保证兼容性，建议手动把 C:\\Windows\\System32\\vmconnect.exe "
        L"复制到程序同目录，否则可能无法正常运行。");

    if (GetEnvironmentVariableW(L"SystemRoot", sysroot, MAX_PATH))
        _snwprintf(sysPath, MAX_PATH - 1, L"%ls\\System32\\vmconnect.exe", sysroot);
    else
        wcscpy(sysPath, L"vmconnect.exe");
    sysPath[MAX_PATH - 1] = 0;

    LaunchExe(sysPath, args, FALSE);
}

static void ActStart(void)
{
    Item *it = SelItem();
    WBuf b;
    if (!it) return;
    WInit(&b);
    WCat(&b, L"Start-VM -Name "); WCatQ(&b, it->name);
    PsRunAsync(b.p, RA_REFRESH);
    WFree(&b);
}

static void ActStop(void)
{
    Item *it = SelItem();
    WBuf b;
    BOOL shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    BOOL ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

    if (!it) return;

    if (shift) {                      /* Shift：强制关机 */
        WInit(&b);
        WCat(&b, L"Stop-VM -Name "); WCatQ(&b, it->name); WCat(&b, L" -Force");
        PsRunAsync(b.p, RA_REFRESH);
        WFree(&b);
        return;
    }
    if (ctrl) {                       /* Ctrl：保存状态 */
        WInit(&b);
        WCat(&b, L"Save-VM -Name "); WCatQ(&b, it->name);
        PsRunAsync(b.p, RA_REFRESH);
        WFree(&b);
        return;
    }
    if (MessageBoxW(g_hwnd, L"关闭选中的虚拟机？（按住 Shift 点击可强制关机，按住 Ctrl 可保存状态）",
                    L"确认", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;

    WInit(&b);
    WCat(&b, L"Stop-VM -Name "); WCatQ(&b, it->name);
    PsRunAsync(b.p, RA_REFRESH);
    WFree(&b);
}

static void ActDelete(void)
{
    Item *it = SelItem();
    WBuf b;
    if (!it) return;
    WInit(&b);
    if (g_mode == MODE_VM) {
        wchar_t msg[1200];
        _snwprintf(msg, 1199, L"确定删除虚拟机 \"%ls\"？此操作不可撤销。", it->name);
        msg[1199] = 0;
        if (MessageBoxW(g_hwnd, msg, L"警告", MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
            WFree(&b); return;
        }
        WCat(&b, L"Remove-VM -VMName "); WCatQ(&b, it->name); WCat(&b, L" -Force");
    } else if (g_mode == MODE_SWITCH) {
        if (MessageBoxW(g_hwnd, L"删除选中的虚拟交换机？", L"确认",
                        MB_OKCANCEL | MB_ICONQUESTION) != IDOK) { WFree(&b); return; }
        WCat(&b, L"Remove-VMSwitch -Name "); WCatQ(&b, it->name); WCat(&b, L" -Force");
    } else {
        if (MessageBoxW(g_hwnd, L"删除选中的 NAT？", L"确认",
                        MB_OKCANCEL | MB_ICONQUESTION) != IDOK) { WFree(&b); return; }
        WCat(&b, L"Remove-NetNat -Name "); WCatQ(&b, it->name); WCat(&b, L" -Confirm:$false");
    }
    PsRunAsync(b.p, RA_REFRESH);
    WFree(&b);
}

static void ActDetails(void)
{
    Item *it = SelItem();
    WBuf b;
    if (!it) return;
    WInit(&b);
    if (g_mode == MODE_VM)          WCat(&b, L"Get-VM -Name ");
    else if (g_mode == MODE_SWITCH) WCat(&b, L"Get-VMSwitch -Name ");
    else                            WCat(&b, L"Get-NetNat -Name ");
    WCatQ(&b, it->name);
    WCat(&b, L" | Format-List * | Out-String -Width 200");
    PsRunAsync(b.p, RA_LOG);
    WFree(&b);
}

static void ActVmSettings(void)
{
    Item *it = SelItem();
    if (!it || !it->id) return;
    EnsureHvintegrate();
    {
        wchar_t args[MAX_PATH + 64];
        _snwprintf(args, MAX_PATH + 63, L"vm %ls", it->id);
        args[MAX_PATH + 63] = 0;
        LaunchExe(HvintegratePath(), args, FALSE);
    }
}

static void ActNested(void)
{
    Item *it = SelItem();
    WBuf b;
    if (!it) return;
    WInit(&b);
    WCat(&b, L"Set-VMProcessor -VMName "); WCatQ(&b, it->name);
    WCat(&b, L" -ExposeVirtualizationExtensions $true");
    PsRunAsync(b.p, RA_LOG);
    WFree(&b);
}

static void ActNew(void)
{
    if (g_mode == MODE_VM) {
        EnsureHvintegrate();
        LaunchExe(HvintegratePath(), L"av", FALSE);
        return;
    }
    {
        wchar_t name[256] = L"";
        wchar_t addr[128] = L"";
        WBuf b;
        const wchar_t *t, *l1, *i1, *l2, *i2;

        if (g_mode == MODE_SWITCH) {
            t = L"新建内部交换机（带静态 IP）";
            l1 = L"交换机名称"; i1 = L"DHCP_SWITCH";
            l2 = L"静态 IP / 前缀长度"; i2 = L"192.168.56.1/24";
        } else {
            t = L"新建 NetNat";
            l1 = L"名称"; i1 = L"NAT";
            l2 = L"内部地址前缀"; i2 = L"192.168.56.0/24";
        }
        if (!SHVPopup(g_hwnd, t, l1, i1, l2, i2, name, 256, addr, 128)) return;

        WInit(&b);
        if (g_mode == MODE_SWITCH) {
            wchar_t ip[128], prefix[16];
            SplitCidr(addr, ip, 128, prefix, 16);
            /* 三条命令合成一个脚本，避免把中间结果（ifIndex）传回 C 再发一次 */
            WCat(&b, L"$n='vEthernet ('+"); WCatQ(&b, name); WCat(&b, L"+')';");
            WCat(&b, L"New-VMSwitch -SwitchName "); WCatQ(&b, name);
            WCat(&b, L" -SwitchType Internal;");
            WCat(&b, L"$idx=$null;");
            WCat(&b, L"for($i=0;$i -lt 15 -and -not $idx;$i++){Start-Sleep -Milliseconds 400;"
                     L"$idx=(Get-NetAdapter -Name $n -ErrorAction SilentlyContinue"
                     L"|Select-Object -ExpandProperty ifIndex)};");
            WCat(&b, L"if(-not $idx){throw ('网络适配器未出现: '+$n)};");
            WCat(&b, L"New-NetIPAddress -IPAddress "); WCatQ(&b, ip);
            WCat(&b, L" -PrefixLength "); WCat(&b, prefix);
            WCat(&b, L" -InterfaceIndex $idx");
        } else {
            WCat(&b, L"New-NetNat -Name "); WCatQ(&b, name);
            WCat(&b, L" -InternalIPInterfaceAddressPrefix "); WCatQ(&b, addr);
        }
        PsRunAsync(b.p, RA_REFRESH);
        WFree(&b);
    }
}

static void ActOptimizeVhd(void)
{
    OPENFILENAMEW ofn;
    wchar_t file[MAX_PATH] = L"";
    static const wchar_t filter[] =
        L"虚拟磁盘 (*.vhd;*.vhdx)\0*.vhd;*.vhdx\0所有文件 (*.*)\0*.*\0\0";

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = L"选择要优化的虚拟磁盘";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&ofn)) return;
    {
        WBuf b;
        WInit(&b);
        WCat(&b, L"Optimize-VHD -Path "); WCatQ(&b, file); WCat(&b, L" -Mode Full");
        PsRunAsync(b.p, RA_LOG);
        WFree(&b);
    }
}

static void ActAutostart(BOOL on)
{
    wchar_t exe[MAX_PATH], cmd[2048];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (on)
        _snwprintf(cmd, 2047,
                   L"schtasks.exe /Create /F /TN \"%ls\" /SC ONLOGON /RL HIGHEST "
                   L"/TR \"\\\"%ls\\\" -m\"", SHV_TASKNAME, exe);
    else
        _snwprintf(cmd, 2047, L"schtasks.exe /Delete /F /TN \"%ls\"", SHV_TASKNAME);
    cmd[2047] = 0;
    RunHiddenWait(cmd);
    g_autoStartup = on ? 1 : 0;
    SettingsSave();
}

/* ---------------------------------------------------- 内嵌 hvintegrate.exe 释放 */

static void HvintegrateDir(wchar_t *out, size_t cch)
{
    GetModuleFileNameW(NULL, out, (DWORD)cch);
    out[cch - 1] = 0;
    {
        wchar_t *s = wcsrchr(out, L'\\');
        if (s) *s = 0;
    }
}

static wchar_t *HvintegratePath(void)
{
    static wchar_t path[MAX_PATH];
    HvintegrateDir(path, MAX_PATH);
    wcscat(path, L"\\hvintegrate.exe");
    return path;
}

static void EnsureHvintegrate(void)
{
    wchar_t *path = HvintegratePath();
    HRSRC hr;
    HGLOBAL hg;
    const void *data;
    DWORD size;

    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return;

    hr = FindResourceW(g_inst, MAKEINTRESOURCEW(IDR_HVINTEGRATE), RT_RCDATA);
    if (!hr) { Log(L"内嵌资源 hvintegrate.exe 不存在"); return; }
    hg = LoadResource(g_inst, hr);
    if (!hg) { Log(L"LoadResource 失败"); return; }
    data = LockResource(hg);
    size = SizeofResource(g_inst, hr);
    if (!data || !size) { Log(L"资源为空"); return; }

    {
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD wrote = 0;
        if (f == INVALID_HANDLE_VALUE) {
            Log(L"无法释放 hvintegrate.exe（错误 %lu）", (unsigned long)GetLastError());
            return;
        }
        WriteFile(f, data, size, &wrote, NULL);
        CloseHandle(f);
        Log(L"已释放 hvintegrate.exe（%lu 字节）", (unsigned long)wrote);
    }
}

/* ------------------------------------------------------------------ 模态小对话框 */

typedef struct {
    HWND   hwnd, owner;
    HWND   edit[2];
    const wchar_t *labels[2];
    const wchar_t *init[2];
    wchar_t *out[2];
    size_t   cch[2];
    int      result;
    HFONT    font;
    UINT     dpi;
} Popup;

static LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Popup *p = (Popup *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        int i;
        p = (Popup *)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)p);
        p->hwnd = hwnd;
        for (i = 0; i < 2; i++) {
            HWND st = CreateWindowExW(0, L"STATIC", p->labels[i],
                                      WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      MulDiv(16, p->dpi, 96), MulDiv(14 + i * 56, p->dpi, 96),
                                      MulDiv(340, p->dpi, 96), MulDiv(18, p->dpi, 96),
                                      hwnd, NULL, g_inst, NULL);
            p->edit[i] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", p->init[i],
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                      MulDiv(16, p->dpi, 96), MulDiv(34 + i * 56, p->dpi, 96),
                                      MulDiv(348, p->dpi, 96), MulDiv(24, p->dpi, 96),
                                      hwnd, (HMENU)(INT_PTR)(100 + i), g_inst, NULL);
            SendMessageW(st, WM_SETFONT, (WPARAM)p->font, TRUE);
            SendMessageW(p->edit[i], WM_SETFONT, (WPARAM)p->font, TRUE);
        }
        CreateWindowExW(0, L"BUTTON", L"确定",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        MulDiv(184, p->dpi, 96), MulDiv(130, p->dpi, 96),
                        MulDiv(86, p->dpi, 96), MulDiv(28, p->dpi, 96),
                        hwnd, (HMENU)IDOK, g_inst, NULL);
        CreateWindowExW(0, L"BUTTON", L"取消",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        MulDiv(278, p->dpi, 96), MulDiv(130, p->dpi, 96),
                        MulDiv(86, p->dpi, 96), MulDiv(28, p->dpi, 96),
                        hwnd, (HMENU)IDCANCEL, g_inst, NULL);
        {   /* 给两个按钮也设字体 */
            HWND c = GetWindow(hwnd, GW_CHILD);
            while (c) { SendMessageW(c, WM_SETFONT, (WPARAM)p->font, TRUE); c = GetWindow(c, GW_HWNDNEXT); }
        }
        SetFocus(p->edit[0]);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDOK) {
            int i;
            for (i = 0; i < 2; i++) {
                GetWindowTextW(p->edit[i], p->out[i], (int)p->cch[i]);
                if (p->out[i][0] == 0) {
                    MessageBoxW(hwnd, L"两项都必须填写。", L"提示", MB_OK | MB_ICONWARNING);
                    SetFocus(p->edit[i]);
                    return 0;
                }
            }
            p->result = 1;
            DestroyWindow(hwnd);
        } else if (id == IDCANCEL) {
            p->result = 0;
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_CLOSE:
        if (p) p->result = 0;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int SHVPopup(HWND owner, const wchar_t *title,
             const wchar_t *l1, const wchar_t *i1,
             const wchar_t *l2, const wchar_t *i2,
             wchar_t *o1, size_t c1, wchar_t *o2, size_t c2)
{
    Popup p;
    MSG msg;
    RECT r;
    UINT dpi = DpiOf(owner);
    int dw, dh;

    ZeroMemory(&p, sizeof(p));
    p.owner = owner;
    p.labels[0] = l1; p.labels[1] = l2;
    p.init[0]   = i1; p.init[1]   = i2;
    p.out[0]    = o1; p.out[1]    = o2;
    p.cch[0]    = c1; p.cch[1]    = c2;
    p.dpi       = dpi;
    p.font      = g_font;

    {   /* 按 DPI 算窗口外框尺寸 */
        r.left = 0; r.top = 0;
        r.right  = MulDiv(380, dpi, 96);
        r.bottom = MulDiv(172, dpi, 96);
        AdjustWindowRectEx(&r, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
        dw = r.right - r.left;
        dh = r.bottom - r.top;
    }

    {
        HWND h = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
            SHV_POPUPCLASS, title,
            WS_POPUP | WS_CAPTION | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT, dw, dh,
            owner, NULL, g_inst, &p);
        if (!h) return 0;

        {   /* 居中到 owner */
            RECT orr, wr;
            GetWindowRect(owner, &orr);
            GetWindowRect(h, &wr);
            SetWindowPos(h, HWND_TOP,
                         orr.left + ((orr.right - orr.left) - (wr.right - wr.left)) / 2,
                         orr.top  + ((orr.bottom - orr.top) - (wr.bottom - wr.top)) / 2,
                         0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
        }

        EnableWindow(owner, FALSE);
        while (IsWindow(h)) {
            if (GetMessageW(&msg, NULL, 0, 0) <= 0) break;
            /* 这不是真对话框，Tab/Enter/Esc 自己处理，行为完全可预期 */
            if (msg.message == WM_KEYDOWN) {
                if (msg.wParam == VK_ESCAPE) { p.result = 0; DestroyWindow(h); continue; }
                if (msg.wParam == VK_RETURN) { SendMessageW(h, WM_COMMAND, IDOK, 0); continue; }
                if (msg.wParam == VK_TAB) {
                    HWND f = GetFocus();
                    SetFocus(f == p.edit[0] ? p.edit[1] : p.edit[0]);
                    continue;
                }
            }
            if (!IsWindow(h)) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    return p.result;
}

/* ------------------------------------------------------------------ 布局与字体 */

static void MakeFonts(UINT dpi)
{
    NONCLIENTMETRICSW ncm;
    LOGFONTW lf;
    int pt;

    if (g_font)      DeleteObject(g_font);
    if (g_fontFixed) DeleteObject(g_fontFixed);

    ZeroMemory(&ncm, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        lf = ncm.lfMessageFont;
    } else {
        ZeroMemory(&lf, sizeof(lf));
        lf.lfHeight  = -12;
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy(lf.lfFaceName, L"Segoe UI");
    }
    /* 系统度量是按 96dpi 给的，先还原成磅值再按当前 dpi 生成 */
    pt = MulDiv(-lf.lfHeight, 72, 96);
    if (pt <= 0) pt = 9;
    lf.lfHeight = -MulDiv(pt, (int)dpi, 72);
    lf.lfQuality = CLEARTYPE_QUALITY;
    g_font = CreateFontIndirectW(&lf);

    g_fontFixed = CreateFontW(-MulDiv(9, (int)dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (!g_fontFixed) g_fontFixed = g_font;
}

static void ApplyFonts(void)
{
    HWND ctrls[] = { g_combo, g_progress, g_list, g_log };
    HWND b;
    int i;
    for (i = 0; i < 4; i++) {
        if (!ctrls[i]) continue;
        SendMessageW(ctrls[i], WM_SETFONT,
                     (WPARAM)((ctrls[i] == g_log) ? g_fontFixed : g_font), TRUE);
    }
    b = GetWindow(g_hwnd, GW_CHILD);
    while (b) {
        wchar_t cls[64];
        GetClassNameW(b, cls, 64);
        if (!wcscmp(cls, L"Button"))
            SendMessageW(b, WM_SETFONT, (WPARAM)g_font, TRUE);
        b = GetWindow(b, GW_HWNDNEXT);
    }
}

static void LayoutAll(HWND hwnd)
{
    RECT rc;
    UINT dpi = DpiOf(hwnd);
    int W, H, pad, rowH, btnH, gap, logH;
    int y, listY, listH, btnY, logY;
    int comboW, btnW;

    GetClientRect(hwnd, &rc);
    W = rc.right; H = rc.bottom;
    pad  = MulDiv(8,  dpi, 96);
    rowH = MulDiv(26, dpi, 96);
    btnH = MulDiv(30, dpi, 96);
    gap  = MulDiv(6,  dpi, 96);
    logH = g_showLogs ? MulDiv(170, dpi, 96) : 0;

    if (logH && H < MulDiv(300, dpi, 96)) logH = 0;

    y = pad;
    comboW = MulDiv(190, dpi, 96);
    btnW   = MulDiv(92, dpi, 96);

    MoveWindow(g_combo, pad, y, comboW, MulDiv(200, dpi, 96), TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_REFRESH), pad + comboW + gap, y,
               btnW, rowH, TRUE);
    MoveWindow(g_progress, W - pad - MulDiv(130, dpi, 96), y + (rowH - MulDiv(14, dpi, 96)) / 2,
               MulDiv(130, dpi, 96), MulDiv(14, dpi, 96), TRUE);

    listY = y + rowH + pad;
    btnY  = H - pad - btnH - (logH ? logH + pad : 0);
    listH = btnY - pad - listY;
    if (listH < MulDiv(60, dpi, 96)) listH = MulDiv(60, dpi, 96);
    MoveWindow(g_list, pad, listY, W - pad * 2, listH, TRUE);

    {   /* 左下：Start / Stop / Connect / More，右下：Delete / New */
        int x = pad;
        int wStart = MulDiv(76,  dpi, 96);
        int wStop  = MulDiv(76,  dpi, 96);
        int wConn  = MulDiv(88,  dpi, 96);
        int wMore  = MulDiv(96,  dpi, 96);
        int wDel   = MulDiv(76,  dpi, 96);
        int wNew   = MulDiv(96,  dpi, 96);

        MoveWindow(GetDlgItem(hwnd, IDC_START),   x, btnY, wStart, btnH, TRUE); x += wStart + gap;
        MoveWindow(GetDlgItem(hwnd, IDC_STOP),    x, btnY, wStop,  btnH, TRUE); x += wStop  + gap;
        MoveWindow(GetDlgItem(hwnd, IDC_CONNECT), x, btnY, wConn,  btnH, TRUE); x += wConn  + gap;
        MoveWindow(GetDlgItem(hwnd, IDC_MORE),    x, btnY, wMore,  btnH, TRUE);

        MoveWindow(GetDlgItem(hwnd, IDC_DELETE), W - pad - wNew - gap - wDel, btnY, wDel, btnH, TRUE);
        MoveWindow(GetDlgItem(hwnd, IDC_NEW),    W - pad - wNew,              btnY, wNew, btnH, TRUE);
    }

    logY = H - pad - logH;
    if (logH) MoveWindow(g_log, pad, logY, W - pad * 2, logH, TRUE);
    else      MoveWindow(g_log, pad, logY, 0, 0, TRUE);
}

/* --------------------------------------------------------------------- 按钮状态 */

static void UpdateButtons(void)
{
    BOOL busy = g_busy;
    BOOL vm   = (g_mode == MODE_VM);
    Item *it  = SelItem();
    BOOL sel  = (it != NULL);
    BOOL canStart = busy ? FALSE : (vm && sel);
    BOOL canStop  = busy ? FALSE : (vm && sel);

    if (vm && it && it->state) {
        if (!wcscmp(it->state, L"Off") || !wcscmp(it->state, L"Saved")) canStop  = FALSE;
        else                                                          canStart = FALSE;
    }
    EnableWindow(GetDlgItem(g_hwnd, IDC_REFRESH), !busy);
    EnableWindow(GetDlgItem(g_hwnd, IDC_START),   canStart);
    EnableWindow(GetDlgItem(g_hwnd, IDC_STOP),    canStop);
    EnableWindow(GetDlgItem(g_hwnd, IDC_CONNECT), !busy && vm && sel);
    EnableWindow(GetDlgItem(g_hwnd, IDC_DELETE),  !busy && sel);
    EnableWindow(GetDlgItem(g_hwnd, IDC_NEW),     !busy);
    EnableWindow(GetDlgItem(g_hwnd, IDC_MORE),    !busy && vm && sel);
}

/* ------------------------------------------------------------------------ 托盘 */

static void TrayAdd(void)
{
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd   = g_hwnd;
    nid.uID    = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon  = g_iconSmall ? g_iconSmall : g_iconBig;
    wcscpy(nid.szTip, L"Simple Hyper-V");
    if (Shell_NotifyIconW(NIM_ADD, &nid)) g_trayAdded = TRUE;
}

static void TrayRemove(void)
{
    NOTIFYICONDATAW nid;
    if (!g_trayAdded) return;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID  = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = FALSE;
}

static void ShowFromTray(void)
{
    ShowWindow(g_hwnd, SW_SHOW);
    ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
    if (g_mode == MODE_VM) StartRefresh();
}

static void HideToTray(void)
{
    ShowWindow(g_hwnd, SW_HIDE);
}

/* ------------------------------------------------------------------------ 菜单 */

static HMENU BuildMenu(void)
{
    HMENU bar = CreateMenu();
    HMENU tools = CreatePopupMenu();
    HMENU opts  = CreatePopupMenu();
    HMENU help  = CreatePopupMenu();

    AppendMenuW(tools, MF_STRING, IDM_MGR,      L"Hyper-V 管理器(&M)");
    AppendMenuW(tools, MF_STRING, IDM_HVSET,    L"Hyper-V 设置(&S)");
    AppendMenuW(tools, MF_STRING, IDM_VSSET,    L"虚拟交换机管理器(&V)");
    AppendMenuW(tools, MF_STRING, IDM_EDITDISK, L"编辑磁盘(&E)");
    AppendMenuW(tools, MF_STRING, IDM_CREATEVM, L"新建虚拟机(&C)");
    AppendMenuW(tools, MF_STRING, IDM_OPTVHD,   L"优化 VHD(&O)...");
    AppendMenuW(tools, MF_SEPARATOR, 0, NULL);
    AppendMenuW(tools, MF_STRING, IDM_NETCONN,  L"网络连接(&N)");
    AppendMenuW(tools, MF_STRING, IDM_MSTSC,    L"远程桌面 mstsc(&R)");
    AppendMenuW(tools, MF_STRING, IDM_TASKSCHD, L"任务计划 taskschd.msc(&T)");
    AppendMenuW(tools, MF_STRING, IDM_OPENDIR,  L"打开程序目录(&D)");
    AppendMenuW(tools, MF_SEPARATOR, 0, NULL);
    AppendMenuW(tools, MF_STRING, IDM_EXIT,     L"退出(&X)");

    AppendMenuW(opts, MF_STRING, IDM_SHOWLOGS,  L"显示日志(&L)");
    AppendMenuW(opts, MF_STRING, IDM_TRAYOPT,   L"关闭时最小化到托盘(&T)");
    AppendMenuW(opts, MF_STRING, IDM_AUTOSTART, L"开机自启(&A)");
    AppendMenuW(opts, MF_SEPARATOR, 0, NULL);
    AppendMenuW(opts, MF_STRING, IDM_ELEVATE,   L"以管理员身份重启(&R)");
    AppendMenuW(opts, MF_STRING, IDM_RESETDEF,  L"恢复默认设置(&D)");

    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"关于(&A)");

    AppendMenuW(bar, MF_POPUP, (UINT_PTR)tools, L"工具(&T)");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)opts,  L"选项(&O)");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help,  L"帮助(&H)");
    return bar;
}

/* ------------------------------------------------------------------ 主窗口过程 */

/* 三个开关菜单项的勾选状态与全局变量保持一致 */
static void ApplyOptionChecks(void)
{
    HMENU m = GetMenu(g_hwnd);
    if (!m) return;
    CheckMenuItem(m, IDM_SHOWLOGS, MF_BYCOMMAND | (g_showLogs    ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_TRAYOPT,  MF_BYCOMMAND | (g_closeToTray ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_AUTOSTART,MF_BYCOMMAND | (g_autoStartup ? MF_CHECKED : MF_UNCHECKED));
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;

        g_combo = CreateWindowExW(0, L"COMBOBOX", NULL,
                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                     0, 0, 100, 200, hwnd, (HMENU)IDC_COMBO, g_inst, NULL);
        SendMessageW(g_combo, CB_ADDSTRING, 0, (LPARAM)L"虚拟机 (VM)");
        SendMessageW(g_combo, CB_ADDSTRING, 0, (LPARAM)L"虚拟交换机 (Switch)");
        SendMessageW(g_combo, CB_ADDSTRING, 0, (LPARAM)L"NAT 网络 (NetNat)");
        SendMessageW(g_combo, CB_SETCURSEL, 0, 0);

        CreateWindowExW(0, L"BUTTON", L"刷新(&R)",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                        0, 0, 10, 10, hwnd, (HMENU)IDC_REFRESH, g_inst, NULL);
        g_progress = CreateWindowExW(0, PROGRESS_CLASSW, NULL,
                        WS_CHILD | PBS_MARQUEE,
                        0, 0, 10, 10, hwnd, (HMENU)IDC_PROGRESS, g_inst, NULL);
        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                        LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                        0, 0, 10, 10, hwnd, (HMENU)IDC_LIST, g_inst, NULL);
        g_log  = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL,
                        WS_CHILD | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                        ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                        0, 0, 10, 10, hwnd, (HMENU)IDC_LOG, g_inst, NULL);

        CreateWindowExW(0, L"BUTTON", L"启动(&S)",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0,0,10,10,
                        hwnd, (HMENU)IDC_START, g_inst, NULL);
        CreateWindowExW(0, L"BUTTON", L"关闭(&O)",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0,0,10,10,
                        hwnd, (HMENU)IDC_STOP, g_inst, NULL);
        CreateWindowExW(0, L"BUTTON", L"连接(&C)",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0,0,10,10,
                        hwnd, (HMENU)IDC_CONNECT, g_inst, NULL);
        CreateWindowExW(0, L"BUTTON", L"更多操作(&M)",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0,0,10,10,
                        hwnd, (HMENU)IDC_MORE, g_inst, NULL);
        CreateWindowExW(0, L"BUTTON", L"删除(&D)",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0,0,10,10,
                        hwnd, (HMENU)IDC_DELETE, g_inst, NULL);
        CreateWindowExW(0, L"BUTTON", L"新建(&N)",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0,0,10,10,
                        hwnd, (HMENU)IDC_NEW, g_inst, NULL);

        SendMessageW(g_log, EM_SETLIMITTEXT, 1000000, 0);

        ApplyOptionChecks();

        MakeFonts(DpiOf(hwnd));
        ApplyFonts();
        ShowWindow(g_log, g_showLogs ? SW_SHOW : SW_HIDE);
        LayoutAll(hwnd);
        UpdateButtons();
        return 0;
    }

    case WM_SIZE:
        LayoutAll(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        UINT dpi = DpiOf(hwnd);
        mmi->ptMinTrackSize.x = MulDiv(660, dpi, 96);
        mmi->ptMinTrackSize.y = MulDiv(420, dpi, 96);
        return 0;
    }

    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        MakeFonts(HIWORD(wp));
        ApplyFonts();
        SetWindowPos(hwnd, NULL, r->left, r->top,
                     r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        LayoutAll(hwnd);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }

    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g_log) {           /* 只读 EDIT 会走 STATIC 分支，保持白底 */
            SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
            SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
            return (LRESULT)g_whiteBrush;
        }
        break;

    case WM_COMMAND: {
        int id = LOWORD(wp);
        int code = HIWORD(wp);

        if (id == IDC_COMBO && code == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(g_combo, CB_GETCURSEL, 0, 0);
            if (sel != CB_ERR && sel != g_mode) {
                g_mode = sel;
                FreeItems();
                SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
                UpdateButtons();
                StartRefresh();
            }
            return 0;
        }
        if (id == IDC_LIST && (code == LBN_SELCHANGE || code == LBN_DBLCLK)) {
            UpdateButtons();
            if (code == LBN_DBLCLK) ActDetails();
            return 0;
        }
        switch (id) {
        case IDC_REFRESH: StartRefresh(); return 0;
        case IDC_START:   ActStart();     return 0;
        case IDC_STOP:    ActStop();      return 0;
        case IDC_CONNECT: ActConnect();   return 0;
        case IDC_NEW:     ActNew();       return 0;
        case IDC_DELETE:  ActDelete();    return 0;
        case IDC_MORE: {
            HMENU pm = CreatePopupMenu();
            RECT r;
            POINT pt;
            Item *it = SelItem();
            int cmd;
            if (!it) return 0;
            AppendMenuW(pm, MF_STRING, IDM_VMINFO,   L"打印虚拟机信息(&I)");
            AppendMenuW(pm, MF_STRING, IDM_VMSET,    L"虚拟机设置(&S)...");
            AppendMenuW(pm, MF_STRING, IDM_NESTED,   L"启用嵌套虚拟化(&N)");
            AppendMenuW(pm, MF_SEPARATOR, 0, NULL);
            AppendMenuW(pm, MF_STRING, IDM_VMDELETE, L"删除虚拟机(&D)...");
            GetWindowRect(GetDlgItem(hwnd, IDC_MORE), &r);
            pt.x = r.left; pt.y = r.top;
            cmd = (int)TrackPopupMenu(pm, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN,
                                      pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(pm);
            switch (cmd) {
            case IDM_VMINFO:   ActDetails();    break;
            case IDM_VMSET:    ActVmSettings(); break;
            case IDM_NESTED:   ActNested();     break;
            case IDM_VMDELETE: ActDelete();     break;
            }
            return 0;
        }

        case IDM_MGR: {
            wchar_t sysroot[MAX_PATH], mmc[MAX_PATH];
            if (GetEnvironmentVariableW(L"SystemRoot", sysroot, MAX_PATH))
                _snwprintf(mmc, MAX_PATH - 1, L"%ls\\System32\\mmc.exe", sysroot);
            else
                wcscpy(mmc, L"mmc.exe");
            mmc[MAX_PATH - 1] = 0;
            LaunchExe(mmc, L"virtmgmt.msc", FALSE);
            return 0;
        }
        case IDM_HVSET:    EnsureHvintegrate(); LaunchExe(HvintegratePath(), L"hv", FALSE); return 0;
        case IDM_VSSET:    EnsureHvintegrate(); LaunchExe(HvintegratePath(), L"vs", FALSE); return 0;
        case IDM_EDITDISK: EnsureHvintegrate(); LaunchExe(HvintegratePath(), L"ed", FALSE); return 0;
        case IDM_CREATEVM: EnsureHvintegrate(); LaunchExe(HvintegratePath(), L"av", FALSE); return 0;
        case IDM_OPTVHD:   ActOptimizeVhd(); return 0;
        case IDM_NETCONN:  LaunchExe(L"control.exe", L"netconnections", FALSE); return 0;
        case IDM_MSTSC:    LaunchExe(L"mstsc.exe", NULL, FALSE); return 0;
        case IDM_TASKSCHD: LaunchExe(L"taskschd.msc", NULL, FALSE); return 0;
        case IDM_OPENDIR: {
            wchar_t dir[MAX_PATH];
            HvintegrateDir(dir, MAX_PATH);
            LaunchExe(dir, NULL, FALSE);
            return 0;
        }
        case IDM_EXIT:
            g_exiting = TRUE;
            DestroyWindow(hwnd);
            return 0;

        case IDM_SHOWLOGS:
            ShowLogPane(!g_showLogs);
            return 0;
        case IDM_TRAYOPT:
            g_closeToTray = !g_closeToTray;
            Dbg(L"IDM_TRAYOPT    : toggled -> %d", g_closeToTray);
            ApplyOptionChecks();
            SettingsSave();
            return 0;
        case IDM_AUTOSTART:
            ActAutostart(!g_autoStartup);
            ApplyOptionChecks();
            return 0;
        case IDM_RESETDEF:
            RegDeleteTreeW(HKEY_CURRENT_USER, SHV_REGKEY);
            g_showLogs = 0; g_closeToTray = 0; g_autoStartup = 0;
            ApplyOptionChecks();
            ShowLogPane(FALSE);   /* 内部会 SettingsSave，把默认值写回去 */
            Log(L"已恢复默认设置：注册表项已删除并写回默认值。");
            return 0;
        case IDM_ELEVATE: {
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(NULL, exe, MAX_PATH);
            Log(L"请求以管理员身份重启...");
            LaunchExe(exe, L"-m", TRUE);
            g_exiting = TRUE;
            DestroyWindow(hwnd);
            return 0;
        }
        case IDM_ABOUT:
            MessageBoxW(hwnd,
                L"Simple Hyper-V (Win32)\n\n"
                L"极简 Hyper-V 图形前端。所有操作都是把脚本交给\n"
                L"powershell.exe 子进程执行 —— 本程序不链接 .NET，\n"
                L"不依赖 System.Management.Automation。\n\n"
                L"需要管理员权限，且 Hyper-V 的 PowerShell 模块只有 64 位版，\n"
                L"所以本程序必须编译为 x64。",
                L"关于", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        return 0;
    }

    case WM_APP_DONE: {
        DoneMsg *d = (DoneMsg *)lp;
        int action = d->action;
        DWORD code = d->exitCode;

        g_busy = FALSE;
        SetBusyUI(FALSE);

        if (action == RA_LIST) {
            if (code == 0) {
                PopulateList(d->text ? d->text : L"");
                Log(L"已加载 %d 项。", g_nItems);
            } else {
                /* 刷新失败时不能把 "ERROR: ..." 当成一个列表项填进去 */
                FreeItems();
                SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
                if (d->text && d->text[0]) LogText(d->text);
            }
        } else {
            if (d->text && d->text[0]) LogText(d->text);
            else                       Log(L"(无输出)");
        }
        if (code != 0) {
            Log(L"命令失败，退出码 %lu。", (unsigned long)code);
            if (!g_showLogs) ShowLogPane(TRUE);
        }
        free(d->text);
        free(d);

        if (action == RA_REFRESH && !g_exiting) StartRefresh();
        return 0;
    }

    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) {
            ShowFromTray();
        } else if (LOWORD(lp) == WM_RBUTTONUP) {
            HMENU pm = CreatePopupMenu();
            POINT pt;
            int cmd;
            AppendMenuW(pm, MF_STRING, IDM_TRAYSHOW, L"显示主窗口(&S)");
            AppendMenuW(pm, MF_SEPARATOR, 0, NULL);
            AppendMenuW(pm, MF_STRING, IDM_TRAYEXIT, L"退出(&X)");
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);   /* 不设前台，菜单点外面不会消失 */
            cmd = (int)TrackPopupMenu(pm, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_BOTTOMALIGN,
                                      pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(pm);
            PostMessageW(hwnd, WM_NULL, 0, 0);
            if (cmd == IDM_TRAYSHOW) ShowFromTray();
            else if (cmd == IDM_TRAYEXIT) { g_exiting = TRUE; DestroyWindow(hwnd); }
        }
        return 0;

    case WM_APP_RESTORE:
        ShowFromTray();
        return 0;

    case WM_CLOSE:
        Dbg(L"WM_CLOSE      : tray=%d exiting=%d", g_closeToTray, g_exiting);
        if (g_closeToTray && !g_exiting) { HideToTray(); return 0; }
        g_exiting = TRUE;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        SettingsSave();
        TrayRemove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------------ 入口 */

static BOOL RunCli(const wchar_t *script, const wchar_t *outFile)
{
    wchar_t *text = NULL;
    DWORD code = 0;
    BOOL ok;
    wchar_t *wscript = _wcsdup(script);

    ok = PsRunSync(wscript, &text, &code);
    free(wscript);
    if (!ok) return FALSE;

    if (outFile && *outFile) {
        HANDLE f = CreateFileW(outFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            char *u8 = U8(text ? text : L"");
            DWORD wrote = 0;
            WriteFile(f, u8, (DWORD)strlen(u8), &wrote, NULL);
            CloseHandle(f);
            free(u8);
        } else {
            return FALSE;
        }
    } else {
        /* GUI 子系统进程默认没有控制台；能附加到父进程就有输出 */
        char *u8 = U8(text ? text : L"");
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
            DWORD wrote = 0;
            if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, u8, (DWORD)strlen(u8), &wrote, NULL);
        }
        free(u8);
    }
    free(text);
    return code == 0;
}

#ifndef SHV_SELFTEST
/* 从资源里取应用图标。
 * res.rc 里图标必须写成 "101 ICON app.ico"（ICON 关键字 -> RT_GROUP_ICON）。
 * 写成 "101 3 app.ico" 会把整个 .ico 当成一张原始 RT_ICON 塞进去，
 * 资源目录里就没有 RT_GROUP_ICON，这里必然取不到 —— 表现是全程序退回默认图标。
 * 所以这条失败路径要吵一声，并且兜底一个系统图标（不然托盘图标为 NULL，
 * Shell_NotifyIcon 会整个失败，连托盘都出不来）。 */
static BOOL g_iconFallback = FALSE;

static HICON LoadAppIcon(HINSTANCE hi, int cx, int cy)
{
    HICON ic = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, cx, cy, 0);
    if (!ic) {
        Dbg(L"LoadAppIcon  : 取不到图标资源 IDI_APP=%d (%dx%d) err=%lu；"
            L"检查 res.rc 里图标是否写成了 ICON 关键字", IDI_APP, cx, cy, GetLastError());
        g_iconFallback = TRUE;
        ic = (HICON)LoadIconW(NULL, IDI_APPLICATION);
    }
    return ic;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR lpCmdLine, int nShow)
{
    WNDCLASSEXW wc;
    MSG msg;
    HANDLE mutex;
    int argc = 0;
    wchar_t **argv;
    const wchar_t *runScript = NULL, *runOut = NULL;
    BOOL noElevate = FALSE;
    int i;

    (void)hPrev;
    (void)nShow;
    g_inst = hInst;

    Dbg(L"=== start  lpCmdLine=[%ls]  admin=%d", lpCmdLine, (int)IsAdmin());

    argv = CommandLineToArgvW(lpCmdLine, &argc);
    for (i = 0; argv && i < argc; i++) {
        if (!wcscmp(argv[i], L"--no-elevate")) noElevate = TRUE;
        else if (!wcscmp(argv[i], L"--run") && i + 1 < argc) runScript = argv[++i];
        else if (!wcscmp(argv[i], L"--out") && i + 1 < argc) runOut = argv[++i];
    }

    /* 自检/脚本模式：走的是和 GUI 完全相同的那条 PowerShell 执行路径 */
    if (runScript) {
        BOOL ok = RunCli(runScript, runOut);
        if (argv) LocalFree(argv);
        return ok ? 0 : 1;
    }

    /* 先提权再做单实例判断：两个实例都提权后，自定义消息才不会被 UIPI 拦掉 */
    if (!noElevate && !IsAdmin()) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        {
            SHELLEXECUTEINFOW sei;
            ZeroMemory(&sei, sizeof(sei));
            sei.cbSize = sizeof(sei);
            sei.fMask  = SEE_MASK_FLAG_NO_UI;
            sei.lpVerb = L"runas";
            sei.lpFile = exe;
            sei.lpParameters = lpCmdLine;
            sei.nShow  = SW_SHOWNORMAL;
            if (ShellExecuteExW(&sei)) {
                if (argv) LocalFree(argv);
                return 0;
            }
        }
        /* 用户拒绝 UAC 就继续以普通权限运行，至少界面能看 */
    }

    mutex = CreateMutexW(NULL, TRUE, SHV_MUTEX);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND prev = FindWindowW(SHV_CLASS, NULL);
        if (prev) PostMessageW(prev, WM_APP_RESTORE, 0, 0);
        if (argv) LocalFree(argv);
        return 0;
    }

    EnableBestDpiAwareness();

    {   /* 注册两个窗口类 */
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = SHV_CLASS;
        wc.hIcon         = g_iconBig   = LoadAppIcon(hInst, GetSystemMetrics(SM_CXICON),
                                              GetSystemMetrics(SM_CYICON));
        wc.hIconSm       = g_iconSmall = LoadAppIcon(hInst, GetSystemMetrics(SM_CXSMICON),
                                              GetSystemMetrics(SM_CYSMICON));
        if (!RegisterClassExW(&wc)) {
            MessageBoxW(NULL, L"RegisterClassEx 失败", L"错误", MB_OK | MB_ICONERROR);
            return 1;
        }
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = PopupProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = SHV_POPUPCLASS;
        RegisterClassExW(&wc);
    }

    {
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof(icc);
        icc.dwICC  = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
        InitCommonControlsEx(&icc);
    }

    g_whiteBrush = CreateSolidBrush(GetSysColor(COLOR_WINDOW));

    SettingsLoad();

    {
        RECT r;
        int w = MulDiv(760, 96, 96), h = MulDiv(520, 96, 96);
        int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
        HMENU bar = BuildMenu();
        if (SettingsLoadRect(&r)) {
            w = r.right; h = r.bottom; x = r.left; y = r.top;
            Dbg(L"恢复窗口几何 %d,%d %dx%d", x, y, w, h);
        } else {
            Dbg(L"没有保存的窗口几何，用默认 %dx%d", w, h);
        }
        if (!CreateWindowExW(0, SHV_CLASS, SHV_TITLE,
                             WS_OVERLAPPEDWINDOW,
                             x, y, w, h, NULL, bar, hInst, NULL)) {
            MessageBoxW(NULL, L"CreateWindowEx 失败", L"错误", MB_OK | MB_ICONERROR);
            return 2;
        }
    }

    /* 允许低权限进程把恢复消息发给本窗口（--no-elevate 混用时需要） */
    ChangeWindowMessageFilterEx(g_hwnd, WM_APP_RESTORE, MSGFLT_ALLOW, NULL);

    TrayAdd();
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    if (!IsAdmin())
        Log(L"当前不是管理员权限，Hyper-V 相关命令会报“拒绝访问”。"
            L"可从 选项 -> 以管理员身份重启。");

    /* -m：静默启动（对齐原版行为） */
    for (i = 0; argv && i < argc; i++) {
        if (!wcscmp(argv[i], L"-m")) {
            if (g_closeToTray) HideToTray();
            else ShowWindow(g_hwnd, SW_MINIMIZE);
            break;
        }
    }
    if (argv) LocalFree(argv);

    StartRefresh();

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        /* 原版行为：主窗口按 Esc = 到托盘 / 最小化 */
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE && msg.hwnd &&
            (msg.hwnd == g_hwnd || IsChild(g_hwnd, msg.hwnd))) {
            if (g_closeToTray) HideToTray();
            else ShowWindow(g_hwnd, SW_MINIMIZE);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_font)      DeleteObject(g_font);
    if (g_fontFixed && g_fontFixed != g_font) DeleteObject(g_fontFixed);
    if (g_whiteBrush) DeleteObject(g_whiteBrush);
    /* 兜底的系统共享图标不属于我们，不能 DestroyIcon */
    if (!g_iconFallback) {
        if (g_iconBig)   DestroyIcon(g_iconBig);
        if (g_iconSmall) DestroyIcon(g_iconSmall);
    }
    FreeItems();
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
#endif /* !SHV_SELFTEST */

/* ============================================================================
 * 自检模式：用 -DSHV_SELFTEST 编成控制台程序，断言那些"出错了也看不出来"的纯逻辑。
 * 这些函数一旦写错，表现是 VM 名带引号就整个脚本崩、或列表字段串位，
 * 光靠肉眼点界面很难发现，所以单独做成可运行的断言。
 *   build.cmd selftest
 * ========================================================================== */
#ifdef SHV_SELFTEST

static int g_selftestFail;

static void Check(const char *what, int cond)
{
    printf("%-52s %s\n", what, cond ? "PASS" : "FAIL");
    if (!cond) g_selftestFail++;
}

static int WideIs(const wchar_t *a, const wchar_t *b)
{
    return a != NULL && b != NULL && wcscmp(a, b) == 0;
}

/* 把宽字符串转成能安全打印的形式（非 ASCII 用 \u 形式，避免控制台代码页干扰） */
static void ShowW(const wchar_t *s, char *out, size_t cch)
{
    size_t i = 0;
    out[0] = 0;
    for (; s && *s && i + 8 < cch; s++) {
        if (*s >= 32 && *s < 127) { out[i++] = (char)*s; out[i] = 0; }
        else { i += (size_t)_snprintf(out + i, cch - i, "\\u%04X", (unsigned)*s); }
    }
}

int main(void)
{
    char buf[512];

    SetConsoleOutputCP(CP_UTF8);

    /* ---- Base64Of：UTF-16LE -> Base64，-EncodedCommand 的基础 ---- */
    {
        wchar_t *b = Base64Of(L"abc");
        ShowW(b, buf, sizeof(buf));
        printf("  Base64Of(L\"abc\") = %s\n", buf);
        /* "abc" 的 UTF-16LE 是 61 00 62 00 63 00 */
        Check("Base64Of(L\"abc\") == YQBiAGMA", WideIs(b, L"YQBiAGMA"));
        free(b);
    }
    {
        wchar_t *b = Base64Of(L"");
        Check("Base64Of(L\"\") == \"\"", WideIs(b, L""));
        free(b);
    }
    {
        wchar_t *b = Base64Of(L"a");   /* 2 字节 -> 补一个 '=' */
        ShowW(b, buf, sizeof(buf));
        printf("  Base64Of(L\"a\") = %s\n", buf);
        Check("Base64Of(L\"a\") == YQA=", WideIs(b, L"YQA="));
        free(b);
    }

    /* ---- WCatQ：唯一一处用户输入转义，错了就是注入/脚本崩 ---- */
    {
        WBuf b;
        WInit(&b); WCatQ(&b, L"plain");
        Check("WCatQ(plain) -> 'plain'", WideIs(b.p, L"'plain'"));
        WFree(&b);
    }
    {
        WBuf b;
        WInit(&b); WCatQ(&b, L"it's");
        ShowW(b.p, buf, sizeof(buf));
        printf("  WCatQ(\"it's\") = %s\n", buf);
        Check("WCatQ(it's) -> 'it''s' (单引号翻倍)", WideIs(b.p, L"'it''s'"));
        WFree(&b);
    }
    {
        WBuf b;
        WInit(&b); WCatQ(&b, L"a'; Remove-VM -VMName 'x");
        ShowW(b.p, buf, sizeof(buf));
        printf("  WCatQ(注入尝试) = %s\n", buf);
        Check("WCatQ 注入尝试被完整包在单引号里",
              b.p[0] == L'\'' && b.p[b.len - 1] == L'\'' &&
              wcsstr(b.p, L"''") != NULL);
        WFree(&b);
    }
    {
        WBuf b;
        WInit(&b); WCat(&b, L"Get-VM -Name "); WCatQ(&b, L"我的 VM");
        Check("WCat + WCatQ 拼接形状", WideIs(b.p, L"Get-VM -Name '我的 VM'"));
        WFree(&b);
    }

    /* ---- SplitCidr ---- */
    {
        wchar_t ip[128], pre[16];
        SplitCidr(L"192.168.56.1/24", ip, 128, pre, 16);
        Check("SplitCidr 192.168.56.1/24 -> ip/24",
              WideIs(ip, L"192.168.56.1") && WideIs(pre, L"24"));
    }
    {
        wchar_t ip[128], pre[16];
        SplitCidr(L"10.0.0.1", ip, 128, pre, 16);
        Check("SplitCidr 无前缀 -> 默认 24",
              WideIs(ip, L"10.0.0.1") && WideIs(pre, L"24"));
    }
    {
        wchar_t ip[128], pre[16];
        SplitCidr(L"1.2.3.4/abc", ip, 128, pre, 16);
        Check("SplitCidr 非数字前缀 -> 退回 24",
              WideIs(ip, L"1.2.3.4") && WideIs(pre, L"24"));
    }

    /* ---- ParseListLine：VM 行契约 Name<TAB>State<TAB>Id ---- */
    {
        wchar_t line[] = L"vm1\tRunning\t{11111111-2222-3333-4444-555555555555}";
        wchar_t *n, *s, *i;
        ParseListLine(line, MODE_VM, &n, &s, &i);
        Check("ParseListLine VM 三字段", WideIs(n, L"vm1") &&
              WideIs(s, L"Running") &&
              WideIs(i, L"{11111111-2222-3333-4444-555555555555}"));
        free(n); free(s); free(i);
    }
    {
        wchar_t line[] = L"名字里有空格的 VM\tOff\t{g}";
        wchar_t *n, *s, *i;
        ParseListLine(line, MODE_VM, &n, &s, &i);
        Check("ParseListLine 保留名字里的空格", WideIs(n, L"名字里有空格的 VM"));
        free(n); free(s); free(i);
    }
    {
        wchar_t line[] = L"只有名字";
        wchar_t *n, *s, *i;
        ParseListLine(line, MODE_VM, &n, &s, &i);
        Check("ParseListLine 无 tab 时整行当名字", WideIs(n, L"只有名字") &&
              s == NULL && i == NULL);
        free(n);
    }
    {
        wchar_t line[] = L"vSwitch1";
        wchar_t *n, *s, *i;
        ParseListLine(line, MODE_SWITCH, &n, &s, &i);
        Check("ParseListLine Switch 模式只取名字", WideIs(n, L"vSwitch1"));
        free(n);
    }

    /* ---- PsWrap：外壳形状错了，退出码/错误文本就全乱 ---- */
    {
        wchar_t *s = PsWrap(L"Get-Date");
        Check("PsWrap 以 OutputEncoding 开头",
              wcsncmp(s, L"[Console]::OutputEncoding", 25) == 0);
        Check("PsWrap 含 try{ 与 Get-Date", wcsstr(s, L"try{Get-Date}") != NULL);
        Check("PsWrap 以 catch+exit 1 收尾", wcsstr(s, L"exit 1}") != NULL);
        free(s);
    }

    /* ---- IsClixml：语法错误时唯一会漏出 XML 垃圾的路径 ---- */
    Check("IsClixml 识别 #< CLIXML", IsClixml(L"#< CLIXML\r\n<Objs>"));
    Check("IsClixml 允许前导空白", IsClixml(L" \r\n\t#< CLIXML"));
    Check("IsClixml 不误判普通输出", !IsClixml(L"ERROR: 拒绝访问"));
    Check("IsClixml 不误判 NULL/空串", !IsClixml(NULL) && !IsClixml(L""));

    /* ---- 资源：图标必须是 RT_GROUP_ICON ----------------------------------
     * 这条断言是用血换来的：res.rc 里若把图标写成数字类型 "101 3 app.ico"，
     * 整个 .ico 会被当成一张原始 RT_ICON 塞进去，资源目录里没有 RT_GROUP_ICON，
     * LoadImage(IMAGE_ICON) 返回 NULL，表现是"exe 显示默认图标"。
     * 编译能过、能跑、界面正常，只有肉眼看图标才发现 —— 所以必须断言。
     * 注意：自检程序必须和 res.o 一起链接，否则这三条必然 FAIL。 */
    {
        HMODULE self = GetModuleHandleW(NULL);
        HICON big = (HICON)LoadImageW(self, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 32, 32, 0);
        HICON sml = (HICON)LoadImageW(self, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 16, 16, 0);
        Check("资源存在 RT_GROUP_ICON(101)",
              FindResourceW(self, MAKEINTRESOURCEW(IDI_APP), RT_GROUP_ICON) != NULL);
        Check("LoadImage(IDI_APP) 32x32 成功", big != NULL);
        Check("LoadImage(IDI_APP) 16x16 成功", sml != NULL);
        if (big) DestroyIcon(big);
        if (sml) DestroyIcon(sml);
    }

    printf("\n%s (%d 项失败)\n",
           g_selftestFail ? "SELFTEST FAILED" : "SELFTEST OK", g_selftestFail);
    return g_selftestFail ? 1 : 0;
}
#endif /* SHV_SELFTEST */