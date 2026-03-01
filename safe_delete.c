#include "mv_lib.h"
#include "safe_rm.h"
#include <stdio.h>
#include <fcntl.h>

typedef enum {
    PATH_SAFE = 0,           // 可以正常操作
    PATH_IS_DOT,             // 路径是 "." 本身
    PATH_IS_DOTDOT,          // 路径是 ".." 本身
    PATH_ENDS_WITH_DOT,      // 以 "/." 结尾（如 ./ 或 /path/.）
    PATH_ENDS_WITH_DOTDOT,   // 以 "/.." 结尾（如 ../ 或 /path/..）
    PATH_IS_CWD,             // 解析后指向当前工作目录
    PATH_IS_ROOT             // 解析后是根目录（删除危险）
} path_danger_t;

// 跳过的rm选项
static const char *skip_options[] = {"-r", "-f", "-rf", "-fr", "-R", NULL};

// 检查是否是rm选项
static int is_rm_option(const char *str) {
    int i = 0;
    while (skip_options[i] != NULL) {
        if (strcmp(str, skip_options[i]) == 0) {
            return 1;
        }
        i++;
    }
    return 0;
}

// 获取路径的最后组成部分（处理边缘情况）
static const char* get_last_component(const char *path) {
    if (!path || !*path) return "";

    // 跳过末尾的斜杠
    size_t len = strlen(path);
    while (len > 1 && path[len-1] == '/') len--;

    // 查找最后一个斜杠
    const char *last = path + len - 1;
    while (last >= path && *last != '/') last--;

    return (last < path) ? path : last + 1;
}

// 主检测函数：判断路径是否会导致 rm/mv 操作被拒绝
path_danger_t check_dangerous_path(const char *path) {
    if (!path || !*path) return PATH_SAFE;

    // 1. 直接检测 "." 或 ".."
    if (strcmp(path, ".") == 0) return PATH_IS_DOT;
    if (strcmp(path, "./") == 0) return PATH_IS_DOT;
    if (strcmp(path, "..") == 0) return PATH_IS_DOTDOT;
    if (strcmp(path, "../") == 0) return PATH_IS_DOTDOT;

    // 2. 检测以 "/." 或 "/.." 结尾（包括 "path/" 这种隐含当前目录的情况）
    // 规范化：去除末尾斜杠（根目录 "/" 除外）
    char clean_path[PATH_MAX];
    strncpy(clean_path, path, PATH_MAX - 1);
    clean_path[PATH_MAX - 1] = '\0';

    while (strlen(clean_path) > 1 && clean_path[strlen(clean_path)-1] == '/') {
        clean_path[strlen(clean_path)-1] = '\0';
    }

    const char *base = get_last_component(clean_path);

    if (strcmp(base, ".") == 0) return PATH_ENDS_WITH_DOT;
    if (strcmp(base, "..") == 0) return PATH_ENDS_WITH_DOTDOT;

    // 3. 检测解析后是否指向当前工作目录（通过 inode 比较）
    char resolved[PATH_MAX];
    if (realpath(clean_path, resolved) != NULL) {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            struct stat st_path, st_cwd;
            if (stat(resolved, &st_path) == 0 && stat(cwd, &st_cwd) == 0) {
                if (st_path.st_ino == st_cwd.st_ino &&
                    st_path.st_dev == st_cwd.st_dev) {
                    return PATH_IS_CWD;
                }
            }
        }

        // 4. 检测是否为根目录（通常也不应删除）
        if (strcmp(resolved, "/") == 0) return PATH_IS_ROOT;
    }

    return PATH_SAFE;
}

// 转换为 rm/mv 操作的错误提示
const char* get_danger_description(path_danger_t code) {
    switch (code) {
        case PATH_IS_DOT: 
            return "Cannot remove directory";
        case PATH_IS_DOTDOT: 
            return "Cannot remove directory";
        case PATH_ENDS_WITH_DOT: 
            return "Directory ends with '.'(points to self)";
        case PATH_ENDS_WITH_DOTDOT: 
            return "Directory ends with '..'(points to parent)";
        case PATH_IS_CWD: 
            return "Is current working directory";
        case PATH_IS_ROOT: 
            return "Is root directory";
        default: 
            return "Safe to operate";
    }
}

// 安全检查
int check_safety_constraints(const char *path) {
    struct stat st;
    int constraint_result;
    
    // 检查文件是否存在
    if (stat(path, &st) != 0) {
        fprintf(stderr, "错误: 无法访问 '%s': %s\n", path, strerror(errno));
        return -1;
    }
    
    // 检查是否是回收站或其子目录
    constraint_result = is_rubbishbin_or_parent(path);
    if (constraint_result == 1) {
        fprintf(stderr, "错误: 不能删除回收站或其子目录 '%s'\n", path);
        return -1;
    }
    
    // 检查是否是回收站的上级目录
    if (constraint_result == 2) {
        fprintf(stderr, "错误: 不能删除回收站的上级目录 '%s'\n", path);
        return -1;
    }

    // 检查是否有其他不行的目录
    int is_ok = check_dangerous_path(path);
    if (is_ok != PATH_SAFE) {
        fprintf(stderr, "错误: %s '%s'\n", 
                get_danger_description(is_ok), path);
        return -1;
    }
    
    return 0;
}

// 移动文件到回收站
int move_to_rubbishbin(const char *filepath, const char *timestamp, int index) {
    char timestamp_dir[MAX_PATH_LEN];
    char index_dir[MAX_PATH_LEN];
    char info_file[MAX_PATH_LEN];
    char rubbish_dir[MAX_PATH_LEN];
    char dest_path[MAX_PATH_LEN];
    char *abs_path;
    char *dir_path;
    char *base_name;
    FILE *fp;
    struct stat st;
    
    // 获取原文件的绝对路径
    abs_path = get_absolute_path(filepath);
    
    // 获取原文件所在目录和文件名
    char path_copy[MAX_PATH_LEN];
    strncpy(path_copy, abs_path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';
    
    dir_path = dirname(path_copy);
    
    strncpy(path_copy, abs_path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';
    base_name = basename(path_copy);
    
    // 创建时间戳目录
    snprintf(timestamp_dir, sizeof(timestamp_dir), "%s/%s", 
             get_rubbish_bin_path(), timestamp);
    
    if (create_directory_recursive(timestamp_dir) != 0) {
        fprintf(stderr, "错误: 无法创建目录 '%s': %s\n", timestamp_dir, strerror(errno));
        return -1;
    }
    
    // 创建编号子目录
    snprintf(index_dir, sizeof(index_dir), "%s/%d", timestamp_dir, index);
    if (mkdir(index_dir, 0755) != 0) {
        if (errno != EEXIST) {
            fprintf(stderr, "错误: 无法创建目录 '%s': %s\n", index_dir, strerror(errno));
            return -1;
        }
        // 目录已存在（异常情况），继续执行
    }
    
    // 创建 rubbish 子目录
    snprintf(rubbish_dir, sizeof(rubbish_dir), "%s/rubbish", index_dir);
    if (mkdir(rubbish_dir, 0755) != 0) {
        if (errno != EEXIST) {
            fprintf(stderr, "错误: 无法创建目录 '%s': %s\n", rubbish_dir, strerror(errno));
            return -1;
        }
        // 目录已存在（异常情况），继续执行
    }
    
    // 创建 info 文件
    snprintf(info_file, sizeof(info_file), "%s/info", index_dir);
    fp = fopen(info_file, "w");
    if (fp == NULL) {
        fprintf(stderr, "错误: 无法创建 info 文件 '%s': %s\n", info_file, strerror(errno));
        return -1;
    }
    fprintf(fp, "%s\n", dir_path);
    fclose(fp);
    
    // 构建目标路径
    snprintf(dest_path, sizeof(dest_path), "%s/%s", rubbish_dir, base_name);
    
    // 检查目标是否已存在
    if (stat(dest_path, &st) == 0) {
        // 添加数字后缀
        int suffix = 1;
        char new_dest[MAX_PATH_LEN];
        while (1) {
            snprintf(new_dest, sizeof(new_dest), "%s/%s.%d", rubbish_dir, base_name, suffix);
            if (stat(new_dest, &st) != 0) {
                strncpy(dest_path, new_dest, sizeof(dest_path) - 1);
                dest_path[sizeof(dest_path) - 1] = '\0';
                break;
            }
            suffix++;
        }
    }
    
    // 使用 mv 命令移动文件（保留权限）
    if (mv(abs_path, dest_path) == -1) {
        perror("Error: mv");
        fprintf(stderr, "Error: Failed to move the file '%s'\n", filepath);
        return -1;
    }
    return 0;
}

// 获取回收站目录锁（阻塞式）
// 返回值：锁定的文件描述符，失败返回 -1
static int lock_rubbish_bin(void) {
    char *rubbish_bin = get_rubbish_bin_path();
    char lock_file[MAX_PATH_LEN];
    int fd;
    struct flock fl;
    
    // 确保回收站目录存在
    create_directory_recursive(rubbish_bin);
    
    // 使用 .lock 文件作为锁
    snprintf(lock_file, sizeof(lock_file), "%s/.lock", rubbish_bin);
    fd = open(lock_file, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return -1;
    }
    
    // 获取排他锁（阻塞式）
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    
    if (fcntl(fd, F_SETLKW, &fl) < 0) {
        close(fd);
        return -1;
    }
    
    return fd;
}

// 释放回收站目录锁
static void unlock_rubbish_bin(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

// 安全删除文件
int safe_delete_files(int file_count, char **files) {
    char timestamp[FULL_TIME_LEN];
    int i;
    int success_count = 0;
    int index = 1;
    int lock_fd;
    
    // 获取回收站目录锁，确保单例操作
    lock_fd = lock_rubbish_bin();
    if (lock_fd < 0) {
        fprintf(stderr, "错误: 无法获取回收站锁\n");
        return -1;
    }
    
    // 获取当前时间戳（在锁保护下获取，确保唯一性）
    get_current_timestamp(timestamp, sizeof(timestamp));
    
    // 确保回收站根目录存在
    char *rubbish_bin = get_rubbish_bin_path();
    if (create_directory_recursive(rubbish_bin) != 0) {
        fprintf(stderr, "错误: 无法创建回收站目录 '%s': %s\n", rubbish_bin, strerror(errno));
        unlock_rubbish_bin(lock_fd);
        return -1;
    }
    
    // 创建 old 目录（用于存放压缩包）
    char old_dir[MAX_PATH_LEN];
    snprintf(old_dir, sizeof(old_dir), "%s/%s", rubbish_bin, OLD_DIR);
    if (create_directory_recursive(old_dir) != 0) {
        fprintf(stderr, "警告: 无法创建归档目录 '%s': %s\n", old_dir, strerror(errno));
    }
    
    // 处理每个文件
    for (i = 0; i < file_count; i++) {
        // 跳过rm选项
        if (is_rm_option(files[i])) {
            // printf("提示: 跳过选项 '%s'\n", files[i]);
            continue;
        }
        
        // 安全检查
        if (check_safety_constraints(files[i]) != 0) {
            continue;
        }
        
        // 移动到回收站
        if (move_to_rubbishbin(files[i], timestamp, index) == 0) {
            success_count++;
            index++;
        }
    }
    
    // 释放锁
    unlock_rubbish_bin(lock_fd);
    
    // 触发守护进程进行维护（在锁外触发，避免阻塞）
    check_and_trigger_daemon();
    
    return success_count;
}
