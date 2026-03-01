/**
 * tarlib.c - tar+gzip库实现
 * 仅使用标准C库
 */

#include "tarlib.h"
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>

/* ============== CRC32 表 ============== */

static const uint32_t crc32_table[256] = {
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
    0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
    0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
    0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
    0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
    0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
    0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
    0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
    0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
    0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106,
    0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
    0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
    0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
    0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
    0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
    0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
    0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
    0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
    0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
    0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
    0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
    0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
    0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
    0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
    0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
    0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
    0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
    0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
    0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
    0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
    0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
    0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
    0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
    0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
    0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
    0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
    0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
    0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
    0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
    0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
    0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
    0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
};

uint32_t tarlib_crc32(uint32_t crc, const uint8_t *data, size_t len) {
    crc = ~crc;
    while (len--) {
        crc = (crc >> 8) ^ crc32_table[(crc ^ *data++) & 0xff];
    }
    return ~crc;
}

uint32_t tarlib_adler32(uint32_t adler, const uint8_t *data, size_t len) {
    const uint32_t MOD_ADLER = 65521;
    uint32_t a = adler & 0xffff;
    uint32_t b = (adler >> 16) & 0xffff;
    
    while (len > 0) {
        size_t tlen = len > 5552 ? 5552 : len;
        len -= tlen;
        do {
            a += *data++;
            b += a;
        } while (--tlen);
        a %= MOD_ADLER;
        b %= MOD_ADLER;
    }
    
    return (b << 16) | a;
}

/* ============== 内部工具函数 ============== */

/* 计算TAR头部校验和 */
static unsigned int calc_checksum(const tar_header_t *header) {
    const unsigned char *p = (const unsigned char *)header;
    unsigned int sum = 0;
    int i;
    
    for (i = 0; i < 512; i++) {
        if (i >= 148 && i < 156) {
            sum += ' ';
        } else {
            sum += p[i];
        }
    }
    return sum;
}

/* 将数字转换为八进制字符串 */
void int_to_octal(char *str, size_t size, unsigned long long num) {
    int i = (int)size - 2;
    str[size - 1] = '\0';
    
    while (i >= 0) {
        str[i] = '0' + (num & 7);
        num >>= 3;
        i--;
    }
}

/* 解析八进制字符串 */
unsigned long long octal_to_int(const char *str, size_t size) {
    unsigned long long result = 0;
    size_t i;
    
    for (i = 0; i < size && str[i]; i++) {
        if (str[i] >= '0' && str[i] <= '7') {
            result = result * 8 + (str[i] - '0');
        }
    }
    return result;
}

/* 创建tar头部 */
static void create_header(tar_header_t *header, const char *name, 
                         struct stat *st, char typeflag) {
    memset(header, 0, sizeof(tar_header_t));
    
    strncpy(header->name, name, TAR_NAME_SIZE);
    
    int_to_octal(header->mode, sizeof(header->mode), st->st_mode & 0777);
    int_to_octal(header->uid, sizeof(header->uid), st->st_uid);
    int_to_octal(header->gid, sizeof(header->gid), st->st_gid);
    int_to_octal(header->size, sizeof(header->size), 
                 typeflag == '0' ? st->st_size : 0);
    int_to_octal(header->mtime, sizeof(header->mtime), st->st_mtime);
    
    header->typeflag = typeflag;
    
    memcpy(header->magic, "ustar\0", 6);
    memcpy(header->version, "00", 2);
    
    unsigned int chksum = calc_checksum(header);
    int_to_octal(header->chksum, sizeof(header->chksum), chksum);
}

/* 写入填充块 */
static int write_padding(FILE *fp, size_t size) {
    size_t pad = (TAR_BLOCK_SIZE - (size % TAR_BLOCK_SIZE)) % TAR_BLOCK_SIZE;
    if (pad > 0) {
        char buf[TAR_BLOCK_SIZE] = {0};
        if (fwrite(buf, 1, pad, fp) != pad) {
            return TARLIB_IO_ERROR;
        }
    }
    return TARLIB_OK;
}

/* 确保目录存在 */
static int ensure_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return 0;
        }
        return -1;
    }
    
    char parent[512];
    strncpy(parent, path, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    
    char *p = strrchr(parent, '/');
    if (p && p != parent) {
        *p = '\0';
        if (ensure_directory(parent) < 0) {
            return -1;
        }
    }
    
    return mkdir(path, 0755);
}


/* ============== TAR 实现 ============== */

int tar_create(const char *filename) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        return TARLIB_IO_ERROR;
    }
    fclose(fp);
    return TARLIB_OK;
}

int tar_add_file(const char *tarfile, const char *filepath, const char *entryname) {
    struct stat st;
    if (stat(filepath, &st) < 0) {
        return TARLIB_IO_ERROR;
    }
    
    if (!S_ISREG(st.st_mode)) {
        return TARLIB_ERROR;
    }
    
    FILE *src = fopen(filepath, "rb");
    if (!src) {
        return TARLIB_IO_ERROR;
    }
    
    FILE *dst = fopen(tarfile, "ab");
    if (!dst) {
        fclose(src);
        return TARLIB_IO_ERROR;
    }
    
    tar_header_t header;
    const char *name = entryname ? entryname : filepath;
    create_header(&header, name, &st, '0');
    
    if (fwrite(&header, 1, sizeof(header), dst) != sizeof(header)) {
        fclose(src);
        fclose(dst);
        return TARLIB_IO_ERROR;
    }
    
    char buf[TAR_BLOCK_SIZE];
    size_t n;
    size_t total = 0;
    
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) {
            fclose(src);
            fclose(dst);
            return TARLIB_IO_ERROR;
        }
        total += n;
    }
    
    if (write_padding(dst, total) != TARLIB_OK) {
        fclose(src);
        fclose(dst);
        return TARLIB_IO_ERROR;
    }
    
    fclose(src);
    fclose(dst);
    return TARLIB_OK;
}

int tar_add_directory(const char *tarfile, const char *dirpath, const char *entryname) {
    struct stat st;
    if (stat(dirpath, &st) < 0) {
        return TARLIB_IO_ERROR;
    }
    
    if (!S_ISDIR(st.st_mode)) {
        return TARLIB_ERROR;
    }
    
    FILE *fp = fopen(tarfile, "ab");
    if (!fp) {
        return TARLIB_IO_ERROR;
    }
    
    tar_header_t header;
    const char *name = entryname ? entryname : dirpath;
    
    char dirname[256];
    strncpy(dirname, name, sizeof(dirname) - 2);
    dirname[sizeof(dirname) - 2] = '\0';
    size_t len = strlen(dirname);
    if (len > 0 && dirname[len - 1] != '/') {
        dirname[len] = '/';
        dirname[len + 1] = '\0';
    }
    
    create_header(&header, dirname, &st, '5');
    
    if (fwrite(&header, 1, sizeof(header), fp) != sizeof(header)) {
        fclose(fp);
        return TARLIB_IO_ERROR;
    }
    
    fclose(fp);
    return TARLIB_OK;
}

int tar_close_archive(const char *tarfile) {
    FILE *fp = fopen(tarfile, "ab");
    if (!fp) {
        return TARLIB_IO_ERROR;
    }
    
    char buf[TAR_BLOCK_SIZE * 2] = {0};
    if (fwrite(buf, 1, sizeof(buf), fp) != sizeof(buf)) {
        fclose(fp);
        return TARLIB_IO_ERROR;
    }
    
    fclose(fp);
    return TARLIB_OK;
}

int tar_extract(const char *tarfile, const char *outdir) {
    FILE *fp = fopen(tarfile, "rb");
    if (!fp) {
        return TARLIB_IO_ERROR;
    }
    
    if (ensure_directory(outdir) < 0 && errno != EEXIST) {
        fclose(fp);
        return TARLIB_IO_ERROR;
    }
    
    tar_header_t header;
    char fullpath[512];
    int result = TARLIB_OK;
    
    while (fread(&header, 1, sizeof(header), fp) == sizeof(header)) {
        int empty = 1;
        int i;
        for (i = 0; i < (int)sizeof(header); i++) {
            if (((char *)&header)[i] != 0) {
                empty = 0;
                break;
            }
        }
        if (empty) {
            break;
        }
        
        unsigned int chksum = (unsigned int)octal_to_int(header.chksum, sizeof(header.chksum));
        unsigned int calc = calc_checksum(&header);
        if (chksum != calc) {
            result = TARLIB_FORMAT_ERR;
            break;
        }
        
        size_t size = (size_t)octal_to_int(header.size, sizeof(header.size));
        
        snprintf(fullpath, sizeof(fullpath), "%s/%s", outdir, header.name);
        
        switch (header.typeflag) {
            case '0':
            case '\0': {
                char *p = strrchr(fullpath, '/');
                if (p) {
                    *p = '\0';
                    ensure_directory(fullpath);
                    *p = '/';
                }
                
                FILE *out = fopen(fullpath, "wb");
                if (!out) {
                    result = TARLIB_IO_ERROR;
                    goto cleanup;
                }
                
                char buf[TAR_BLOCK_SIZE];
                size_t remaining = size;
                while (remaining > 0) {
                    size_t to_read = remaining < sizeof(buf) ? remaining : sizeof(buf);
                    size_t n = fread(buf, 1, to_read, fp);
                    if (n != to_read) {
                        fclose(out);
                        result = TARLIB_IO_ERROR;
                        goto cleanup;
                    }
                    if (fwrite(buf, 1, n, out) != n) {
                        fclose(out);
                        result = TARLIB_IO_ERROR;
                        goto cleanup;
                    }
                    remaining -= n;
                }
                fclose(out);
                
                size_t pad = (TAR_BLOCK_SIZE - (size % TAR_BLOCK_SIZE)) % TAR_BLOCK_SIZE;
                if (pad > 0) {
                    fseek(fp, pad, SEEK_CUR);
                }
                
                mode_t mode = (mode_t)octal_to_int(header.mode, sizeof(header.mode));
                chmod(fullpath, mode);
                break;
            }
            
            case '5':
                ensure_directory(fullpath);
                break;
                
            case '2':
                symlink(header.linkname, fullpath);
                break;
                
            default:
                fseek(fp, ((size + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE) * TAR_BLOCK_SIZE, SEEK_CUR);
                break;
        }
    }
    
cleanup:
    fclose(fp);
    return result;
}

int tar_list(const char *tarfile, tar_list_callback callback, void *userdata) {
    FILE *fp = fopen(tarfile, "rb");
    if (!fp) {
        return TARLIB_IO_ERROR;
    }
    
    tar_header_t header;
    int result = TARLIB_OK;
    
    while (fread(&header, 1, sizeof(header), fp) == sizeof(header)) {
        int empty = 1;
        int i;
        for (i = 0; i < (int)sizeof(header); i++) {
            if (((char *)&header)[i] != 0) {
                empty = 0;
                break;
            }
        }
        if (empty) {
            break;
        }
        
        unsigned int chksum = (unsigned int)octal_to_int(header.chksum, sizeof(header.chksum));
        unsigned int calc = calc_checksum(&header);
        if (chksum != calc) {
            result = TARLIB_FORMAT_ERR;
            break;
        }
        
        size_t size = (size_t)octal_to_int(header.size, sizeof(header.size));
        
        char fullname[TAR_MAX_PATH];
        if (header.prefix[0]) {
            snprintf(fullname, sizeof(fullname), "%s/%s", header.prefix, header.name);
        } else {
            strncpy(fullname, header.name, sizeof(fullname) - 1);
            fullname[sizeof(fullname) - 1] = '\0';
        }
        
        if (callback) {
            callback(&header, fullname, userdata);
        } else {
            mode_t mode = (mode_t)octal_to_int(header.mode, sizeof(header.mode));
            time_t mtime = (time_t)octal_to_int(header.mtime, sizeof(header.mtime));
            struct tm *tm = localtime(&mtime);
            char timestr[32];
            strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M", tm);
            
            printf("%c%c%c%c%c%c%c%c%c%c %8zu %s %s\n",
                   (mode & 0400) ? 'r' : '-',
                   (mode & 0200) ? 'w' : '-',
                   (mode & 0100) ? 'x' : '-',
                   (mode & 0040) ? 'r' : '-',
                   (mode & 0020) ? 'w' : '-',
                   (mode & 0010) ? 'x' : '-',
                   (mode & 0004) ? 'r' : '-',
                   (mode & 0002) ? 'w' : '-',
                   (mode & 0001) ? 'x' : '-',
                   (char)(0),
                   size,
                   timestr,
                   fullname);
        }
        
        if (header.typeflag == '0' || header.typeflag == '\0') {
            size_t blocks = (size + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE;
            fseek(fp, blocks * TAR_BLOCK_SIZE, SEEK_CUR);
        }
    }
    
    fclose(fp);
    return result;
}


int tar_iter_init(tar_iterator_t *iter, const char *tarfile) {
    memset(iter, 0, sizeof(tar_iterator_t));
    iter->fp = fopen(tarfile, "rb");
    if (!iter->fp) {
        return TARLIB_IO_ERROR;
    }
    return TARLIB_OK;
}

int tar_iter_next(tar_iterator_t *iter) {
    if (!iter->fp || iter->eof) {
        return TARLIB_EOF;
    }
    
    if (iter->current_size > 0) {
        size_t blocks = (iter->current_size + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE;
        fseek(iter->fp, blocks * TAR_BLOCK_SIZE, SEEK_CUR);
    }
    
    if (fread(&iter->header, 1, sizeof(tar_header_t), iter->fp) != sizeof(tar_header_t)) {
        iter->eof = 1;
        return TARLIB_EOF;
    }
    
    int empty = 1;
    int i;
    for (i = 0; i < (int)sizeof(tar_header_t); i++) {
        if (((char *)&iter->header)[i] != 0) {
            empty = 0;
            break;
        }
    }
    if (empty) {
        iter->eof = 1;
        return TARLIB_EOF;
    }
    
    unsigned int chksum = (unsigned int)octal_to_int(iter->header.chksum, sizeof(iter->header.chksum));
    unsigned int calc = calc_checksum(&iter->header);
    if (chksum != calc) {
        return TARLIB_FORMAT_ERR;
    }
    
    iter->current_size = (size_t)octal_to_int(iter->header.size, sizeof(iter->header.size));
    iter->current_pos = 0;
    iter->data_offset = ftell(iter->fp);
    
    return TARLIB_OK;
}

int tar_iter_extract(tar_iterator_t *iter, const char *outpath) {
    if (!iter->fp || iter->eof) {
        return TARLIB_ERROR;
    }
    
    char pathcopy[512];
    strncpy(pathcopy, outpath, sizeof(pathcopy) - 1);
    pathcopy[sizeof(pathcopy) - 1] = '\0';
    char *p = strrchr(pathcopy, '/');
    if (p) {
        *p = '\0';
        ensure_directory(pathcopy);
    }
    
    FILE *out = fopen(outpath, "wb");
    if (!out) {
        return TARLIB_IO_ERROR;
    }
    
    long saved_pos = ftell(iter->fp);
    
    fseek(iter->fp, iter->data_offset, SEEK_SET);
    
    char buf[TAR_BLOCK_SIZE];
    size_t remaining = iter->current_size;
    while (remaining > 0) {
        size_t to_read = remaining < sizeof(buf) ? remaining : sizeof(buf);
        size_t n = fread(buf, 1, to_read, iter->fp);
        if (n != to_read) {
            fclose(out);
            fseek(iter->fp, saved_pos, SEEK_SET);
            return TARLIB_IO_ERROR;
        }
        if (fwrite(buf, 1, n, out) != n) {
            fclose(out);
            fseek(iter->fp, saved_pos, SEEK_SET);
            return TARLIB_IO_ERROR;
        }
        remaining -= n;
    }
    
    fclose(out);
    
    fseek(iter->fp, saved_pos, SEEK_SET);
    
    mode_t mode = (mode_t)octal_to_int(iter->header.mode, sizeof(iter->header.mode));
    chmod(outpath, mode);
    
    return TARLIB_OK;
}

void tar_iter_close(tar_iterator_t *iter) {
    if (iter->fp) {
        fclose(iter->fp);
        iter->fp = NULL;
    }
}

int tar_find_entry(const char *tarfile, const char *entryname, tar_header_t *header, long *offset) {
    FILE *fp = fopen(tarfile, "rb");
    if (!fp) {
        return TARLIB_IO_ERROR;
    }
    
    tar_header_t h;
    int found = -1;
    
    while (fread(&h, 1, sizeof(h), fp) == sizeof(h)) {
        int empty = 1;
        int i;
        for (i = 0; i < (int)sizeof(h); i++) {
            if (((char *)&h)[i] != 0) {
                empty = 0;
                break;
            }
        }
        if (empty) {
            break;
        }
        
        unsigned int chksum = (unsigned int)octal_to_int(h.chksum, sizeof(h.chksum));
        unsigned int calc = calc_checksum(&h);
        if (chksum != calc) {
            continue;
        }
        
        char fullname[TAR_MAX_PATH];
        if (h.prefix[0]) {
            snprintf(fullname, sizeof(fullname), "%s/%s", h.prefix, h.name);
        } else {
            strncpy(fullname, h.name, sizeof(fullname) - 1);
            fullname[sizeof(fullname) - 1] = '\0';
        }
        
        if (strcmp(fullname, entryname) == 0) {
            if (header) {
                *header = h;
            }
            if (offset) {
                *offset = ftell(fp);
            }
            found = 0;
            break;
        }
        
        size_t size = (size_t)octal_to_int(h.size, sizeof(h.size));
        size_t blocks = (size + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE;
        fseek(fp, blocks * TAR_BLOCK_SIZE, SEEK_CUR);
    }
    
    fclose(fp);
    return found;
}


/* ============== 位操作工具 ============== */

typedef struct {
    uint8_t *buf;
    size_t size;
    size_t pos;
    uint32_t bitbuf;
    int bitcnt;
} bitstream_t;

static void bitstream_init(bitstream_t *bs, uint8_t *buf, size_t size) {
    bs->buf = buf;
    bs->size = size;
    bs->pos = 0;
    bs->bitbuf = 0;
    bs->bitcnt = 0;
}

static int bitstream_write_bits(bitstream_t *bs, uint32_t bits, int n) {
    bs->bitbuf |= bits << bs->bitcnt;
    bs->bitcnt += n;
    while (bs->bitcnt >= 8) {
        if (bs->pos >= bs->size) {
            return TARLIB_ERROR;
        }
        bs->buf[bs->pos++] = bs->bitbuf & 0xff;
        bs->bitbuf >>= 8;
        bs->bitcnt -= 8;
    }
    return TARLIB_OK;
}

static void bitstream_flush(bitstream_t *bs) {
    if (bs->bitcnt > 0) {
        if (bs->pos < bs->size) {
            bs->buf[bs->pos++] = bs->bitbuf & 0xff;
        }
    }
}

static int bitstream_read_bits(bitstream_t *bs, int n) {
    while (bs->bitcnt < n) {
        if (bs->pos >= bs->size) {
            return -1;
        }
        bs->bitbuf |= (uint32_t)bs->buf[bs->pos++] << bs->bitcnt;
        bs->bitcnt += 8;
    }
    uint32_t result = bs->bitbuf & ((1U << n) - 1);
    bs->bitbuf >>= n;
    bs->bitcnt -= n;
    return result;
}

/* ============== DEFLATE常量 ============== */

static const uint16_t len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};

static const uint8_t len_bits[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

static const uint16_t dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};

static const uint8_t dist_bits[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* ============== 存储块压缩 ============== */

static int deflate_store(uint8_t *dst, size_t *dst_len, const uint8_t *src, size_t src_len) {
    size_t len = src_len > 65535 ? 65535 : src_len;
    if (*dst_len < 5 + len) {
        return TARLIB_ERROR;
    }
    
    size_t pos = 0;
    
    dst[pos++] = 0x01;
    
    dst[pos++] = len & 0xff;
    dst[pos++] = (len >> 8) & 0xff;
    
    dst[pos++] = (~len) & 0xff;
    dst[pos++] = ((~len) >> 8) & 0xff;
    
    memcpy(dst + pos, src, len);
    pos += len;
    
    *dst_len = pos;
    return TARLIB_OK;
}

/* ============== 固定Huffman编码 ============== */

static int write_fixed_symbol(bitstream_t *bs, int sym) {
    int code, bits;
    if (sym <= 143) {
        code = sym + 48;
        bits = 8;
    } else if (sym <= 255) {
        code = sym + 256;
        bits = 9;
    } else if (sym <= 279) {
        code = sym - 256;
        bits = 7;
    } else if (sym <= 287) {
        code = sym - 80;
        bits = 8;
    } else {
        return TARLIB_ERROR;
    }
    
    int reversed = 0;
    int i;
    for (i = 0; i < bits; i++) {
        reversed = (reversed << 1) | (code & 1);
        code >>= 1;
    }
    
    return bitstream_write_bits(bs, reversed, bits);
}

static int reverse_bits(int code, int bits) {
    int reversed = 0;
    int i;
    for (i = 0; i < bits; i++) {
        reversed = (reversed << 1) | (code & 1);
        code >>= 1;
    }
    return reversed;
}

static int decode_fixed_huffman(bitstream_t *bs) {
    int code7 = bitstream_read_bits(bs, 7);
    if (code7 < 0) return -1;
    code7 = reverse_bits(code7, 7);
    
    if (code7 <= 23) {
        return 256 + code7;
    }
    
    int bit8 = bitstream_read_bits(bs, 1);
    if (bit8 < 0) return -1;
    
    int code8 = (code7 << 1) | bit8;
    
    if (code8 >= 48 && code8 <= 191) {
        return code8 - 48;
    }
    
    if (code8 >= 192 && code8 <= 199) {
        return 280 + (code8 - 192);
    }
    
    int bit9 = bitstream_read_bits(bs, 1);
    if (bit9 < 0) return -1;
    
    int code9 = (code8 << 1) | bit9;
    
    if (code9 >= 400 && code9 <= 511) {
        return 144 + (code9 - 400);
    }
    
    return -1;
}

static int deflate_compress(uint8_t *dst, size_t *dst_len, const uint8_t *src, size_t src_len, int level) {
    if (level == 0 || src_len < 32) {
        return deflate_store(dst, dst_len, src, src_len);
    }
    
    bitstream_t bs;
    bitstream_init(&bs, dst, *dst_len);
    
    if (bitstream_write_bits(&bs, 1, 1) != TARLIB_OK) return TARLIB_ERROR;
    if (bitstream_write_bits(&bs, 1, 2) != TARLIB_OK) return TARLIB_ERROR;
    
    size_t i;
    for (i = 0; i < src_len; i++) {
        if (write_fixed_symbol(&bs, src[i]) != TARLIB_OK) return TARLIB_ERROR;
    }
    
    if (write_fixed_symbol(&bs, 256) != TARLIB_OK) return TARLIB_ERROR;
    
    bitstream_flush(&bs);
    *dst_len = bs.pos;
    return TARLIB_OK;
}

static int deflate_decompress(uint8_t *dst, size_t *dst_len, const uint8_t *src, size_t src_len) {
    bitstream_t bs;
    bitstream_init(&bs, (uint8_t *)src, src_len);
    
    size_t outpos = 0;
    size_t maxout = *dst_len;
    
    int bfinal;
    do {
        bfinal = bitstream_read_bits(&bs, 1);
        if (bfinal < 0) return TARLIB_FORMAT_ERR;
        
        int btype = bitstream_read_bits(&bs, 2);
        if (btype < 0) return TARLIB_FORMAT_ERR;
        
        if (btype == 0) {
            if (bs.bitcnt > 0) {
                bs.bitbuf >>= bs.bitcnt;
                bs.bitcnt = 0;
            }
            if (bs.pos + 4 > bs.size) return TARLIB_FORMAT_ERR;
            int len = bs.buf[bs.pos] | (bs.buf[bs.pos + 1] << 8);
            int nlen = bs.buf[bs.pos + 2] | (bs.buf[bs.pos + 3] << 8);
            bs.pos += 4;
            if ((len ^ nlen) != 0xffff) {
                return TARLIB_FORMAT_ERR;
            }
            int i;
            for (i = 0; i < len; i++) {
                if (outpos >= maxout) return TARLIB_ERROR;
                if (bs.pos >= bs.size) return TARLIB_FORMAT_ERR;
                dst[outpos++] = bs.buf[bs.pos++];
            }
        } else if (btype == 1) {
            while (1) {
                int sym = decode_fixed_huffman(&bs);
                if (sym < 0) return TARLIB_FORMAT_ERR;
                
                if (sym < 256) {
                    if (outpos >= maxout) return TARLIB_ERROR;
                    dst[outpos++] = (uint8_t)sym;
                } else if (sym == 256) {
                    break;
                } else if (sym <= 285) {
                    int len_code = sym - 257;
                    int len = len_base[len_code];
                    if (len_bits[len_code] > 0) {
                        int extra = bitstream_read_bits(&bs, len_bits[len_code]);
                        if (extra < 0) return TARLIB_FORMAT_ERR;
                        len += extra;
                    }
                    
                    int dist_code = bitstream_read_bits(&bs, 5);
                    if (dist_code < 0) return TARLIB_FORMAT_ERR;
                    if (dist_code >= 30) return TARLIB_FORMAT_ERR;
                    int dist = dist_base[dist_code];
                    if (dist_bits[dist_code] > 0) {
                        int extra = bitstream_read_bits(&bs, dist_bits[dist_code]);
                        if (extra < 0) return TARLIB_FORMAT_ERR;
                        dist += extra;
                    }
                    
                    int i;
                    for (i = 0; i < len; i++) {
                        if (outpos >= maxout) return TARLIB_ERROR;
                        if (outpos < (size_t)dist) return TARLIB_FORMAT_ERR;
                        dst[outpos] = dst[outpos - dist];
                        outpos++;
                    }
                } else {
                    return TARLIB_FORMAT_ERR;
                }
            }
        } else if (btype == 2) {
            return TARLIB_FORMAT_ERR;
        } else {
            return TARLIB_FORMAT_ERR;
        }
    } while (!bfinal);
    
    *dst_len = outpos;
    return TARLIB_OK;
}


/* ============== GZIP 文件格式实现 ============== */

#define GZIP_FTEXT      1
#define GZIP_FHCRC      2
#define GZIP_FEXTRA     4
#define GZIP_FNAME      8
#define GZIP_FCOMMENT   16

int gzip_compress_file(const char *srcfile, const char *dstfile, int level) {
    FILE *src = fopen(srcfile, "rb");
    if (!src) {
        return TARLIB_IO_ERROR;
    }
    
    fseek(src, 0, SEEK_END);
    long src_size = ftell(src);
    fseek(src, 0, SEEK_SET);
    
    if (src_size < 0) {
        fclose(src);
        return TARLIB_IO_ERROR;
    }
    
    uint8_t *inbuf = malloc(src_size);
    if (!inbuf) {
        fclose(src);
        return TARLIB_MEM_ERROR;
    }
    
    if (fread(inbuf, 1, src_size, src) != (size_t)src_size) {
        free(inbuf);
        fclose(src);
        return TARLIB_IO_ERROR;
    }
    fclose(src);
    
    size_t max_compressed = src_size + src_size / 100 + 1024;
    uint8_t *compressed = malloc(max_compressed);
    if (!compressed) {
        free(inbuf);
        return TARLIB_MEM_ERROR;
    }
    
    size_t compressed_size = max_compressed;
    int result = deflate_compress(compressed, &compressed_size, inbuf, src_size, level);
    if (result != TARLIB_OK) {
        free(compressed);
        free(inbuf);
        return result;
    }
    
    FILE *dst = fopen(dstfile, "wb");
    if (!dst) {
        free(compressed);
        free(inbuf);
        return TARLIB_IO_ERROR;
    }
    
    uint8_t header[10];
    header[0] = GZIP_MAGIC1;
    header[1] = GZIP_MAGIC2;
    header[2] = GZIP_DEFLATE;
    header[3] = GZIP_FNAME;
    uint32_t mtime = (uint32_t)time(NULL);
    header[4] = mtime & 0xff;
    header[5] = (mtime >> 8) & 0xff;
    header[6] = (mtime >> 16) & 0xff;
    header[7] = (mtime >> 24) & 0xff;
    header[8] = 0;
    header[9] = 3;
    
    fwrite(header, 1, 10, dst);
    
    const char *basename = strrchr(srcfile, '/');
    if (basename) basename++;
    else basename = srcfile;
    fwrite(basename, 1, strlen(basename) + 1, dst);
    
    fwrite(compressed, 1, compressed_size, dst);
    
    uint32_t crc = tarlib_crc32(0, inbuf, src_size);
    uint32_t isize = (uint32_t)src_size;
    
    fwrite(&crc, 1, 4, dst);
    fwrite(&isize, 1, 4, dst);
    
    fclose(dst);
    free(compressed);
    free(inbuf);
    
    return TARLIB_OK;
}

int gzip_decompress_file(const char *srcfile, const char *dstfile) {
    FILE *src = fopen(srcfile, "rb");
    if (!src) {
        return TARLIB_IO_ERROR;
    }
    
    uint8_t header[10];
    if (fread(header, 1, 10, src) != 10) {
        fclose(src);
        return TARLIB_FORMAT_ERR;
    }
    
    if (header[0] != GZIP_MAGIC1 || header[1] != GZIP_MAGIC2) {
        fclose(src);
        return TARLIB_FORMAT_ERR;
    }
    
    if (header[2] != GZIP_DEFLATE) {
        fclose(src);
        return TARLIB_FORMAT_ERR;
    }
    
    uint8_t flags = header[3];
    
    if (flags & GZIP_FEXTRA) {
        uint8_t xlen[2];
        fread(xlen, 1, 2, src);
        uint16_t xlen_val = xlen[0] | (xlen[1] << 8);
        fseek(src, xlen_val, SEEK_CUR);
    }
    
    if (flags & GZIP_FNAME) {
        int c;
        while ((c = fgetc(src)) != 0 && c != EOF);
    }
    
    if (flags & GZIP_FCOMMENT) {
        int c;
        while ((c = fgetc(src)) != 0 && c != EOF);
    }
    
    if (flags & GZIP_FHCRC) {
        fseek(src, 2, SEEK_CUR);
    }
    
    long data_start = ftell(src);
    fseek(src, -8, SEEK_END);
    long data_end = ftell(src);
    size_t compressed_size = data_end - data_start;
    
    uint32_t expected_crc, expected_size;
    fread(&expected_crc, 1, 4, src);
    fread(&expected_size, 1, 4, src);
    
    uint8_t *compressed = malloc(compressed_size);
    if (!compressed) {
        fclose(src);
        return TARLIB_MEM_ERROR;
    }
    
    fseek(src, data_start, SEEK_SET);
    if (fread(compressed, 1, compressed_size, src) != compressed_size) {
        free(compressed);
        fclose(src);
        return TARLIB_IO_ERROR;
    }
    fclose(src);
    
    size_t uncompressed_size = expected_size > 0 ? expected_size : compressed_size * 10;
    uint8_t *uncompressed = malloc(uncompressed_size);
    if (!uncompressed) {
        free(compressed);
        return TARLIB_MEM_ERROR;
    }
    
    int result = deflate_decompress(uncompressed, &uncompressed_size, compressed, compressed_size);
    free(compressed);
    
    if (result != TARLIB_OK) {
        free(uncompressed);
        return result;
    }
    
    uint32_t crc = tarlib_crc32(0, uncompressed, uncompressed_size);
    if (crc != expected_crc) {
        free(uncompressed);
        return TARLIB_CRC_ERROR;
    }
    
    FILE *dst = fopen(dstfile, "wb");
    if (!dst) {
        free(uncompressed);
        return TARLIB_IO_ERROR;
    }
    
    if (fwrite(uncompressed, 1, uncompressed_size, dst) != uncompressed_size) {
        free(uncompressed);
        fclose(dst);
        return TARLIB_IO_ERROR;
    }
    
    fclose(dst);
    free(uncompressed);
    
    return TARLIB_OK;
}

int gzip_info(const char *filename, size_t *uncompressed_size, time_t *mtime, char *filename_in_gzip) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        return TARLIB_IO_ERROR;
    }
    
    uint8_t header[10];
    if (fread(header, 1, 10, fp) != 10) {
        fclose(fp);
        return TARLIB_FORMAT_ERR;
    }
    
    if (header[0] != GZIP_MAGIC1 || header[1] != GZIP_MAGIC2) {
        fclose(fp);
        return TARLIB_FORMAT_ERR;
    }
    
    uint8_t flags = header[3];
    
    if (mtime) {
        *mtime = (time_t)(header[4] | (header[5] << 8) | (header[6] << 16) | (header[7] << 24));
    }
    
    if (flags & GZIP_FEXTRA) {
        uint8_t xlen[2];
        fread(xlen, 1, 2, fp);
        uint16_t xlen_val = xlen[0] | (xlen[1] << 8);
        fseek(fp, xlen_val, SEEK_CUR);
    }
    
    if (flags & GZIP_FNAME) {
        if (filename_in_gzip) {
            int i = 0;
            int c;
            while ((c = fgetc(fp)) != 0 && c != EOF && i < 255) {
                filename_in_gzip[i++] = (char)c;
            }
            filename_in_gzip[i] = '\0';
        } else {
            int c;
            while ((c = fgetc(fp)) != 0 && c != EOF);
        }
    } else if (filename_in_gzip) {
        filename_in_gzip[0] = '\0';
    }
    
    if (flags & GZIP_FCOMMENT) {
        int c;
        while ((c = fgetc(fp)) != 0 && c != EOF);
    }
    
    if (flags & GZIP_FHCRC) {
        fseek(fp, 2, SEEK_CUR);
    }
    
    if (uncompressed_size) {
        fseek(fp, -4, SEEK_END);
        uint32_t isize;
        fread(&isize, 1, 4, fp);
        *uncompressed_size = (size_t)isize;
    }
    
    fclose(fp);
    return TARLIB_OK;
}


int gzip_compress_data(const uint8_t *src, size_t src_len, uint8_t *dst, size_t *dst_len, int level) {
    size_t header_size = 10;
    size_t min_size = header_size + 8 + src_len;
    
    if (*dst_len < min_size) {
        return TARLIB_ERROR;
    }
    
    dst[0] = GZIP_MAGIC1;
    dst[1] = GZIP_MAGIC2;
    dst[2] = GZIP_DEFLATE;
    dst[3] = 0;
    dst[4] = 0; dst[5] = 0; dst[6] = 0; dst[7] = 0;
    dst[8] = 0;
    dst[9] = 255;
    
    size_t max_compressed = *dst_len - header_size - 8;
    int result = deflate_compress(dst + header_size, &max_compressed, src, src_len, level);
    if (result != TARLIB_OK) {
        return result;
    }
    
    uint32_t crc = tarlib_crc32(0, src, src_len);
    size_t pos = header_size + max_compressed;
    
    dst[pos++] = crc & 0xff;
    dst[pos++] = (crc >> 8) & 0xff;
    dst[pos++] = (crc >> 16) & 0xff;
    dst[pos++] = (crc >> 24) & 0xff;
    
    dst[pos++] = src_len & 0xff;
    dst[pos++] = (src_len >> 8) & 0xff;
    dst[pos++] = (src_len >> 16) & 0xff;
    dst[pos++] = (src_len >> 24) & 0xff;
    
    *dst_len = pos;
    return TARLIB_OK;
}

int gzip_decompress_data(const uint8_t *src, size_t src_len, uint8_t *dst, size_t *dst_len) {
    if (src_len < 20) {
        return TARLIB_FORMAT_ERR;
    }
    
    if (src[0] != GZIP_MAGIC1 || src[1] != GZIP_MAGIC2) {
        return TARLIB_FORMAT_ERR;
    }
    
    if (src[2] != GZIP_DEFLATE) {
        return TARLIB_FORMAT_ERR;
    }
    
    uint8_t flags = src[3];
    size_t pos = 10;
    
    if (flags & GZIP_FEXTRA) {
        uint16_t xlen = src[pos] | (src[pos + 1] << 8);
        pos += 2 + xlen;
    }
    
    if (flags & GZIP_FNAME) {
        while (pos < src_len && src[pos] != 0) pos++;
        pos++;
    }
    
    if (flags & GZIP_FCOMMENT) {
        while (pos < src_len && src[pos] != 0) pos++;
        pos++;
    }
    
    if (flags & GZIP_FHCRC) {
        pos += 2;
    }
    
    if (pos + 8 > src_len) {
        return TARLIB_FORMAT_ERR;
    }
    
    size_t compressed_size = src_len - pos - 8;
    
    uint32_t expected_crc = src[src_len - 8] | (src[src_len - 7] << 8) |
                           (src[src_len - 6] << 16) | (src[src_len - 5] << 24);
    uint32_t expected_size = src[src_len - 4] | (src[src_len - 3] << 8) |
                            (src[src_len - 2] << 16) | (src[src_len - 1] << 24);
    
    if (expected_size > *dst_len) {
        return TARLIB_ERROR;
    }
    
    size_t out_size = *dst_len;
    int result = deflate_decompress(dst, &out_size, src + pos, compressed_size);
    if (result != TARLIB_OK) {
        return result;
    }
    
    uint32_t crc = tarlib_crc32(0, dst, out_size);
    if (crc != expected_crc) {
        return TARLIB_CRC_ERROR;
    }
    
    *dst_len = out_size;
    return TARLIB_OK;
}

/* ============== TAR+GZIP 组合实现 ============== */

// 递归添加目录内容到tar（辅助函数）
static int tar_add_directory_recursive(const char *tarfile, const char *dirpath, const char *entryname) {
    DIR *dir = opendir(dirpath);
    if (!dir) {
        return TARLIB_IO_ERROR;
    }
    
    // 添加目录本身
    tar_add_directory(tarfile, dirpath, entryname);
    
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        
        char fullpath[512];
        char entrypath[512];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, ent->d_name);
        snprintf(entrypath, sizeof(entrypath), "%s/%s", entryname, ent->d_name);
        
        struct stat st;
        if (stat(fullpath, &st) < 0) continue;
        
        if (S_ISDIR(st.st_mode)) {
            // 递归处理子目录
            tar_add_directory_recursive(tarfile, fullpath, entrypath);
        } else if (S_ISREG(st.st_mode)) {
            tar_add_file(tarfile, fullpath, entrypath);
        }
    }
    closedir(dir);
    
    return TARLIB_OK;
}

int tgz_create(const char *tgzfile, const char **paths, int count, int level) {
    char tartmp[256];
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = "/tmp";
    snprintf(tartmp, sizeof(tartmp), "%s/tarlib_tmp_%d.tar",
            tmpdir, (int)getpid());
    
    if (tar_create(tartmp) != TARLIB_OK) {
        return TARLIB_IO_ERROR;
    }
    
    int i;
    for (i = 0; i < count; i++) {
        struct stat st;
        if (stat(paths[i], &st) < 0) {
            unlink(tartmp);
            return TARLIB_IO_ERROR;
        }
        
        if (S_ISDIR(st.st_mode)) {
            // 递归添加目录及其所有内容
            tar_add_directory_recursive(tartmp, paths[i], paths[i]);
        } else if (S_ISREG(st.st_mode)) {
            tar_add_file(tartmp, paths[i], paths[i]);
        }
    }
    
    tar_close_archive(tartmp);
    
    int result = gzip_compress_file(tartmp, tgzfile, level);
    unlink(tartmp);
    
    return result;
}

int tgz_extract(const char *tgzfile, const char *outdir) {
    char tartmp[256];
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = "/tmp";
    snprintf(tartmp, sizeof(tartmp), "%s/tarlib_tmp_%d.tar",
            tmpdir, (int)getpid());
    
    int result = gzip_decompress_file(tgzfile, tartmp);
    if (result != TARLIB_OK) {
        return result;
    }
    
    result = tar_extract(tartmp, outdir);
    unlink(tartmp);
    
    return result;
}

int tgz_list(const char *tgzfile, tar_list_callback callback, void *userdata) {
    FILE *fp = fopen(tgzfile, "rb");
    if (!fp) {
        return TARLIB_IO_ERROR;
    }
    
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    uint8_t *compressed = malloc(size);
    if (!compressed) {
        fclose(fp);
        return TARLIB_MEM_ERROR;
    }
    
    if (fread(compressed, 1, size, fp) != (size_t)size) {
        free(compressed);
        fclose(fp);
        return TARLIB_IO_ERROR;
    }
    fclose(fp);
    
    size_t uncompressed_size = size * 20 + 1024;
    if (uncompressed_size < 64 * 1024) uncompressed_size = 64 * 1024;
    uint8_t *uncompressed = malloc(uncompressed_size);
    if (!uncompressed) {
        free(compressed);
        return TARLIB_MEM_ERROR;
    }
    
    int result = gzip_decompress_data(compressed, size, uncompressed, &uncompressed_size);
    free(compressed);
    
    if (result != TARLIB_OK) {
        free(uncompressed);
        return result;
    }
    
    size_t pos = 0;
    while (pos + sizeof(tar_header_t) <= uncompressed_size) {
        tar_header_t *header = (tar_header_t *)(uncompressed + pos);
        
        int empty = 1;
        int i;
        for (i = 0; i < (int)sizeof(tar_header_t); i++) {
            if (((char *)header)[i] != 0) {
                empty = 0;
                break;
            }
        }
        if (empty) {
            break;
        }
        
        unsigned int chksum = (unsigned int)octal_to_int(header->chksum, sizeof(header->chksum));
        unsigned int calc = calc_checksum(header);
        if (chksum != calc) {
            free(uncompressed);
            return TARLIB_FORMAT_ERR;
        }
        
        size_t file_size = (size_t)octal_to_int(header->size, sizeof(header->size));
        
        char fullname[TAR_MAX_PATH];
        if (header->prefix[0]) {
            snprintf(fullname, sizeof(fullname), "%s/%s", header->prefix, header->name);
        } else {
            strncpy(fullname, header->name, sizeof(fullname) - 1);
            fullname[sizeof(fullname) - 1] = '\0';
        }
        
        if (callback) {
            callback(header, fullname, userdata);
        } else {
            mode_t mode = (mode_t)octal_to_int(header->mode, sizeof(header->mode));
            time_t mtime = (time_t)octal_to_int(header->mtime, sizeof(header->mtime));
            struct tm *tm = localtime(&mtime);
            char timestr[32];
            strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M", tm);
            
            printf("%c%c%c%c%c%c%c%c%c%c %8zu %s %s\n",
                   (mode & 0400) ? 'r' : '-',
                   (mode & 0200) ? 'w' : '-',
                   (mode & 0100) ? 'x' : '-',
                   (mode & 0040) ? 'r' : '-',
                   (mode & 0020) ? 'w' : '-',
                   (mode & 0010) ? 'x' : '-',
                   (mode & 0004) ? 'r' : '-',
                   (mode & 0002) ? 'w' : '-',
                   (mode & 0001) ? 'x' : '-',
                   (char)(0),
                   file_size,
                   timestr,
                   fullname);
        }
        
        pos += sizeof(tar_header_t);
        size_t blocks = (file_size + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE;
        pos += blocks * TAR_BLOCK_SIZE;
    }
    
    free(uncompressed);
    return TARLIB_OK;
}

/* ============== 错误信息 ============== */

const char *tarlib_strerror(int errcode) {
    switch (errcode) {
        case TARLIB_OK:         return "Success";
        case TARLIB_ERROR:      return "General error";
        case TARLIB_EOF:        return "End of file";
        case TARLIB_CRC_ERROR:  return "CRC error";
        case TARLIB_MEM_ERROR:  return "Memory allocation error";
        case TARLIB_IO_ERROR:   return "I/O error";
        case TARLIB_FORMAT_ERR: return "Format error";
        default:                return "Unknown error";
    }
}
