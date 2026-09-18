# simple-hyperv-win32 -- Makefile（纯 ASCII；GNU make / mingw32-make）
#
# 坑：不要写 "CC ?= x86_64-w64-mingw32-gcc"。
# make 内建的 CC=cc 已经算作"已定义"，?= 永远不生效，结果会静默改用 PATH 上的 cc
# （可能是 32 位编译器，编出来的 exe 加载不了 Hyper-V 模块）。
# 必须判断 $(origin CC) == default 才可靠。

GCC_FALLBACK     := D:/Projects/tools/mingw64/bin/x86_64-w64-mingw32-gcc.exe
WINDRES_FALLBACK := D:/Projects/tools/mingw64/bin/windres.exe

# 坑：$(origin CC) 是 default（make 有内建 CC=cc），但 GNU make 没有内建 RC，
# 所以 $(origin RC) 是 undefined。两个判断条件不一样，写成同一个就静默取到空值，
# 接着 recipe 里的 "-O coff" 会被 make 当成"忽略错误"的前缀，报出莫名其妙的错误。
ifeq ($(origin CC),default)
CC := $(firstword $(strip $(shell where x86_64-w64-mingw32-gcc 2>nul)))
ifeq ($(strip $(CC)),)
CC := $(GCC_FALLBACK)
endif
endif

ifeq ($(origin RC),undefined)
RC := $(firstword $(strip $(shell where windres 2>nul)))
ifeq ($(strip $(RC)),)
RC := $(WINDRES_FALLBACK)
endif
endif

TARGET   := simplehyperv.exe
SELFTEST := selftest.exe

CFLAGS ?= -O2 -Wall -Wextra -municode -mwindows -static -s -Wl,--no-insert-timestamp
LIBS   := -lcomctl32 -lshell32 -lcomdlg32 -ladvapi32 -lgdi32 -luser32
ALLDEPS := simplehyperv.c res.o

all: $(TARGET)

res.o: res.rc app.manifest app.ico hvintegrate.exe
	"$(RC)" -O coff -i res.rc -o res.o

$(TARGET): $(ALLDEPS)
	"$(CC)" $(CFLAGS) -o $@ simplehyperv.c res.o $(LIBS)

# 自检：同一份源码 + -DSHV_SELFTEST 编成控制台程序，断言纯逻辑
# res.o 必须一起链接：自检会断言图标资源确实是 RT_GROUP_ICON(101)，
# 这个错误编译能过、界面也正常，只有肉眼看图标才发现，所以必须断言。
$(SELFTEST): simplehyperv.c res.o
	"$(CC)" -DSHV_SELFTEST -O2 -Wall -Wextra -Wno-unused-function -Wno-unused-variable -static -s -o $@ simplehyperv.c res.o $(LIBS)

test: $(SELFTEST)
	cmd /c $(SELFTEST)

run: $(TARGET)
	cmd /c start "" $(TARGET)

clean:
	-cmd /c del /q res.o $(TARGET) $(SELFTEST) _shv_dbg.txt 2>nul

.PHONY: all test run clean