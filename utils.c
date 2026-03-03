#include "safe_rm.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

// 获取回收站根目录路径
char* get_rubbish_bin_path(void) {
    static char path[MAX_PATH_LEN];
    const char *home = getenv("HOME");
    if (home == NULL) {
#ifdef ARM64
        home = "/data/data/com.termux/files/home";
#else
        home = "/var";
#endif
    }
    snprintf(path, sizeof(path), "%s/%s", home, RUBBISH_BIN_NAME);
    return path;
}

// 获取时间戳目录路径
char* get_timestamp_dir_path(const char *timestamp) {
    static char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s", get_rubbish_bin_path(), timestamp);
    return path;
}

// 递归创建目录
int create_directory_recursive(const char *path) {
    char tmp[MAX_PATH_LEN];
    char *p = NULL;
    size_t len;
    struct stat st;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    if (stat(tmp, &st) == 0 && S_ISDIR(st.st_mode)) {
        return 0; // 目录已存在
    }

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (stat(tmp, &st) != 0) {
                if (mkdir(tmp, 0755) != 0) {
                    return -1;
                }
            }
            *p = '/';
        }
    }
    
    if (mkdir(tmp, 0755) != 0) {
        if (errno != EEXIST) {
            return -1;
        }
        // 目录已存在（可能是其他并发进程创建），继续执行
    }
    
    return 0;
}

// 检查路径是否是回收站或其上级目录
int is_rubbishbin_or_parent(const char *path) {
    char *rubbish_bin = get_rubbish_bin_path();
    char abs_path[MAX_PATH_LEN];
    char resolved_path[MAX_PATH_LEN];
    
    // 获取绝对路径
    if (realpath(path, resolved_path) == NULL) {
        strncpy(abs_path, path, sizeof(abs_path) - 1);
        abs_path[sizeof(abs_path) - 1] = '\0';
    } else {
        strncpy(abs_path, resolved_path, sizeof(abs_path) - 1);
        abs_path[sizeof(abs_path) - 1] = '\0';
    }
    
    // 获取回收站的绝对路径
    char rubbish_abs[MAX_PATH_LEN];
    if (realpath(rubbish_bin, rubbish_abs) == NULL) {
        strncpy(rubbish_abs, rubbish_bin, sizeof(rubbish_abs) - 1);
        rubbish_abs[sizeof(rubbish_abs) - 1] = '\0';
    }
    
    // 检查是否是回收站本身
    if (strcmp(abs_path, rubbish_abs) == 0) {
        return 1;
    }
    
    // 检查是否是回收站的子目录
    size_t rubbish_len = strlen(rubbish_abs);
    if (strncmp(abs_path, rubbish_abs, rubbish_len) == 0) {
        // 确保是子目录（下一个字符是/或者是结束符）
        if (abs_path[rubbish_len] == '/' || abs_path[rubbish_len] == '\0') {
            return 1;
        }
    }
    
    // 检查是否是回收站的上级目录
    size_t path_len = strlen(abs_path);
    if (strncmp(rubbish_abs, abs_path, path_len) == 0) {
        // 确保是上级目录
        if (rubbish_abs[path_len] == '/' || rubbish_abs[path_len] == '\0') {
            return 2; // 是上级目录
        }
    }
    
    return 0;
}

// 检查目录名是否符合时间戳格式 YYYY-MM-DD-HH:MM:SS
int is_valid_timestamp_format(const char *name) {
    regex_t regex;
    int ret;
    
    // 正则表达式匹配 YYYY-MM-DD-HH:MM:SS
    const char *pattern = "^[0-9]{4}-[0-9]{2}-[0-9]{2}-[0-9]{2}:[0-9]{2}:[0-9]{2}$";
    
    ret = regcomp(&regex, pattern, REG_EXTENDED);
    if (ret != 0) {
        return 0;
    }
    
    ret = regexec(&regex, name, 0, NULL, 0);
    regfree(&regex);
    
    return (ret == 0);
}

// 获取当前时间戳
void get_current_timestamp(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buffer, size, "%Y-%m-%d-%H:%M:%S", tm_info);
}

// 计算目录创建至今的天数
int days_since_creation(const char *path) {
    struct stat st;
    time_t now;
    double diff;
    
    if (stat(path, &st) != 0) {
        return -1;
    }
    
    now = time(NULL);
    diff = difftime(now, st.st_mtime);
    
    return (int)(diff / (24 * 3600));
}

// 获取绝对路径
char* get_absolute_path(const char *path) {
    static char abs_path[MAX_PATH_LEN];
    
    if (path[0] == '/') {
        strncpy(abs_path, path, sizeof(abs_path) - 1);
    } else {
        char cwd[MAX_PATH_LEN];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            strncpy(abs_path, path, sizeof(abs_path) - 1);
        } else {
            snprintf(abs_path, sizeof(abs_path), "%s/%s", cwd, path);
        }
    }
    abs_path[sizeof(abs_path) - 1] = '\0';
    
    return abs_path;
}

// 复制文件元数据
int copy_file_metadata(const char *src, const char *dst) {
    struct stat st;
    struct utimbuf times;
    
    if (stat(src, &st) != 0) {
        return -1;
    }
    
    // 复制权限
    if (chmod(dst, st.st_mode) != 0) {
        return -1;
    }
    
    // 复制时间戳
    times.actime = st.st_atime;
    times.modtime = st.st_mtime;
    if (utime(dst, &times) != 0) {
        return -1;
    }
    
    // 复制所有者（需要root权限）
    // chown(dst, st.st_uid, st.st_gid);
    
    return 0;
}

// 打印帮助信息
void print_help(const char *program_name) {
    printf("safe_rm - 安全的文件删除工具\n\n");
    printf("用法:\n");
    printf("  %s [选项] <文件1> [文件2] ...\n\n", program_name);
    printf("选项:\n");
    printf("  --remove-completely  彻底删除文件（带打字验证）\n");
    printf("  -h, --help           显示此帮助信息\n");
    printf("  -v, --version        显示版本信息\n\n");
    printf("模式:\n");
    printf("  默认模式（无选项）: 将文件移动到回收站（安全删除）\n");
    printf("  --remove-completely: 彻底删除文件，需通过打字验证\n\n");
    printf("回收站位置: ~/.rubbishbin/\n");
    printf("恢复工具: restore\n");
}
