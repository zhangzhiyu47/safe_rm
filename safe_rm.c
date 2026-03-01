#include "safe_rm.h"
#include "proctitle.h"

int main(int argc, char *argv[]) {
    save_main_args(argc, argv);

    OperationMode mode = MODE_SAFE_DELETE;
    int file_start_index = 1;
    int file_count;
    
    // 检查参数
    if (argc < 2) {
        print_help(argv[0]);
        return 0;
    }
    
    // 检查帮助和版本选项
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_help(argv[0]);
        return 0;
    }
    
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
        printf("safe_rm version %s\n", VERSION);
        return 0;
    }
    
    // 检查是否是彻底删除模式
    if (strcmp(argv[1], "--remove-completely") == 0) {
        mode = MODE_COMPLETE_REMOVE;
        file_start_index = 2;
    }
    
    // 检查是否有文件参数
    if (file_start_index >= argc) {
        fprintf(stderr, "错误: 没有指定要删除的文件\n");
        print_help(argv[0]);
        return 1;
    }
    
    // 计算文件数量
    file_count = argc - file_start_index;
    
    // 根据模式执行
    if (mode == MODE_COMPLETE_REMOVE) {
        // 彻底删除模式
        return complete_remove_files(file_count, argv + file_start_index);
    } else {
        // 安全删除模式
        return !safe_delete_files(file_count, argv + file_start_index);
    }
}
