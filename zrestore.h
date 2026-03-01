#ifndef ZRESTORE_H
#define ZRESTORE_H

#include "safe_rm.h"

// UI布局常量
#define HEADER_HEIGHT    2
#define FOOTER_HEIGHT    1
#define PREVIEW_HEIGHT   8
#define MIN_ROWS         15
#define MIN_COLS         80

// 搜索模式
#define SEARCH_MODE_EXACT   0   // 精确匹配（子串匹配）
#define SEARCH_MODE_FUZZY   1   // 模糊匹配（相似度匹配）

// 相似度阈值 (60%)
#define FUZZY_THRESHOLD     60

// 最大项目数
#define MAX_ITEMS           10000

// UI状态结构
typedef struct {
    int selected;           // 当前选中的索引
    int scroll_offset;      // 滚动偏移
    char filter[256];       // 搜索过滤条件
    int filter_len;         // 过滤条件长度
    int show_help;          // 是否显示帮助
    int show_search;        // 是否显示搜索框
    int show_confirm;       // 是否显示确认对话框
    char status_msg[256];   // 状态消息
    int status_type;        // 0=成功, 1=警告, 2=错误
    time_t status_time;     // 状态消息时间
    
    // 多选相关
    int selected_count;     // 已选择的项目数
    
    // 搜索模式
    int search_mode;        // 0=精确匹配, 1=模糊匹配
} zrestore_ui_t;

// 用户操作类型
typedef enum {
    ACTION_NONE,
    ACTION_QUIT,
    ACTION_RESTORE_SELECTED,    // 恢复单个选中项目（Enter）
    ACTION_RESTORE_SELECTED_MULTI, // 恢复多个选中的项目（r）
    ACTION_RESTORE_ALL,         // 恢复所有可见项目（R）
    ACTION_SHOW_DETAILS,
    ACTION_TOGGLE_SELECT,       // 切换选择状态（空格）
    ACTION_SELECT_ALL,          // 全选（a）
    ACTION_SELECT_NONE,         // 取消全选（A）
    ACTION_TOGGLE_SEARCH_MODE,  // 切换搜索模式（Tab）
    ACTION_DELETE_SELECTED,     // 删除选中的项目（x）
    ACTION_DELETE_ALL           // 删除所有可见项目（X）
} zrestore_action_t;

// 过滤后的项目索引结构
typedef struct {
    int original_idx;       // 原始items数组中的索引
    int is_selected;        // 是否被选中（多选）
    int match_score;        // 模糊匹配分数 (0-100)
} filtered_item_t;

// 初始化/清理函数
void zrestore_init(void);
void zrestore_cleanup(void);

// 绘制函数
void zrestore_draw(void);

// 输入处理
zrestore_action_t zrestore_handle_input(void);

// 数据设置
void zrestore_set_items(RestoreItem *items, int count);

// 状态管理
void zrestore_set_status(const char *msg, int type);
void zrestore_clear_status(void);

// 获取信息
int zrestore_get_selected_id(void);
int zrestore_get_filtered_count(void);
const char* zrestore_get_filter(void);

// 多选相关
int* zrestore_get_selected_ids(int *count);
void zrestore_clear_selection(void);
int zrestore_get_selection_count(void);

// 获取过滤后的原始索引
int zrestore_get_filtered_original_idx(int filtered_idx);

// 搜索相关
int zrestore_get_search_mode(void);
void zrestore_set_search_mode(int mode);

// 确认对话框
int zrestore_show_confirm_dialog(const char *title, const char *message);
void zrestore_close_confirm_dialog(void);

// 删除确认对话框（带警告颜色）
int zrestore_show_delete_confirm(const char *title, const char *message);

// 进度条显示
void zrestore_show_progress(const char *title, const char *message, int current, int total);
void zrestore_hide_progress(void);

#endif // ZRESTORE_H
