/**
 * @file mv_lib.h
 * @brief 兼容Termux/Linux的mv操作库头文件
 *
 * 提供与Termux/Linux mv命令行为完全一致的文件/目录移动功能。
 * 支持同文件系统快速重命名和跨文件系统的复制删除回退机制。
 *
 * @author Assistant
 * @date 2026-02-24
 */

#ifndef MV_LIB_H
#define MV_LIB_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 移动文件或目录到目标位置
 *
 * 将源文件或目录移动到目标位置。自动判断文件/目录类型，
 * 支持同文件系统rename和跨文件系统复制删除。
 *
 * 行为规则：
 * - 目标为目录时，源移动到目录内
 * - 目标不存在时，源重命名为目标名
 * - 同一文件系统内：使用rename()系统调用，原子操作
 * - 跨文件系统：自动回退到复制+删除模式
 * - 符号链接：操作链接本身，而非链接目标
 * - 目录：支持递归移动，包括跨文件系统
 *
 * @param source 源文件或目录路径
 * @param dest   目标路径（可以是目录或新名称）
 * @return       成功返回0，失败返回-1并设置errno
 *
 * @note 错误时errno值：
 *       - ENOENT: 源文件不存在
 *       - EACCES: 权限不足
 *       - ENOTDIR: 路径组件不是目录
 *       - EISDIR: 目标存在且为目录，但源不是目录
 *       - ENOTEMPTY: 目标目录非空
 *       - ENOSPC: 目标文件系统空间不足
 *       - EBUSY: 文件或目录正在被使用
 *       - EINVAL: 源和目标相同
 */
int mv(const char *source, const char *dest);

/**
 * @brief 批量移动多个文件/目录到目标目录
 *
 * 将多个源文件/目录移动到目标目录中。变参列表以NULL结尾。
 *
 * @param dest    目标目录路径
 * @param source  第一个源文件/目录路径
 * @param ...     更多源路径，必须以NULL结尾
 * @return        全部成功返回0，任一失败返回-1并设置errno
 *
 * @note 如果任一移动失败，不会回滚已成功的操作
 * @note 目标必须是已存在的目录
 */
int mv2(const char *dest, const char *source, ...);

/**
 * @brief 通过文件描述符移动文件或目录到目标位置
 *
 * 功能同mv()，但使用打开的文件描述符。源fd对应的文件会被移动，
 * fd本身不会被关闭。
 *
 * @param source_fd 源文件描述符（已打开的文件或目录）
 * @param dest      目标路径
 * @return          成功返回0，失败返回-1并设置errno
 */
int mvfd(int source_fd, const char *dest);

/**
 * @brief 通过文件描述符批量移动多个文件/目录到目标目录
 *
 * 功能同mv2()，但使用打开的文件描述符数组。
 *
 * @param dest_fd   目标目录文件描述符
 * @param fds       源文件描述符数组
 * @param count     源文件描述符数量
 * @return          全部成功返回0，任一失败返回-1并设置errno
 */
int mvfd2(int dest_fd, int *fds, size_t count);

#ifdef __cplusplus
}
#endif

#endif // MV_LIB_H
