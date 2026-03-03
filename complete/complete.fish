#!/usr/bin/env fish
# safe_rm, restore, zrestore 的 Fish 补全脚本（带 ID 描述）

# 辅助函数：提取 ID 和文件名，格式为 "ID\t文件名"
function __restore_get_ids_with_desc
    # $1是ID，$4是文件名（根据 restore -l 输出格式）
    restore -l 2>/dev/null | string match -r '^[0-9]+\s.*$' | awk '{print $1 "\t" $4}'
end

# safe_rm 选项补全
complete -c safe_rm -l remove-completely -d "彻底删除文件"
complete -c safe_rm -s h -l help -d "显示帮助信息"
complete -c safe_rm -s v -l version -d "显示版本信息"

# rm 选项补全
if test -f $HOME/../.covered_sys_rm
    complete -c rm -e
    complete -c rm -l remove-completely -d "彻底删除文件"
    complete -c rm -s h -l help -d "显示帮助信息"
    complete -c rm -s v -l version -d "显示版本信息"
end

# restore 选项补全
complete -c restore -s l -l list -d "列出所有可恢复的项目"
complete -c restore -s a -l all -d "恢复所有项目"
complete -c restore -s d -l delete -r -d "永久删除指定的项目" -a "(__restore_get_ids_with_desc)"
complete -c restore -l delete-all -d "删除所有项目（清空回收站）"
complete -c restore -s h -l help -d "显示帮助信息"
complete -c restore -s v -l version -d "显示版本信息"

# restore ID 补全（用于 restore <id> 形式）
complete -c restore -n "__fish_use_subcommand" -a "(__restore_get_ids_with_desc)" -d "恢复指定项目"

# zrestore 选项补全
complete -c zrestore -s l -l list -d "使用普通列表模式"
complete -c zrestore -s i -l interactive -d "启动交互式ncurses界面"
complete -c zrestore -s h -l help -d "显示帮助信息"
complete -c zrestore -s v -l version -d "显示版本信息"
