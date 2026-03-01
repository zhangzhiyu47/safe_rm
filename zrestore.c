#include "zrestore.h"
#include "safe_rm.h"
#include <ncurses.h>
#include <locale.h>
#include <time.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

// 颜色对定义 - htop风格柔和配色
#define COLOR_HEADER       1
#define COLOR_SELECTED     2
#define COLOR_ACTIVE       3
#define COLOR_ARCHIVED     4
#define COLOR_FOOTER       5
#define COLOR_LABEL        6
#define COLOR_BORDER       7
#define COLOR_STATUS_OK    8
#define COLOR_STATUS_WARN  9
#define COLOR_TEXT         10
#define COLOR_TITLE        11
#define COLOR_MATCH        12   // 搜索匹配高亮
#define COLOR_SELECTED_MARK 13  // 选中标记
#define COLOR_DIALOG_BG    14   // 对话框背景
#define COLOR_DIALOG_BTN   15   // 对话框按钮
#define COLOR_FUZZY_MATCH  16   // 模糊匹配高亮
#define COLOR_MATCH_SEL    17   // 选中行的匹配高亮（保持选中背景）
#define COLOR_DELETE_WARN  18   // 删除警告颜色（红色）
#define COLOR_DIALOG_WARN  19   // 删除确认对话框标题

// UI状态
static zrestore_ui_t ui_state;
static RestoreItem *g_items = NULL;
static int g_count = 0;
static filtered_item_t g_filtered[1024];
static int g_filtered_count = 0;

// 确认对话框状态
static int g_confirm_result = -1;  // -1=未选择, 0=否, 1=是
static const char *g_confirm_title = NULL;
static const char *g_confirm_message = NULL;
static int g_confirm_is_delete = 0;  // 0=普通确认, 1=删除确认（红色警告）

// 计算两个字符串的最长公共子序列相似度 (0-100)
static int fuzzy_match_score(const char *pattern, const char *text) {
    if (!pattern || !text) return 0;
    if (pattern[0] == '\0') return 100;
    
    int pattern_len = strlen(pattern);
    int text_len = strlen(text);
    
    if (text_len == 0) return 0;
    
    // 转换为小写进行比较
    char *p = strdup(pattern);
    char *t = strdup(text);
    for (int i = 0; p[i]; i++) p[i] = tolower((unsigned char)p[i]);
    for (int i = 0; t[i]; i++) t[i] = tolower((unsigned char)t[i]);
    
    // 如果直接包含，给高分
    if (strstr(t, p) != NULL) {
        free(p);
        free(t);
        return 100;
    }
    
    // 计算编辑距离（Levenshtein距离）
    int **dp = (int **)malloc((pattern_len + 1) * sizeof(int *));
    for (int i = 0; i <= pattern_len; i++) {
        dp[i] = (int *)malloc((text_len + 1) * sizeof(int));
    }
    
    for (int i = 0; i <= pattern_len; i++) dp[i][0] = i;
    for (int j = 0; j <= text_len; j++) dp[0][j] = j;
    
    for (int i = 1; i <= pattern_len; i++) {
        for (int j = 1; j <= text_len; j++) {
            int cost = (p[i-1] == t[j-1]) ? 0 : 1;
            int deletion = dp[i-1][j] + 1;
            int insertion = dp[i][j-1] + 1;
            int substitution = dp[i-1][j-1] + cost;
            
            dp[i][j] = deletion;
            if (insertion < dp[i][j]) dp[i][j] = insertion;
            if (substitution < dp[i][j]) dp[i][j] = substitution;
        }
    }
    
    int distance = dp[pattern_len][text_len];
    
    for (int i = 0; i <= pattern_len; i++) {
        free(dp[i]);
    }
    free(dp);
    free(p);
    free(t);
    
    // 计算相似度 (0-100)
    int max_len = pattern_len > text_len ? pattern_len : text_len;
    if (max_len == 0) return 100;
    
    int score = 100 - (distance * 100 / max_len);
    return score < 0 ? 0 : score;
}

// 检查是否匹配（根据搜索模式）
static int check_match(const char *filter, RestoreItem *item, int *score) {
    if (filter[0] == '\0') {
        if (score) *score = 100;
        return 1;
    }
    
    if (ui_state.search_mode == SEARCH_MODE_EXACT) {
        // 精确匹配：子串匹配
        if (strstr(item->filename, filter) != NULL ||
            strstr(item->original_path, filter) != NULL) {
            if (score) *score = 100;
            return 1;
        }
        if (score) *score = 0;
        return 0;
    } else {
        // 模糊匹配
        int name_score = fuzzy_match_score(filter, item->filename);
        int path_score = fuzzy_match_score(filter, item->original_path);
        int max_score = name_score > path_score ? name_score : path_score;
        
        if (score) *score = max_score;
        return max_score >= FUZZY_THRESHOLD;
    }
}

// 更新过滤列表
static void update_filtered_list(void) {
    g_filtered_count = 0;
    ui_state.selected_count = 0;
    
    // 从后往前遍历，让最新的（ID大的）显示在上面
    for (int i = g_count - 1; i >= 0 && g_filtered_count < 1024; i--) {
        int score = 0;
        if (check_match(ui_state.filter, &g_items[i], &score)) {
            g_filtered[g_filtered_count].original_idx = i;
            g_filtered[g_filtered_count].is_selected = 0;
            g_filtered[g_filtered_count].match_score = score;
            g_filtered_count++;
        }
    }
    
    // 模糊匹配模式下按相似度排序
    if (ui_state.search_mode == SEARCH_MODE_FUZZY && ui_state.filter[0] != '\0') {
        for (int i = 0; i < g_filtered_count - 1; i++) {
            for (int j = i + 1; j < g_filtered_count; j++) {
                if (g_filtered[j].match_score > g_filtered[i].match_score) {
                    filtered_item_t temp = g_filtered[i];
                    g_filtered[i] = g_filtered[j];
                    g_filtered[j] = temp;
                }
            }
        }
    }
    
    // 调整选中位置
    if (ui_state.selected >= g_filtered_count) {
        ui_state.selected = (g_filtered_count > 0) ? g_filtered_count - 1 : 0;
    }
}

// 初始化颜色方案
static void init_colors(void) {
    start_color();
    use_default_colors();
    
    if (!has_colors()) {
        return;
    }
    
    // 标题栏 - 深灰蓝背景，浅灰文字
    init_pair(COLOR_HEADER, 252, 24);
    
    // 选中行 - 浅灰背景，深灰文字
    init_pair(COLOR_SELECTED, 238, 253);
    
    // 活跃状态 - 柔和的绿色
    init_pair(COLOR_ACTIVE, 65, -1);
    
    // 归档状态 - 柔和的黄色/棕色
    init_pair(COLOR_ARCHIVED, 136, -1);
    
    // 底部栏
    init_pair(COLOR_FOOTER, 252, 24);
    
    // 标签/高亮 - 柔和的青色
    init_pair(COLOR_LABEL, 66, -1);
    
    // 边框 - 中灰色
    init_pair(COLOR_BORDER, 244, -1);
    
    // 成功状态 - 柔和绿色
    init_pair(COLOR_STATUS_OK, 64, -1);
    
    // 警告状态 - 柔和橙色
    init_pair(COLOR_STATUS_WARN, 130, -1);
    
    // 普通文本 - 浅灰色
    init_pair(COLOR_TEXT, 250, -1);
    
    // 标题高亮 - 柔和的蓝色
    init_pair(COLOR_TITLE, 67, -1);
    
    // 搜索匹配高亮 - 柔和的琥珀色前景+淡黄色背景（更护眼）
    init_pair(COLOR_MATCH, 94, 230);
    
    // 选中标记 - 柔和的深绿色
    init_pair(COLOR_SELECTED_MARK, 22, -1);
    
    // 对话框背景
    init_pair(COLOR_DIALOG_BG, 250, 237);
    
    // 对话框按钮
    init_pair(COLOR_DIALOG_BTN, 255, 24);
    
    // 模糊匹配高亮 - 青色
    init_pair(COLOR_FUZZY_MATCH, 51, -1);
    
    // 选中行的匹配高亮 - 琥珀色前景，保持选中行背景
    init_pair(COLOR_MATCH_SEL, 130, 253);
    
    // 删除警告颜色 - 柔和的红色
    init_pair(COLOR_DELETE_WARN, 160, -1);
    
    // 删除确认对话框标题 - 红色背景
    init_pair(COLOR_DIALOG_WARN, 255, 160);
}

// 绘制水平分隔线
static void draw_hline(int y, int x, int width, chtype ch) {
    mvhline(y, x, ch, width);
}

// 绘制垂直分隔线
static void draw_vline(int y, int x, int height, chtype ch) {
    mvvline(y, x, ch, height);
}

// 绘制带高亮的字符串（高亮匹配的字符）
static void draw_str_highlighted(int y, int x, const char *str, const char *filter, 
                                  int max_width, int is_selected) {
    if (!filter || filter[0] == '\0' || ui_state.search_mode == SEARCH_MODE_FUZZY) {
        // 没有过滤条件或者是模糊匹配模式，直接普通绘制
        int len = strlen(str);
        if (len <= max_width) {
            mvaddnstr(y, x, str, max_width);
        } else {
            int side_len = (max_width - 3) / 2;
            char buf[512];
            snprintf(buf, sizeof(buf), "%.*s...%s", 
                     side_len, str, str + len - side_len);
            mvaddnstr(y, x, buf, max_width);
        }
        return;
    }
    
    // 精确匹配模式：高亮匹配的子串
    int str_len = strlen(str);
    int filter_len = strlen(filter);
    
    // 找到所有匹配位置
    int match_positions[256];
    int match_count = 0;
    
    char *lower_str = strdup(str);
    char *lower_filter = strdup(filter);
    for (int i = 0; lower_str[i]; i++) lower_str[i] = tolower((unsigned char)lower_str[i]);
    for (int i = 0; lower_filter[i]; i++) lower_filter[i] = tolower((unsigned char)lower_filter[i]);
    
    char *pos = lower_str;
    while ((pos = strstr(pos, lower_filter)) != NULL && match_count < 256) {
        match_positions[match_count++] = pos - lower_str;
        pos += filter_len;
    }
    
    free(lower_str);
    free(lower_filter);
    
    // 绘制字符串，高亮匹配部分
    int cx = x;
    int chars_drawn = 0;
    
    for (int i = 0; i < str_len && chars_drawn < max_width; i++) {
        int is_match = 0;
        for (int j = 0; j < match_count; j++) {
            if (i >= match_positions[j] && i < match_positions[j] + filter_len) {
                is_match = 1;
                break;
            }
        }
        
        if (is_match) {
            // 选中行使用特殊的高亮颜色（保持选中背景）
            if (is_selected) {
                attron(COLOR_PAIR(COLOR_MATCH_SEL));
            } else {
                attron(COLOR_PAIR(COLOR_MATCH));
            }
            mvaddch(y, cx++, str[i]);
            if (is_selected) {
                attroff(COLOR_PAIR(COLOR_MATCH_SEL));
            } else {
                attroff(COLOR_PAIR(COLOR_MATCH));
            }
        } else {
            if (is_selected) {
                attron(COLOR_PAIR(COLOR_SELECTED));
            } else {
                attron(COLOR_PAIR(COLOR_TEXT));
            }
            mvaddch(y, cx++, str[i]);
            if (is_selected) {
                attroff(COLOR_PAIR(COLOR_SELECTED));
            } else {
                attroff(COLOR_PAIR(COLOR_TEXT));
            }
        }
        chars_drawn++;
    }
    
    // 填充剩余空间
    if (is_selected) {
        attron(COLOR_PAIR(COLOR_SELECTED));
        for (int i = chars_drawn; i < max_width; i++) {
            mvaddch(y, cx++, ' ');
        }
        attroff(COLOR_PAIR(COLOR_SELECTED));
    }
}

// 绘制带截断的字符串
static void draw_str_truncated(int y, int x, const char *str, int max_width, int selected) {
    int len = strlen(str);
    if (len <= max_width) {
        mvaddstr(y, x, str);
        for (int i = len; i < max_width; i++) {
            addch(' ');
        }
    } else {
        int side_len = (max_width - 3) / 2;
        char buf[512];
        snprintf(buf, sizeof(buf), "%.*s...%s", 
                 side_len, str, str + len - side_len);
        mvaddnstr(y, x, buf, max_width);
    }
    if (selected) {
        int cx, cy;
        getyx(stdscr, cy, cx);
        (void)cy;
        for (int i = cx - x; i < max_width; i++) {
            addch(' ');
        }
    }
}

// 绘制边框
static void draw_border(void) {
    attron(COLOR_PAIR(COLOR_BORDER));
    draw_hline(0, 0, COLS, ACS_HLINE);
    mvaddch(0, 0, ACS_ULCORNER);
    mvaddch(0, COLS - 1, ACS_URCORNER);
    
    draw_hline(LINES - 2, 0, COLS, ACS_HLINE);
    mvaddch(LINES - 2, 0, ACS_LLCORNER);
    mvaddch(LINES - 2, COLS - 1, ACS_LRCORNER);
    
    draw_vline(1, 0, LINES - 3, ACS_VLINE);
    draw_vline(1, COLS - 1, LINES - 3, ACS_VLINE);
    
    attroff(COLOR_PAIR(COLOR_BORDER));
}

// 绘制标题栏
static void draw_header(void) {
    attron(COLOR_PAIR(COLOR_HEADER));
    for (int x = 1; x < COLS - 1; x++) {
        mvaddch(0, x, ' ');
    }
    
    char title[256];
    snprintf(title, sizeof(title), " zrestore v%s - 交互式文件恢复工具 ", VERSION);
    mvaddstr(0, 2, title);
    
    // 右上方显示项目统计和搜索模式
    char stats[64];
    const char *mode_str = (ui_state.search_mode == SEARCH_MODE_EXACT) ? "精确" : "模糊";
    snprintf(stats, sizeof(stats), " [%s] %d/%d 项 ", mode_str, g_filtered_count, g_count);
    int stats_x = COLS - strlen(stats) - 2;
    if (stats_x > 40) {
        mvaddstr(0, stats_x, stats);
    }
    attroff(COLOR_PAIR(COLOR_HEADER));
}

// 绘制表头
static void draw_column_headers(void) {
    int y = HEADER_HEIGHT;
    
    attron(COLOR_PAIR(COLOR_HEADER));
    for (int x = 1; x < COLS - 1; x++) {
        mvaddch(y, x, ' ');
    }
    
    mvaddch(y, 0, ACS_LTEE);
    draw_hline(y, 1, COLS - 2, ACS_HLINE);
    mvaddch(y, COLS - 1, ACS_RTEE);
    
    mvaddstr(y, 2, "☐");
    mvaddstr(y, 6, "ID");
    mvaddstr(y, 12, "删除时间");
    mvaddstr(y, 32, "状态");
    mvaddstr(y, 42, "原路径");
    
    int name_x = 42 + (COLS - 44) / 2;
    if (name_x < 55) name_x = 55;
    mvaddstr(y, name_x, "文件名");
    attroff(COLOR_PAIR(COLOR_HEADER));
}

// 绘制文件列表
static void draw_file_list(void) {
    int list_y = HEADER_HEIGHT + 1;
    int list_height = LINES - FOOTER_HEIGHT - HEADER_HEIGHT - PREVIEW_HEIGHT - 1;
    int name_x = 42 + (COLS - 44) / 2;
    if (name_x < 55) name_x = 55;
    int path_width = name_x - 43;
    int name_width = COLS - name_x - 3;
    
    // 更新过滤列表
    static char last_filter[256] = "";
    static int last_search_mode = -1;
    if (strcmp(last_filter, ui_state.filter) != 0 || last_search_mode != ui_state.search_mode) {
        update_filtered_list();
        strncpy(last_filter, ui_state.filter, sizeof(last_filter) - 1);
        last_search_mode = ui_state.search_mode;
    }
    
    // 调整选中位置
    if (ui_state.selected >= g_filtered_count) {
        ui_state.selected = (g_filtered_count > 0) ? g_filtered_count - 1 : 0;
    }
    
    // 调整滚动位置
    if (ui_state.selected < ui_state.scroll_offset) {
        ui_state.scroll_offset = ui_state.selected;
    }
    if (ui_state.selected >= ui_state.scroll_offset + list_height) {
        ui_state.scroll_offset = ui_state.selected - list_height + 1;
    }
    if (ui_state.scroll_offset < 0) ui_state.scroll_offset = 0;
    
    // 绘制可见项目
    for (int row = 0; row < list_height; row++) {
        int idx = ui_state.scroll_offset + row;
        int y = list_y + row;
        
        for (int x = 1; x < COLS - 1; x++) {
            mvaddch(y, x, ' ');
        }
        
        if (idx < g_filtered_count) {
            int item_idx = g_filtered[idx].original_idx;
            RestoreItem *item = &g_items[item_idx];
            int is_selected = (idx == ui_state.selected);
            int is_checked = g_filtered[idx].is_selected;
            
            if (is_selected) {
                attron(COLOR_PAIR(COLOR_SELECTED));
                for (int x = 1; x < COLS - 1; x++) {
                    mvaddch(y, x, ' ');
                }
                attroff(COLOR_PAIR(COLOR_SELECTED));
            }
            
            // 复选框 [*] 或 [ ]
            if (is_selected) attron(COLOR_PAIR(COLOR_SELECTED));
            if (is_checked) {
                attron(COLOR_PAIR(COLOR_SELECTED_MARK));
                mvaddstr(y, 2, "[*]");
                attroff(COLOR_PAIR(COLOR_SELECTED_MARK));
            } else {
                mvaddstr(y, 2, "[ ]");
            }
            if (is_selected) attroff(COLOR_PAIR(COLOR_SELECTED));
            
            // ID
            char id_str[8];
            snprintf(id_str, sizeof(id_str), "%d", item->id);
            if (is_selected) attron(COLOR_PAIR(COLOR_SELECTED));
            else attron(COLOR_PAIR(COLOR_TEXT));
            mvaddstr(y, 6, id_str);
            if (is_selected) attroff(COLOR_PAIR(COLOR_SELECTED));
            else attroff(COLOR_PAIR(COLOR_TEXT));
            
            // 时间戳
            if (is_selected) attron(COLOR_PAIR(COLOR_SELECTED));
            else attron(COLOR_PAIR(COLOR_TEXT));
            mvaddstr(y, 12, item->timestamp);
            if (is_selected) attroff(COLOR_PAIR(COLOR_SELECTED));
            else attroff(COLOR_PAIR(COLOR_TEXT));
            
            // 状态
            if (is_selected) {
                attron(COLOR_PAIR(COLOR_SELECTED));
            } else {
                attron(COLOR_PAIR(item->is_archived ? COLOR_ARCHIVED : COLOR_ACTIVE));
            }
            mvaddstr(y, 32, item->is_archived ? "已归档" : "活跃  ");
            if (is_selected) {
                attroff(COLOR_PAIR(COLOR_SELECTED));
            } else {
                attroff(COLOR_PAIR(item->is_archived ? COLOR_ARCHIVED : COLOR_ACTIVE));
            }
            
            // 原路径（带高亮）
            draw_str_highlighted(y, 42, item->original_path, ui_state.filter, 
                                 path_width, is_selected);
            
            // 文件名（带高亮）
            draw_str_highlighted(y, name_x, item->filename, ui_state.filter, 
                                 name_width, is_selected);
            
            // 模糊匹配分数显示
            if (ui_state.search_mode == SEARCH_MODE_FUZZY && ui_state.filter[0] != '\0') {
                char score_str[8];
                snprintf(score_str, sizeof(score_str), "%d%%", g_filtered[idx].match_score);
                int score_x = COLS - 8;
                if (is_selected) attron(COLOR_PAIR(COLOR_SELECTED));
                attron(COLOR_PAIR(COLOR_FUZZY_MATCH));
                mvaddstr(y, score_x, score_str);
                attroff(COLOR_PAIR(COLOR_FUZZY_MATCH));
                if (is_selected) attroff(COLOR_PAIR(COLOR_SELECTED));
            }
        }
    }
    
    // 绘制滚动条
    if (g_filtered_count > list_height) {
        int scrollbar_height = (list_height * list_height) / g_filtered_count;
        if (scrollbar_height < 1) scrollbar_height = 1;
        int scrollbar_pos = (ui_state.scroll_offset * list_height) / g_filtered_count;
        
        attron(COLOR_PAIR(COLOR_BORDER));
        for (int i = 0; i < list_height; i++) {
            if (i >= scrollbar_pos && i < scrollbar_pos + scrollbar_height) {
                mvaddch(list_y + i, COLS - 2, ACS_BLOCK);
            } else {
                mvaddch(list_y + i, COLS - 2, ACS_VLINE);
            }
        }
        attroff(COLOR_PAIR(COLOR_BORDER));
    }
}

// 绘制预览面板
static void draw_preview_panel(void) {
    int panel_y = LINES - FOOTER_HEIGHT - PREVIEW_HEIGHT;
    int panel_width = COLS - 2;
    
    for (int row = 0; row < PREVIEW_HEIGHT; row++) {
        for (int col = 1; col < COLS - 1; col++) {
            mvaddch(panel_y + row, col, ' ');
        }
    }
    
    attron(COLOR_PAIR(COLOR_BORDER));
    mvaddch(panel_y, 0, ACS_LTEE);
    draw_hline(panel_y, 1, COLS - 2, ACS_HLINE);
    mvaddch(panel_y, COLS - 1, ACS_RTEE);
    attroff(COLOR_PAIR(COLOR_BORDER));
    
    int sep_x = panel_width / 2 + 10;
    if (sep_x < 45) sep_x = 45;
    if (sep_x > COLS - 25) sep_x = COLS - 25;
    
    attron(COLOR_PAIR(COLOR_BORDER));
    mvvline(panel_y + 1, sep_x, ACS_VLINE, PREVIEW_HEIGHT - 2);
    mvaddch(panel_y, sep_x, ACS_TTEE);
    mvaddch(LINES - 2, sep_x, ACS_BTEE);
    attroff(COLOR_PAIR(COLOR_BORDER));
    
    int selected_count = 0;
    for (int i = 0; i < g_filtered_count; i++) {
        if (g_filtered[i].is_selected) selected_count++;
    }
    
    if (ui_state.selected >= 0 && ui_state.selected < g_filtered_count && g_count > 0) {
        int item_idx = g_filtered[ui_state.selected].original_idx;
        RestoreItem *item = &g_items[item_idx];
        
        int left_x = 2;
        int line = panel_y + 1;
        
        attron(COLOR_PAIR(COLOR_LABEL));
        mvaddstr(line, left_x, "文件名:");
        attroff(COLOR_PAIR(COLOR_LABEL));
        
        attron(COLOR_PAIR(COLOR_TITLE));
        int max_filename_width = sep_x - left_x - 10;
        if (max_filename_width > 5) {
            if ((int)strlen(item->filename) > max_filename_width) {
                char truncated[256];
                snprintf(truncated, max_filename_width, "%s", item->filename);
                truncated[max_filename_width - 3] = '.';
                truncated[max_filename_width - 2] = '.';
                truncated[max_filename_width - 1] = '.';
                truncated[max_filename_width] = '\0';
                mvaddstr(line, left_x + 9, truncated);
            } else {
                mvaddstr(line, left_x + 9, item->filename);
            }
        }
        attroff(COLOR_PAIR(COLOR_TITLE));
        line += 2;
        
        attron(COLOR_PAIR(COLOR_LABEL));
        mvaddstr(line, left_x, "原路径:");
        attroff(COLOR_PAIR(COLOR_LABEL));
        
        attron(COLOR_PAIR(COLOR_TEXT));
        int max_path_width = sep_x - left_x - 10;
        if (max_path_width > 10) {
            draw_str_truncated(line, left_x + 9, item->original_path, max_path_width, 0);
        }
        attroff(COLOR_PAIR(COLOR_TEXT));
        line += 2;
        
        attron(COLOR_PAIR(COLOR_LABEL));
        mvaddstr(line, left_x, "状态:");
        attroff(COLOR_PAIR(COLOR_LABEL));
        
        attron(COLOR_PAIR(item->is_archived ? COLOR_ARCHIVED : COLOR_ACTIVE));
        mvaddstr(line, left_x + 9, item->is_archived ? "已归档 (在old/目录中)" : "活跃 (可直接恢复)");
        attroff(COLOR_PAIR(item->is_archived ? COLOR_ARCHIVED : COLOR_ACTIVE));
        line += 2;
        
        attron(COLOR_PAIR(COLOR_LABEL));
        mvaddstr(line, left_x, "删除时间:");
        attroff(COLOR_PAIR(COLOR_LABEL));
        
        attron(COLOR_PAIR(COLOR_TEXT));
        mvaddstr(line, left_x + 9, item->timestamp);
        attroff(COLOR_PAIR(COLOR_TEXT));
        
        int right_x = sep_x + 3;
        line = panel_y + 1;
        
        attron(COLOR_PAIR(COLOR_LABEL));
        mvaddstr(line, right_x, "操作提示");
        attroff(COLOR_PAIR(COLOR_LABEL));
        line += 2;
        
        attron(COLOR_PAIR(COLOR_TEXT));
        mvaddstr(line++, right_x, "Space   选择/取消");
        mvaddstr(line++, right_x, "a/A     全选/取消全选");
        mvaddstr(line++, right_x, "Enter   恢复当前项");
        mvaddstr(line++, right_x, "r       恢复选中项");
        mvaddstr(line++, right_x, "R       恢复所有可见");
        attroff(COLOR_PAIR(COLOR_TEXT));
        
        attron(COLOR_PAIR(COLOR_DELETE_WARN));
        mvaddstr(line++, right_x, "d       删除选中项");
        mvaddstr(line++, right_x, "D       删除所有可见");
        attroff(COLOR_PAIR(COLOR_DELETE_WARN));
        
        // 显示选中数量（在左侧面板底部，避免覆盖右侧操作提示）
        if (selected_count > 0) {
            attron(COLOR_PAIR(COLOR_SELECTED_MARK));
            char sel_msg[64];
            snprintf(sel_msg, sizeof(sel_msg), "已选择: %d 项", selected_count);
            mvaddstr(panel_y + PREVIEW_HEIGHT - 2, left_x, sel_msg);
            attroff(COLOR_PAIR(COLOR_SELECTED_MARK));
        }
        
    } else if (g_filtered_count == 0 || g_count == 0) {
        attron(COLOR_PAIR(COLOR_STATUS_WARN));
        char *msg = " 回收站为空 ";
        int msg_x = (COLS - strlen(msg)) / 2;
        mvaddstr(panel_y + PREVIEW_HEIGHT / 2, msg_x, msg);
        attroff(COLOR_PAIR(COLOR_STATUS_WARN));
    }
    
    attron(COLOR_PAIR(COLOR_BORDER));
    mvaddch(LINES - 2, 0, ACS_LTEE);
    draw_hline(LINES - 2, 1, COLS - 2, ACS_HLINE);
    mvaddch(LINES - 2, COLS - 1, ACS_RTEE);
    attroff(COLOR_PAIR(COLOR_BORDER));
}

// 绘制底部状态栏
static void draw_footer(void) {
    int y = LINES - 1;
    
    attron(COLOR_PAIR(COLOR_FOOTER));
    for (int x = 0; x < COLS; x++) {
        mvaddch(y, x, ' ');
    }
    
    mvaddstr(y, 1, "↑↓:选择");
    mvaddstr(y, 9, "Space:多选");
    mvaddstr(y, 19, "a:全选");
    mvaddstr(y, 26, "r:恢复");
    mvaddstr(y, 33, "d:删除");
    mvaddstr(y, 40, "/:搜索");
    mvaddstr(y, 47, "Tab:模式");
    mvaddstr(y, 55, "?:帮助");
    mvaddstr(y, 62, "Q:退出");
    
    attroff(COLOR_PAIR(COLOR_FOOTER));
}

// 绘制搜索框
static void draw_search_box(void) {
    if (!ui_state.show_search) return;
    
    int box_width = 60;
    int box_height = 5;
    int box_x = (COLS - box_width) / 2;
    int box_y = (LINES - box_height) / 2;
    
    for (int row = box_y - 1; row < box_y + box_height + 1; row++) {
        for (int col = box_x - 2; col < box_x + box_width + 2; col++) {
            if (row >= 0 && row < LINES && col >= 0 && col < COLS) {
                mvaddch(row, col, ' ');
            }
        }
    }
    
    attron(COLOR_PAIR(COLOR_BORDER));
    for (int x = 0; x < box_width; x++) {
        mvaddch(box_y, box_x + x, ACS_HLINE);
        mvaddch(box_y + box_height - 1, box_x + x, ACS_HLINE);
    }
    for (int y = 0; y < box_height; y++) {
        mvaddch(box_y + y, box_x, ACS_VLINE);
        mvaddch(box_y + y, box_x + box_width - 1, ACS_VLINE);
    }
    mvaddch(box_y, box_x, ACS_ULCORNER);
    mvaddch(box_y, box_x + box_width - 1, ACS_URCORNER);
    mvaddch(box_y + box_height - 1, box_x, ACS_LLCORNER);
    mvaddch(box_y + box_height - 1, box_x + box_width - 1, ACS_LRCORNER);
    attroff(COLOR_PAIR(COLOR_BORDER));
    
    attron(COLOR_PAIR(COLOR_HEADER));
    const char *mode_str = (ui_state.search_mode == SEARCH_MODE_EXACT) ? "精确" : "模糊";
    char title[64];
    snprintf(title, sizeof(title), " 搜索文件 [%s模式] ", mode_str);
    mvaddstr(box_y, box_x + 2, title);
    attroff(COLOR_PAIR(COLOR_HEADER));
    
    // 提示文字
    attron(COLOR_PAIR(COLOR_TEXT));
    if (ui_state.search_mode == SEARCH_MODE_EXACT) {
        mvaddstr(box_y + 1, box_x + 3, "精确: 子串匹配");
    } else {
        mvaddstr(box_y + 1, box_x + 3, "模糊: 相似度>60%");
    }
    attroff(COLOR_PAIR(COLOR_TEXT));
    
    attron(COLOR_PAIR(COLOR_SELECTED));
    for (int x = 2; x < box_width - 2; x++) {
        mvaddch(box_y + 2, box_x + x, ' ');
    }
    mvaddstr(box_y + 2, box_x + 3, ui_state.filter);
    mvaddch(box_y + 2, box_x + 3 + ui_state.filter_len, ACS_BLOCK);
    attroff(COLOR_PAIR(COLOR_SELECTED));
    
    // Tab切换提示
    attron(COLOR_PAIR(COLOR_LABEL));
    mvaddstr(box_y + 3, box_x + 3, "Tab:切换模式  Esc:关闭");
    attroff(COLOR_PAIR(COLOR_LABEL));
}

// 绘制确认对话框
static void draw_confirm_dialog(void) {
    if (!ui_state.show_confirm) return;
    
    int box_width = 50;
    int box_height = 8;
    int box_x = (COLS - box_width) / 2;
    int box_y = (LINES - box_height) / 2;
    
    // 不再绘制背景遮罩，保留列表背景
    
    // 对话框背景
    for (int y = box_y; y < box_y + box_height; y++) {
        for (int x = box_x; x < box_x + box_width; x++) {
            attron(COLOR_PAIR(COLOR_DIALOG_BG));
            mvaddch(y, x, ' ');
            attroff(COLOR_PAIR(COLOR_DIALOG_BG));
        }
    }
    
    // 边框
    attron(COLOR_PAIR(COLOR_BORDER));
    for (int x = 0; x < box_width; x++) {
        mvaddch(box_y, box_x + x, ACS_HLINE);
        mvaddch(box_y + box_height - 1, box_x + x, ACS_HLINE);
    }
    for (int y = 0; y < box_height; y++) {
        mvaddch(box_y + y, box_x, ACS_VLINE);
        mvaddch(box_y + y, box_x + box_width - 1, ACS_VLINE);
    }
    mvaddch(box_y, box_x, ACS_ULCORNER);
    mvaddch(box_y, box_x + box_width - 1, ACS_URCORNER);
    mvaddch(box_y + box_height - 1, box_x, ACS_LLCORNER);
    mvaddch(box_y + box_height - 1, box_x + box_width - 1, ACS_LRCORNER);
    attroff(COLOR_PAIR(COLOR_BORDER));
    
    // 标题
    if (g_confirm_title) {
        if (g_confirm_is_delete) {
            attron(COLOR_PAIR(COLOR_DIALOG_WARN));
        } else {
            attron(COLOR_PAIR(COLOR_HEADER));
        }
        mvaddstr(box_y, box_x + (box_width - strlen(g_confirm_title)) / 2, g_confirm_title);
        if (g_confirm_is_delete) {
            attroff(COLOR_PAIR(COLOR_DIALOG_WARN));
        } else {
            attroff(COLOR_PAIR(COLOR_HEADER));
        }
    }
    
    // 消息
    if (g_confirm_message) {
        attron(COLOR_PAIR(COLOR_TEXT));
        int msg_x = box_x + (box_width - strlen(g_confirm_message)) / 2;
        if (msg_x < box_x + 2) msg_x = box_x + 2;
        mvaddstr(box_y + 3, msg_x, g_confirm_message);
        attroff(COLOR_PAIR(COLOR_TEXT));
    }
    
    // 按钮
    int btn_y = box_y + box_height - 2;
    int yes_x = box_x + box_width / 4 - 2;
    int no_x = box_x + box_width * 3 / 4 - 2;
    
    if (g_confirm_result == 1) {
        attron(COLOR_PAIR(COLOR_DIALOG_BTN));
        mvaddstr(btn_y, yes_x, " [ 是 ] ");
        attroff(COLOR_PAIR(COLOR_DIALOG_BTN));
    } else {
        attron(COLOR_PAIR(COLOR_TEXT));
        mvaddstr(btn_y, yes_x, "  是   ");
        attroff(COLOR_PAIR(COLOR_TEXT));
    }
    
    if (g_confirm_result == 0) {
        attron(COLOR_PAIR(COLOR_DIALOG_BTN));
        mvaddstr(btn_y, no_x, " [ 否 ] ");
        attroff(COLOR_PAIR(COLOR_DIALOG_BTN));
    } else {
        attron(COLOR_PAIR(COLOR_TEXT));
        mvaddstr(btn_y, no_x, "  否   ");
        attroff(COLOR_PAIR(COLOR_TEXT));
    }
}

// 绘制帮助窗口
static void draw_help_window(void) {
    if (!ui_state.show_help) return;
    
    int box_width = 65;
    int box_height = 26;  // 增加高度以容纳所有内容
    int box_x = (COLS - box_width) / 2;
    int box_y = (LINES - box_height) / 2;
    
    for (int y = box_y; y < box_y + box_height; y++) {
        for (int x = box_x; x < box_x + box_width; x++) {
            mvaddch(y, x, ' ');
        }
    }
    
    attron(COLOR_PAIR(COLOR_BORDER));
    for (int x = 0; x < box_width; x++) {
        mvaddch(box_y, box_x + x, ACS_HLINE);
        mvaddch(box_y + box_height - 1, box_x + x, ACS_HLINE);
    }
    for (int y = 0; y < box_height; y++) {
        mvaddch(box_y + y, box_x, ACS_VLINE);
        mvaddch(box_y + y, box_x + box_width - 1, ACS_VLINE);
    }
    mvaddch(box_y, box_x, ACS_ULCORNER);
    mvaddch(box_y, box_x + box_width - 1, ACS_URCORNER);
    mvaddch(box_y + box_height - 1, box_x, ACS_LLCORNER);
    mvaddch(box_y + box_height - 1, box_x + box_width - 1, ACS_LRCORNER);
    attroff(COLOR_PAIR(COLOR_BORDER));
    
    attron(COLOR_PAIR(COLOR_HEADER));
    mvaddstr(box_y, box_x + (box_width - 14) / 2, " zrestore 帮助 ");
    attroff(COLOR_PAIR(COLOR_HEADER));
    
    int y = box_y + 2;
    attron(COLOR_PAIR(COLOR_LABEL));
    mvaddstr(y++, box_x + 2, "键盘快捷键:");
    attroff(COLOR_PAIR(COLOR_LABEL));
    y++;
    
    attron(COLOR_PAIR(COLOR_TEXT));
    mvaddstr(y++, box_x + 4, "↑/↓/k/j     - 上下移动选择");
    mvaddstr(y++, box_x + 4, "PgUp/PgDn   - 翻页");
    mvaddstr(y++, box_x + 4, "Home/End    - 跳到首/尾");
    mvaddstr(y++, box_x + 4, "Space       - 选择/取消选择当前项");
    mvaddstr(y++, box_x + 4, "a           - 全选所有匹配项");
    mvaddstr(y++, box_x + 4, "A           - 取消全选");
    mvaddstr(y++, box_x + 4, "Enter       - 恢复当前选中的项目");
    mvaddstr(y++, box_x + 4, "r           - 恢复所有选中的项目");
    mvaddstr(y++, box_x + 4, "R           - 恢复所有可见项目（忽略选择）");
    mvaddstr(y++, box_x + 4, "d           - 删除选中的项目（永久删除）");
    mvaddstr(y++, box_x + 4, "D           - 删除所有可见项目（清空）");
    
    attron(COLOR_PAIR(COLOR_TEXT));
    mvaddstr(y++, box_x + 4, "/           - 搜索过滤");
    mvaddstr(y++, box_x + 4, "Tab         - 切换搜索模式（精确/模糊）");
    mvaddstr(y++, box_x + 4, "Esc         - 清除搜索/取消");
    mvaddstr(y++, box_x + 4, "F1/?        - 显示此帮助");
    mvaddstr(y++, box_x + 4, "Q           - 退出");
    attroff(COLOR_PAIR(COLOR_TEXT));
    y++;
    
    attron(COLOR_PAIR(COLOR_LABEL));
    mvaddstr(y++, box_x + 2, "搜索模式:");
    attroff(COLOR_PAIR(COLOR_LABEL));
    y++;
    
    attron(COLOR_PAIR(COLOR_TEXT));
    mvaddstr(y++, box_x + 4, "精确模式: 子串匹配，匹配字符高亮显示");
    mvaddstr(y++, box_x + 4, "模糊模式: 相似度匹配，显示相似度分数");
    attroff(COLOR_PAIR(COLOR_TEXT));
    y++;
    
    attron(COLOR_PAIR(COLOR_STATUS_OK));
    mvaddstr(y++, box_x + 2, "按任意键关闭此帮助...");
    attroff(COLOR_PAIR(COLOR_STATUS_OK));
}

// 绘制状态消息
static void draw_status_message(void) {
    if (ui_state.status_msg[0] == '\0') return;
    
    int msg_len = strlen(ui_state.status_msg);
    int x = (COLS - msg_len) / 2;
    int y = LINES / 2;
    
    attron(COLOR_PAIR(ui_state.status_type == 0 ? COLOR_STATUS_OK : 
                      ui_state.status_type == 1 ? COLOR_STATUS_WARN : COLOR_HEADER));
    mvaddstr(y, x, ui_state.status_msg);
    attroff(COLOR_PAIR(ui_state.status_type == 0 ? COLOR_STATUS_OK : 
                       ui_state.status_type == 1 ? COLOR_STATUS_WARN : COLOR_HEADER));
}

// 主绘制函数
void zrestore_draw(void) {
    clear();
    
    draw_border();
    draw_header();
    draw_column_headers();
    draw_file_list();
    draw_preview_panel();
    draw_footer();
    
    if (ui_state.show_help) {
        draw_help_window();
    } else if (ui_state.show_search) {
        draw_search_box();
    } else if (ui_state.show_confirm) {
        draw_confirm_dialog();
    }
    
    draw_status_message();
    
    refresh();
}

// 初始化UI
void zrestore_init(void) {
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, FALSE);
    curs_set(0);
    
    init_colors();
    
    memset(&ui_state, 0, sizeof(ui_state));
    ui_state.selected = 0;
    ui_state.scroll_offset = 0;
    ui_state.search_mode = SEARCH_MODE_EXACT;
}

// 清理UI
void zrestore_cleanup(void) {
    endwin();
}

// 设置状态消息
void zrestore_set_status(const char *msg, int type) {
    strncpy(ui_state.status_msg, msg, sizeof(ui_state.status_msg) - 1);
    ui_state.status_msg[sizeof(ui_state.status_msg) - 1] = '\0';
    ui_state.status_type = type;
    ui_state.status_time = time(NULL);
}

// 清除状态消息
void zrestore_clear_status(void) {
    if (time(NULL) - ui_state.status_time > 3) {
        ui_state.status_msg[0] = '\0';
    }
}

// 获取当前选中项目的ID
int zrestore_get_selected_id(void) {
    if (ui_state.selected >= 0 && ui_state.selected < g_filtered_count) {
        return g_items[g_filtered[ui_state.selected].original_idx].id;
    }
    return -1;
}

// 获取过滤后的项目数
int zrestore_get_filtered_count(void) {
    return g_filtered_count;
}

// 获取选中的项目ID列表
int* zrestore_get_selected_ids(int *count) {
    static int ids[1024];
    int n = 0;
    
    for (int i = 0; i < g_filtered_count && n < 1024; i++) {
        if (g_filtered[i].is_selected) {
            ids[n++] = g_items[g_filtered[i].original_idx].id;
        }
    }
    
    *count = n;
    return ids;
}

// 清除所有选择
void zrestore_clear_selection(void) {
    for (int i = 0; i < g_filtered_count; i++) {
        g_filtered[i].is_selected = 0;
    }
    ui_state.selected_count = 0;
}

// 获取选择数量
int zrestore_get_selection_count(void) {
    int count = 0;
    for (int i = 0; i < g_filtered_count; i++) {
        if (g_filtered[i].is_selected) count++;
    }
    return count;
}

// 获取过滤后的原始索引
int zrestore_get_filtered_original_idx(int filtered_idx) {
    if (filtered_idx >= 0 && filtered_idx < g_filtered_count) {
        return g_filtered[filtered_idx].original_idx;
    }
    return -1;
}

// 获取搜索模式
int zrestore_get_search_mode(void) {
    return ui_state.search_mode;
}

// 设置搜索模式
void zrestore_set_search_mode(int mode) {
    ui_state.search_mode = mode;
    update_filtered_list();
}

// 显示确认对话框
int zrestore_show_confirm_dialog(const char *title, const char *message) {
    g_confirm_title = title;
    g_confirm_message = message;
    g_confirm_result = 1;  // 默认选择"是"
    g_confirm_is_delete = 0;
    ui_state.show_confirm = 1;
    return 0;
}

// 显示删除确认对话框（带红色警告）
int zrestore_show_delete_confirm(const char *title, const char *message) {
    g_confirm_title = title;
    g_confirm_message = message;
    g_confirm_result = 0;  // 默认选择"否"（更安全）
    g_confirm_is_delete = 1;
    ui_state.show_confirm = 1;
    return 0;
}

// 关闭确认对话框
void zrestore_close_confirm_dialog(void) {
    ui_state.show_confirm = 0;
    g_confirm_title = NULL;
    g_confirm_message = NULL;
    g_confirm_result = -1;
    g_confirm_is_delete = 0;
}

// 显示进度条
void zrestore_show_progress(const char *title, const char *message, int current, int total) {
    int box_width = 60;
    int box_height = 7;
    int box_x = (COLS - box_width) / 2;
    int box_y = (LINES - box_height) / 2;
    
    // 绘制对话框背景
    for (int y = box_y; y < box_y + box_height; y++) {
        for (int x = box_x; x < box_x + box_width; x++) {
            attron(COLOR_PAIR(COLOR_DIALOG_BG));
            mvaddch(y, x, ' ');
            attroff(COLOR_PAIR(COLOR_DIALOG_BG));
        }
    }
    
    // 绘制边框
    attron(COLOR_PAIR(COLOR_BORDER));
    for (int x = 0; x < box_width; x++) {
        mvaddch(box_y, box_x + x, ACS_HLINE);
        mvaddch(box_y + box_height - 1, box_x + x, ACS_HLINE);
    }
    for (int y = 0; y < box_height; y++) {
        mvaddch(box_y + y, box_x, ACS_VLINE);
        mvaddch(box_y + y, box_x + box_width - 1, ACS_VLINE);
    }
    mvaddch(box_y, box_x, ACS_ULCORNER);
    mvaddch(box_y, box_x + box_width - 1, ACS_URCORNER);
    mvaddch(box_y + box_height - 1, box_x, ACS_LLCORNER);
    mvaddch(box_y + box_height - 1, box_x + box_width - 1, ACS_LRCORNER);
    attroff(COLOR_PAIR(COLOR_BORDER));
    
    // 标题
    if (title) {
        attron(COLOR_PAIR(COLOR_HEADER));
        int title_x = box_x + (box_width - strlen(title)) / 2;
        mvaddstr(box_y, title_x, title);
        attroff(COLOR_PAIR(COLOR_HEADER));
    }
    
    // 消息
    if (message) {
        attron(COLOR_PAIR(COLOR_TEXT));
        int msg_x = box_x + (box_width - strlen(message)) / 2;
        if (msg_x < box_x + 2) msg_x = box_x + 2;
        mvaddstr(box_y + 2, msg_x, message);
        attroff(COLOR_PAIR(COLOR_TEXT));
    }
    
    // 进度条
    int bar_width = box_width - 10;
    int filled = (current * bar_width) / total;
    if (filled > bar_width) filled = bar_width;
    
    int bar_y = box_y + 4;
    int bar_x = box_x + 5;
    
    // 进度条背景
    attron(COLOR_PAIR(COLOR_BORDER));
    mvaddch(bar_y, bar_x - 1, '[');
    mvaddch(bar_y, bar_x + bar_width, ']');
    attroff(COLOR_PAIR(COLOR_BORDER));
    
    // 进度条填充
    for (int i = 0; i < bar_width; i++) {
        if (i < filled) {
            attron(COLOR_PAIR(COLOR_STATUS_OK));
            mvaddch(bar_y, bar_x + i, ACS_BLOCK);
            attroff(COLOR_PAIR(COLOR_STATUS_OK));
        } else {
            attron(COLOR_PAIR(COLOR_DIALOG_BG));
            mvaddch(bar_y, bar_x + i, ' ');
            attroff(COLOR_PAIR(COLOR_DIALOG_BG));
        }
    }
    
    // 进度百分比
    attron(COLOR_PAIR(COLOR_LABEL));
    char percent_str[16];
    snprintf(percent_str, sizeof(percent_str), "%d/%d", current, total);
    int percent_x = box_x + (box_width - strlen(percent_str)) / 2;
    mvaddstr(bar_y + 1, percent_x, percent_str);
    attroff(COLOR_PAIR(COLOR_LABEL));
    
    refresh();
}

// 隐藏进度条（重绘界面）
void zrestore_hide_progress(void) {
    zrestore_draw();
}

// 获取过滤条件
const char* zrestore_get_filter(void) {
    return ui_state.filter;
}

// 处理键盘输入
zrestore_action_t zrestore_handle_input(void) {
    int ch = getch();
    zrestore_clear_status();
    
    // 帮助窗口模式
    if (ui_state.show_help) {
        ui_state.show_help = 0;
        return ACTION_NONE;
    }
    
    // 确认对话框模式
    if (ui_state.show_confirm) {
        switch (ch) {
            case '\n':
            case KEY_ENTER:
                ui_state.show_confirm = 0;
                if (g_confirm_is_delete) {
                    return g_confirm_result == 1 ? ACTION_DELETE_SELECTED : ACTION_NONE;
                } else {
                    return g_confirm_result == 1 ? ACTION_RESTORE_SELECTED_MULTI : ACTION_NONE;
                }
            
            case '\t':
            case KEY_LEFT:
            case KEY_RIGHT:
                g_confirm_result = !g_confirm_result;
                return ACTION_NONE;
            
            case 'y':
            case 'Y':
                ui_state.show_confirm = 0;
                return g_confirm_is_delete ? ACTION_DELETE_SELECTED : ACTION_RESTORE_SELECTED_MULTI;
            
            case 'n':
            case 'N':
            case 27: // Esc
                ui_state.show_confirm = 0;
                return ACTION_NONE;
            
            default:
                return ACTION_NONE;
        }
    }
    
    // 搜索框模式
    if (ui_state.show_search) {
        switch (ch) {
            case 27: // Esc
                ui_state.show_search = 0;
                return ACTION_NONE;
            
            case '\n':
            case KEY_ENTER:
                ui_state.show_search = 0;
                ui_state.selected = 0;
                ui_state.scroll_offset = 0;
                return ACTION_NONE;
            
            case '\t': // Tab - 切换搜索模式
                ui_state.search_mode = !ui_state.search_mode;
                update_filtered_list();
                return ACTION_NONE;
            
            case KEY_BACKSPACE:
            case 127:
            case '\b':
                if (ui_state.filter_len > 0) {
                    ui_state.filter[--ui_state.filter_len] = '\0';
                    update_filtered_list();
                }
                return ACTION_NONE;
            
            default:
                if (isprint(ch) && (size_t)ui_state.filter_len < sizeof(ui_state.filter) - 1) {
                    ui_state.filter[ui_state.filter_len++] = ch;
                    ui_state.filter[ui_state.filter_len] = '\0';
                    ui_state.selected = 0;
                    update_filtered_list();
                }
                return ACTION_NONE;
        }
    }
    
    // 正常模式
    switch (ch) {
        case 'q':
        case 'Q':
            return ACTION_QUIT;
        
        case KEY_UP:
        case 'k':
            if (ui_state.selected > 0) ui_state.selected--;
            break;
        
        case KEY_DOWN:
        case 'j':
            if (ui_state.selected < g_filtered_count - 1) ui_state.selected++;
            break;
        
        case KEY_PPAGE:
            ui_state.selected -= (LINES - FOOTER_HEIGHT - HEADER_HEIGHT - PREVIEW_HEIGHT - 3);
            if (ui_state.selected < 0) ui_state.selected = 0;
            break;
        
        case KEY_NPAGE:
            ui_state.selected += (LINES - FOOTER_HEIGHT - HEADER_HEIGHT - PREVIEW_HEIGHT - 3);
            if (ui_state.selected >= g_filtered_count) {
                ui_state.selected = g_filtered_count - 1;
            }
            break;
        
        case KEY_HOME:
            ui_state.selected = 0;
            break;
        
        case KEY_END:
            ui_state.selected = g_filtered_count - 1;
            break;
        
        case ' ': // 空格 - 切换选择状态
            if (ui_state.selected >= 0 && ui_state.selected < g_filtered_count) {
                g_filtered[ui_state.selected].is_selected = !g_filtered[ui_state.selected].is_selected;
            }
            break;
        
        case 'a': // 全选
            for (int i = 0; i < g_filtered_count; i++) {
                g_filtered[i].is_selected = 1;
            }
            zrestore_set_status("已全选所有匹配项", 0);
            break;
        
        case 'A': // 取消全选
            for (int i = 0; i < g_filtered_count; i++) {
                g_filtered[i].is_selected = 0;
            }
            zrestore_set_status("已取消全选", 0);
            break;
        
        case '\n':
        case KEY_ENTER:
            return ACTION_RESTORE_SELECTED;
        
        case 'r': // 恢复选中的
            if (zrestore_get_selection_count() > 0) {
                char msg[256];
                snprintf(msg, sizeof(msg), "确认恢复选中的 %d 个项目?", zrestore_get_selection_count());
                zrestore_show_confirm_dialog(" 确认恢复 ", msg);
            } else {
                zrestore_set_status("没有选中的项目", 1);
            }
            break;
        
        case 'R': // 恢复所有可见
            if (g_filtered_count > 0) {
                char msg[256];
                snprintf(msg, sizeof(msg), "确认恢复所有 %d 个可见项目?", g_filtered_count);
                zrestore_show_confirm_dialog(" 确认恢复 ", msg);
                // 标记为恢复所有
                return ACTION_RESTORE_ALL;
            }
            break;
        
        case '/':
        case 's':
        case 'S':
            ui_state.show_search = 1;
            ui_state.filter_len = 0;
            ui_state.filter[0] = '\0';
            break;
        
        case '\t': // Tab - 切换搜索模式
            ui_state.search_mode = !ui_state.search_mode;
            update_filtered_list();
            zrestore_set_status(ui_state.search_mode == SEARCH_MODE_EXACT ? "精确匹配模式" : "模糊匹配模式", 0);
            break;
        
        case 27: // Esc
            ui_state.filter[0] = '\0';
            ui_state.filter_len = 0;
            ui_state.selected = 0;
            update_filtered_list();
            break;
        
        case KEY_F(1):
        case '?':
        case 'h':
        case 'H':
            ui_state.show_help = 1;
            break;
        
        case 'd': // 删除选中的
            if (zrestore_get_selection_count() > 0) {
                char msg[256];
                snprintf(msg, sizeof(msg), "将永久删除选中的 %d 个项目!", zrestore_get_selection_count());
                zrestore_show_delete_confirm(" ⚠ 警告：永久删除 ", msg);
            } else {
                zrestore_set_status("没有选中的项目", 1);
            }
            break;
        
        case 'D': // 删除所有可见
            if (g_filtered_count > 0) {
                char msg[256];
                snprintf(msg, sizeof(msg), "将永久删除所有 %d 个可见项目!", g_filtered_count);
                zrestore_show_delete_confirm(" ⚠ 警告：清空回收站 ", msg);
                return ACTION_DELETE_ALL;
            }
            break;
        
        default:
            break;
    }
    
    return ACTION_NONE;
}

// 设置项目数据
void zrestore_set_items(RestoreItem *items, int count) {
    g_items = items;
    g_count = count;
    ui_state.selected = 0;
    ui_state.scroll_offset = 0;
    update_filtered_list();
}
