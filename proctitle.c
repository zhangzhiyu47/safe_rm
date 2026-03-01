#define _GNU_SOURCE
#include "proctitle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>

/* 状态结构 */
static struct {
    int initialized;
    int available;
    pthread_mutex_t lock;      /* 保护 set 操作 */
    
    /* 内存布局信息 */
    char **argv;
    int argc;
    char *base;                /* argv[0] 起始地址 */
    size_t maxlen;             /* argv + environ 总长度 */
    
    /* 环境变量备份 */
    char **heap_environ;       /* 堆上的 environ 副本 */
    int env_count;             /* 环境变量数量 */
} g_pt = {
    .initialized = 0,
    .available = 0,
    .lock = PTHREAD_MUTEX_INITIALIZER
};

extern char **environ;  /* 全局环境变量指针 */
static volatile int g_main_argc = 0;
static volatile char **g_main_argv = NULL;

int proctitle_init(int argc, char **argv) {
    if (g_pt.initialized) {
        errno = EBUSY;  /* 已初始化 */
        return -1;
    }
    
    if (argc <= 0 || !argv || !argv[0]) {
        errno = EINVAL;
        return -1;
    }
    
    g_pt.argc = argc;
    g_pt.argv = argv;
    g_pt.base = argv[0];
    
    /* 计算 argv 占用的空间 */
    char *ptr = argv[0];
    for (int i = 0; i < argc; i++) {
        if (argv[i]) {
            size_t len = strlen(argv[i]) + 1;
            ptr = argv[i] + len;
        }
    }
    
    /* 检查 environ 是否存在且连续 */
    size_t argv_space = ptr - g_pt.base;
    g_pt.maxlen = argv_space;  /* 默认最少有 argv 空间 */
    g_pt.available = (argv_space > 0);
    
    if (environ && environ[0]) {
        /* 检查连续性：environ[0] 应该在 ptr 附近（±1页内） */
        ptrdiff_t gap = environ[0] - ptr;
        
        if (gap >= 0 && gap < 4096) {
            /* 连续，可以合并使用 */
            char **env;
            for (env = environ; *env; env++) {
                ptr = *env + strlen(*env) + 1;
            }
            g_pt.maxlen = ptr - g_pt.base;
            g_pt.env_count = env - environ;
            
            /* 备份环境变量到堆（关键：必须复制字符串内容） */
            g_pt.heap_environ = malloc((g_pt.env_count + 1) * sizeof(char *));
            if (!g_pt.heap_environ) {
                errno = ENOMEM;
                return -1;
            }
            
            for (int i = 0; i < g_pt.env_count; i++) {
                g_pt.heap_environ[i] = strdup(environ[i]);
                if (!g_pt.heap_environ[i]) {
                    /* 清理已分配内存 */
                    while (i-- > 0) free(g_pt.heap_environ[i]);
                    free(g_pt.heap_environ);
                    g_pt.heap_environ = NULL;
                    errno = ENOMEM;
                    return -1;
                }
            }
            g_pt.heap_environ[g_pt.env_count] = NULL;
            
            /* 原子替换 environ 指针（此后 getenv/setenv 使用堆内存） */
            environ = g_pt.heap_environ;
        }
        /* 如果不连续，gap 太大，则只使用 argv 空间，不移动 environ */
    }
    
    g_pt.initialized = 1;
    return 0;
}

int proctitle_set(const char *fmt, ...) {
    if (!g_pt.initialized) {
        errno = EINVAL;
        return -1;
    }
    
    if (!g_pt.available || !fmt) {
        errno = ENOTSUP;
        return -1;
    }
    
    /* 格式化字符串（使用栈缓冲区避免 malloc） */
    char buf[4096];
    va_list ap;
    
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    
    if (len < 0) {
        errno = EINVAL;
        return -1;
    }
    
    /* 线程安全：加锁修改内存 */
    pthread_mutex_lock(&g_pt.lock);
    
    /* 截断到可用空间（保留1字节给 \0） */
    size_t copy_len = (size_t)len < g_pt.maxlen ? (size_t)len : g_pt.maxlen - 1;
    
    /* 安全清零：使用 volatile 防止编译器优化掉 */
    volatile char *p = g_pt.base;
    for (size_t i = 0; i < g_pt.maxlen; i++) {
        p[i] = '\0';
    }
    
    /* 写入新内容 */
    if (copy_len > 0) {
        memcpy(g_pt.base, buf, copy_len);
    }
    
    /* 调整 argv 指针：让 ps 只看到一个参数（新标题） */
    /* 注意：argv[0] 指向 base，其他指向末尾的空字符串 */
    g_pt.argv[0] = g_pt.base;
    for (int i = 1; i < g_pt.argc; i++) {
        if (g_pt.argv[i]) {
            g_pt.argv[i] = g_pt.base + copy_len;  /* 指向 \0 */
        }
    }
    
    pthread_mutex_unlock(&g_pt.lock);
    return (int)copy_len;
}

void save_main_args(int argc, char **argv) {
    g_main_argc = argc;
    g_main_argv = (volatile char**)argv;
}

/* 子进程初始化接口 */
int proctitle_init_global(void) {
    if (!g_main_argv) return -1;  /* 未保存 */
    return proctitle_init(g_main_argc, (char**)g_main_argv);
}

size_t proctitle_get_maxlen(void) {
    return g_pt.initialized ? g_pt.maxlen : 0;
}

int proctitle_is_available(void) {
    return g_pt.initialized && g_pt.available;
}

/* 清理函数（可选，进程退出时调用） */
__attribute__((destructor))
static void proctitle_destroy(void) {
    if (g_pt.initialized) {
        pthread_mutex_destroy(&g_pt.lock);
        
        /* 注意：不能释放 heap_environ，因为程序可能还在使用环境变量 */
        /* 如果一定要释放，需要确保所有线程都已退出 */
    }
}
