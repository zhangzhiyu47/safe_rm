#include "mv_lib.h"
#include "safe_rm.h"
#include "tarlib.h"
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <sys/ioctl.h>

// 从归档文件中列出项目 - 使用迭代器直接读取内容
static int scan_archive_items(const char *archive_path, RestoreItem **items, int *count, int *capacity, int start_id) {
    regex_t regex;
    tar_iterator_t iter;
    
    // 编译正则表达式提取时间戳/编号/info
    // 匹配完整路径中的时间戳部分，如：/path/2026-02-20-10:00:00/1/info
    if (regcomp(&regex, "/([0-9]{4}-[0-9]{2}-[0-9]{2}-[0-9]{2}:[0-9]{2}:[0-9]{2})/([0-9]+)/info$", REG_EXTENDED) != 0) {
        return start_id;
    }
    
    // 先解压到临时 tar 文件，然后用 tar 迭代器
    char tartmp[256];
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = "/tmp";
    snprintf(tartmp, sizeof(tartmp), "%s/restore_tmp_%d.tar", tmpdir, (int)getpid());
    
    if (gzip_decompress_file(archive_path, tartmp) != TARLIB_OK) {
        regfree(&regex);
        return start_id;
    }
    
    // 使用 tar 迭代器遍历
    if (tar_iter_init(&iter, tartmp) != TARLIB_OK) {
        unlink(tartmp);
        regfree(&regex);
        return start_id;
    }
    
    // 第一遍扫描：收集所有条目信息
    typedef struct {
        char timestamp[FULL_TIME_LEN];
        char index_str[16];
        char original_path[MAX_PATH_LEN];
        char filename[MAX_NAME_LEN];
        int has_info;
        int has_file;
    } temp_entry_t;
    
    temp_entry_t entries[100];  // 假设每个归档最多100个条目
    int entry_count = 0;
    
    while (tar_iter_next(&iter) == 0 && entry_count < 100) {
        regmatch_t matches[4];
        const char *fullname = iter.header.name;
        
        // 检查是否匹配 info$ 模式
        if (regexec(&regex, fullname, 4, matches, 0) == 0) {
            // 提取时间戳和编号
            int ts_len = matches[1].rm_eo - matches[1].rm_so;
            int idx_len = matches[2].rm_eo - matches[2].rm_so;
            
            if (ts_len < FULL_TIME_LEN && idx_len < 16) {
                strncpy(entries[entry_count].timestamp, 
                        fullname + matches[1].rm_so, ts_len);
                entries[entry_count].timestamp[ts_len] = '\0';
                
                strncpy(entries[entry_count].index_str, 
                        fullname + matches[2].rm_so, idx_len);
                entries[entry_count].index_str[idx_len] = '\0';
                
                // 读取 info 文件内容
                // 注意：读取后需要跳到下一个 512 字节块边界，
                // 这样迭代器下次能正常工作
                size_t size = (size_t)octal_to_int(iter.header.size, 12);
                
                if (size > 0 && size < MAX_PATH_LEN) {
                    char *content = malloc(size + 1);
                    if (content) {
                        if (fread(content, 1, size, iter.fp) == size) {
                            content[size] = '\0';
                            char *newline = strchr(content, '\n');
                            if (newline) *newline = '\0';
                            strncpy(entries[entry_count].original_path, content, MAX_PATH_LEN - 1);
                            entries[entry_count].original_path[MAX_PATH_LEN - 1] = '\0';
                        }
                        free(content);
                        // 跳到下一个 512 字节块边界
                        long current = ftell(iter.fp);
                        long next_block = ((current + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE) * TAR_BLOCK_SIZE;
                        fseek(iter.fp, next_block, SEEK_SET);
                    }
                }
                entries[entry_count].has_info = 1;
                entries[entry_count].has_file = 0;
                strcpy(entries[entry_count].filename, "未知文件");
                entry_count++;
            }
        }
    }
    
    tar_iter_close(&iter);
    
    // 第二遍扫描：查找文件名
    if (tar_iter_init(&iter, tartmp) == TARLIB_OK) {
        while (tar_iter_next(&iter) == 0) {
            const char *fullname = iter.header.name;
            
            // 检查是否是普通文件
            if (iter.header.typeflag != '0' && iter.header.typeflag != '\0') {
                continue;
            }
            
            // 匹配每个 entry 的 rubbish 目录
            for (int i = 0; i < entry_count; i++) {
                char pattern[MAX_PATH_LEN];
                snprintf(pattern, sizeof(pattern), "/%s/%s/rubbish/", 
                        entries[i].timestamp, entries[i].index_str);
                
                char *match = strstr(fullname, pattern);
                if (match != NULL) {
                    // 提取文件名（最后一个 / 之后）
                    char *last_slash = strrchr(fullname, '/');
                    if (last_slash && *(last_slash + 1) != '\0') {
                        strncpy(entries[i].filename, last_slash + 1, MAX_NAME_LEN - 1);
                        entries[i].filename[MAX_NAME_LEN - 1] = '\0';
                        entries[i].has_file = 1;
                    }
                    break;
                }
            }
        }
        tar_iter_close(&iter);
    }
    
    // 添加到 items 数组
    for (int i = 0; i < entry_count; i++) {
        if (*count >= *capacity) {
            *capacity *= 2;
            RestoreItem *new_items = realloc(*items, *capacity * sizeof(RestoreItem));
            if (!new_items) break;
            *items = new_items;
        }
        
        RestoreItem *item = &(*items)[*count];
        item->id = start_id++;
        strncpy(item->timestamp, entries[i].timestamp, sizeof(item->timestamp) - 1);
        strncpy(item->original_path, entries[i].original_path, sizeof(item->original_path) - 1);
        strncpy(item->filename, entries[i].filename, sizeof(item->filename) - 1);
        item->is_archived = 1;
        strncpy(item->archive_file, archive_path, sizeof(item->archive_file) - 1);
        (*count)++;
    }
    
    unlink(tartmp);
    regfree(&regex);
    return start_id;
}

// 扫描可恢复项目
int scan_restore_items(RestoreItem **items, int *count) {
    DIR *dp, *old_dp;
    struct dirent *entry;
    char *rubbish_bin = get_rubbish_bin_path();
    char old_dir[MAX_PATH_LEN];
    int capacity = 10;
    int id = 1;
    
    *count = 0;
    *items = malloc(capacity * sizeof(RestoreItem));
    if (*items == NULL) {
        return -1;
    }
    
    // 扫描活跃的A类目录
    dp = opendir(rubbish_bin);
    if (dp != NULL) {
        while ((entry = readdir(dp)) != NULL) {
            if (entry->d_type == DT_DIR || entry->d_type == DT_UNKNOWN) {
                if (strcmp(entry->d_name, ".") == 0 || 
                    strcmp(entry->d_name, "..") == 0 ||
                    strcmp(entry->d_name, OLD_DIR) == 0) {
                    continue;
                }
                
                if (is_valid_timestamp_format(entry->d_name)) {
                    char timestamp_dir[MAX_PATH_LEN];
                    DIR *idx_dp;
                    struct dirent *idx_entry;
                    
                    snprintf(timestamp_dir, sizeof(timestamp_dir), 
                             "%s/%s", rubbish_bin, entry->d_name);
                    
                    idx_dp = opendir(timestamp_dir);
                    if (idx_dp != NULL) {
                        while ((idx_entry = readdir(idx_dp)) != NULL) {
                            if (idx_entry->d_type == DT_DIR || idx_entry->d_type == DT_UNKNOWN) {
                                if (strcmp(idx_entry->d_name, ".") == 0 || 
                                    strcmp(idx_entry->d_name, "..") == 0) {
                                    continue;
                                }
                                
                                // 检查是否是数字目录
                                char *endptr;
                                long idx = strtol(idx_entry->d_name, &endptr, 10);
                                if (*endptr == '\0' && idx > 0) {
                                    char info_file[MAX_PATH_LEN];
                                    char rubbish_dir[MAX_PATH_LEN];
                                    FILE *fp;
                                    
                                    // 读取info文件
                                    snprintf(info_file, sizeof(info_file), 
                                             "%s/%s/info", timestamp_dir, idx_entry->d_name);
                                    
                                    fp = fopen(info_file, "r");
                                    if (fp != NULL) {
                                        char original_path[MAX_PATH_LEN];
                                        
                                        if (fgets(original_path, sizeof(original_path), fp) != NULL) {
                                            original_path[strcspn(original_path, "\n")] = '\0';
                                            
                                            // 查找rubbish目录中的文件
                                            snprintf(rubbish_dir, sizeof(rubbish_dir),
                                                     "%s/%s/rubbish", timestamp_dir, idx_entry->d_name);
                                            
                                            DIR *rubbish_dp = opendir(rubbish_dir);
                                            if (rubbish_dp != NULL) {
                                                struct dirent *file_entry;
                                                while ((file_entry = readdir(rubbish_dp)) != NULL) {
                                                    if (file_entry->d_type == DT_REG || 
                                                        file_entry->d_type == DT_DIR ||
                                                        file_entry->d_type == DT_UNKNOWN) {
                                                        if (strcmp(file_entry->d_name, ".") == 0 || 
                                                            strcmp(file_entry->d_name, "..") == 0) {
                                                            continue;
                                                        }
                                                        
                                                        // 扩展数组
                                                        if (*count >= capacity) {
                                                            capacity *= 2;
                                                            RestoreItem *new_items = realloc(*items, 
                                                                                capacity * sizeof(RestoreItem));
                                                            if (new_items == NULL) {
                                                                closedir(rubbish_dp);
                                                                fclose(fp);
                                                                closedir(idx_dp);
                                                                closedir(dp);
                                                                return -1;
                                                            }
                                                            *items = new_items;
                                                        }
                                                        
                                                        (*items)[*count].id = id++;
                                                        strncpy((*items)[*count].timestamp, 
                                                                entry->d_name, 
                                                                sizeof((*items)[*count].timestamp) - 1);
                                                        strncpy((*items)[*count].original_path, 
                                                                original_path,
                                                                sizeof((*items)[*count].original_path) - 1);
                                                        strncpy((*items)[*count].filename, 
                                                                file_entry->d_name,
                                                                sizeof((*items)[*count].filename) - 1);
                                                        (*items)[*count].is_archived = 0;
                                                        (*items)[*count].archive_file[0] = '\0';
                                                        
                                                        (*count)++;
                                                    }
                                                }
                                                closedir(rubbish_dp);
                                            }
                                        }
                                        fclose(fp);
                                    }
                                }
                            }
                        }
                        closedir(idx_dp);
                    }
                }
            }
        }
        closedir(dp);
    }
    
    // 扫描归档目录
    snprintf(old_dir, sizeof(old_dir), "%s/%s", rubbish_bin, OLD_DIR);
    old_dp = opendir(old_dir);
    if (old_dp != NULL) {
        while ((entry = readdir(old_dp)) != NULL) {
            size_t len = strlen(entry->d_name);
            if (len > 7 && strcmp(entry->d_name + len - 7, ".tar.gz") == 0) {
                char archive_path[MAX_PATH_LEN];
                snprintf(archive_path, sizeof(archive_path), "%s/%s", old_dir, entry->d_name);
                id = scan_archive_items(archive_path, items, count, &capacity, id);
            }
        }
        closedir(old_dp);
    }
    
    return 0;
}

// 从归档中恢复 - 使用 tarlib 替代 tar 命令
int restore_from_archive(const char *archive_path, const char *item_timestamp, 
                         const char *original_path, const char *filename) {
    char temp_dir[MAX_PATH_LEN];
    char extract_dir[MAX_PATH_LEN];
    char dest_path[MAX_PATH_LEN];
    char source_path[MAX_PATH_LEN];
    struct stat st;
    
    // 检查目标路径
    snprintf(dest_path, sizeof(dest_path), "%s/%s", original_path, filename);
    if (stat(dest_path, &st) == 0) {
        fprintf(stderr, "错误: 目标位置已存在 '%s'\n", dest_path);
        return -1;
    }
    
    // 创建临时目录
    const char *tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL) tmpdir = "/tmp";
    snprintf(temp_dir, sizeof(temp_dir), "%s/safe_rm_restore_%d", tmpdir, getpid());
    if (mkdir(temp_dir, 0755) != 0) {
        perror("创建临时目录失败");
        return -1;
    }
    
    // 使用 tarlib 解压特定目录
    char prefix_pattern[MAX_PATH_LEN];
    snprintf(prefix_pattern, sizeof(prefix_pattern), "%s/", item_timestamp);
    
    tar_iterator_t iter;
    int found = 0;
    
    if (tar_iter_init(&iter, archive_path) != TARLIB_OK) {
        fprintf(stderr, "无法打开归档文件\n");
        rmdir(temp_dir);
        return -1;
    }
    
    // 迭代提取属于该时间戳的文件
    while (tar_iter_next(&iter) == 0) {
        // 检查是否以目标时间戳开头
        if (strncmp(iter.header.name, prefix_pattern, strlen(prefix_pattern)) != 0) {
            continue;
        }
        
        // 构建输出路径：temp_dir/条目名
        char outpath[MAX_PATH_LEN];
        snprintf(outpath, sizeof(outpath), "%s/%s", temp_dir, iter.header.name);
        
        // 创建父目录
        char *path_copy = strdup(outpath);
        if (path_copy != NULL) {
            char *parent = dirname(path_copy);
            create_directory_recursive(parent);
            free(path_copy);
        }
        
        // 根据类型提取
        if (iter.header.typeflag == TAR_FILE_REGULAR || iter.header.typeflag == '0') {
            if (tar_iter_extract(&iter, outpath) != TARLIB_OK) {
                fprintf(stderr, "提取文件失败: %s\n", iter.header.name);
            }
        } else if (iter.header.typeflag == TAR_FILE_DIR || iter.header.typeflag == '5') {
            mkdir(outpath, 0755);
        }
    }
    tar_iter_close(&iter);
    
    // 查找 rubbish 目录中的目标文件
    snprintf(extract_dir, sizeof(extract_dir), "%s/%s", temp_dir, item_timestamp);
    
    DIR *dp = opendir(extract_dir);
    if (dp == NULL) {
        fprintf(stderr, "无法打开解压目录: %s\n", extract_dir);
        char *const dir[] = {temp_dir, NULL};
        redel3(dir);
        return -1;
    }
    
    struct dirent *entry;
    found = 0;
    while ((entry = readdir(dp)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        char idx_dir[MAX_PATH_LEN];
        snprintf(idx_dir, sizeof(idx_dir), "%s/%s/rubbish", extract_dir, entry->d_name);
        
        DIR *rubbish_dp = opendir(idx_dir);
        if (rubbish_dp != NULL) {
            struct dirent *file_entry;
            while ((file_entry = readdir(rubbish_dp)) != NULL) {
                if (strcmp(file_entry->d_name, filename) == 0) {
                    snprintf(source_path, sizeof(source_path), "%s/%s", idx_dir, filename);
                    found = 1;
                    break;
                }
            }
            closedir(rubbish_dp);
        }
        
        if (found) break;
    }
    closedir(dp);
    
    if (!found) {
        fprintf(stderr, "在归档中找不到文件 '%s'\n", filename);
        char *const dir[] = {temp_dir, NULL};
        redel3(dir);
        return -1;
    }
    
    // 确保目标目录存在
    create_directory_recursive(original_path);
    
    // 移动文件
    if (mv(source_path, dest_path) == -1) {
        perror("Error: mv");
        char *const dir[] = {temp_dir, NULL};
        redel3(dir);
        return -1;
    }
    
    // 清理临时目录
    char *const dir[] = {temp_dir, NULL};
    redel3(dir);
    
    return 0;
}

// 按ID恢复项目
int restore_item_by_id(int id, RestoreItem *items, int count) {
    int i;
    
    for (i = 0; i < count; i++) {
        if (items[i].id == id) {
            char source_path[MAX_PATH_LEN];
            char dest_path[MAX_PATH_LEN];
            struct stat st;
            
            // 检查目标路径
            snprintf(dest_path, sizeof(dest_path), "%s/%s", 
                     items[i].original_path, items[i].filename);
            
            if (stat(dest_path, &st) == 0) {
                fprintf(stderr, "错误: 目标位置已存在 '%s'，跳过恢复\n", dest_path);
                return -1;
            }
            
            if (items[i].is_archived) {
                // 从归档恢复（已使用 tarlib 重写）
                return restore_from_archive(items[i].archive_file, items[i].timestamp,
                                           items[i].original_path, items[i].filename);
            } else {
                // 从活跃目录恢复
                char *rubbish_bin = get_rubbish_bin_path();
                DIR *dp;
                struct dirent *entry;
                int found = 0;
                
                dp = opendir(rubbish_bin);
                if (dp == NULL) {
                    return -1;
                }
                
                while ((entry = readdir(dp)) != NULL) {
                    if (strcmp(entry->d_name, items[i].timestamp) == 0) {
                        char timestamp_dir[MAX_PATH_LEN];
                        DIR *idx_dp;
                        struct dirent *idx_entry;
                        
                        snprintf(timestamp_dir, sizeof(timestamp_dir), 
                                 "%s/%s", rubbish_bin, entry->d_name);
                        
                        idx_dp = opendir(timestamp_dir);
                        if (idx_dp != NULL) {
                            while ((idx_entry = readdir(idx_dp)) != NULL) {
                                if (strcmp(idx_entry->d_name, ".") == 0 || 
                                    strcmp(idx_entry->d_name, "..") == 0) {
                                    continue;
                                }
                                
                                char rubbish_dir[MAX_PATH_LEN];
                                snprintf(rubbish_dir, sizeof(rubbish_dir),
                                         "%s/%s/rubbish/%s", timestamp_dir, 
                                         idx_entry->d_name, items[i].filename);
                                
                                if (stat(rubbish_dir, &st) == 0) {
                                    strncpy(source_path, rubbish_dir, sizeof(source_path) - 1);
                                    source_path[sizeof(source_path) - 1] = '\0';
                                    found = 1;
                                    break;
                                }
                            }
                            closedir(idx_dp);
                        }
                        break;
                    }
                }
                closedir(dp);
                
                if (!found) {
                    fprintf(stderr, "错误: 找不到源文件\n");
                    return -1;
                }
                
                // 确保目标目录存在
                create_directory_recursive(items[i].original_path);
                
                // 移动文件
                if (mv(source_path, dest_path) == -1) {
                    perror("Error: mv");
                    fprintf(stderr, "恢复文件失败\n");
                    return -1;
                } else {
                    printf("已恢复: %s -> %s\n", items[i].filename, dest_path);

                    // 尝试清理空目录
                    char *dir = dirname(source_path);
                    rmdir(dir); // rubbish
                    dir = dirname(dir);
                    rmdir(dir); // index
                    dir = dirname(dir);
                    rmdir(dir); // timestamp

                    return 0;
                }
            }
        }
    }
    
    fprintf(stderr, "错误: 找不到ID为 %d 的项目\n", id);
    return -1;
}

// 获取终端宽度
static int get_terminal_width(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        return ws.ws_col;
    }
    return 80; // 默认宽度
}

// 打印分隔线
static void print_separator(int term_width) {
    int i;
    for (i = 0; i < term_width; i++) {
        printf("-");
    }
    printf("\n");
}

// 计算字符串的显示宽度（中文字符占2，ASCII占1）
static int display_width(const char *str) {
    int width = 0;
    while (*str) {
        unsigned char c = (unsigned char)*str;
        // 简单判断：如果高位为1，认为是多字节字符（中文等）
        if (c >= 0x80) {
            width += 2;
            // 跳过UTF-8多字节的当前字节和后续字节
            str++;
            while (*str && ((unsigned char)*str & 0xC0) == 0x80) {
                str++;
            }
        } else {
            width += 1;
            str++;
        }
    }
    return width;
}

// 带宽度限制的打印，处理中文对齐
static void print_field(const char *str, int width) {
    int w = display_width(str);
    printf("%s", str);
    // 填充空格
    while (w < width) {
        printf(" ");
        w++;
    }
}

// 打印恢复列表
void print_restore_list(RestoreItem *items, int count) {
    int i;
    int term_width = get_terminal_width();
    
    // 列宽配置（显示宽度）
    const int ID_WIDTH = 4;       // ID 列宽度
    const int TIME_WIDTH = 20;    // 时间列宽度
    const int STATUS_WIDTH = 8;   // 状态列宽度（已归档=8，在桶中=6）
    const int GAP = 1;            // 列之间的空格数
    
    if (count == 0) {
        printf("回收站为空，没有可恢复的项目\n");
        return;
    }
    
    // 计算路径和文件名的可用宽度
    // 固定列总宽度：ID + TIME + STATUS + 4个间隙
    int fixed_display_width = ID_WIDTH + TIME_WIDTH + STATUS_WIDTH + 4 * GAP;
    int remaining = term_width - fixed_display_width;
    
    // 路径和文件名至少各10个字符
    int path_width, name_width;
    if (remaining >= 30) {
        path_width = remaining * 55 / 100;
        name_width = remaining - path_width;
    } else {
        path_width = 15;
        name_width = (remaining > 15) ? remaining - 15 : 15;
    }
    
    printf("\n可恢复项目列表:\n");
    
    // 打印表头 - 全部使用 print_field 确保宽度计算一致
    print_field("ID", ID_WIDTH);
    printf(" ");
    print_field("删除时间", TIME_WIDTH);
    printf(" ");
    print_field("原路径", path_width);
    printf(" ");
    print_field("文件名", name_width);
    printf(" ");
    print_field("状态", STATUS_WIDTH);
    printf("\n");
    
    print_separator(term_width);
    
    for (i = 0; i < count; i++) {
        const char *status_str = items[i].is_archived ? "已归档" : "在桶中";
        char *short_path = malloc(path_width + 4);
        char *short_name = malloc(name_width + 4);
        char id_str[16];
        
        if (!short_path || !short_name) {
            free(short_path);
            free(short_name);
            continue;
        }
        
        // 截断路径（按字节长度截断）
        size_t path_len = strlen(items[i].original_path);
        if (path_len > (size_t)path_width) {
            int suffix_len = path_width - 3;
            if (suffix_len < 1) suffix_len = path_width;
            snprintf(short_path, path_width + 1, "...%s",
                     items[i].original_path + path_len - suffix_len);
        } else {
            strncpy(short_path, items[i].original_path, path_width + 1);
            short_path[path_width] = '\0';
        }
        
        // 截断文件名
        size_t name_len = strlen(items[i].filename);
        if (name_len > (size_t)name_width) {
            int prefix_len = name_width - 3;
            if (prefix_len < 1) prefix_len = name_width;
            snprintf(short_name, name_width + 1, "%.*s...", prefix_len, items[i].filename);
        } else {
            strncpy(short_name, items[i].filename, name_width + 1);
            short_name[name_width] = '\0';
        }
        
        // 打印数据行 - 全部使用 print_field
        snprintf(id_str, sizeof(id_str), "%d", items[i].id);
        print_field(id_str, ID_WIDTH);
        printf(" ");
        print_field(items[i].timestamp, TIME_WIDTH);
        printf(" ");
        print_field(short_path, path_width);
        printf(" ");
        print_field(short_name, name_width);
        printf(" ");
        print_field(status_str, STATUS_WIDTH);
        printf("\n");
        
        free(short_path);
        free(short_name);
    }
    
    print_separator(term_width);
    printf("共 %d 个项目可恢复\n", count);
}

// 释放恢复项目列表
void free_restore_items(RestoreItem *items, int count) {
    (void)count;
    free(items);
}

// ========== 永久删除功能（从回收站彻底删除） ==========

// 从活跃目录删除项目
static int delete_from_active(const char *timestamp, const char *filename) {
    char *rubbish_bin = get_rubbish_bin_path();
    char timestamp_dir[MAX_PATH_LEN];
    DIR *dp;
    struct dirent *entry;
    int result = -1;
    
    snprintf(timestamp_dir, sizeof(timestamp_dir), "%s/%s", rubbish_bin, timestamp);
    
    dp = opendir(timestamp_dir);
    if (dp == NULL) {
        return -1;
    }
    
    while ((entry = readdir(dp)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        char rubbish_dir[MAX_PATH_LEN];
        snprintf(rubbish_dir, sizeof(rubbish_dir),
                 "%s/%s/rubbish/%s", timestamp_dir, entry->d_name, filename);
        
        struct stat st;
        if (stat(rubbish_dir, &st) == 0) {
            // 找到文件，删除它
            if (S_ISDIR(st.st_mode)) {
                // 递归删除目录
                char *const paths[] = {rubbish_dir, NULL};
                redel3(paths);
            } else {
                unlink(rubbish_dir);
            }
            // 尝试清理空目录
            char idx_dir[MAX_PATH_LEN];
            snprintf(idx_dir, sizeof(idx_dir), "%s/%s", timestamp_dir, entry->d_name);
            
            char rubbish_parent[MAX_PATH_LEN];
            snprintf(rubbish_parent, sizeof(rubbish_parent), "%s/rubbish", idx_dir);
            rmdir(rubbish_parent);
            rmdir(idx_dir);
            
            result = 0;
            break;
        }
    }
    
    closedir(dp);
    
    // 如果时间戳目录为空，删除它
    DIR *check_dp = opendir(timestamp_dir);
    if (check_dp != NULL) {
        int empty = 1;
        struct dirent *check_entry;
        while ((check_entry = readdir(check_dp)) != NULL) {
            if (strcmp(check_entry->d_name, ".") != 0 && 
                strcmp(check_entry->d_name, "..") != 0) {
                empty = 0;
                break;
            }
        }
        closedir(check_dp);
        if (empty) {
            rmdir(timestamp_dir);
        }
    }
    
    return result;
}

// 递归添加目录内容到 tar（辅助函数）
static int tar_add_directory_recursive(const char *tarfile, const char *dirpath, const char *entry_prefix) {
    DIR *dp = opendir(dirpath);
    if (!dp) return -1;
    
    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        char fullpath[MAX_PATH_LEN];
        char entryname[MAX_PATH_LEN];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, entry->d_name);
        snprintf(entryname, sizeof(entryname), "%s/%s", entry_prefix, entry->d_name);
        
        struct stat st;
        if (stat(fullpath, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                if (tar_add_directory_recursive(tarfile, fullpath, entryname) != 0) {
                    closedir(dp);
                    return -1;
                }
            } else {
                if (tar_add_file(tarfile, fullpath, entryname) != 0) {
                    closedir(dp);
                    return -1;
                }
            }
        }
    }
    closedir(dp);
    return 0;
}

// 从归档中删除项目
int delete_from_archive(const char *archive_path, const char *item_timestamp,
                        const char *filename) {
    char temp_dir[MAX_PATH_LEN];
    char extract_dir[MAX_PATH_LEN];
    char new_archive[MAX_PATH_LEN];
    int found = 0;
    
    // 创建临时目录
    const char *tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL) tmpdir = "/tmp";
    snprintf(temp_dir, sizeof(temp_dir), "%s/safe_rm_delete_%d", tmpdir, getpid());
    
    if (mkdir(temp_dir, 0755) != 0) {
        perror("创建临时目录失败");
        return -1;
    }
    
    // 先解压归档（支持 .tar.gz）
    if (tgz_extract(archive_path, temp_dir) != 0) {
        fprintf(stderr, "解压归档失败\n");
        char *const dir[] = {temp_dir, NULL};
        redel3(dir);
        return -1;
    }
    
    // 查找并删除目标文件
    snprintf(extract_dir, sizeof(extract_dir), "%s/%s", temp_dir, item_timestamp);
    
    DIR *dp = opendir(extract_dir);
    if (dp == NULL) {
        fprintf(stderr, "无法打开解压目录\n");
        char *const dir[] = {temp_dir, NULL};
        redel3(dir);
        return -1;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        char rubbish_dir[MAX_PATH_LEN];
        snprintf(rubbish_dir, sizeof(rubbish_dir), "%s/%s/rubbish/%s", 
                 extract_dir, entry->d_name, filename);
        
        struct stat st;
        if (stat(rubbish_dir, &st) == 0) {
            // 找到文件，删除它
            if (S_ISDIR(st.st_mode)) {
                char *const paths[] = {rubbish_dir, NULL};
                redel3(paths);
            } else {
                unlink(rubbish_dir);
            }
            found = 1;
            
            // 尝试清理空目录
            char idx_dir[MAX_PATH_LEN];
            snprintf(idx_dir, sizeof(idx_dir), "%s/%s", extract_dir, entry->d_name);
            
            char rubbish_parent[MAX_PATH_LEN];
            snprintf(rubbish_parent, sizeof(rubbish_parent), "%s/rubbish", idx_dir);
            rmdir(rubbish_parent);
            rmdir(idx_dir);
            
            break;
        }
    }
    closedir(dp);
    
    if (!found) {
        fprintf(stderr, "在归档中找不到文件 '%s'\n", filename);
        char *const dir[] = {temp_dir, NULL};
        redel3(dir);
        return -1;
    }
    
    // 检查时间戳目录是否为空
    int timestamp_empty = 1;
    DIR *check_dp = opendir(extract_dir);
    if (check_dp != NULL) {
        struct dirent *check_entry;
        while ((check_entry = readdir(check_dp)) != NULL) {
            if (strcmp(check_entry->d_name, ".") != 0 && 
                strcmp(check_entry->d_name, "..") != 0) {
                timestamp_empty = 0;
                break;
            }
        }
        closedir(check_dp);
    }
    
    // 如果整个时间戳目录为空，删除整个时间戳目录
    if (timestamp_empty) {
        char *const dir[] = {extract_dir, NULL};
        redel3(dir);
    }
    
    // 重新打包归档
    snprintf(new_archive, sizeof(new_archive), "%s.new.tar.gz", archive_path);
    
    // 检查 temp_dir 中是否还有内容
    int has_content = 0;
    DIR *root_dp = opendir(temp_dir);
    if (root_dp != NULL) {
        struct dirent *root_entry;
        while ((root_entry = readdir(root_dp)) != NULL) {
            if (strcmp(root_entry->d_name, ".") != 0 && 
                strcmp(root_entry->d_name, "..") != 0) {
                has_content = 1;
                break;
            }
        }
        closedir(root_dp);
    }
    
    if (has_content) {
        // 重新创建归档
        // 创建临时 tar 文件
        char temp_tar[MAX_PATH_LEN];
        snprintf(temp_tar, sizeof(temp_tar), "%s/archive.tar", temp_dir);
        
        // 移除 temp_dir 前缀创建归档
        char work_dir[MAX_PATH_LEN];
        getcwd(work_dir, sizeof(work_dir));
        
        if (tar_create(temp_tar) != 0) {
            fprintf(stderr, "创建新归档失败\n");
            char *const dir[] = {temp_dir, NULL};
            redel3(dir);
            return -1;
        }
        
        // 添加所有内容到归档
        DIR *content_dp = opendir(temp_dir);
        if (content_dp != NULL) {
            struct dirent *content_entry;
            while ((content_entry = readdir(content_dp)) != NULL) {
                if (strcmp(content_entry->d_name, ".") == 0 || 
                    strcmp(content_entry->d_name, "..") == 0 ||
                    strcmp(content_entry->d_name, "archive.tar") == 0) {
                    continue;
                }
                
                char item_path[MAX_PATH_LEN];
                snprintf(item_path, sizeof(item_path), "%s/%s", temp_dir, content_entry->d_name);
                
                struct stat st;
                if (stat(item_path, &st) == 0) {
                    if (S_ISDIR(st.st_mode)) {
                        tar_add_directory_recursive(temp_tar, item_path, content_entry->d_name);
                    } else {
                        tar_add_file(temp_tar, item_path, content_entry->d_name);
                    }
                }
            }
            closedir(content_dp);
        }
        
        tar_close_archive(temp_tar);
        
        // 压缩 tar 文件
        if (gzip_compress_file(temp_tar, new_archive, 6) != 0) {
            fprintf(stderr, "压缩归档失败\n");
            unlink(temp_tar);
            char *const dir[] = {temp_dir, NULL};
            redel3(dir);
            return -1;
        }
        unlink(temp_tar);
        
        // 替换旧归档
        if (rename(new_archive, archive_path) != 0) {
            perror("替换归档文件失败");
            unlink(new_archive);
            char *const dir[] = {temp_dir, NULL};
            redel3(dir);
            return -1;
        }
    } else {
        // 没有内容了，直接删除归档文件
        unlink(archive_path);
    }
    
    // 清理临时目录
    char *const dir[] = {temp_dir, NULL};
    redel3(dir);
    
    return 0;
}

// 按ID删除项目
int delete_item_by_id(int id, RestoreItem *items, int count) {
    for (int i = 0; i < count; i++) {
        if (items[i].id == id) {
            if (items[i].is_archived) {
                return delete_from_archive(items[i].archive_file, items[i].timestamp,
                                          items[i].filename);
            } else {
                return delete_from_active(items[i].timestamp, items[i].filename);
            }
        }
    }
    
    fprintf(stderr, "错误: 找不到ID为 %d 的项目\n", id);
    return -1;
}

// 批量删除项目
int delete_items_batch(int *ids, int id_count, RestoreItem *items, int count) {
    int success = 0;
    
    for (int i = 0; i < id_count; i++) {
        if (delete_item_by_id(ids[i], items, count) == 0) {
            success++;
        }
    }
    
    return success;
}
