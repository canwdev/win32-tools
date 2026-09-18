# simple-hyperv-win32

`winform-tools\simple-hyperv`（C# WinForms + 进程内 PowerShell SDK）的 **Win32 重写试验品**。

不是逐像素等价移植，是**功能等价的极简版**：界面全用 Win32 原生控件，脚本一律交给
`powershell.exe` 子进程跑，**本程序不链接 .NET，不依赖 `System.Management.Automation`**。

```
simplehyperv.c      2020 行   单文件，全部逻辑（含自检块）
res.rc / app.manifest / app.ico   资源：图标 + comctl32 v6 清单
hvintegrate.exe     15360 B   内嵌进 exe，运行时释放到程序目录
build.cmd / Makefile          两条互相独立的构建路径
simplehyperv.exe    89088 B   产物，单文件，无外部依赖
```

---

## 1. 平台要求

| 项 | 要求 | 原因 |
| --- | --- | --- |
| **必须是 x64** | 编译成 64 位 | Hyper-V 的 PowerShell 模块**只有 64 位版**，32 位进程加载不了。32 位 exe 必须绕 `%SystemRoot%\Sysnative\...` 才行 |
| OS | Win10/11 的 Pro / Enterprise / Education | Hyper-V 不在家庭版里；Client Win7 根本没有 Hyper-V |
| 权限 | 管理员 | 程序启动时用 `ShellExecuteEx(runas)` 自我提权 |

因为平台下限是 Win10，**高 DPI 那套"必须动态解析 API"的约束在这里其实可以省掉**；
但代码仍然沿用了 `GetProcAddress` 动态解析（`EnableBestDpiAwareness`），
目的是让它在缺 API 的系统上**能起来并给出可读提示**，而不是"找不到入口点"直接加载失败。

## 2. 构建

```cmd
build.cmd              :: 编译（windres + gcc）
build.cmd test         :: 同一份源码编出控制台自检程序并运行
build.cmd run          :: 编译后启动
build.cmd clean
```
```cmd
mingw32-make           :: 等价路径
mingw32-make test
mingw32-make clean
```

工具链：mingw-w64 GCC 16.2.0 (x86_64, msvcrt) + windres。
脚本优先用 PATH 上的编译器，找不到就回退 `D:\Projects\tools\mingw64\bin\`。

编译选项：`-O2 -Wall -Wextra -municode -mwindows -static -s -Wl,--no-insert-timestamp`
（最后一项去掉 PE 时间戳，让构建可复现）。

## 3. 与原版的差异

**有意砍掉的**（极简版的范围）：

| 原版 | 这里 | 说明 |
| --- | --- | --- |
| 4 页 TabControl + 图标 ImageList | 一个 ComboBox 切换 VM / 交换机 / NetNat | 省掉 `WC_TABCONTROL` + `ImageList_*` 约 250 行 |
| 2 个菜单栏、21 个菜单项 | 1 个菜单栏（工具 / 选项 / 帮助） | |
| 命名管道单实例 | `CreateMutex` + `FindWindow` + `WM_APP_RESTORE` | 少一个服务端线程和一套 ACL |
| `Microsoft.Win32.TaskScheduler` COM 库 | `schtasks.exe` | 一行命令顶掉一个 COM 库封装 |
| 反射遍历 PSObject 属性打印详情 | `Format-List * \| Out-String` | 文本输出，不再需要结构化对象 |
| RichTextBox + 超链接 | 纯 EDIT | |
| ToolTip、ImageList 图标 | 无 | |

**新增 / 改进的**：

- **出错时自动展开日志窗口**（原版出错只往隐藏的日志里写，用户看不到）
- **选项 → 恢复默认设置**（删注册表项并写回默认值）
- **`SHV_DBG` 环境变量**：把关键状态 + 日志窗口内容镜像到 `_shv_dbg.txt`。GUI 程序没有控制台，这是唯一能看内部状态的办法
- **非管理员不强制退出**：提权失败也先把界面给你看，并明确告诉你哪一条命令会失败
- **`--run` / `--out` CLI 模式**：走的是和 GUI 完全相同的 `PsRunSync`，可脚本化自检
- 建内部交换机时**内置等待适配器出现的重试循环**（原版 `Get-NetAdapter` 可能抢在适配器就绪前执行）
- 配置保存失败会**在界面上告警一次**（组策略锁死注册表时不再静默失败）

## 4. 九个坑（每个都在代码里有对应处理）

1. **脚本用 `-EncodedCommand`（Base64/UTF-16LE）传**，不用 `-Command`。
   彻底绕开命令行引号地狱，脚本里的中文也不会被代码页破坏。见 `Base64Of()`。
2. **必须加 `-OutputFormat Text`**。不加的话 PowerShell 5.1 会把 stderr 序列化成
   CLIXML（`#< CLIXML ...` XML 垃圾）灌进日志。
3. **脚本第一句必须是 `[Console]::OutputEncoding=[Text.Encoding]::UTF8`**，
   否则管道里拿到的是 OEM 代码页（中文系统 936），VM 名带中文就乱码。
4. **用户输入一律单引号包裹 + `'` 翻倍**，绝不拼双引号字符串 —— 否则名字里带引号就能
   破坏脚本结构。这是唯一的注入防线：`WCatQ()`。自检里有注入尝试的断言。
5. **每个脚本包在 `try/catch` 里，失败输出 `ERROR: <原因>` 并以退出码 1 结束**，
   这样 C 侧既能显示原因也能判成败：`PsWrap()`。
6. **不要去解析 `Get-VM` 的默认表格**。默认表格按控制台宽度截断并加 `...`，
   还随每台机器变化。让脚本主动产出 tab 分隔的一行（`Name<TAB>State<TAB>Id`），
   见 `ScriptRefresh()` / `ParseListLine()`。
7. **`-OutputFormat Text` 不能覆盖"语法错误"这一条路径**。脚本没通过解析时，
   我那行前置语句根本没跑，编码也还是 OEM，于是吐出 CLIXML 乱码。
   `IsClixml()` 识别出来替换成人话。**只有这一处会漏 XML 垃圾。**
8. **注册表写入可能被拒绝**（组策略、受限环境）。`RegCreateKeyEx` 失败要在界面上
   说一次，别静默。这次开发环境的沙箱就一直在拒（`rc=5`），正好把这条路径测出来了。
9. **图标资源必须用 `ICON` 关键字，不能用数字类型。** 写成 `101 3 "app.ico"`
   （`3` = `RT_ICON`）是把整个 `.ico` 当成**一张**原始位图塞进去，资源目录里
   只有 `TYPE 3 / name 101`，**没有 `TYPE 14`**。而 `LoadImageW(..., IMAGE_ICON, ...)`
   找的是 `RT_GROUP_ICON`，于是返回 NULL、`wc.hIcon` 为 NULL、
   程序全程用默认图标、`ExtractIconEx` 返回 0 —— **编译能过、能跑、界面完全正常，
   只有肉眼看图标才发现**。必须写 `101 ICON "app.ico"`，让 windres 自己把
   `.ico` 拆成 `RT_GROUP_ICON(101)` + `RT_ICON(1..4)`。
   这条已经做成自检断言（见第 5 节），因为它是典型的"测不出来"的 bug。

另外两个踩过的构建坑：

- **`Makefile` 里 `CC ?= x86_64-w64-mingw32-gcc` 永远不生效** —— make 内建的 `CC=cc`
  已经算"已定义"。必须判 `$(origin CC) == default`。
- **`$(origin RC)` 是 `undefined` 而不是 `default`**（GNU make 没有内建 `RC`）。
  判断条件写成和 `CC` 一样，`RC` 就会静默取到空值，接着 recipe 里的 `-O coff`
  会被 make 当成"忽略错误"的前缀，报出莫名其妙的 `CreateProcess(NULL, O coff ...) failed`。
- **`build.cmd` 必须是纯 ASCII + CRLF**。LF + UTF-8 会被 cmd.exe 解析错乱（上次那个
  死循环就是这么来的）。当前文件：`non-ASCII=0 CR=LF=51 BOM=False`。

## 5. 自检

`build.cmd test` 用**同一份源码**加 `-DSHV_SELFTEST` 编成控制台程序，
断言那些"写错了也看不出来"的纯逻辑：base64、单引号转义、CIDR 拆分、
列表行解析、`PsWrap` 外壳形状、CLIXML 识别、**以及图标资源确实是 `RT_GROUP_ICON`**。
**24 项断言，全部通过。**

> 自检程序**必须和 `res.o` 一起链接**（`build.cmd` / `Makefile` 已经这么做）。
> 最后那 3 条图标断言会真的去 `FindResource` / `LoadImage` 自己的模块，
> 资源没链进去就必然 FAIL —— 这是刻意的。
> 已验证这条断言**能失败**：把 `res.rc` 改回 `101 3 "app.ico"` 后重跑，
> 3 条图标断言全 FAIL、`build.cmd test` 退出码 1（`BUILD FAILED`）。

其中 base64 与 PowerShell 自身双向一致：

```
C  产出 Base64Of("abc") = YQBiAGMA
PS 解码该串             -> "abc"
PS 编码 "abc"           = YQBiAGMA   （与 C 一致）
```

## 6. 实测结果（本机 Win11 x64，非管理员）

| 项目 | 结果 |
| --- | --- |
| 编译 `-O2 -Wall -Wextra` | **0 warning** |
| 架构 / 子系统 | `pei-x86-64` / Windows GUI |
| 依赖 DLL | 仅 `ADVAPI32 COMCTL32 comdlg32 GDI32 KERNEL32 msvcrt SHELL32 USER32`（全部系统自带） |
| 危险导入扫描 | **0 命中**（无 `api-ms-win-*`、无 `ucrtbase`、无 `libgcc`、无 `libwinpthread`、无静态 `shcore`） |
| 内嵌清单 | comctl32 v6 + `asInvoker` 均在 exe 内（控件有主题外观） |
| **图标资源结构** | `RT_GROUP_ICON(101)` + `RT_ICON(1,2,3,4)` 齐全（`RT_ICON` 是 ico 里 4 张位图，不是整个 ico 一个资源） |
| **图标真的生效** | 像素级比对：窗口图标 `5117D89F…` == 资源图标 == `ExtractIconEx`（资源管理器看到的）**且 != 默认图标** `DD77E486…`；`LoadAppIcon` 兜底分支未被触发 |
| 主窗口 | 标题 `Simple Hyper-V (Win32)`，760×520，位置从注册表恢复 |
| 子控件 | 11 个全部创建，尺寸/中文标签正确（ComboBox、6 按钮、ListBox、进度条、Edit） |
| 参数解析 | `lpCmdLine=[--no-elevate]`，`--run`/`--out` 均正确 |
| **刷新失败时不污染列表** | `LB_GETCOUNT = 0`（`ERROR: 拒绝访问` 没有被当成列表项） |
| **出错自动展开日志** | Edit `visible=True`，内容为刷新脚本 + `ERROR: 拒绝访问` + `命令失败，退出码 1。` |
| **关闭到托盘** | `CloseToTray=1` 时 WM_CLOSE → 进程存活、窗口隐藏 |
| **第二实例唤起** | `WM_APP_RESTORE` → 窗口重新可见 |
| **正常退出** | `CloseToTray=0` 时 WM_CLOSE → **退出码 0** |
| 内嵌资源释放 | 删掉 `hvintegrate.exe` 后点"Hyper-V 设置"→ 重新释放出 15,360 字节 |
| 命令行模式 | `--run "Get-Date"` 等 7 组用例，退出码/UTF-8 中文/`ERROR:` 文本全部正确 |

## 7. 未验证项（重要）

**没有验证过的，不要当成已验证：**

- **所有真正操作 Hyper-V 的命令一次都没跑过**：`Get-VM`、`Start-VM`、`Stop-VM`、`Save-VM`、
  `Remove-VM`、`Set-VMProcessor`、`Get/New/Remove-VMSwitch`、`Get/New/Remove-NetNat`、
  `New-NetIPAddress`、`Optimize-VHD`。本机当前不是管理员，凡涉及这些的调用都返回
  `ERROR: 拒绝访问`。**上面验证的是"失败路径"和"列表解析契约"，不是这些命令的成功路径。**
- **`vmconnect.exe` 未验证**。原版注释里那个 "token does not exist" 的坑，
  代码沿用了同样的规避方式（优先用程序目录下的副本，否则用 `System32` 那个并给出提示），
  但没有实际点过"连接"按钮。
- **`schtasks.exe` 开机自启未验证**（会修改系统计划任务，没在未经确认的情况下执行）。
- **`hvintegrate.exe` 的四个参数（`hv` / `vs` / `ed` / `av` / `vm <id>`）照搬原版，
  实际对话框没打开确认过**。资源释放本身验证了。
- **`WM_DPICHANGED` 分支没有触发过**：本机单显示器 100% 缩放。
- **设置持久化**：本开发环境的沙箱**拒绝注册表写入**（`RegCreateKeyEx rc=5`），
  所以写入没能真正落盘验证。读到的是早先一次成功的写入（`227,292 760x520`，
  且宽高存取方式正确）。真实机器上应当正常；**"写失败要在界面上告警"这条路径反而实测到了。**
- **Win10 真机未测**（本机是 Win11 26200）。代码把平台下限设为 Win10，
  但 API 层面没有做过 Win10 的实测。

## 8. 代码地图

| 区域 | 函数 |
| --- | --- |
| PowerShell 执行层 | `PsExePath` `PsWrap` `PsRunSync` `PsRunAsync` `JobThread` `Base64Of` `IsClixml` |
| 脚本构造（唯一拼脚本的地方） | `ScriptRefresh` `WCatQ` `SplitCidr` + 各 `Act*` |
| 输出解析 | `ParseListLine` `PopulateList` |
| 界面 | `WndProc` `LayoutAll` `MakeFonts` `ApplyFonts` `UpdateButtons` `ApplyOptionChecks` |
| 托盘 / 单实例 | `TrayAdd` `TrayRemove` `ShowFromTray` `HideToTray` |
| 设置 | `SettingsLoad` `SettingsSave` `SettingsLoadRect` |
| 进程 / 外壳 | `LaunchExe` `RunHiddenWait` `IsAdmin` |
| 资源 | `EnsureHvintegrate` `HvintegratePath` `LoadAppIcon` |
| 自检 | `#ifdef SHV_SELFTEST` 块（24 项断言，含图标资源） |

**改动时最需要注意的一条**：往这个程序里加任何 Win8+/Win10+ 的 API 都必须用
`GetProcAddress` 动态解析，否则在缺该 API 的系统上不是报错而是**加载失败**。

## 9. 已知限制

- 列表不支持多选，一次只操作一个对象
- 没有 VM 状态自动刷新，要手动点"刷新"
- `PopulateList` 单行上限 2047 字符；日志超过 400000 字符后砍掉前半段
- 进度条是跑马灯，没有真实进度（原版也是）
- 用户输入虽然转义了，但**没有做 CIDR / IP 的合法性校验**，非法输入靠 PowerShell 报错

## 10. 资产来源

- `app.ico`：从原项目 `Resources\icon-hyperv.ico` 裁出 16/24/32/48 四个尺寸，
  丢掉占 270KB 的 256×256 帧，432KB → 17.5KB。
  **`res.rc` 里必须用 `ICON` 关键字（见坑 9），windres 会把它拆成
  `RT_GROUP_ICON(101)` + 四个 `RT_ICON`，`LoadImageW(IMAGE_ICON)` 才取得到。**
- `hvintegrate.exe`：原项目 `Resources\hvintegrate.exe` 原样复制，
  以 10 号类型（RCDATA）资源嵌入。RCDATA 是原始字节、没有内部结构，
  所以这里用数字类型写法是正确的。