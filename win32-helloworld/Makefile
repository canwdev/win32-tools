# win32-helloworld — 32 位（i686 / msvcrt / Win7 兼容）构建
#
# 本机的 make 名字是 mingw32-make：
#   mingw32-make            # 构建 hello.exe
#   mingw32-make run        # 构建并运行
#   mingw32-make clean
#
# 想构建 64 位版本可覆盖 CC：
#   mingw32-make CC=x86_64-w64-mingw32-gcc

# 注意：不能用 CC ?= ...，因为 GNU make 内建 CC=cc 已经算“已定义”，?= 不会生效，
# 结果会用 PATH 里的 cc（本机是 64 位工具链）静默编出错误的架构。
ifeq ($(origin CC),default)
ifneq ($(shell where i686-w64-mingw32-gcc 2>nul),)
CC = i686-w64-mingw32-gcc
else
CC = D:/Projects/tools/mingw32/bin/i686-w64-mingw32-gcc.exe
endif
endif

CFLAGS  ?= -O2 -Wall -Wextra -mwindows -static -s
LDFLAGS ?=
RM       = del /q 2>nul
TARGET   = hello.exe
SRC      = hello.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

run: $(TARGET)
	$(TARGET)

clean:
	-$(RM) $(TARGET)
	-$(RM) *.o

.PHONY: all run clean