#include "zrestore.h"
#include "safe_rm.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <locale.h>
#include <string.h>
#include <ncurses.h>
#include <unistd.h>

// 信号处理
static volatile sig_atomic_t got_sigint = 0;

static void sigint_handler(int sig) {
    (void)sig;
    got_sigint = 1;
}

// 打印普通版本的帮助
static void print_cli_help(const char *program_name) {
    printf("zrestore - 交互式回收站管理工具 (ncurses版)\n\n");
    printf("用法:\n");
    printf("  %s [选项]\n\n", program_name);
    printf("选项:\n");
    printf("  -l, --list           使用普通列表模式（非ncurses）\n");
    printf("  -i, --interactive    启动交互式ncurses界面（默认）\n");
    printf("  -h, --help           显示此帮助信息\n");
    printf("  -v, --version        显示版本信息\n\n");
    printf("交互式模式快捷键:\n");
    printf("  ↑/↓ 或 k/j          上下移动选择\n");
    printf("  Space                选择/取消选择当前项\n");
    printf("  a                    全选所有匹配项\n");
    printf("  A                    取消全选\n");
    printf("  Enter                恢复选中的项目\n");
    printf("  r                    恢复所有选中的项目\n");
    printf("  R                    恢复所有可见项目\n");
    printf("  d                    删除选中的项目（永久删除）\n");
    printf("  D                    删除所有可见项目\n");
    printf("  /                    搜索过滤\n");
    printf("  Tab                  切换搜索模式（精确/模糊）\n");
    printf("  Esc                  清除搜索\n");
    printf("  F1/?                 显示帮助\n");
    printf("  Q                    退出\n\n");
    printf("搜索模式:\n");
    printf("  精确模式 - 子串匹配，匹配的字符会高亮显示\n");
    printf("  模糊模式 - 相似度匹配(>60%%)，按相似度排序\n\n");
    printf("示例:\n");
    printf("  %s                   启动交互式界面\n", program_name);
    printf("  %s -l                使用普通列表模式\n", program_name);
}

// 使用普通列表模式（调用原有的restore功能）
static int run_cli_list_mode(void) {
    RestoreItem *items = NULL;
    int count = 0;
    
    if (scan_restore_items(&items, &count) != 0) {
        fprintf(stderr, "错误: 扫描回收站失败\n");
        return 1;
    }
    
    print_restore_list(items, count);
    free_restore_items(items, count);
    return 0;
}

// 恢复单个项目（静默模式，用于批量恢复）
static int restore_single_item_silent(RestoreItem *items, int count, int id) {
    return restore_item_by_id(id, items, count);
}

// 删除单个项目（静默模式，用于批量删除）
static int delete_single_item_silent(RestoreItem *items, int count, int id) {
    return delete_item_by_id(id, items, count);
}

// 运行交互式ncurses模式
static int run_interactive_mode(void) {
    RestoreItem *items = NULL;
    int count = 0;
    int result = 0;
    
    // 扫描项目
    if (scan_restore_items(&items, &count) != 0) {
        fprintf(stderr, "错误: 扫描回收站失败\n");
        return 1;
    }
    
    if (count == 0) {
        printf("回收站为空，没有可恢复的项目\n");
        free_restore_items(items, count);
        return 0;
    }
    
    // 初始化ncurses
    zrestore_init();
    zrestore_set_items(items, count);
    
    // 设置信号处理
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    
    // 主循环
    int running = 1;
    int need_rescan = 0;
    int action_for_delete_all = 0;  // 标记是否是删除所有操作
    
    while (running && !got_sigint) {
        zrestore_draw();
        
        zrestore_action_t action = zrestore_handle_input();
        
        // 如果用户取消了操作（ACTION_NONE），重置删除所有标记
        if (action == ACTION_NONE) {
            action_for_delete_all = 0;
        }
        
        // 检查是否需要重新扫描（在上次操作后）
        if (need_rescan) {
            free_restore_items(items, count);
            items = NULL;
            count = 0;
            if (scan_restore_items(&items, &count) != 0) {
                fprintf(stderr, "错误: 重新扫描失败\n");
                zrestore_cleanup();
                return 1;
            }
            if (count == 0) {
                zrestore_cleanup();
                printf("回收站已空\n");
                return 0;
            }
            zrestore_set_items(items, count);
            need_rescan = 0;
            continue;
        }
        
        switch (action) {
            case ACTION_QUIT:
                running = 0;
                break;
            
            case ACTION_RESTORE_SELECTED: {
                int id = zrestore_get_selected_id();
                if (id > 0) {
                    // 显示进度
                    zrestore_show_progress(" 正在恢复 ", "请稍候...", 1, 1);
                    
                    int result = restore_single_item_silent(items, count, id);
                    
                    napms(300);  // 短暂显示进度
                    
                    zrestore_hide_progress();
                    
                    zrestore_set_status(result == 0 ? "恢复成功" : "恢复失败", result == 0 ? 0 : 1);
                    
                    need_rescan = 1;
                }
                break;
            }
            
            case ACTION_RESTORE_SELECTED_MULTI: {
                int selected_count;
                int *selected_ids = zrestore_get_selected_ids(&selected_count);
                
                if (selected_count > 0) {
                    int success = 0;
                    
                    for (int i = 0; i < selected_count; i++) {
                        // 显示进度
                        zrestore_show_progress(" 正在恢复 ", "请稍候...", i + 1, selected_count);
                        
                        if (restore_single_item_silent(items, count, selected_ids[i]) == 0) {
                            success++;
                        }
                        
                        // 短暂延迟以便用户看到进度
                        napms(100);
                    }
                    
                    // 隐藏进度条
                    zrestore_hide_progress();
                    
                    // 显示结果
                    char result_msg[256];
                    snprintf(result_msg, sizeof(result_msg), "成功恢复 %d/%d 个项目", success, selected_count);
                    zrestore_set_status(result_msg, success == selected_count ? 0 : 1);
                    
                    need_rescan = 1;
                }
                break;
            }
            
            case ACTION_RESTORE_ALL: {
                int filtered = zrestore_get_filtered_count();
                if (filtered > 0) {
                    const char *filter = zrestore_get_filter();
                    int success = 0;
                    int total = 0;
                    
                    // 先计算需要恢复的项目
                    int *ids_to_restore = malloc(filtered * sizeof(int));
                    if (!ids_to_restore) break;
                    
                    for (int i = 0; i < count; i++) {
                        int should_restore = 0;
                        
                        if (filter[0] == '\0') {
                            should_restore = 1;
                        } else {
                            if (zrestore_get_search_mode() == SEARCH_MODE_EXACT) {
                                if (strstr(items[i].filename, filter) != NULL ||
                                    strstr(items[i].original_path, filter) != NULL) {
                                    should_restore = 1;
                                }
                            } else {
                                should_restore = 1;
                            }
                        }
                        
                        if (should_restore) {
                            ids_to_restore[total++] = items[i].id;
                        }
                    }
                    
                    // 显示进度并恢复
                    for (int i = 0; i < total; i++) {
                        zrestore_show_progress(" 正在恢复 ", "请稍候...", i + 1, total);
                        
                        if (restore_single_item_silent(items, count, ids_to_restore[i]) == 0) {
                            success++;
                        }
                        
                        napms(100);
                    }
                    
                    free(ids_to_restore);
                    
                    zrestore_hide_progress();
                    
                    char result_msg[256];
                    snprintf(result_msg, sizeof(result_msg), "成功恢复 %d/%d 个项目", success, total);
                    zrestore_set_status(result_msg, success == total ? 0 : 1);
                    
                    need_rescan = 1;
                }
                break;
            }
            
            case ACTION_DELETE_SELECTED: {
                int selected_count;
                int *selected_ids = zrestore_get_selected_ids(&selected_count);
                
                if (action_for_delete_all) {
                    // 删除所有可见
                    int filtered = zrestore_get_filtered_count();
                    if (filtered > 0) {
                        int success = 0;
                        int total = 0;
                        const char *filter = zrestore_get_filter();
                        
                        // 先计算需要删除的项目
                        int *ids_to_delete = malloc(filtered * sizeof(int));
                        if (!ids_to_delete) break;
                        
                        for (int i = 0; i < count; i++) {
                            int should_delete = 0;
                            
                            if (filter[0] == '\0') {
                                should_delete = 1;
                            } else {
                                if (zrestore_get_search_mode() == SEARCH_MODE_EXACT) {
                                    if (strstr(items[i].filename, filter) != NULL ||
                                        strstr(items[i].original_path, filter) != NULL) {
                                        should_delete = 1;
                                    }
                                } else {
                                    should_delete = 1;
                                }
                            }
                            
                            if (should_delete) {
                                ids_to_delete[total++] = items[i].id;
                            }
                        }
                        
                        // 显示进度并删除
                        for (int i = 0; i < total; i++) {
                            zrestore_show_progress(" 正在删除 ", "请稍候...", i + 1, total);
                            
                            if (delete_single_item_silent(items, count, ids_to_delete[i]) == 0) {
                                success++;
                            }
                            
                            napms(100);
                        }
                        
                        free(ids_to_delete);
                        
                        zrestore_hide_progress();
                        
                        char result_msg[256];
                        snprintf(result_msg, sizeof(result_msg), "成功删除 %d/%d 个项目", success, total);
                        zrestore_set_status(result_msg, success == total ? 0 : 1);
                        
                        need_rescan = 1;
                    }
                    action_for_delete_all = 0;
                } else if (selected_count > 0) {
                    // 删除选中的
                    int success = 0;
                    
                    for (int i = 0; i < selected_count; i++) {
                        zrestore_show_progress(" 正在删除 ", "请稍候...", i + 1, selected_count);
                        
                        if (delete_single_item_silent(items, count, selected_ids[i]) == 0) {
                            success++;
                        }
                        
                        napms(100);
                    }
                    
                    zrestore_hide_progress();
                    
                    char result_msg[256];
                    snprintf(result_msg, sizeof(result_msg), "成功删除 %d/%d 个项目", success, selected_count);
                    zrestore_set_status(result_msg, success == selected_count ? 0 : 1);
                    
                    need_rescan = 1;
                }
                break;
            }
            
            case ACTION_DELETE_ALL:
                // 标记为删除所有，实际处理在 ACTION_DELETE_SELECTED 中
                action_for_delete_all = 1;
                break;
            
            case ACTION_SHOW_DETAILS: {
                int id = zrestore_get_selected_id();
                if (id > 0) {
                    for (int i = 0; i < count; i++) {
                        if (items[i].id == id) {
                            zrestore_cleanup();
                            printf("\n========== 项目详情 ==========\n");
                            printf("ID:         %d\n", items[i].id);
                            printf("文件名:     %s\n", items[i].filename);
                            printf("原路径:     %s\n", items[i].original_path);
                            printf("删除时间:   %s\n", items[i].timestamp);
                            printf("状态:       %s\n", items[i].is_archived ? "已归档" : "活跃");
                            if (items[i].is_archived) {
                                printf("归档文件:   %s\n", items[i].archive_file);
                            }
                            printf("==============================\n");
                            printf("\n按 Enter 继续...");
                            fflush(stdout);
                            (void)getchar();
                            
                            zrestore_init();
                            break;
                        }
                    }
                }
                break;
            }
            
            default:
                break;
        }
    }
    
    zrestore_cleanup();
    free_restore_items(items, count);
    
    return result;
}

int main(int argc, char *argv[]) {
    int list_mode = 0;
    int interactive_mode = 0;
    
    // 解析参数
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
                print_cli_help(argv[0]);
                return 0;
            }
            
            if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
                printf("zrestore version %s\n", VERSION);
                return 0;
            }
            
            if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--list") == 0) {
                list_mode = 1;
            } else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interactive") == 0) {
                interactive_mode = 1;
            } else {
                fprintf(stderr, "错误: 未知选项 '%s'\n", argv[i]);
                print_cli_help(argv[0]);
                return 1;
            }
        }
    } else {
        // 默认使用交互式模式
        interactive_mode = 1;
    }
    
    // 检查终端是否支持ncurses
    if (interactive_mode) {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
            fprintf(stderr, "警告: 非交互式终端，切换到列表模式\n");
            list_mode = 1;
            interactive_mode = 0;
        }
    }
    
    if (list_mode) {
        return run_cli_list_mode();
    } else {
        return run_interactive_mode();
    }
}
