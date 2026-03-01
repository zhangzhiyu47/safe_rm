# safe_rm Makefile
# 兼容 Linux x86_64 和 ARM64

# 编译器
CC = gcc

# 编译选项
CFLAGS = -Wall -Wextra -O3
LDFLAGS =

# ncurses库
NCURSES_LIBS = -lncurses 

# 目标架构检测
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),aarch64)
    CFLAGS += -DARM64
	BINDIR = $(shell echo ${PREFIX})/bin
else
    CFLAGS += -DX86_64
	BINDIR = /usr/bin
endif

# 调试模式
ifdef DEBUG
    CFLAGS += -g -DDEBUG
endif

# 源文件
SAFE_RM_SRCS = safe_rm.c utils.c complete_remove.c safe_delete.c daemon.c actual_rm.c proctitle.c mv_lib.c tarlib.c
RESTORE_SRCS = restore_main.c utils.c restore.c mv_lib.c actual_rm.c tarlib.c
ZRESTORE_SRCS = zrestore_main.c zrestore.c utils.c restore.c mv_lib.c actual_rm.c tarlib.c

# 目标文件
SAFE_RM_OBJS = $(SAFE_RM_SRCS:.c=.o)
RESTORE_OBJS = $(RESTORE_SRCS:.c=.o)
ZRESTORE_OBJS = $(ZRESTORE_SRCS:.c=.o)

# 可执行文件
TARGETS = safe_rm restore zrestore

# 默认目标
all: $(TARGETS)

# safe_rm 主程序
safe_rm: $(SAFE_RM_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^
	@echo "finished: $@"

# restore 恢复工具
restore: $(RESTORE_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^
	@echo "finished: $@"

# zrestore 交互式回收站管理工具 (ncurses)
zrestore: $(ZRESTORE_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(NCURSES_LIBS)
	@echo "finished: $@"

# 通用编译规则
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# 清理
clean:
	@rm -f $(SAFE_RM_OBJS) $(RESTORE_OBJS) $(ZRESTORE_OBJS) $(TARGETS)
	@echo "finished"

# 安装
install: all
	@mkdir -p $(BINDIR)
	@cp safe_rm $(BINDIR)/
	@cp restore $(BINDIR)/
	@cp zrestore $(BINDIR)/
	@echo "finished"

# 卸载
uninstall:
	@rm -f $(BINDIR)/safe_rm
	@rm -f $(BINDIR)/restore
	@rm -f $(BINDIR)/zrestore
	@echo "finished"

# 覆盖系统默认的 rm 命令
cover:
	@mv $(BINDIR)/rm $(BINDIR)/real_rm
	@cp $(BINDIR)/safe_rm $(BINDIR)/rm
	@touch $(shell echo ${HOME})/../.covered_sys_rm
	@echo "finished"

# 恢复系统默认的 rm 命令
uncover: $(shell echo ${HOME})/../.covered_sys_rm
	@cp $(BINDIR)/real_rm $(BINDIR)/rm
	@rm -f $(shell echo ${HOME})/../.covered_sys_rm
	@echo "finished"

# 创建alias
alias:
	@echo "alias rm=\"$(BINDIR)/safe_rm\"" >> $(shell echo ${HOME})/.bashrc
	@echo "alias restore=\"$(BINDIR)/restore\"" >> $(shell echo ${HOME})/.bashrc
	@echo "finished"

# 测试
test: all
	@echo "Run basic tests(show version)"
	@./safe_rm --version
	@./restore --version
	@./zrestore --version
	@echo "finished"

.PHONY: all clean install uninstall cover test
