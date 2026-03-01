/**
 * @file mv_lib.c
 * @brief 兼容Termux/Linux的mv操作库实现
 *
 * 实现与Termux/Linux mv命令行为完全一致的文件/目录移动功能。
 * 核心逻辑：优先使用rename()，跨文件系统时回退到复制+删除。
 *
 * @author Assistant
 * @date 2026-02-24
 */

#define _GNU_SOURCE
#include "mv_lib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <dirent.h>
#include <utime.h>
#include <limits.h>
#include <stdarg.h>

// 路径最大长度
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/**
 * @brief 清除路径末尾的斜杠（保留根目录/）
 * @param path 路径字符串（会被修改）
 */
static void strip_trailing_slashes(char *path)
{
    if (!path || !*path) {
        return;
    }
    char *last = path + strlen(path) - 1;
    while (last > path && *last == '/') {
        *last-- = '\0';
    }
}

/**
 * @brief 获取路径的最后组成部分（文件名）
 * @param path 文件路径
 * @return 指向文件名的指针
 */
static const char *last_component(const char *path)
{
    if (!path || !*path) {
        return ".";
    }
    const char *last = strrchr(path, '/');
    if (last) {
        return last + 1;
    }
    return path;
}

/**
 * @brief 连接目录和文件名
 * @param dir 目录路径
 * @param file 文件名
 * @return 新分配的路径字符串，需调用者释放，失败返回NULL
 */
static char *file_name_concat(const char *dir, const char *file)
{
    if (!dir || !file) {
        errno = EINVAL;
        return NULL;
    }
    size_t dir_len = strlen(dir);
    size_t file_len = strlen(file);
    char *result = malloc(dir_len + file_len + 2);
    if (!result) {
        errno = ENOMEM;
        return NULL;
    }
    strcpy(result, dir);
    if (dir_len > 0 && result[dir_len - 1] != '/') {
        result[dir_len] = '/';
        result[dir_len + 1] = '\0';
        dir_len++;
    }
    strcpy(result + dir_len, file);
    return result;
}

/**
 * @brief 复制文件元数据（权限、时间戳）
 * @param dest 目标文件路径
 * @param sb   源文件的stat信息
 * @return     成功返回0，失败返回-1
 */
static int copy_metadata(const char *dest, const struct stat *sb)
{
    // 复制权限
    chmod(dest, sb->st_mode);
    // 复制所有权（可能失败，忽略）
    chown(dest, sb->st_uid, sb->st_gid);
    // 复制时间戳
    struct utimbuf times;
    times.actime = sb->st_atime;
    times.modtime = sb->st_mtime;
    utime(dest, &times);
    return 0;
}

/**
 * @brief 复制文件数据（使用sendfile优化）
 * @param source_fd 源文件描述符
 * @param dest_fd   目标文件描述符
 * @param size      文件大小
 * @return          成功返回0，失败返回-1
 */
static int copy_file_data(int source_fd, int dest_fd, off_t size)
{
    off_t copied = 0;
    while (copied < size) {
        ssize_t n = sendfile(dest_fd, source_fd, &copied, size - copied);
        if (n < 0) {
            // sendfile失败，回退到read/write
            char buffer[8192];
            ssize_t bytes_read, bytes_written;
            lseek(source_fd, copied, SEEK_SET);
            lseek(dest_fd, copied, SEEK_SET);
            while ((bytes_read = read(source_fd, buffer, sizeof(buffer))) > 0) {
                char *ptr = buffer;
                while (bytes_read > 0) {
                    bytes_written = write(dest_fd, ptr, bytes_read);
                    if (bytes_written < 0) {
                        return -1;
                    }
                    bytes_read -= bytes_written;
                    ptr += bytes_written;
                    copied += bytes_written;
                }
            }
            if (bytes_read < 0) {
                return -1;
            }
            return 0;
        }
    }
    return 0;
}

/**
 * @brief 跨文件系统复制文件
 * @param source 源文件路径
 * @param dest   目标文件路径
 * @param sb     源文件的stat信息
 * @return       成功返回0，失败返回-1
 */
static int copy_file_cross_fs(const char *source, const char *dest,
                               const struct stat *sb)
{
    int source_fd = open(source, O_RDONLY);
    if (source_fd < 0) {
        return -1;
    }
    int dest_fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, sb->st_mode);
    if (dest_fd < 0) {
        close(source_fd);
        return -1;
    }
    int result = copy_file_data(source_fd, dest_fd, sb->st_size);
    if (result == 0) {
        fsync(dest_fd);
        copy_metadata(dest, sb);
    }
    close(source_fd);
    close(dest_fd);
    if (result < 0) {
        unlink(dest);
    }
    return result;
}

/**
 * @brief 递归复制目录（跨文件系统移动时使用）
 * @param source 源目录路径
 * @param dest   目标目录路径
 * @return       成功返回0，失败返回-1
 */
static int copy_directory_recursive(const char *source, const char *dest)
{
    struct stat sb;
    if (stat(source, &sb) < 0) {
        return -1;
    }
    if (mkdir(dest, sb.st_mode) < 0) {
        if (errno != EEXIST) {
            return -1;
        }
    }
    DIR *dir = opendir(source);
    if (!dir) {
        return -1;
    }
    struct dirent *entry;
    int result = 0;
    while ((entry = readdir(dir)) != NULL && result == 0) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char *source_path = file_name_concat(source, entry->d_name);
        char *dest_path = file_name_concat(dest, entry->d_name);
        if (!source_path || !dest_path) {
            free(source_path);
            free(dest_path);
            errno = ENOMEM;
            result = -1;
            break;
        }
        struct stat entry_sb;
        if (lstat(source_path, &entry_sb) < 0) {
            free(source_path);
            free(dest_path);
            result = -1;
            break;
        }
        if (S_ISDIR(entry_sb.st_mode)) {
            result = copy_directory_recursive(source_path, dest_path);
        } else if (S_ISLNK(entry_sb.st_mode)) {
            char link_target[PATH_MAX];
            ssize_t len = readlink(source_path, link_target, sizeof(link_target) - 1);
            if (len < 0) {
                result = -1;
            } else {
                link_target[len] = '\0';
                result = symlink(link_target, dest_path);
            }
        } else {
            result = copy_file_cross_fs(source_path, dest_path, &entry_sb);
        }
        free(source_path);
        free(dest_path);
    }
    closedir(dir);
    if (result == 0) {
        copy_metadata(dest, &sb);
    }
    return result;
}

/**
 * @brief 递归删除目录
 * @param path 要删除的目录路径
 * @return     成功返回0，失败返回-1
 */
static int remove_directory_recursive(const char *path)
{
    struct stat sb;
    if (lstat(path, &sb) < 0) {
        return -1;
    }
    if (!S_ISDIR(sb.st_mode)) {
        return unlink(path);
    }
    DIR *dir = opendir(path);
    if (!dir) {
        return -1;
    }
    struct dirent *entry;
    int result = 0;
    while ((entry = readdir(dir)) != NULL && result == 0) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char *entry_path = file_name_concat(path, entry->d_name);
        if (!entry_path) {
            errno = ENOMEM;
            result = -1;
            break;
        }
        if (lstat(entry_path, &sb) < 0) {
            free(entry_path);
            result = -1;
            break;
        }
        if (S_ISDIR(sb.st_mode)) {
            result = remove_directory_recursive(entry_path);
        } else {
            result = unlink(entry_path);
        }
        free(entry_path);
    }
    closedir(dir);
    if (result == 0) {
        result = rmdir(path);
    }
    return result;
}

/**
 * @brief 跨文件系统移动（复制+删除）
 * @param source 源路径
 * @param dest   目标路径
 * @param sb     源文件stat信息
 * @return       成功返回0，失败返回-1
 */
static int move_cross_fs(const char *source, const char *dest,
                         const struct stat *sb)
{
    if (S_ISDIR(sb->st_mode)) {
        // 移动目录
        if (copy_directory_recursive(source, dest) < 0) {
            return -1;
        }
        remove_directory_recursive(source);
        return 0;
    } else if (S_ISLNK(sb->st_mode)) {
        // 移动符号链接
        char link_target[PATH_MAX];
        ssize_t len = readlink(source, link_target, sizeof(link_target) - 1);
        if (len < 0) {
            return -1;
        }
        link_target[len] = '\0';
        if (symlink(link_target, dest) < 0) {
            return -1;
        }
        copy_metadata(dest, sb);
        unlink(source);
        return 0;
    } else {
        // 移动普通文件
        if (copy_file_cross_fs(source, dest, sb) < 0) {
            return -1;
        }
        unlink(source);
        return 0;
    }
}

/**
 * @brief 检查目标目录是否为空
 * @param path 目录路径
 * @return     空返回1，非空返回0，出错返回-1
 */
static int is_dir_empty(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) {
        return -1;
    }
    struct dirent *entry;
    int empty = 1;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            empty = 0;
            break;
        }
    }
    closedir(dir);
    return empty;
}

/**
 * @brief 核心移动函数
 * @param source  源路径
 * @param dest    目标路径
 * @return        成功返回0，失败返回-1
 */
static int do_move(const char *source, const char *dest)
{
    struct stat source_sb, dest_sb;
    char *actual_dest = NULL;
    int result = -1;

    // 检查源文件是否存在
    if (lstat(source, &source_sb) < 0) {
        return -1;
    }

    // 检查源是否是.或..
    const char *src_base = last_component(source);
    if (strcmp(src_base, ".") == 0 || strcmp(src_base, "..") == 0) {
        errno = EINVAL;
        return -1;
    }

    // 检查目标是否存在
    if (lstat(dest, &dest_sb) == 0) {
        // 如果目标是目录，构建完整目标路径
        if (S_ISDIR(dest_sb.st_mode)) {
            actual_dest = file_name_concat(dest, last_component(source));
            if (!actual_dest) {
                errno = ENOMEM;
                return -1;
            }
            // 检查新目标是否存在
            if (lstat(actual_dest, &dest_sb) == 0) {
                // 检查是否是同一文件（硬链接）
                if (source_sb.st_ino == dest_sb.st_ino &&
                    source_sb.st_dev == dest_sb.st_dev) {
                    free(actual_dest);
                    errno = EINVAL;
                    return -1;
                }
                // 如果都是目录，检查目标是否为空
                if (S_ISDIR(source_sb.st_mode) && S_ISDIR(dest_sb.st_mode)) {
                    if (is_dir_empty(actual_dest) != 1) {
                        free(actual_dest);
                        errno = ENOTEMPTY;
                        return -1;
                    }
                }
            }
            dest = actual_dest;
        } else {
            // 检查是否是同一文件（硬链接）
            if (source_sb.st_ino == dest_sb.st_ino &&
                source_sb.st_dev == dest_sb.st_dev) {
                errno = EINVAL;
                return -1;
            }
            // 如果目标是目录且源不是目录，报错
            if (S_ISDIR(dest_sb.st_mode) && !S_ISDIR(source_sb.st_mode)) {
                errno = EISDIR;
                return -1;
            }
            // 如果源是目录且目标不是目录，报错
            if (S_ISDIR(source_sb.st_mode) && !S_ISDIR(dest_sb.st_mode)) {
                errno = ENOTDIR;
                return -1;
            }
        }
    }

    // 尝试使用rename（同文件系统）
    if (rename(source, dest) == 0) {
        free(actual_dest);
        return 0;
    }

    // rename失败，检查是否是跨文件系统错误
    if (errno != EXDEV) {
        free(actual_dest);
        return -1;
    }

    // 跨文件系统，使用复制+删除
    result = move_cross_fs(source, dest, &source_sb);
    free(actual_dest);
    return result;
}

// ==================== 公共接口实现 ====================

int mv(const char *source, const char *dest)
{
    if (!source || !dest) {
        errno = EINVAL;
        return -1;
    }
    char *source_copy = strdup(source);
    char *dest_copy = strdup(dest);
    if (!source_copy || !dest_copy) {
        free(source_copy);
        free(dest_copy);
        errno = ENOMEM;
        return -1;
    }
    strip_trailing_slashes(source_copy);
    strip_trailing_slashes(dest_copy);
    int result = do_move(source_copy, dest_copy);
    free(source_copy);
    free(dest_copy);
    return result;
}

int mv2(const char *dest, const char *source, ...)
{
    if (!dest || !source) {
        errno = EINVAL;
        return -1;
    }
    // 检查目标是否是目录
    struct stat sb;
    if (stat(dest, &sb) < 0) {
        return -1;
    }
    if (!S_ISDIR(sb.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    int overall_result = 0;
    // 移动第一个源
    if (mv(source, dest) < 0) {
        overall_result = -1;
    }
    // 移动变参列表中的源
    va_list args;
    va_start(args, source);
    const char *src;
    while ((src = va_arg(args, const char *)) != NULL) {
        if (mv(src, dest) < 0) {
            overall_result = -1;
        }
    }
    va_end(args);
    return overall_result;
}

int mvfd(int source_fd, const char *dest)
{
    if (source_fd < 0 || !dest) {
        errno = EINVAL;
        return -1;
    }
    // 获取源文件路径
    char source_path[PATH_MAX];
    char fd_path[64];
    snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", source_fd);
    ssize_t len = readlink(fd_path, source_path, sizeof(source_path) - 1);
    if (len < 0) {
        return -1;
    }
    source_path[len] = '\0';
    return mv(source_path, dest);
}

int mvfd2(int dest_fd, int *fds, size_t count)
{
    if (dest_fd < 0 || !fds || count == 0) {
        errno = EINVAL;
        return -1;
    }
    // 获取目标目录路径
    char dest_path[PATH_MAX];
    char fd_path[64];
    snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", dest_fd);
    ssize_t len = readlink(fd_path, dest_path, sizeof(dest_path) - 1);
    if (len < 0) {
        return -1;
    }
    dest_path[len] = '\0';
    int overall_result = 0;
    for (size_t i = 0; i < count; i++) {
        if (mvfd(fds[i], dest_path) < 0) {
            overall_result = -1;
        }
    }
    return overall_result;
}
