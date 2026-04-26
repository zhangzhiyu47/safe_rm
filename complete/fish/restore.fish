# Fish completion script for restore

# Options
complete -c restore -s l -l list -d "List all restorable items"
complete -c restore -s a -l all -d "Restore all items"
complete -c restore -s d -l delete -d "Delete specified items (permanently)"
complete -c restore -l delete-all -d "Delete all items (empty rubbish bin)"
complete -c restore -s h -l help -d "Print help"
complete -c restore -s V -l version -d "Print version"

# Item IDs for -d option and positional arguments
complete -c restore -s d -a \
    "(restore -l 2>/dev/null | string match -r '^[0-9]+' | string split ' ' -f 1)"
complete -c restore -a \
    "(restore -l 2>/dev/null | string match -r '^[0-9]+' | string split ' ' -f 1)"
