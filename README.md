# safe_rm - 安全删除工具（C语言版）

一个替代 Linux `rm` 命令的安全删除工具，提供"回收站"机制和"打字验证"双重保护。

## 功能特性

### 模式1：安全删除（默认）
- 将文件移动到回收站而非永久删除
- 支持文件恢复
- 自动后台压缩和清理

### 模式2：彻底删除
- 带打字验证机制防止误删
- 实时字符级颜色反馈（绿色=正确，红色=错误）
- 支持退格回退

### 守护进程
- 自动压缩旧文件（超过3天的目录，10个一组）
- 自动清理过期归档（超过60天）
- 单例模式，信号触发维护

### 回收站管理工具
- **restore**: 命令行回收站管理工具（恢复/删除）
- **zrestore**: 基于ncurses的交互式回收站管理工具

## 编译安装

### 编译
```bash
cd safe_rm
make
```

### 安装
```bash
make install
```

### 添加别名（推荐）
在 `.bashrc` 或 `.zshrc` 中添加：
```bash
alias rm='safe_rm'
```

## 使用方法

### safe_rm - 安全删除工具

```bash
# 安全删除（移动到回收站）
safe_rm <文件1> [文件2] ...

# 彻底删除（带打字验证）
safe_rm --remove-completely <文件1> [文件2] ...

# 显示帮助
safe_rm --help

# 显示版本
safe_rm --version
```

### restore - 回收站管理工具

```bash
# 列出所有可恢复项目
restore -l

# 恢复指定ID的项目
restore <ID>

# 恢复多个项目
restore 1 2 3

# 恢复所有项目
restore -a

# 删除指定ID的项目（永久删除）
restore -d 1 2 3

# 清空回收站（删除所有项目）
restore --delete-all

# 显示帮助
restore --help
```

### zrestore - 交互式回收站管理工具 (ncurses)

zrestore 提供了一个类似 **htop** 的精美终端界面，支持键盘导航、实时搜索、多选操作和彩色显示。

```bash
# 启动交互式界面（默认）
zrestore

# 或使用列表模式
zrestore -l

# 显示帮助
zrestore --help
```

**快捷键:**

| 按键 | 功能 |
|------|------|
| `↑/↓` 或 `k/j` | 上下移动选择 |
| `PgUp/PgDn` | 翻页 |
| `Home/End` | 跳到列表首尾 |
| `Space` | 选择/取消选择当前项（多选） |
| `a` | 全选所有匹配项 |
| `A` | 取消全选 |
| `Enter` | 恢复当前选中的项目 |
| `r` | 恢复所有选中的项目 |
| `R` | 恢复所有可见项目（忽略选择） |
| `d` | 删除选中的项目（永久删除） |
| `D` | 删除所有可见项目（清空） |
| `/` | 搜索过滤 |
| `Tab` | 切换搜索模式（精确/模糊） |
| `Esc` | 清除搜索条件 |
| `F1/?` | 显示帮助 |
| `Q` | 退出 |

**搜索模式:**

- **精确模式**: 子串匹配，匹配的字符会高亮显示
- **模糊模式**: 相似度匹配（显示相似度>60%的项目），按相似度排序

**界面特点:**
- 🎨 彩色高亮：绿色表示活跃项目，黄色表示已归档项目
- 📊 顶部状态栏显示项目统计和搜索模式
- 📁 中间列表显示：复选框、ID、删除时间、状态、原路径、文件名
- 🔍 实时搜索过滤，支持精确和模糊两种模式
- ✓ 多选支持（空格键选择，a/A全选/取消）
- 📋 底部预览面板显示选中项目的详细信息和操作提示
- ⚠️ 删除操作带红色警告确认对话框

## 目录结构

```
~/.rubbishbin/
├── YYYY-MM-DD-HH:MM:SS/     # 时间戳目录
│   ├── 1/
│   │   ├── info              # 原文件所在目录的绝对路径
│   │   └── rubbish/          # 实际存放被移动的文件
│   │       └── filename
│   └── 2/
│       └── ...
├── old/                      # 归档目录
│   └── YYYY-MM-DD-HH:MM:SS.tar.gz
└── .daemon.pid               # 守护进程PID文件
```

## 安全红线

1. **绝不能删除** `$HOME/.rubbishbin` 及其任何子内容
2. **绝不能删除** 垃圾桶根目录的上级目录（如 `/home`, `/`）
3. **恢复时绝不覆盖** 现有文件（跳过冲突）
4. **删除确认** - 从回收站永久删除前需要确认
5. **守护进程异常退出后**，下次调用自动清理失效PID文件并重启

## 技术细节

- 使用 POSIX 标准 API（`dirent.h`, `stat`, `mv` 等）
- 终端控制使用 `termios` 实现原始输入模式
- 信号处理：`SIGUSR1` 用于守护进程触发
- 文件锁和 PID 文件实现守护进程单例
- 模糊匹配使用 Levenshtein 距离算法
- 兼容 Linux x86_64 和 ARM64 架构

## 测试验证

```bash
# 测试安全删除
echo "测试内容" > $TMPDIR/test.txt
./safe_rm $TMPDIR/test.txt

# 查看回收站内容（普通列表模式）
./restore -l

# 使用交互式界面
./zrestore

# 恢复文件
./restore 1

# 从回收站永久删除文件
./restore -d 1

# 测试彻底删除
echo "测试内容" > $TMPDIR/test2.txt
./safe_rm --remove-completely $TMPDIR/test2.txt
# 输入: I am sure I want to remove these files or directories

# 测试安全检查
./safe_rm ~/.rubbishbin  # 应该拒绝
```

## 许可证

MIT License
