# win32-helloworld

一个源文件的 Win32 窗口程序：**32 位 exe，兼容 Windows 7（32/64 位），支持高分屏，也能在 Win11 上运行**。

```
win32-helloworld/
├── hello.c      全部逻辑（206 行，含注释）
├── build.cmd    一键构建（无需 make）
├── Makefile     等价构建规则
└── hello.exe    构建产物：pei-i386，18,944 字节
```

## 构建

```bat
build.cmd          :: 只构建（i686 编译器不在 PATH 时自动回退到 D:\Projects\tools\mingw32\bin\）
build.cmd run      :: 构建并运行
```

或

```bat
mingw32-make                                        :: 32 位
mingw32-make CC=D:/Projects/tools/mingw64/bin/x86_64-w64-mingw32-gcc.exe   :: 64 位
mingw32-make clean
```

或直接一行：

```bat
i686-w64-mingw32-gcc -O2 -Wall -Wextra -mwindows -static -s -o hello.exe hello.c
```

工具链：`D:\Projects\tools\mingw32`（GCC 16.2.0，`i686-win32-dwarf`，**默认 msvcrt**，win32 线程模型）。

## 三条兼容性约定

1. **`WINVER` / `_WIN32_WINNT` = `0x0601`**（Win7），只调用 Win7 就有的 API，并让 `NONCLIENTMETRICSW` 使用 Win7 的结构布局。
2. **所有 DPI API 都用 `GetProcAddress` 动态解析**。静态导入会让 exe 在 Win7 上因“找不到过程入口点”直接加载失败：

   | API | 引入版本 | 作用 |
   | --- | --- | --- |
   | `user32!SetProcessDpiAwarenessContext` | Win10 1703+ | Per-Monitor V2（Win11 走这条） |
   | `shcore!SetProcessDpiAwareness` | Win8.1+ | Per-Monitor |
   | `user32!SetProcessDPIAware` | Vista+ / Win7 | 系统 DPI 感知（Win7 走这条） |
   | `user32!GetDpiForWindow` | Win10 1607+ | 取窗口 DPI；缺失时回退 `GetDeviceCaps(LOGPIXELSY)` |

3. **只依赖 msvcrt.dll**（Win7 系统自带），不用 UCRT —— UCRT 在 Win7 上需要 KB2999226。`-static` 保证不拖带 `libgcc_s_dw2-1.dll` / `libwinpthread-1.dll`，单文件即可分发。

## 高分屏行为

| 系统 | 实际启用 | 表现 |
| --- | --- | --- |
| Win10 1703+ / Win11 | Per-Monitor V2 | 每个显示器独立 DPI；拖动到不同缩放显示器时收 `WM_DPICHANGED`，按系统建议矩形重排窗口并重建字体 |
| Win8.1 / 早期 Win10 | Per-Monitor | 同上（Win8.1 起） |
| Win7 / Vista | System DPI Aware | 按主显示器 DPI 缩放（Win7 无 Per-Monitor 概念） |
| 全部失败 | 不设置 | 由系统做 DPI 虚拟化缩放 |

窗口初始客户区按 **320×120 逻辑像素**缩放（`MulDiv(320, dpi, 96)`），字体取系统消息字体（`SPI_GETNONCLIENTMETRICS` → `lfMessageFont`，Win11 上是 Segoe UI、Win7 上是 Tahoma），再把磅值按目标 DPI 换算，因此用户改系统字号也会被尊重。

## 本机实测结果（Win11 x64 / WOW64）

```bat
:: 构建
i686-w64-mingw32-gcc -O2 -Wall -Wextra -mwindows -static -s -o hello.exe hello.c   -> 0 warning

:: 架构与依赖
objdump -f hello.exe    -> file format pei-i386
objdump -p hello.exe    -> DLL Name: GDI32.dll / KERNEL32.dll / msvcrt.dll / USER32.dll

:: 黑名单扫描（期望 0 命中）
SetProcessDpiAwarenessContext|SetProcessDpiAwareness|GetDpiForWindow|GetDpiForSystem
|ProcessPrng|GetSystemTimePreciseAsFileTime|api-ms-win|ucrtbase|libgcc|libwinpthread|shcore
                        -> 0 hits

:: 运行
窗口标题                -> Hello, World!
DPI 感知（精确比较 -4） -> Per-Monitor V2 = True
GetDpiForWindow         -> 96（本机 100% 缩放）
客户区                  -> 320 x 120
关闭窗口                -> 退出码 0
```

- 无 `ucrtbase.dll`、无 `api-ms-win-crt-*`（不需要 KB2999226）、无 `libgcc_s_dw2-1.dll`、无 `libwinpthread-1.dll`。
- 导入的函数全部是 Win7 时代就有的（`KERNEL32`: `GetModuleHandleW`/`GetProcAddress`/`MulDiv`/`VirtualQuery`…；`USER32`: `CreateWindowExW`/`SystemParametersInfoW`/`DrawTextW`…；`msvcrt`: `__getmainargs`/`malloc`/`memcpy`…）。
- 每次构建的 `hello.exe` SHA256 都不同：ld 默认会往 PE 头写时间戳。想要逐字节可复现，可加 `-Wl,--no-insert-timestamp`。
- 同一份 `hello.c` 用 `x86_64-w64-mingw32-gcc`（mingw64 的 msvcrt 版）也能编过，依赖同样只有那 4 个 DLL。

## 尚未验证的部分（重要）

- **没有 Win7 真机/虚拟机**：Win7 兼容性目前只有静态保证（导入表 + API 版本 + msvcrt 运行时），没有真机运行记录。
- 本机是单显示器、100% 缩放，因此 **`WM_DPICHANGED` 分支没有被真正触发**，`GetDeviceCaps` 回退分支也没触发（Win11 上 `GetDpiForWindow` 存在）。这两条路径要到 Win7 或高缩放显示器上才会走到。
- 建议在 Win7 上做一次冒烟测试：窗口能出现、字体正常、可拖动；有条件的话把窗口拖到缩放不同的显示器上确认文字清晰、窗口按建议矩形重排。

## 改动时注意

- **新增任何 Win8+/Win10+ 的 API，都必须走 `GetProcAddress`**，否则 exe 在 Win7 上会直接加载失败（不是运行时报错，是根本起不来）。
- **Makefile 里不要写 `CC ?= ...`**：GNU make 内建 `CC=cc` 已算“已定义”，`?=` 不生效，会静默用 PATH 里的 `cc`（本机是 64 位工具链）编出错误架构。现在用 `ifeq ($(origin CC),default)` 处理。
- 只想支持 Win10+/Win11 的话，可以换成 UCRT 工具链并去掉这套动态解析，代码会更短；代价是 Win7 需要 KB2999226。