#!/bin/bash
# 统一加载当前目录下所有 .sh 补全脚本

# 获取本脚本所在目录
_COMPLETION_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"

for script in "$_COMPLETION_DIR"/*.sh; do
    # 避免循环加载自身
    [ "${script##*/}" = "complete.sh" ] && continue
    if [ -f "$script" ]; then
        source "$script"
    fi
done

unset _COMPLETION_DIR script
