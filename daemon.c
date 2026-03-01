#include "proctitle.h"
#include "safe_rm.h"
#include <stdio.h>
#include <syslog.h>
#include <fcntl.h>
#include <dirent.h>

static volatile int sigusr1_received = 0;
static volatile int sigterm_received = 0;

// 信号处理函数
void handle_sigusr1(int sig) {
    (void)sig;
    sigusr1_received = 1;
}

void handle_sigterm(int sig) {
    (void)sig;
    sigterm_received = 1;
}

// 检查进程是否存活
int is_process_alive(pid_t pid) {
    if (kill(pid, 0) == 0) {
        return 1;
    }
    return 0;
}

// 获取PID文件路径
static char* get_pid_file_path(void) {
    static char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s", get_rubbish_bin_path(), DAEMON_PID_FILE);
    return path;
}

// 读取PID文件
static pid_t read_pid_file(void) {
    char *pid_file = get_pid_file_path();
    FILE *fp = fopen(pid_file, "r");
    pid_t pid = -1;
    
    if (fp != NULL) {
        if (fscanf(fp, "%d", &pid) != 1) {
            pid = -1;
        }
        fclose(fp);
    }
    
    return pid;
}

// 写入PID文件
static void write_pid_file(pid_t pid) {
    char *pid_file = get_pid_file_path();
    FILE *fp = fopen(pid_file, "w");
    
    if (fp != NULL) {
        fprintf(fp, "%d\n", pid);
        fclose(fp);
    }
}

// 删除PID文件
static void remove_pid_file(void) {
    char *pid_file = get_pid_file_path();
    unlink(pid_file);
}

// 统计A类目录
int count_a_directories(char ***dirs, int *count) {
    DIR *dp;
    struct dirent *entry;
    char *rubbish_bin = get_rubbish_bin_path();
    int capacity = 10;
    int n = 0;
    
    *dirs = malloc(capacity * sizeof(char*));
    if (*dirs == NULL) {
        return -1;
    }
    
    dp = opendir(rubbish_bin);
    if (dp == NULL) {
        free(*dirs);
        return -1;
    }
    
    while ((entry = readdir(dp)) != NULL) {
        if (entry->d_type == DT_DIR || entry->d_type == DT_UNKNOWN) {
            if (strcmp(entry->d_name, ".") == 0 || 
                strcmp(entry->d_name, "..") == 0 ||
                strcmp(entry->d_name, OLD_DIR) == 0) {
                continue;
            }
            
            if (is_valid_timestamp_format(entry->d_name)) {
                if (n >= capacity) {
                    capacity *= 2;
                    char **new_dirs = realloc(*dirs, capacity * sizeof(char*));
                    if (new_dirs == NULL) {
                        break;
                    }
                    *dirs = new_dirs;
                }
                
                (*dirs)[n] = malloc(strlen(entry->d_name) + 1);
                if ((*dirs)[n] != NULL) {
                    strcpy((*dirs)[n], entry->d_name);
                    n++;
                }
            }
        }
    }
    
    closedir(dp);
    *count = n;
    return 0;
}

// 比较时间戳（用于排序）
static int compare_timestamp(const void *a, const void *b) {
    return strcmp(*(const char**)a, *(const char**)b);
}

// 递归删除目录（压缩成功后清理）
static void remove_directory_recursive(const char *path) {
    DIR *dp;
    struct dirent *entry;
    struct stat st;
    char full_path[MAX_PATH_LEN];
    
    dp = opendir(path);
    if (dp == NULL) {
        return;
    }
    
    while ((entry = readdir(dp)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        if (stat(full_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                remove_directory_recursive(full_path);
            } else {
                unlink(full_path);
            }
        }
    }
    
    closedir(dp);
    rmdir(path);
}

// 压缩旧目录
int compress_old_directories(void) {
    char **dirs = NULL;
    int count = 0;
    int i;
    int compressed = 0;
    char *rubbish_bin = get_rubbish_bin_path();
    char old_dir[MAX_PATH_LEN];
    
    snprintf(old_dir, sizeof(old_dir), "%s/%s", rubbish_bin, OLD_DIR);
    
    // 获取所有A类目录
    if (count_a_directories(&dirs, &count) != 0) {
        return -1;
    }
    
    if (count <= COMPRESS_THRESHOLD) {
        // 目录数量不足，不压缩
        for (i = 0; i < count; i++) {
            free(dirs[i]);
        }
        free(dirs);
        return 0;
    }
    
    // 按时间排序（老的在前）
    qsort(dirs, count, sizeof(char*), compare_timestamp);
    
    // 检查最老的目录是否超过压缩天数
    char oldest_dir[MAX_PATH_LEN];
    snprintf(oldest_dir, sizeof(oldest_dir), "%s/%s", rubbish_bin, dirs[0]);
    int days = days_since_creation(oldest_dir);
    
    if (days < COMPRESS_DAYS) {
        // 最老的目录也不够旧，不压缩
        for (i = 0; i < count; i++) {
            free(dirs[i]);
        }
        free(dirs);
        return 0;
    }
    
    // 确保old目录存在
    create_directory_recursive(old_dir);
    
    // 按COMPRESS_THRESHOLD个一组压缩（从老的开始）
    // 保留最新的 COMPRESS_THRESHOLD 个目录不压缩
    // 可压缩的目录数量 = 总数 - 保留数量
    int dirs_to_compress = count - COMPRESS_THRESHOLD;
    
    for (i = 0; i < dirs_to_compress; i += COMPRESS_THRESHOLD) {
        char tar_file[MAX_PATH_LEN];
        char timestamp[FULL_TIME_LEN];
        int j;
        
        // 生成压缩包时间戳
        get_current_timestamp(timestamp, sizeof(timestamp));
        snprintf(tar_file, sizeof(tar_file), "%s/%s.tar.gz", old_dir, timestamp);
        
        // 构建完整路径数组
        char **paths = malloc(COMPRESS_THRESHOLD * sizeof(char*));
        if (paths == NULL) {
            continue;
        }

        int path_count = 0;
        for (j = 0; j < COMPRESS_THRESHOLD && (i + j) < dirs_to_compress; j++) {
            // 构建完整路径: rubbish_bin/dirs[i+j]
            size_t len = strlen(rubbish_bin) + strlen(dirs[i + j]) + 2;
            paths[j] = malloc(len);
            if (paths[j]) {
                snprintf(paths[j], len, "%s/%s", rubbish_bin, dirs[i + j]);
                path_count++;
            }
        }

        // 使用 tarlib 创建 tar.gz（压缩级别 6，默认）
        int retval = tgz_create(tar_file, (const char **)paths, path_count, 6);
        
        if (retval == 0) {
            // 压缩成功，删除原目录
            for (j = 0; j < path_count; j++) {
                remove_directory_recursive(paths[j]);
            }
            compressed++;
        }

        // 释放路径内存
        for (j = 0; j < path_count; j++) {
            free(paths[j]);
        }
        free(paths);
    }
    
    for (i = 0; i < count; i++) {
        free(dirs[i]);
    }
    free(dirs);
    
    return compressed;
}

// 清理旧压缩包
int cleanup_old_archives(void) {
    DIR *dp;
    struct dirent *entry;
    char *rubbish_bin = get_rubbish_bin_path();
    char old_dir[MAX_PATH_LEN];
    int deleted = 0;
    regex_t regex;
    
    snprintf(old_dir, sizeof(old_dir), "%s/%s", rubbish_bin, OLD_DIR);
    
    dp = opendir(old_dir);
    if (dp == NULL) {
        return 0;
    }
    
    // 编译正则表达式匹配时间戳格式
    regcomp(&regex, "^[0-9]{4}-[0-9]{2}-[0-9]{2}-[0-9]{2}:[0-9]{2}:[0-9]{2}\\.tar\\.gz$", REG_EXTENDED);
    
    while ((entry = readdir(dp)) != NULL) {
        if (regexec(&regex, entry->d_name, 0, NULL, 0) == 0) {
            char file_path[MAX_PATH_LEN];
            struct stat st;
            time_t now;
            double diff;
            
            snprintf(file_path, sizeof(file_path), "%s/%s", old_dir, entry->d_name);
            
            if (stat(file_path, &st) == 0) {
                now = time(NULL);
                diff = difftime(now, st.st_mtime);
                
                if (diff > CLEANUP_DAYS * 24 * 3600) {
                    // 超过60天，删除
                    if (unlink(file_path) == 0) {
                        deleted++;
                    }
                }
            }
        }
    }
    
    regfree(&regex);
    closedir(dp);
    
    return deleted;
}

// 守护进程主函数
void daemon_main(void) {
    struct sigaction sa;
    
    // 设置信号处理
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigusr1;
    sigaction(SIGUSR1, &sa, NULL);
    
    sa.sa_handler = handle_sigterm;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    
    // 忽略SIGCHLD防止僵尸进程
    signal(SIGCHLD, SIG_IGN);
    
    // 写入PID文件
    write_pid_file(getpid());
    
    // 主循环
    while (!sigterm_received) {
        if (sigusr1_received) {
            sigusr1_received = 0;
            
            // 调试日志
            FILE *logfp = fopen("/data/data/com.termux/files/home/.rubbishbin/.daemon.log", "a");
            if (logfp) {
                fprintf(logfp, "[trashd] Received SIGUSR1, starting maintenance\n");
                fclose(logfp);
            }
            
            // 执行维护任务
            int compressed = compress_old_directories();
            int deleted = cleanup_old_archives();
            
            // 记录日志
            openlog("safe_rm_daemon", LOG_PID, LOG_USER);
            syslog(LOG_INFO, "Maintenance: compressed %d groups, deleted %d old archives", 
                   compressed, deleted);
            closelog();
        }
        
        // 睡眠等待信号
        sleep(1);
    }
    
    // 清理
    remove_pid_file();
}

// 启动守护进程
int start_daemon(void) {
    pid_t pid;
    
    // 创建子进程
    pid = fork();
    if (pid == -1) {
        return -1;
    }
    
    if (pid > 0) {
        // 父进程退出
        return 0;
    }
    
    // 子进程
    
    // 创建新会话
    if (setsid() == -1) {
        exit(1);
    }
    
    // 再次fork防止重新获取控制终端
    pid = fork();
    if (pid == -1) {
        exit(1);
    }
    
    if (pid > 0) {
        // 第一个子进程退出
        exit(0);
    }
    
    // 守护进程
    proctitle_init_global();
    proctitle_set("trashd");
    
    // 更改工作目录
    chdir("/");
    
    // 关闭标准文件描述符
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    
    // 守护进程主循环
    daemon_main();
    
    exit(0);
}

// 获取PID文件锁（非阻塞）
// 返回值：锁定的文件描述符，如果获取失败返回 -1
static int lock_pid_file(void) {
    char *pid_file = get_pid_file_path();
    int fd = open(pid_file, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return -1;
    }
    
    // 尝试获取排他锁（非阻塞）
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    
    if (fcntl(fd, F_SETLK, &fl) < 0) {
        // 获取锁失败，可能有其他进程正在启动守护进程
        close(fd);
        return -1;
    }
    
    return fd;
}

// 检查并触发守护进程（带文件锁，确保单例）
int check_and_trigger_daemon(void) {
    // 首先尝试获取文件锁
    int lock_fd = lock_pid_file();
    if (lock_fd < 0) {
        // 获取锁失败，说明有其他进程正在启动守护进程
        // 短暂等待后再次检查
        usleep(100000);  // 100ms
        pid_t pid = read_pid_file();
        if (pid > 0 && is_process_alive(pid)) {
            kill(pid, SIGUSR1);
        }
        return 0;
    }
    
    // 获得锁后，再次检查PID文件（双重检查锁定模式）
    pid_t pid = read_pid_file();
    
    if (pid > 0 && is_process_alive(pid)) {
        // 守护进程已运行，发送SIGUSR1触发维护
        kill(pid, SIGUSR1);
        close(lock_fd);
        return 0;
    }
    
    // 清理失效的PID文件
    if (pid > 0) {
        remove_pid_file();
        // 重新打开文件以获取新的文件描述符
        close(lock_fd);
        lock_fd = lock_pid_file();
        if (lock_fd < 0) {
            return -1;
        }
    }
    
    // 启动新的守护进程
    int result = start_daemon();
    
    // 关闭锁文件描述符
    close(lock_fd);
    
    return result;
}
