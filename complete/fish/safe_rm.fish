# Fish completion script for safe_rm

# Disable file completions for options
complete -c safe_rm -s f -l force -d "Force mode - no output"
complete -c safe_rm -l remove-completely -d "Permanently delete (with confirmation)"
complete -c safe_rm -s h -l help -d "Print help"
complete -c safe_rm -s V -l version -d "Print version"
complete -c safe_rm -l trigger -d "Trigger (or start) daemon to maintain rubbish bin"
complete -c safe_rm -l stop -d "Stop the running daemon"
complete -c safe_rm -l status -d "Check if daemon is running"
