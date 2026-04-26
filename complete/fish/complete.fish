#!/usr/bin/env fish
# 统一加载当前目录下所有 .fish 补全脚本

set -l script_dir (dirname (status --current-filename))

for f in $script_dir/*.fish
    # 避免循环加载自身
    if test (basename $f) = "complete.fish"
        continue
    end
    if test -f $f
        source $f
    end
end
