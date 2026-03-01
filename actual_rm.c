#define _XOPEN_SOURCE 500
#include <ftw.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

static int unlink_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
    (void)sb;
    (void)ftwbuf;
    int rv;

    /* 根据类型选择删除方式：
       FTW_DP: 后序遍历中的目录（子项已处理完毕）
       FTW_F:  普通文件
       FTW_SL: 符号链接（不跟随）
       FTW_SLN: 断开的符号链接 */
    if (typeflag == FTW_DP) {
        rv = rmdir(fpath);
    } else if (typeflag == FTW_D) {
        // 前序遍历中的目录（有 FTW_DEPTH 时不应出现，但保险起见）
        return 0;
    } else {
        rv = unlink(fpath);
    }

    if (rv == -1) {
        // 文件删除失败可能是并发操作导致，忽略并继续
        perror("Error: unlink");
        return 0;
    }
    return 0;
}

/**
 * redel3 - 递归删除多个文件或目录
 * 
 * @argv: 以 NULL 结尾的路径数组（类似 execv 格式）
 *
 * @retval
 *   >=0: 成功删除的顶层路径数量（仅统计 argv 中的项，不包括子文件/目录）
 *   -1:  发生错误，errno 被设置
 * 
 * 说明:
 *   - 如果 argv 中某项不存在（ENOENT），则跳过该项，不计入成功数，也不视为失败
 *   - 使用 FTW_PHYS 不跟随符号链接，防止误删链接指向的目标
 *   - 线程不安全（依赖全局变量传递错误状态）
 */
int redel3(char *const argv[])
{
    int count = 0;
    int i = 0;

    if (argv == NULL) {
        errno = EINVAL;
        return -1;
    }

    while (argv[i] != NULL) {
        const char *path = argv[i];
        struct stat st;

        // 检查路径是否存在，不存在则跳过
        if (stat(path, &st) == -1) {
            perror("Error: stat");
            i++;
            continue;  // 跳过无法删除的文件
        }

        /* 
         * FTW_DEPTH: 后序遍历（先删文件，再删目录）
         * FTW_PHYS:  不跟随符号链接（防止删除链接指向的真实文件）
         * 64:        最大打开文件描述符数（nftw 会尝试保持这么多目录打开）
         */
        if (nftw(path, unlink_cb, 64, FTW_DEPTH | FTW_PHYS) == -1) {
            perror("Error: nftw");
            i++;
            continue;
        }

        count++;
        i++;
    }

    return count;
}
