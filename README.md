# SafeRm - Rust Version

A safe deletion tool as an alternative to Linux `rm` command, providing a "rubbish bin" mechanism and "typing verification" double protection.

This is a Rust rewrite of the original C version, using modern Rust libraries:
- `tar` + `flate2` instead of hand-written tar/gzip implementation
- `ratatui` instead of ncurses
- `clap` for command line parsing
- `chrono` for time handling

## Features

### safe_rm - Safe Deletion Tool
- **Safe Delete (default)**: Move files to rubbish bin (~/.rubbishbin/) instead of permanent deletion
- **Complete Delete**: With typing verification mechanism to prevent accidental deletion, real-time character-level color feedback
- **Safety Check**: Prevent deletion of rubbish bin or its parent directories
- **Auto Cleanup**: Empty directories are automatically cleaned up after delete/restore

### restore - Command Line Rubbish Bin Management Tool
- List all restorable items (with adaptive screen size)
- Restore items by ID
- Restore all items
- Permanently delete items
- Empty rubbish bin
- **Cache Support**: Fast loading with automatic cache (5 min expiry), auto-detects external changes

### zrestore - Interactive Rubbish Bin Management Tool (TUI)
- Beautiful htop-style interface (using ratatui)
- Keyboard navigation
- Real-time search (exact/fuzzy)
- Multi-select operations
- Colorful display
- Confirmation dialogs
- Language switching (English/Chinese)
- **Lazy Loading**: Only renders visible items for large lists
- **Cache Support**: Fast startup with automatic cache

## Quick Start (Pre-built Binaries)

Pre-built binaries are included in the `bin/` directory:

```bash
# Set execute permissions
chmod +x bin/safe_rm bin/restore bin/zrestore bin/trashd

# Copy to system directory
sudo cp bin/safe_rm bin/restore bin/zrestore bin/trashd /usr/local/bin/

# Or copy to user directory
mkdir -p ~/.local/bin
cp bin/safe_rm bin/restore bin/zrestore bin/trashd ~/.local/bin/
```

## Build from Source

### Install Rust
```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source $HOME/.cargo/env
```

### Build
```bash
cd safe_rm_rust
cargo build --release
```

The compiled binaries will be in `target/release/`.

## Usage

### safe_rm
```bash
safe_rm file.txt                    # Safe delete (move to rubbish bin)
safe_rm --remove-completely file.txt # Permanently delete
```

### restore
```bash
restore -l              # List all items
restore 1               # Restore item with ID=1
restore -a              # Restore all items
restore -d 1 2          # Permanently delete items
restore --delete-all    # Empty rubbish bin
```

### zrestore (TUI)
```bash
zrestore                # Start interactive TUI
zrestore -l             # List mode
```

### trashd - Daemon for Automatic Maintenance
```bash
trashd                  # Start the daemon
trashd --stop           # Stop the running daemon
trashd --status         # Check if daemon is running
trashd --trigger        # Trigger maintenance immediately
```

**Daemon Features:**
- **Auto-compression**: Compresses old directories (older than 3 days) to save space
- **Keep uncompressed**: Retains latest 20 directories in uncompressed form
- **Auto-cleanup**: Deletes archives older than 60 days
- **Signal-driven**: Triggered by `safe_rm` after each deletion
- **Single instance**: File lock ensures only one daemon runs at a time

**Keyboard Shortcuts:**

| Key | Function |
|-----|----------|
| `↑/↓` or `k/j` | Navigate up/down |
| `PgUp/PgDn` | Page up/down |
| `Home/End` | Jump to first/last |
| `Space` | Select/deselect current item |
| `a` | Select all matching items |
| `A` | Deselect all |
| `Enter` | Restore current item |
| `r` | Restore selected items |
| `R` | Restore all visible items |
| `d` | Delete selected items (permanent) |
| `D` | Delete all visible items |
| `/` or `s/S` | Open search box |
| `Tab` | Toggle search mode (Exact/Fuzzy) |
| `L` | Toggle language (EN/CN) |
| `Esc` | Clear search filter / Close dialog |
| `?` or `h/H` or `F1` | Show help |
| `q/Q` | Quit |

**Search Mode:**
- Press `/` to open search box
- Type to filter items
- Press `Tab` to toggle between Exact/Fuzzy mode
- Press `Enter` to confirm search
- Press `Esc` to clear and close search box

## Directory Structure

```
~/.rubbishbin/
├── YYYY-MM-DD-HH:MM:SS/     # Timestamp directory
│   ├── 1/
│   │   ├── info              # Absolute path of original directory
│   │   └── rubbish/          # Actual moved files
│   │       └── filename
│   └── 2/
│       └── ...
└── old/                      # Archive directory
    └── YYYY-MM-DD-HH:MM:SS.tar.gz
```

## Safety Rules

1. **Never delete** `$HOME/.rubbishbin` or any of its contents
2. **Never delete** parent directory of rubbish bin (e.g., `/home`, `/`)
3. **Never overwrite** existing files during restore (skip conflicts)
4. **Confirmation required** before permanent deletion from rubbish bin

## Dependencies

- `clap` - Command line argument parsing
- `chrono` - Date/time handling
- `tar` - Tar archive
- `flate2` - Gzip compression/decompression
- `ratatui` - TUI interface
- `crossterm` - Terminal control
- `anyhow` - Error handling
- `dirs` - Directory paths
- `tempfile` - Temporary files
- `nix` - Unix system calls (signals, processes, file locks)
- `libc` - Low-level system interfaces
- `regex` - Regular expression matching

## License

MIT License
