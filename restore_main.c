#include "safe_rm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void print_restore_help(const char *program_name) {
    printf("restore - 回收站管理工具（恢复/删除）\n\n");
    printf("用法:\n");
    printf("  %s [选项] [ID...]\n\n", program_name);
    printf("选项:\n");
    printf("  -l, --list           列出所有可恢复的项目\n");
    printf("  -a, --all            恢复所有项目\n");
    printf("  -d, --delete         删除指定的项目（永久删除）\n");
    printf("  --delete-all         删除所有项目（清空回收站）\n");
    printf("  -h, --help           显示此帮助信息\n");
    printf("  -v, --version        显示版本信息\n\n");
    printf("示例:\n");
    printf("  %s -l                列出所有可恢复项目\n", program_name);
    printf("  %s 1                 恢复ID为1的项目\n", program_name);
    printf("  %s 1 2 3             恢复ID为1、2、3的项目\n", program_name);
    printf("  %s -a                恢复所有项目\n", program_name);
    printf("  %s -d 1 2            永久删除ID为1、2的项目\n", program_name);
    printf("  %s --delete-all      清空回收站\n", program_name);
}

// 确认对话框
static int confirm_action(const char *action, int count) {
    printf("\n确认要%s %d 个项目? [y/N]: ", action, count);
    fflush(stdout);
    
    char buf[16];
    if (fgets(buf, sizeof(buf), stdin) != NULL) {
        return (buf[0] == 'y' || buf[0] == 'Y');
    }
    return 0;
}

int main(int argc, char *argv[]) {
    RestoreItem *items = NULL;
    int count = 0;
    int i;
    int list_mode = 0;
    int all_mode = 0;
    int delete_mode = 0;
    int delete_all_mode = 0;
    int id_count = 0;
    int success_count = 0;
    int *ids = NULL;
    
    // 检查参数
    if (argc < 2) {
        // 默认列出所有项目
        list_mode = 1;
    } else {
        // 解析选项
        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
                print_restore_help(argv[0]);
                return 0;
            }
            
            if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
                printf("restore version %s\n", VERSION);
                return 0;
            }
            
            if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--list") == 0) {
                list_mode = 1;
            } else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--all") == 0) {
                all_mode = 1;
            } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--delete") == 0) {
                delete_mode = 1;
            } else if (strcmp(argv[i], "--delete-all") == 0) {
                delete_all_mode = 1;
            } else if (argv[i][0] == '-') {
                fprintf(stderr, "错误: 未知选项 '%s'\n", argv[i]);
                print_restore_help(argv[0]);
                return 1;
            } else {
                // 认为是ID
                id_count++;
            }
        }
    }
    
    // 扫描可恢复项目
    if (scan_restore_items(&items, &count) != 0) {
        fprintf(stderr, "错误: 扫描回收站失败\n");
        return 1;
    }
    
    // 列表模式
    if (list_mode) {
        print_restore_list(items, count);
        free_restore_items(items, count);
        return 0;
    }
    
    // 全部恢复模式
    if (all_mode) {
        if (count == 0) {
            printf("回收站为空，没有可恢复的项目\n");
            free_restore_items(items, count);
            return 0;
        }
        
        if (!confirm_action("恢复", count)) {
            printf("已取消\n");
            free_restore_items(items, count);
            return 0;
        }
        
        printf("正在恢复所有 %d 个项目...\n", count);
        for (i = 0; i < count; i++) {
            if (restore_item_by_id(items[i].id, items, count) == 0) {
                success_count++;
            }
        }
        printf("\n成功恢复 %d/%d 个项目\n", success_count, count);
        free_restore_items(items, count);
        return 0;
    }
    
    // 全部删除模式（清空回收站）
    if (delete_all_mode) {
        if (count == 0) {
            printf("回收站为空\n");
            free_restore_items(items, count);
            return 0;
        }
        
        printf("\n" COLOR_RED "警告: 此操作将永久删除所有 %d 个项目，无法恢复!" COLOR_RESET "\n", count);
        if (!confirm_action("永久删除", count)) {
            printf("已取消\n");
            free_restore_items(items, count);
            return 0;
        }
        
        printf("正在删除所有 %d 个项目...\n", count);
        for (i = 0; i < count; i++) {
            if (delete_item_by_id(items[i].id, items, count) == 0) {
                success_count++;
                printf("  已删除: %s\n", items[i].filename);
            }
        }
        printf("\n成功删除 %d/%d 个项目\n", success_count, count);
        free_restore_items(items, count);
        return 0;
    }
    
    // 按ID删除模式
    if (delete_mode) {
        if (id_count == 0) {
            fprintf(stderr, "错误: 请指定要删除的ID\n");
            print_restore_help(argv[0]);
            free_restore_items(items, count);
            return 1;
        }
        
        // 收集ID
        ids = malloc(id_count * sizeof(int));
        if (!ids) {
            fprintf(stderr, "错误: 内存分配失败\n");
            free_restore_items(items, count);
            return 1;
        }
        
        int idx = 0;
        for (i = 1; i < argc; i++) {
            if (argv[i][0] != '-') {
                int id = atoi(argv[i]);
                if (id > 0) {
                    ids[idx++] = id;
                } else {
                    fprintf(stderr, "错误: 无效的ID '%s'\n", argv[i]);
                }
            }
        }
        
        printf("\n" COLOR_RED "警告: 此操作将永久删除 %d 个项目，无法恢复!" COLOR_RESET "\n", idx);
        if (!confirm_action("永久删除", idx)) {
            printf("已取消\n");
            free(ids);
            free_restore_items(items, count);
            return 0;
        }
        
        printf("正在删除 %d 个项目...\n", idx);
        for (i = 0; i < idx; i++) {
            // 查找文件名用于显示
            char *filename = NULL;
            for (int j = 0; j < count; j++) {
                if (items[j].id == ids[i]) {
                    filename = items[j].filename;
                    break;
                }
            }
            
            if (delete_item_by_id(ids[i], items, count) == 0) {
                success_count++;
                printf("  已删除 ID=%d: %s\n", ids[i], filename ? filename : "未知");
            } else {
                printf("  删除失败 ID=%d\n", ids[i]);
            }
        }
        printf("\n成功删除 %d/%d 个项目\n", success_count, idx);
        
        free(ids);
        free_restore_items(items, count);
        return 0;
    }
    
    // 按ID恢复
    if (id_count > 0) {
        // 收集ID
        ids = malloc(id_count * sizeof(int));
        if (!ids) {
            fprintf(stderr, "错误: 内存分配失败\n");
            free_restore_items(items, count);
            return 1;
        }
        
        int idx = 0;
        for (i = 1; i < argc; i++) {
            if (argv[i][0] != '-') {
                int id = atoi(argv[i]);
                if (id > 0) {
                    ids[idx++] = id;
                } else {
                    fprintf(stderr, "错误: 无效的ID '%s'\n", argv[i]);
                }
            }
        }
        
        if (!confirm_action("恢复", idx)) {
            printf("已取消\n");
            free(ids);
            free_restore_items(items, count);
            return 0;
        }
        
        printf("正在恢复 %d 个项目...\n", idx);
        for (i = 0; i < idx; i++) {
            if (restore_item_by_id(ids[i], items, count) == 0) {
                success_count++;
            }
        }
        printf("\n成功恢复 %d/%d 个项目\n", success_count, idx);
        
        free(ids);
    }
    
    free_restore_items(items, count);
    return 0;
}
