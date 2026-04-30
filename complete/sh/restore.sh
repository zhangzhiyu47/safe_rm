#!/bin/bash
# Shell completion script for restore (supports Bash and Zsh)

# Bash completion
if [[ -n ${BASH_VERSION-} ]]; then
    _restore_completions() {
        local cur prev opts
        COMPREPLY=()
        cur="${COMP_WORDS[COMP_CWORD]}"
        prev="${COMP_WORDS[COMP_CWORD-1]}"

        # No fallback to file name completion allowed
        compopt +o default 2>/dev/null || true

        opts="-l --list -a --all -d --delete --delete-all -h --help -V --version"

        case "${prev}" in
            -d|--delete)
                local items
                items=$(restore -l 2>/dev/null | grep -E '^[0-9]+' | awk '{print $1}')
                COMPREPLY=( $(compgen -W "${items}" -- "${cur}") )
                return 0
                ;;
            -l|--list|-a|--all|--delete-all|-h|--help|-V|--version)
                return 0
                ;;
            *)
                ;;
        esac

        if [[ ${cur} == -* ]]; then
            COMPREPLY=( $(compgen -W "${opts}" -- "${cur}") )
        else
            local items
            items=$(restore -l 2>/dev/null | grep -E '^[0-9]+' | awk '{print $1}')
            COMPREPLY=( $(compgen -W "${items}" -- "${cur}") )
        fi
    }
    complete -F _restore_completions restore

# Zsh completion
elif [[ -n ${ZSH_VERSION-} ]]; then
    #compdef restore
    _restore() {
        local curcontext="$curcontext" state line
        typeset -A opt_args

        _arguments -C \
            '(-l --list)'{-l,--list}'[List all restorable items]' \
            '(-a --all)'{-a,--all}'[Restore all items]' \
            '(-d --delete)'{-d,--delete}'[Delete specified items (permanently)]:item IDs:->ids' \
            '(--delete-all)'--delete-all'[Delete all items (empty rubbish bin)]' \
            '(-h --help)'{-h,--help}'[Print help]' \
            '(-V --version)'{-V,--version}'[Print version]' \
            '*:item IDs:->ids'

        case $state in
            ids)
                local items
                items=$(restore -l 2>/dev/null | grep -E '^[0-9]+' | awk '{print $1}')
                _values 'item ID' ${(f)items}
                ;;
        esac
    }
    _restore "$@"
fi
