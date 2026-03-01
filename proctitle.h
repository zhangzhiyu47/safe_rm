#ifndef PROCTITLE_H
#define PROCTITLE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void save_main_args(int argc, char **argv);
int  proctitle_init_global(void);

/**
 * 初始化进程标题修改功能
 * 警告：必须在 main() 开头、任何线程创建前调用，且仅调用一次
 * 
 * @param argc main 的 argc
 * @param argv main 的 argv
 * @return 0 成功，-1 失败（errno 设置：EINVAL/ENOMEM）
 */
int proctitle_init(int argc, char **argv);

/**
 * 线程安全地设置进程标题
 * 
 * @param fmt printf 格式字符串
 * @return 实际写入字节数，-1 失败
 * 
 * 注意：如果长度超过可用空间，自动截断，不会失败
 */
int proctitle_set(const char *fmt, ...);

/**
 * 获取最大可用字节数（含结尾 \0）
 */
size_t proctitle_get_maxlen(void);

/**
 * 检查是否支持修改（Linux/Termux 通常支持）
 */
int proctitle_is_available(void);

#ifdef __cplusplus
}
#endif

#endif
