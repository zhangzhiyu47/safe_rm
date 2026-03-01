#include "safe_rm.h"
#include <sys/ioctl.h>

// 设置终端为原始模式
void setup_terminal(struct termios *old_term) {
    struct termios new_term;
    
    tcgetattr(STDIN_FILENO, old_term);
    new_term = *old_term;
    
    // 关闭规范模式和回显
    new_term.c_lflag &= ~(ICANON | ECHO);
    new_term.c_cc[VMIN] = 1;
    new_term.c_cc[VTIME] = 0;
    
    tcsetattr(STDIN_FILENO, TCSANOW, &new_term);
}

// 恢复终端设置
void restore_terminal(struct termios *old_term) {
    tcsetattr(STDIN_FILENO, TCSANOW, old_term);
}

// 打字验证 - 打字练习式实时字符校验
int typing_verification(void) {
    const char *verify_str = VERIFY_STRING;
    int len = strlen(verify_str);
    int *status = calloc(len, sizeof(int)); // 0=未输入, 1=正确, -1=错误
    int pos = 0;
    struct termios old_term;
    int ch;
    int has_error = 0;
    int result = 0;
    
    if (status == NULL) {
        perror("Error: calloc");
        return -1;
    }
    
    // 设置终端
    setup_terminal(&old_term);
    
    // 安装信号处理，确保能恢复终端
    signal(SIGINT, SIG_IGN);
    
    // 先打印提示信息（上方）
    printf("Please accurately type the following text to confirm (Ctrl+C to cancel):\n");
    
    // 打印验证字符串（作为底稿，白色）
    printf("%s%s%s", COLOR_WHITE, verify_str, COLOR_RESET);
    fflush(stdout);
    
    // 关键：上移光标到验证字符串行首
    printf("\r");
    fflush(stdout);
    
    while (1) {
        ch = getchar();
        
        // Ctrl+C - 取消
        if (ch == 3) {
            result = -1;
            break;
        }
        
        // 回车键确认
        if (ch == '\n' || ch == '\r') {
            if (pos == len && !has_error) {
                result = 1;
            } else {
                result = 0;
            }
            break;
        }
        
        // Backspace (127 = DEL, \b = 退格)
        if (ch == 127 || ch == '\b') {
            if (pos > 0) {
                pos--;
                
                // 更新错误状态
                if (status[pos] == -1) {
                    has_error = 0;
                    int i;
                    for (i = 0; i < pos; i++) {
                        if (status[i] == -1) {
                            has_error = 1;
                            break;
                        }
                    }
                }
                status[pos] = 0;
                
                // 光标回退到该位置，恢复白色原文
                printf("\b%s%c%s", COLOR_WHITE, verify_str[pos], COLOR_RESET);
                // 再回退一格，让光标停留在该位置等待新输入
                printf("\b");
                fflush(stdout);
            }
            continue;
        }
        
        // 普通字符输入
        if (pos < len) {
            if (ch == verify_str[pos]) {
                status[pos] = 1; // 正确 - 绿色
                // 覆盖打印绿色字符
                printf("%s%c%s", COLOR_GREEN, ch, COLOR_RESET);
            } else {
                status[pos] = -1; // 错误 - 红色
                has_error = 1;
                // 覆盖打印红色字符（显示用户实际输入的字符，而不是原文）
                printf("%s%c%s", COLOR_RED, ch, COLOR_RESET);
            }
            pos++;
            fflush(stdout);
        }
    }
    
    // 恢复终端
    restore_terminal(&old_term);
    signal(SIGINT, SIG_DFL);
    
    printf("\n");
    free(status);
    
    return result;
}

// 彻底删除文件
int complete_remove_files(int file_count, char **files) {
    int i;
    int verify_result;
    
    // 打印命令回显
    printf("Command: ./rm --remove-completely");
    for (i = 0; i < file_count; i++) {
        printf(" %s", files[i]);
    }
    printf("\n");
    
    // 检查文件是否存在
    int valid_count = 0;
    for (i = 0; i < file_count; i++) {
        struct stat st;
        if (stat(files[i], &st) != 0) {
            fprintf(stderr, "Error: cannot access '%s': %s\n", files[i], strerror(errno));
        } else {
            valid_count++;
        }
    }
    
    if (valid_count == 0) {
        fprintf(stderr, "No valid files to remove\n");
        return -1;
    }
    
    // 打字验证
    verify_result = typing_verification();
    
    if (verify_result == -1) {
        printf("Operation cancelled\n");
        return -1;
    }
    
    if (verify_result == 0) {
        printf("Verification failed, deletion cancelled\n");
        return -1;
    }
    
    // 执行实际删除操作
    printf("Verification passed, deleting...\n");
    
    char **args = malloc((file_count + 1) * sizeof(char*));
    if (args == NULL) {
        perror("Error: malloc");
        return -1;
    }
    
    int idx = 0;
    for (i = 0; i < file_count; i++) {
        struct stat st;
        if (stat(files[i], &st) == 0) {
            args[idx++] = files[i];
        }
    }
    args[idx] = NULL;
    
    if (redel3(args) == -1) {
        perror("Errro: redel3");
        free(args);
        return -1;
    }

    free(args);
    return 0;
}
