# safe_rm - Agent Development Documentation

## Project Overview

**safe_rm** is a safe file deletion tool written in C, designed as a replacement for the Linux `rm` command. It provides a "trash bin" mechanism with recovery capabilities and a "typing verification" system for permanent deletion.

The project includes three main executables:
- `safe_rm` - Main deletion tool, supporting both safe delete (move to trash) and complete removal (permanent delete with verification)
- `restore` - Trash bin management tool for restoring and permanently deleting files
- `zrestore` - Interactive ncurses-based trash bin manager with multi-select, fuzzy search, and batch operations

**Language**: C (C99/C11 compatible)  
**Platform**: Linux x86_64 and ARM64 (including Termux)  
**License**: MIT License  
**Version**: 1.0.0

## Architecture

### Core Components

```
┌────────────────────────────────────────────────────────────────┐
│                       safe_rm (Main Program)                   │
│  ┌─────────────────┐  ┌──────────────────┐  ┌────────────────┐ │
│  │  safe_delete.c  │  │ complete_remove.c│  │    daemon.c    │ │
│  │  (Safe Mode)    │  │ (Permanent Del)  │  │  (Background)  │ │
│  │                 │  │                  │  │                │ │
│  └────────┬────────┘  └────────┬─────────┘  └───────┬────────┘ │
│           │                    │                    │          │
│           ▼                    ▼                    ▼          │
│  ┌─────────────────┐  ┌──────────────────┐  ┌────────────────┐ │
│  │    mv_lib.c     │  │   actual_rm.c    │  │   tarlib.c     │ │
│  │ (Cross-fs Move) │  │  (nftw removal)  │  │ (tgz support)  │ │
│  └─────────────────┘  └──────────────────┘  └────────────────┘ │
└────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                       restore (Recovery Tool)                   │
│  ┌─────────────────┐  ┌──────────────────┐                      │
│  │ restore_main.c  │  │    restore.c     │                      │
│  │   (Entry Point) │  │  (Recovery Logic)│                      │
│  └─────────────────┘  └──────────────────┘                      │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                    zrestore (Interactive Tool)                  │
│  ┌─────────────────┐  ┌──────────────────┐                      │
│  │ zrestore_main.c │  │    zrestore.c    │                      │
│  │   (Entry Point) │  │  (NCurses UI)    │                      │
│  └─────────────────┘  └──────────────────┘                      │
└─────────────────────────────────────────────────────────────────┘
```

### Trash Bin Directory Structure

```
~/.rubbishbin/
├── YYYY-MM-DD-HH:MM:SS/          # Timestamp-based deletion batches
│   ├── 1/                        # Index for each deleted item
│   │   ├── info                  # Original directory path (absolute)
│   │   └── rubbish/              # Actual file content
│   │       └── filename
│   └── 2/
│       └── ...
├── old/                          # Archived compressed batches
│   └── YYYY-MM-DD-HH:MM:SS.tar.gz
└── .daemon.pid                   # Daemon PID file
```

## Technology Stack

- **Build System**: GNU Make
- **Standard APIs**: POSIX (dirent.h, stat, unistd.h, termios.h)
- **Signal Handling**: SIGUSR1 (daemon trigger), SIGTERM (daemon shutdown)
- **Process Control**: Double-fork daemon pattern, PID file locking
- **Archive Format**: Custom TAR+GZIP implementation (tarlib)
- **Terminal I/O**: Raw mode via termios for typing verification

## Build System

### Make Targets

```bash
# Build both executables (default)
make
# or
make all

# Build with debug symbols
make DEBUG=1

# Install to system directories
# - x86_64: /usr/bin
# - ARM64:  $PREFIX/bin
make install

# Uninstall
make uninstall

# Replace system rm with safe_rm (dangerous!)
make cover

# Restore original system rm
make uncover

# Add alias to ~/.bashrc
make alias

# Run basic version tests
make test

# Clean build artifacts
make clean
```

### Compiler Configuration

| Architecture | Compiler | CFLAGS | Install Path |
|--------------|----------|--------|--------------|
| x86_64 | gcc | -Wall -Wextra -O3 -DX86_64 | /usr/bin |
| ARM64 | gcc | -Wall -Wextra -O3 -DARM64 | $PREFIX/bin |

### Source Files by Target

**safe_rm executable**:
- `safe_rm.c`, `utils.c`, `complete_remove.c`, `safe_delete.c`
- `daemon.c`, `actual_rm.c`, `proctitle.c`, `mv_lib.c`, `tarlib.c`

**restore executable**:
- `restore_main.c`, `utils.c`, `restore.c`, `mv_lib.c`, `actual_rm.c`, `tarlib.c`

**zrestore executable**:
- `zrestore_main.c`, `zrestore.c`, `utils.c`, `restore.c`, `mv_lib.c`, `actual_rm.c`, `tarlib.c`
- Links with `-lncurses` for terminal UI

## Key Configuration Constants

Defined in `safe_rm.h`:

```c
#define RUBBISH_BIN_NAME ".rubbishbin"     // Trash bin directory name
#define DAEMON_PID_FILE ".daemon.pid"     // Daemon PID filename
#define OLD_DIR "old"                     // Archive subdirectory
#define COMPRESS_THRESHOLD 10             // Min directories before compression
#define COMPRESS_DAYS 3                   // Days threshold for compression
#define CLEANUP_DAYS 60                   // Days before archive deletion
#define MAX_PATH_LEN 4096
#define MAX_NAME_LEN 256

// Verification string for permanent deletion
#define VERIFY_STRING "I am sure I want to remove these files or directories"
```

### zrestore UI Constants

Defined in `zrestore.h`:

```c
#define HEADER_HEIGHT    2    // Header + column headers
#define FOOTER_HEIGHT    1    // Bottom shortcut bar
#define PREVIEW_HEIGHT   8    // Bottom preview panel
#define MIN_ROWS         15   // Minimum terminal rows required
#define MIN_COLS         80   // Minimum terminal columns required
```

## Code Organization

### Module Responsibilities

| File | Purpose |
|------|---------|
| `safe_rm.c` | Main entry point, CLI argument parsing, mode dispatch |
| `safe_delete.c` | Safe deletion: validation, trash bin move, index management |
| `complete_remove.c` | Permanent deletion: typing verification, terminal control |
| `daemon.c` | Background daemon: compression, cleanup, signal handling |
| `restore.c` | File recovery: scanning, extraction from archives |
| `restore_main.c` | Recovery tool CLI |
| `zrestore.c` | Interactive NCurses UI: drawing, input handling, colors, multi-select, fuzzy search |
| `zrestore_main.c` | Interactive trash manager entry point with batch operations |
| `utils.c` | Path utilities, directory creation, timestamp handling |
| `actual_rm.c` | Low-level recursive removal using nftw() |
| `mv_lib.c` | Cross-filesystem move with copy+delete fallback |
| `tarlib.c` | Pure C tar.gz creation, extraction and iteration |
| `proctitle.c` | Process title modification for daemon |

### Type Definitions

```c
// Operation mode enum
typedef enum {
    MODE_SAFE_DELETE,       // Move to trash bin
    MODE_COMPLETE_REMOVE    // Permanent delete with verification
} OperationMode;

// Recovery item structure
typedef struct {
    int id;
    char timestamp[FULL_TIME_LEN];
    char original_path[MAX_PATH_LEN];
    char filename[MAX_NAME_LEN];
    int is_archived;        // 0=active, 1=archived
    char archive_file[MAX_PATH_LEN];
} RestoreItem;
```

## Safety Mechanisms

### 1. Trash Bin Protection
- Cannot delete `$HOME/.rubbishbin` or any subdirectory
- Cannot delete parent directory of rubbish bin (e.g., `/home`, `/`)
- Cannot delete current working directory (CWD)
- Cannot delete root directory `/`

### 2. Typing Verification (Permanent Deletion)
- Requires typing exact verification string
- Real-time character-level color feedback (green=correct, red=error)
- Backspace support for corrections
- Terminal set to raw mode for immediate character capture

### 3. Recovery Safety
- Never overwrites existing files (conflict detection via stat())
- Preserves original directory structure during recovery

### 4. Permanent Delete from Trash
- Requires confirmation before permanent deletion from trash bin
- Red warning color in UI to indicate destructive operation
- Default "No" selection in delete confirmation for safety
- Supports batch deletion with confirmation

### 5. Daemon Singleton (Fixed)
The daemon now uses **file locking** to ensure singleton behavior:
- `lock_pid_file()` - Acquires exclusive lock on PID file (non-blocking)
- Double-checked locking pattern prevents race conditions
- Failed lock acquisition means another process is starting the daemon

### 6. Concurrent Delete Protection (Fixed)
Safe delete operations use a **directory lock** (`~/.rubbishbin/.lock`):
- `lock_rubbish_bin()` - Acquires exclusive lock before any operation
- Timestamp generation and directory creation are atomic under lock
- Prevents duplicate timestamps and index conflicts

## Development Guidelines

### Code Style
- All comments in English
- Indentation: 4 spaces
- Function naming: snake_case
- Constants: UPPER_CASE_WITH_UNDERSCORES
- Static functions for internal use

### Error Handling Pattern
```c
if (operation() != 0) {
    perror("error: context");
    // cleanup
    return -1;
}
```

### Memory Management
- Stack allocation preferred for fixed-size buffers (MAX_PATH_LEN)
- `static` for persistent strings within function scope
- Free allocated memory in reverse order of allocation
- Check malloc/calloc return values for NULL

### Path Handling
- Always use `MAX_PATH_LEN` (4096) for path buffers
- Use `realpath()` for path canonicalization before comparison
- Strip trailing slashes before operations (except root `/`)

## Testing

### Manual Test Procedure

```bash
# Build
make clean && make

# Test safe deletion
echo "test content" > ~/test_safe.txt
./safe_rm ~/test_safe.txt
./restore -l  # Should list the file
./restore 1   # Recover the file

# Test permanent deletion from trash
echo "test content" > ~/test_delete.txt
./safe_rm ~/test_delete.txt
./restore -l  # Should list the file
./restore -d 1   # Delete permanently (with confirmation)
./restore -l  # Should be empty

# Test complete removal
echo "test content" > ~/test_remove.txt
./safe_rm --remove-completely ~/test_remove.txt
# Type: I am sure I want to remove these files or directories

# Test safety constraints
./safe_rm ~/.rubbishbin  # Should refuse
./safe_rm /              # Should refuse
./safe_rm .              # Should refuse

# Test daemon compression
# Create 12+ old directories (modify timestamps)
for day in $(seq 1 12); do
    dir="$HOME/.rubbishbin/2026-01-$(printf "%02d" $day)-10:00:00"
    mkdir -p "$dir/1/rubbish"
    echo "test" > "$dir/1/rubbish/file.txt"
    echo "$HOME" > "$dir/1/info"
    touch -d "5 days ago" "$dir"
done

# Trigger daemon maintenance
./safe_rm --help
kill -USR1 $(cat ~/.rubbishbin/.daemon.pid)

# Check archives in ~/.rubbishbin/old/
ls ~/.rubbishbin/old/
```

### zrestore Test Procedure

```bash
# Build (includes zrestore)
make clean && make

# Test interactive mode (requires terminal with ncurses support)
./zrestore

# Test list mode
./zrestore -l

# Test help
./zrestore --help
./zrestore --version

# Test navigation within zrestore:
# - Use ↑/↓ to select items
# - Press Space to toggle selection (multi-select)
# - Press a to select all, A to deselect all
# - Press / to search, type filter
# - Press Tab to switch search mode (exact/fuzzy)
# - Press Enter to restore selected item
# - Press r to restore all selected items
# - Press R to restore all visible items
# - Press d to delete selected items (permanent, with confirmation)
# - Press D to delete all visible items (with confirmation)
# - Press Q to quit

# Test zrestore with many items
cd /tmp
for i in $(seq 1 50); do
    echo "test file $i" > "testfile_$i.txt"
    ~/safe_rm/safe_rm "testfile_$i.txt"
done

# Now test scrolling, multi-select, and search
~/safe_rm/zrestore

# Test fuzzy search mode
# 1. Press / to open search
# 2. Press Tab to switch to fuzzy mode
# 3. Type partial filename to find similar matches
# 4. See similarity scores displayed on the right

# Test batch operations
# 1. Select multiple items with Space
# 2. Press r to restore selected
# 3. Or press x to delete selected (with red confirmation dialog)
```

### Architecture Test
```bash
# Check architecture detection
make clean && make
file safe_rm  # Should show correct architecture
```

## Safety Considerations

1. **Path Traversal**: All paths are canonicalized with `realpath()` before comparison
2. **Race Conditions**: PID file + kill(0) check ensures daemon singleton; file locks protect concurrent operations
3. **Signal Safety**: Terminal settings restored via signal handlers (SIGINT ignored during verification)
4. **Permission Escalation**: No setuid; runs with user privileges
5. **Archive Extraction**: Paths within tar are validated to prevent directory traversal

## Daemon Behavior

The daemon (`daemon.c`) runs as a singleton background process:

1. **Startup**: Triggered on first safe_rm execution
2. **Signal Handling**: SIGUSR1 triggers maintenance, SIGTERM for shutdown
3. **Maintenance Tasks**:
   - Compress directories older than `COMPRESS_DAYS` (3 days)
   - Delete archives older than `CLEANUP_DAYS` (60 days)
   - Compress in batches of `COMPRESS_THRESHOLD` (10 directories)
4. **Logging**: Uses syslog with "safe_rm_daemon" identifier
5. **Process Name**: Sets process title to "trashd"

### Compression Algorithm (Fixed)

```c
// Calculate directories to compress (keep newest COMPRESS_THRESHOLD)
int dirs_to_compress = count - COMPRESS_THRESHOLD;

// Compress in batches of COMPRESS_THRESHOLD
for (i = 0; i < dirs_to_compress; i += COMPRESS_THRESHOLD) {
    // Build path list for this batch
    // Create tar.gz archive
    // On success: delete original directories
}
```

## Platform-Specific Notes

### Termux (ARM64) Support
- Uses `/data/data/com.termux/files/home` as default home directory
- Respects `$PREFIX` environment variable for installation
- Tested on Android devices in Termux environment

### x86_64 Linux
- Standard `/home/user` home directory structure
- Install to `/usr/bin` (requires root privileges)

## Agent Tasks

### Adding New Command Line Options

1. Add option parsing in `main()` of `safe_rm.c`
2. Define any new constants in `safe_rm.h`
3. Implement functionality in appropriate module
4. Update `print_help()` in `utils.c`
5. Add usage examples to README.md

### Modifying Trash Bin Directory Structure

1. Update path construction in `safe_delete.c`
2. Update scanning logic in `restore.c`
3. Update daemon compression in `daemon.c`
4. Test backward compatibility with existing trash bin content

### Extending Recovery System

The `RestoreItem` structure is the main interface between scanning and recovery:
- Add fields to `RestoreItem` in `safe_rm.h` if needed
- Update `scan_restore_items()` in `restore.c`
- Update `print_restore_list()` to display new fields

### Adding Trash Management Features

The restore module now supports both recovery and permanent deletion:

**Deletion Functions (restore.c):**
- `delete_item_by_id()` - Delete a single item by ID
- `delete_from_archive()` - Delete item from compressed archive
- `delete_items_batch()` - Batch delete multiple items

**CLI Integration (restore_main.c):**
- Add command line flags: `-d` for delete, `--delete-all` for empty trash
- Confirmation prompt before destructive operations
- Colored warning messages (red)

**UI Integration (zrestore.c):**
- Add `ACTION_DELETE_SELECTED` and `ACTION_DELETE_ALL` to action enum
- Add `x`/`X` key handlers
- Red warning colors for delete confirmation dialog
- Default "No" selection for safety

## Troubleshooting

### Daemon Not Running
- Check `~/.rubbishbin/.daemon.pid`
- Verify with `kill -0 <pid>`
- Daemon auto-restarts on next safe_rm call

### Permission Denied During Recovery
- Ensure original directory still exists and is writable
- Check if target file already exists (conflict detection)

### Cross-Filesystem Move Fails
- `mv_lib.c` automatically falls back to copy+delete
- Progress logging may be limited for large file transfers

### Compression Not Working
- Verify directory count exceeds `COMPRESS_THRESHOLD` (10)
- Check that oldest directory is older than `COMPRESS_DAYS` (3 days)
- Look for archives in `~/.rubbishbin/old/`

### zrestore Issues

**Terminal too small:**
- zrestore requires at least 15 rows x 80 columns
- Falls back to list mode automatically in non-interactive terminals

**Colors not displaying:**
- Ensure terminal supports 256 colors
- Check `TERM` environment variable (should be `xterm-256color` or similar)
- Try: `export TERM=xterm-256color`

**Keyboard input not working:**
- Ensure terminal supports ncurses
- Check that `stty` settings allow key input
- Try resetting terminal: `reset`

**Build fails with ncurses errors:**
- Install ncurses development package:
  - Debian/Ubuntu: `apt-get install libncurses5-dev`
  - RHEL/CentOS: `yum install ncurses-devel`
  - Termux: `pkg install ncurses`
