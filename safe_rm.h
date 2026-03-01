#ifndef SAFE_RM_H
#define SAFE_RM_H

// 特性测试宏 - 必须在任何头文件之前定义
#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <termios.h>
#include <ctype.h>
#include <limits.h>
#include <libgen.h>
#include <regex.h>
#include <sys/time.h>
#include <utime.h>
#include "mv_lib.h"
#include "tarlib.h"
#include "proctitle.h"

// 版本信息
#define VERSION "1.0.0"

// 回收站根目录名称
#define RUBBISH_BIN_NAME ".rubbishbin"

// 守护进程PID文件名
#define DAEMON_PID_FILE ".daemon.pid"

// 归档目录名
#define OLD_DIR "old"

// 时间格式字符串长度
#define TIME_FORMAT_LEN 20

// 完整时间格式字符串长度 (包含\0)
#define FULL_TIME_LEN 32

// 压缩触发阈值
#define COMPRESS_THRESHOLD 10

// 压缩触发天数
#define COMPRESS_DAYS 3

// 清理天数
#define CLEANUP_DAYS 60

// 最大路径长度
#define MAX_PATH_LEN 4096

// 最大文件名长度
#define MAX_NAME_LEN 256

// 打字验证字符串
#define VERIFY_STRING "I am sure I want to remove these files or directories"

// 颜色代码
#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_WHITE   "\033[37m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_CYAN    "\033[36m"

// 操作模式
typedef enum {
    MODE_SAFE_DELETE,       // 安全删除（默认）
    MODE_COMPLETE_REMOVE    // 彻底删除
} OperationMode;

// 文件信息结构
typedef struct {
    char path[MAX_PATH_LEN];
    char name[MAX_NAME_LEN];
    int is_valid;
} FileInfo;

// 回收站项目信息
typedef struct {
    int id;
    char timestamp[FULL_TIME_LEN];
    char original_path[MAX_PATH_LEN];
    char filename[MAX_NAME_LEN];
    int is_archived;        // 0=在桶中, 1=已归档
    char archive_file[MAX_PATH_LEN]; // 归档文件名
} RestoreItem;

// 工具函数 utils.c
char* get_rubbish_bin_path(void);
char* get_timestamp_dir_path(const char *timestamp);
int create_directory_recursive(const char *path);
int is_rubbishbin_or_parent(const char *path);
int is_valid_timestamp_format(const char *name);
void get_current_timestamp(char *buffer, size_t size);
int days_since_creation(const char *path);
char* get_absolute_path(const char *path);
int copy_file_metadata(const char *src, const char *dst);
void print_help(const char *program_name);

// 彻底删除模块 complete_remove.c
int complete_remove_files(int file_count, char **files);
int typing_verification(void);
void setup_terminal(struct termios *old_term);
void restore_terminal(struct termios *old_term);

// 安全删除模块 safe_delete.c
int safe_delete_files(int file_count, char **files);
int move_to_rubbishbin(const char *filepath, const char *timestamp, int index);
int check_safety_constraints(const char *path);

// 守护进程模块 daemon.c
int start_daemon(void);
int check_and_trigger_daemon(void);
void daemon_main(void);
void handle_sigusr1(int sig);
void handle_sigterm(int sig);
int compress_old_directories(void);
int cleanup_old_archives(void);
int count_a_directories(char ***dirs, int *count);
int is_process_alive(pid_t pid);

// 恢复工具模块 restore.c
int scan_restore_items(RestoreItem **items, int *count);
int restore_item_by_id(int id, RestoreItem *items, int count);
int restore_from_archive(const char *archive_path, const char *item_timestamp,
                         const char *original_path, const char *filename);
void print_restore_list(RestoreItem *items, int count);
void free_restore_items(RestoreItem *items, int count);

// 永久删除模块（从回收站删除）
int delete_item_by_id(int id, RestoreItem *items, int count);
int delete_from_archive(const char *archive_path, const char *item_timestamp,
                        const char *filename);
int delete_items_batch(int *ids, int id_count, RestoreItem *items, int count);

// 实际删除模块 actual_rm.c
int redel3(char *const argv[]);

#endif // SAFE_RM_H
