#!/bin/bash
# safe_rm, restore, zrestore 的 Bash 补全脚本（带 ID 描述）
# 需要安装 bash-completion 包以显示描述

# 提取 ID 和文件名（格式：ID:文件名）
_safe_rm_get_ids_desc() {
    # $1是ID，$4是文件名（根据 restore -l 输出格式）
    restore -l 2>/dev/null | awk '/^[0-9]+/ {print $1 ":" $4}'
}

# safe_rm 补全
_safe_rm() {
    local cur opts
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    opts="--remove-completely -h --help -v --version"

    if [[ ${cur} == -* ]]; then
        COMPREPLY=( $(compgen -W "${opts}" -- "${cur}") )
    fi
}

# restore 补全（带 ID 描述）
_restore() {
    local cur prev
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    opts="-l --list -a --all -d --delete --delete-all -h --help -v --version"

    # 补全选项
    if [[ ${cur} == -* ]]; then
        COMPREPLY=( $(compgen -W "${opts}" -- "${cur}") )
        return 0
    fi

    # 如果前一个选项需要 ID（-d 或无选项时的 restore）
    case "${prev}" in
        -d|--delete|"restore")
            # 检查是否有 _describe 函数（bash-completion 提供）
            if type _describe &>/dev/null; then
                local -a ids_desc
                while IFS=: read -r id filename; do
                    ids_desc+=("$id:$filename")
                done < <(restore -l 2>/dev/null | awk '/^[0-9]+/ {print $1 ":" $4}')
                _describe -t ids "可恢复项目" ids_desc
            else
                # 降级：只补全 ID，无描述
                local ids=$(restore -l 2>/dev/null | awk '/^[0-9]+/ {print $1}')
                COMPREPLY=( $(compgen -W "${ids}" -- "${cur}") )
            fi
            return 0
            ;;
        -h|--help|-v|--version|-l|--list|-a|--all|--delete-all)
            return 0
            ;;
    esac

    # 默认补全 ID（当输入 restore 后接数字时）
    if [[ ${cur} =~ ^[0-9]*$ ]]; then
        local ids=$(restore -l 2>/dev/null | awk '/^[0-9]+/ {print $1}')
        COMPREPLY=( $(compgen -W "${ids}" -- "${cur}") )
    fi
}

# zrestore 补全
_zrestore() {
    local cur opts
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    opts="-l --list -i --interactive -h --help -v --version"

    if [[ ${cur} == -* ]]; then
        COMPREPLY=( $(compgen -W "${opts}" -- "${cur}") )
    fi
}

complete -F _safe_rm safe_rm
if [ -f $HOME/../.covered_sys_rm ]; then
    complete -r rm
    complete -F _safe_rm rm
fi
complete -F _restore restore
complete -F _zrestore zrestore
