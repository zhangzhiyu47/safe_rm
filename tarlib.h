/**
 * tarlib.h - 简单的tar+gzip C语言接口库
 * 兼容Linux/Termux，仅使用标准C库
 */

#ifndef TARLIB_H
#define TARLIB_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 错误码 */
#define TARLIB_OK           0
#define TARLIB_ERROR       -1
#define TARLIB_EOF         -2
#define TARLIB_CRC_ERROR   -3
#define TARLIB_MEM_ERROR   -4
#define TARLIB_IO_ERROR    -5
#define TARLIB_FORMAT_ERR  -6

/* TAR常量 */
#define TAR_BLOCK_SIZE     512
#define TAR_NAME_SIZE      100
#define TAR_PREFIX_SIZE    155
#define TAR_MAX_PATH       256

/* GZIP常量 */
#define GZIP_MAGIC1        0x1f
#define GZIP_MAGIC2        0x8b
#define GZIP_DEFLATE       8

/* 文件类型 */
typedef enum {
    TAR_FILE_REGULAR   = '0',
    TAR_FILE_LINK      = '1',
    TAR_FILE_SYMLINK   = '2',
    TAR_FILE_CHAR      = '3',
    TAR_FILE_BLOCK     = '4',
    TAR_FILE_DIR       = '5',
    TAR_FILE_FIFO      = '6'
} tar_filetype_t;

/* TAR头部结构 - 确保512字节 */
typedef struct {
    char name[TAR_NAME_SIZE];       /* 文件名: 0-99 */
    char mode[8];                   /* 文件权限: 100-107 */
    char uid[8];                    /* 用户ID: 108-115 */
    char gid[8];                    /* 组ID: 116-123 */
    char size[12];                  /* 文件大小: 124-135 */
    char mtime[12];                 /* 修改时间: 136-147 */
    char chksum[8];                 /* 校验和: 148-155 */
    char typeflag;                  /* 文件类型: 156 */
    char linkname[TAR_NAME_SIZE];   /* 链接名: 157-256 */
    char magic[6];                  /* 魔数: 257-262 */
    char version[2];                /* 版本: 263-264 */
    char uname[32];                 /* 用户名: 265-296 */
    char gname[32];                 /* 组名: 297-328 */
    char devmajor[8];               /* 主设备号: 329-336 */
    char devminor[8];               /* 次设备号: 337-344 */
    char prefix[TAR_PREFIX_SIZE];   /* 前缀: 345-499 */
    char padding[12];               /* 填充到512字节: 500-511 */
} tar_header_t;

/* TAR迭代器结构 */
typedef struct {
    FILE *fp;
    tar_header_t header;
    size_t current_size;
    size_t current_pos;
    long data_offset;
    int eof;
} tar_iterator_t;

/* GZIP流结构 */
typedef struct {
    FILE *in;
    FILE *out;
    uint8_t *inbuf;
    uint8_t *outbuf;
    size_t inbuf_size;
    size_t outbuf_size;
    size_t inpos;
    size_t insize;
    size_t outpos;
    uint32_t crc;
    uint32_t total_in;
    uint32_t total_out;
    int level;  /* 压缩级别 0-9 */
} gzip_stream_t;

/* ============== TAR 接口 ============== */

/**
 * 创建新的tar文件
 * @param filename tar文件名
 * @return 成功返回0，失败返回-1
 */
int tar_create(const char *filename);

/**
 * 添加文件到tar
 * @param tarfile tar文件路径
 * @param filepath 要添加的文件路径
 * @param entryname 在tar中的条目名（NULL则使用原文件名）
 * @return 成功返回0，失败返回-1
 */
int tar_add_file(const char *tarfile, const char *filepath, const char *entryname);

/**
 * 添加目录到tar
 * @param tarfile tar文件路径
 * @param dirpath 要添加的目录路径
 * @param entryname 在tar中的条目名
 * @return 成功返回0，失败返回-1
 */
int tar_add_directory(const char *tarfile, const char *dirpath, const char *entryname);

/**
 * 关闭tar文件（添加结束标记）
 * @param tarfile tar文件路径
 * @return 成功返回0，失败返回-1
 */
int tar_close_archive(const char *tarfile);

/**
 * 提取tar文件
 * @param tarfile tar文件路径
 * @param outdir 输出目录
 * @return 成功返回0，失败返回-1
 */
int tar_extract(const char *tarfile, const char *outdir);

/**
 * 列出tar文件内容
 * @param tarfile tar文件路径
 * @param callback 回调函数，为每个条目调用（NULL则直接打印）
 * @param userdata 用户数据
 * @return 成功返回0，失败返回-1
 */
typedef void (*tar_list_callback)(const tar_header_t *header, const char *fullname, void *userdata);
int tar_list(const char *tarfile, tar_list_callback callback, void *userdata);

/**
 * 初始化tar迭代器
 * @param iter 迭代器指针
 * @param tarfile tar文件路径
 * @return 成功返回0，失败返回-1
 */
int tar_iter_init(tar_iterator_t *iter, const char *tarfile);

/**
 * 获取下一个条目
 * @param iter 迭代器指针
 * @return 有条目返回0，结束返回TARLIB_EOF，错误返回TARLIB_ERROR
 */
int tar_iter_next(tar_iterator_t *iter);

/**
 * 提取当前条目到文件
 * @param iter 迭代器指针
 * @param outpath 输出路径
 * @return 成功返回0，失败返回-1
 */
int tar_iter_extract(tar_iterator_t *iter, const char *outpath);

/**
 * 关闭tar迭代器
 * @param iter 迭代器指针
 */
void tar_iter_close(tar_iterator_t *iter);

/**
 * 查找tar中的条目
 * @param tarfile tar文件路径
 * @param entryname 条目名
 * @param header 输出头部信息
 * @param offset 输出数据偏移
 * @return 找到返回0，未找到返回-1
 */
int tar_find_entry(const char *tarfile, const char *entryname, tar_header_t *header, long *offset);

/* ============== GZIP 接口 ============== */

/**
 * 压缩文件为gzip格式
 * @param srcfile 源文件
 * @param dstfile 目标gzip文件
 * @param level 压缩级别0-9（0不压缩，9最大压缩，6默认）
 * @return 成功返回0，失败返回-1
 */
int gzip_compress_file(const char *srcfile, const char *dstfile, int level);

/**
 * 解压gzip文件
 * @param srcfile gzip文件
 * @param dstfile 目标文件
 * @return 成功返回0，失败返回-1
 */
int gzip_decompress_file(const char *srcfile, const char *dstfile);

/**
 * 查看gzip文件信息
 * @param filename gzip文件名
 * @param uncompressed_size 输出解压后大小（可为NULL）
 * @param mtime 输出修改时间（可为NULL）
 * @param filename_in_gzip 输出gzip内文件名（可为NULL，需256字节缓冲区）
 * @return 成功返回0，失败返回-1
 */
int gzip_info(const char *filename, size_t *uncompressed_size, time_t *mtime, char *filename_in_gzip);

/**
 * 压缩内存数据
 * @param src 源数据
 * @param src_len 源数据长度
 * @param dst 目标缓冲区（需预先分配足够空间）
 * @param dst_len 目标缓冲区大小，输出实际压缩大小
 * @param level 压缩级别
 * @return 成功返回0，失败返回-1
 */
int gzip_compress_data(const uint8_t *src, size_t src_len, uint8_t *dst, size_t *dst_len, int level);

/**
 * 解压内存数据
 * @param src gzip数据
 * @param src_len gzip数据长度
 * @param dst 目标缓冲区
 * @param dst_len 目标缓冲区大小，输出实际解压大小
 * @return 成功返回0，失败返回-1
 */
int gzip_decompress_data(const uint8_t *src, size_t src_len, uint8_t *dst, size_t *dst_len);

/* ============== TAR+GZIP 组合接口 ============== */

/**
 * 创建tar.gz文件
 * @param tgzfile 输出的tar.gz文件名
 * @param paths 要打包的路径数组
 * @param count 路径数量
 * @param level 压缩级别
 * @return 成功返回0，失败返回-1
 */
int tgz_create(const char *tgzfile, const char **paths, int count, int level);

/**
 * 解压tar.gz文件
 * @param tgzfile tar.gz文件名
 * @param outdir 输出目录
 * @return 成功返回0，失败返回-1
 */
int tgz_extract(const char *tgzfile, const char *outdir);

/**
 * 列出tar.gz内容
 * @param tgzfile tar.gz文件名
 * @param callback 回调函数
 * @param userdata 用户数据
 * @return 成功返回0，失败返回-1
 */
int tgz_list(const char *tgzfile, tar_list_callback callback, void *userdata);

/* ============== 工具函数 ============== */

/**
 * 将数字转换为八进制字符串
 * @param str 输出字符串
 * @param size 字符串大小
 * @param num 数字
 */
void int_to_octal(char *str, size_t size, unsigned long long num);

/**
 * 解析八进制字符串
 * @param str 八进制字符串
 * @param size 字符串大小
 * @return 转换后的整数
 */
unsigned long long octal_to_int(const char *str, size_t size);

/**
 * 获取错误信息
 * @param errcode 错误码
 * @return 错误描述字符串
 */
const char *tarlib_strerror(int errcode);

/**
 * 计算CRC32
 * @param crc 初始CRC值（0或0xFFFFFFFF）
 * @param data 数据
 * @param len 数据长度
 * @return CRC32值
 */
uint32_t tarlib_crc32(uint32_t crc, const uint8_t *data, size_t len);

/**
 * 计算Adler32
 * @param adler 初始值（1）
 * @param data 数据
 * @param len 数据长度
 * @return Adler32值
 */
uint32_t tarlib_adler32(uint32_t adler, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* TARLIB_H */
