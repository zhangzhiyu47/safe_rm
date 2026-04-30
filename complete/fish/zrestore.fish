# Fish completion script for zrestore

# Disable file name completion
complete -c zrestore -f

# Options
complete -c zrestore -s l -l list -d "List all restorable items (plain text)"
complete -c zrestore -s h -l help -d "Print help"
complete -c zrestore -s V -l version -d "Print version"
