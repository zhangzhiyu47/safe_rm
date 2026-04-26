# SafeRm - Safe Deletion Tool

SafeRm is a command-line safe deletion tool that moves files to a rubbish bin instead of permanently deleting them. It provides automatic maintenance, a TUI management interface, and batch operations.

## Features

- **Safe Deletion**: Moves files to `~/.rubbishbin` instead of permanent deletion
- **Automatic Maintenance**: Daemon (`trashd`) compresses old directories and cleans up expired archives
- **TUI Manager** (`zrestore`): Interactive terminal interface with search, batch operations, and progress display
- **CLI Manager** (`restore`): Command-line tool for listing, restoring, and deleting items
- **Cache System**: Fast item listing with HMAC signature verification
- **Archives**: Groups of old directories are compressed into `.tar.gz` archives with manifests
- **Batch Operations**: Parallel processing of restores and deletions with progress reporting
- **Bilingual**: English and Chinese support in the TUI

## Installation

```bash
cargo build --release
```

The binaries will be available in `target/release/`:

- `safe_rm` - Main deletion tool
- `restore` - CLI rubbish bin manager
- `zrestore` - TUI rubbish bin manager

## Usage

### safe_rm

```bash
# Safe delete files
safe_rm file.txt directory/

# Force mode (no output)
safe_rm -f file.txt

# Permanently delete (with typing verification)
safe_rm --remove-completely file.txt

# Trigger maintenance daemon
safe_rm --trigger

# Stop daemon
safe_rm --stop

# Check daemon status
safe_rm --status
```

### restore (CLI)

```bash
# List all restorable items
restore -l

# Restore item by ID
restore 1

# Restore all items
restore -a

# Delete items permanently by ID
restore -d 1
restore -d 2
restore -d 3

# Delete all items
restore --delete-all
```

### zrestore (TUI)

```bash
# Launch interactive TUI
zrestore

# List items in plain text
zrestore -l
```

#### TUI Keybindings

- `↑` / `↓` / `k` / `j` - Navigate
- `Space` - Toggle selection
- `a` - Select all
- `A` - Deselect all
- `Enter` - Restore current item
- `r` - Restore selected items
- `R` - Restore all visible items
- `d` - Delete selected items
- `D` - Delete all visible items
- `/` - Search
- `Tab` - Toggle exact/fuzzy search
- `L` - Switch language (English/Chinese)
- `?` / `h` / `F1` - Show help
- `q` - Quit

## Daemon Maintenance Policy

The `trashd` daemon performs automatic maintenance:

- **Compression**: When more than 20 timestamp directories exist, the oldest directories (older than 3 days) are compressed into `.tar.gz` archives in groups of 100
- **Cleanup**: Archives older than 60 days are automatically deleted
- **Fully Deleted Cleanup**: Archives where all items have been permanently deleted are removed immediately

## Directory Structure

```
~/.rubbishbin/
├── 2025-01-01-12:00:00/       # Timestamp directories
│   └── 1/                      # Index directories
│       ├── info                # Original path information
│       └── rubbish/            # Deleted files
├── .old/                       # Archives directory
│   ├── 2025-01-01-12:00:00.tar.gz
│   └── 2025-01-01-12:00:00.tar.gz.manifest.json
├── .cache/
│   └── items.json              # Cache file
└── .daemon/
    ├── trashd.pid              # Daemon PID file
    └── daemon.log              # Daemon log
```

## Safety Features

- **Rubbish Bin Protection**: Cannot delete the rubbish bin or its parent directory
- **Path Validation**: Blocks deletion of `.`, `..`, root directory, and current working directory
- **Permanent Deletion Confirmation**: Requires typing verification for `--remove-completely`
- **Symlink Handling**: Symbolic links are permanently removed (not moved to rubbish bin)
- **Cache Integrity**: HMAC-SHA256 signature verification prevents tampering

## Dependencies

- **Rust**: 1.70+
- **Core Libraries**: `anyhow`, `chrono`, `serde`, `rayon`, `sha2`, `hmac`, `base64`
- **System**: `nix`, `libc`
- **TUI**: `ratatui`, `crossterm`
- **File Watching**: `notify`
- **Compression**: `flate2`, `tar`

## License

MIT License
