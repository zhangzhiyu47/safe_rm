#!/bin/bash
# Shell completion script for zrestore (supports Bash and Zsh)

# Bash completion
if [[ -n ${BASH_VERSION-} ]]; then
    _zrestore_completions() {
        local cur prev opts
        COMPREPLY=()
        cur="${COMP_WORDS[COMP_CWORD]}"
        prev="${COMP_WORDS[COMP_CWORD-1]}"

        opts="-l --list -h --help -V --version"

        case "${prev}" in
            -l|--list|-h|--help|-V|--version)
                return 0
                ;;
            *)
                ;;
        esac

        COMPREPLY=( $(compgen -W "${opts}" -- "${cur}") )
    }
    complete -F _zrestore_completions zrestore

# Zsh completion
elif [[ -n ${ZSH_VERSION-} ]]; then
    #compdef zrestore
    _zrestore() {
        local curcontext="$curcontext" state line
        typeset -A opt_args

        _arguments -C \
            '(-l --list)'{-l,--list}'[List all restorable items (plain text)]' \
            '(-h --help)'{-h,--help}'[Print help]' \
            '(-V --version)'{-V,--version}'[Print version]'
    }
    _zrestore "$@"
fi
