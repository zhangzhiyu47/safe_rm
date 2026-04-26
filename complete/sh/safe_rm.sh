#!/bin/bash
# Shell completion script for safe_rm (supports Bash and Zsh)

# Bash completion
if [[ -n ${BASH_VERSION-} ]]; then
    _safe_rm_completions() {
        local cur prev opts
        COMPREPLY=()
        cur="${COMP_WORDS[COMP_CWORD]}"
        prev="${COMP_WORDS[COMP_CWORD-1]}"

        opts="-f --force --remove-completely -h --help -V --version --trigger --stop --status"

        case "${prev}" in
            -f|--force|--remove-completely|-h|--help|-V|--version|--trigger|--stop|--status)
                return 0
                ;;
            *)
                ;;
        esac

        if [[ ${cur} == -* ]]; then
            COMPREPLY=( $(compgen -W "${opts}" -- "${cur}") )
        else
            COMPREPLY=( $(compgen -f -- "${cur}") )
        fi
    }
    complete -F _safe_rm_completions safe_rm

# Zsh completion
elif [[ -n ${ZSH_VERSION-} ]]; then
    #compdef safe_rm
    _safe_rm() {
        local curcontext="$curcontext" state line
        typeset -A opt_args

        _arguments -C \
            '(-f --force)'{-f,--force}'[Force mode - no output]' \
            '(--remove-completely)'--remove-completely'[Permanently delete (with confirmation)]' \
            '(-h --help)'{-h,--help}'[Print help]' \
            '(-V --version)'{-V,--version}'[Print version]' \
            '(--trigger)'--trigger'[Trigger (or start) daemon to maintain rubbish bin]' \
            '(--stop)'--stop'[Stop the running daemon]' \
            '(--status)'--status'[Check if daemon is running]' \
            '*:files:_files'
    }
    _safe_rm "$@"
fi
