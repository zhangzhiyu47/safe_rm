//! trashd - SafeRm daemon for automatic maintenance

use anyhow::Result;
use nix::fcntl::{fcntl, FcntlArg, OFlag};
use nix::sys::stat::Mode;
use nix::unistd::{fork, ForkResult, Pid, execvp, setsid};
use nix::sys::signal::{kill, Signal, SigSet, sigprocmask};
use nix::sys::signalfd::SignalFd;
use libc::nice;
use std::fs;
use std::env;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::time::SystemTime;
use std::ffi::CString;
use std::os::unix::io::RawFd;
use std::os::unix::fs::MetadataExt;
use std::os::unix::ffi::OsStrExt;
use chrono::Local;
use crate::{
    create_dir_all, get_rubbish_bin_path, get_timestamp, 
    parse_timestamp, CLEANUP_DAYS, COMPRESS_DAYS, 
    COMPRESS_THRESHOLD, OLD_DIR, DAEMON_NAME, DAEMON_ENV_VAR, 
    DAEMON_PID_FILE, DAEMON_LOG_FILE, get_daemon_dir,
    ARCHIVE_CAPACITY,
};

const MAX_LOG_SIZE: u64      = 10 * 1024 * 1024;
const MAX_LOG_FILES: u32     = 3;
const COMPRESSION_LEVEL: u32 = 6;

/// Get PID file path
pub fn get_pid_file_path() -> Result<PathBuf> {
    let daemon_dir = get_daemon_dir()?;
    Ok(daemon_dir.join(DAEMON_PID_FILE))
}

/// Read PID from PID file
pub fn read_pid_file() -> Option<Pid> {
    let pid_file = get_pid_file_path().ok()?;
    let content = fs::read_to_string(&pid_file).ok()?;
    let pid: i32 = content.trim().parse().ok()?;
    Some(Pid::from_raw(pid))
}

/// Write PID to PID file
pub fn write_pid_file(pid: Pid) -> Result<()> {
    let pid_file = get_pid_file_path()?;
    fs::write(&pid_file, pid.as_raw().to_string())?;
    Ok(())
}

/// Remove PID file
pub fn remove_pid_file() -> Result<()> {
    let pid_file = get_pid_file_path()?;
    if pid_file.exists() {
        fs::remove_file(&pid_file)?;
    }
    Ok(())
}

/// Check if process is alive
pub fn is_process_alive(pid: Pid) -> bool {
    kill(pid, None).is_ok()
}

/// Lock PID file (non-blocking)
/// Returns file descriptor if successful, None otherwise
pub fn lock_pid_file() -> Option<RawFd> {
    let pid_file = get_pid_file_path().ok()?;
    let fd = nix::fcntl::open(
        &pid_file,
        OFlag::O_RDWR | OFlag::O_CREAT,
        Mode::from_bits_truncate(0o644),
    )
    .ok()?;

    // Try to get exclusive lock (non-blocking)
    let flock = libc::flock {
        l_type: libc::F_WRLCK as i16,
        l_whence: libc::SEEK_SET as i16,
        l_start: 0,
        l_len: 0,
        l_pid: 0,
    };

    if fcntl(fd, FcntlArg::F_SETLK(&flock)).is_err() {
    let _ = nix::unistd::close(fd);
        if let Some(pid) = read_pid_file() {
            if !is_process_alive(pid) {
                return lock_pid_file();
            }
        }
        return None;
    }

    Some(fd)
}

/// Rotate the daemon log file if it exceeds MAX_LOG_SIZE
/// Old logs are kept as daemon.log.1, daemon.log.2, etc.
fn rotate_log_if_needed(daemon_dir: &Path) {
    let log_path = daemon_dir.join(DAEMON_LOG_FILE);

    if let Ok(metadata) = fs::metadata(&log_path) {
        if metadata.len() < MAX_LOG_SIZE {
            return;
        }
    } else {
        return;
    }

    // Rename existing historical logs: .2 → .3, .1 → .2
    for i in (1..MAX_LOG_FILES).rev() {
        let old = daemon_dir.join(format!("{}.{}", DAEMON_LOG_FILE, i));
        let new = daemon_dir.join(format!("{}.{}", DAEMON_LOG_FILE, i + 1));
        let _ = fs::rename(&old, &new);
    }

    // Rename current log to .1
    let backup = daemon_dir.join(format!("{}.1", DAEMON_LOG_FILE));
    let _ = fs::rename(&log_path, &backup);
}

/// Unlock and close PID file
pub fn unlock_pid_file(fd: RawFd) {
    let _ = nix::unistd::close(fd);
}

/// Count A directories (timestamp directories)
pub fn count_a_directories(rubbish_bin: &Path) -> Result<Vec<String>> {
    let mut dirs = Vec::new();

    if !rubbish_bin.exists() {
        return Ok(dirs);
    }

    for entry in fs::read_dir(rubbish_bin)? {
        let entry = entry?;
        let path = entry.path();
        let name = entry.file_name().to_string_lossy().to_string();

        if name == OLD_DIR || name.starts_with('.') {
            continue;
        }

        if path.is_dir() && parse_timestamp(&name).is_some() {
            dirs.push(name);
        }
    }

    Ok(dirs)
}

/// Calculate days since directory creation
pub fn days_since_creation(metadata: &fs::Metadata) -> Result<i64> {
    let modified = metadata.modified()?;
    let now = SystemTime::now();
    let duration = now.duration_since(modified).unwrap_or_default();
    Ok(duration.as_secs() as i64 / (24 * 3600))
}

/// Remove directory recursively
pub fn remove_directory_recursive(path: &Path) -> Result<()> {
    if path.is_dir() {
        fs::remove_dir_all(path)?;
    } else {
        fs::remove_file(path)?;
    }
    Ok(())
}

/// Create tar.gz archive from directories
pub fn create_tar_gz(archive_path: &Path, dirs: &[&Path], compression_level: u32) -> Result<()> {
    use flate2::write::GzEncoder;
    use flate2::Compression;
    use tar::Builder;

    let file = fs::File::create(archive_path)?;
    let encoder = GzEncoder::new(file, Compression::new(compression_level));
    let mut builder = Builder::new(encoder);

    for dir in dirs {
        builder.append_dir_all(
            dir.file_name().unwrap_or_default().to_string_lossy().as_ref(),
            dir,
        )?;
    }

    builder.finish()?;
    Ok(())
}

/// Compress old directories
pub fn compress_old_directories() -> Result<usize> {
    let rubbish_bin = get_rubbish_bin_path()?;
    let old_dir = rubbish_bin.join(OLD_DIR);

    let mut dirs = count_a_directories(&rubbish_bin)?;

    if dirs.len() <= COMPRESS_THRESHOLD {
        return Ok(0);
    }

    // Sort by timestamp (oldest first)
    dirs.sort();

    // Check if oldest directory is old enough
    let oldest_dir = rubbish_bin.join(&dirs[0]);
    let oldest_metadata = fs::metadata(&oldest_dir)?;   // 仅一次 stat
    let days = days_since_creation(&oldest_metadata)?;  // 传入元数据

    if days < COMPRESS_DAYS {
        return Ok(0);
    }

    // Ensure old directory exists
    create_dir_all(&old_dir)?;

    let dirs_to_compress = dirs.len() - COMPRESS_THRESHOLD;
    let full_groups = dirs_to_compress / ARCHIVE_CAPACITY;
    let mut compressed = 0;
    let mut group_index = 0;

    // Compress only full groups (each of ARCHIVE_CAPACITY directories)
    for i in 0..full_groups {
        let timestamp = get_timestamp();
        let tar_file = if group_index == 0 {
            old_dir.join(format!("{}.tar.gz", timestamp))
        } else {
            old_dir.join(format!("{}_{}.tar.gz", timestamp, group_index))
        };
        group_index += 1;

        let start = i * ARCHIVE_CAPACITY;
        let group: Vec<PathBuf> = dirs[start..start + ARCHIVE_CAPACITY]
            .iter()
            .map(|name| rubbish_bin.join(name))
            .collect();

        let group_refs: Vec<&Path> = group.iter().map(|p| p.as_path()).collect();

        if create_tar_gz(&tar_file, &group_refs, COMPRESSION_LEVEL).is_ok() {
            // Generate manifest BEFORE removing original directories
            let _ = crate::generate_manifest(&tar_file, &group_refs);
            // Remove original directories after successful compression
            for dir in &group {
                let _ = remove_directory_recursive(dir);
            }
            compressed += 1;
        }
    }

    Ok(compressed)
}

/// Clean up old archives
pub fn cleanup_old_archives() -> Result<usize> {
    let rubbish_bin = get_rubbish_bin_path()?;
    let old_dir = rubbish_bin.join(OLD_DIR);
    let mut deleted = 0;

    if !old_dir.exists() {
        return Ok(0);
    }

    let regex =
        regex::Regex::new(r"^\d{4}-\d{2}-\d{2}-\d{2}:\d{2}:\d{2}(?:_\d+)?\.tar\.gz$")
            .unwrap();

    for entry in fs::read_dir(&old_dir)? {
        let entry = entry?;
        let name = entry.file_name().to_string_lossy().to_string();

        if regex.is_match(&name) {
            let metadata = entry.metadata()?;
            let modified = metadata.modified()?;
            let now = SystemTime::now();
            let duration = now.duration_since(modified).unwrap_or_default();
            let days = duration.as_secs() as i64 / (24 * 3600);

            if days > CLEANUP_DAYS {
                let path = entry.path();
                if fs::remove_file(&path).is_ok() {
                    // Also remove manifest if exists
                    let manifest_path = crate::manifest_path(&path);
                    let _ = fs::remove_file(manifest_path);
                    deleted += 1;
                }
            }
        }
    }

    Ok(deleted)
}

/// Perform maintenance tasks.
/// Returns (compressed_groups, fully_deleted_archives, expired_archives).
pub fn perform_maintenance() -> Result<(usize, usize, usize)> {
    let compressed = compress_old_directories()?;
    let fully_deleted = crate::cleanup_fully_deleted_archives()?;
    let expired = cleanup_old_archives()?;

    // Only refresh cache if something actually changed
    if compressed > 0 || fully_deleted > 0 || expired > 0 {
        let _ = crate::refresh_cache();
    }

    Ok((compressed, fully_deleted, expired))
}

/// Check and trigger daemon
pub fn check_and_trigger_daemon() -> Result<()> {
    // Try to lock PID file
    let lock_fd = match lock_pid_file() {
        Some(fd) => fd,
        None => {
            // Another process is starting daemon, wait and signal
            std::thread::sleep(std::time::Duration::from_millis(100));
            if let Some(pid) = read_pid_file() {
                if is_process_alive(pid) {
                    let _ = kill(pid, Signal::SIGUSR1);
                }
            }
            return Ok(());
        }
    };

    // Double-check after acquiring lock
    if let Some(pid) = read_pid_file() {
        if is_process_alive(pid) {
            // Daemon already running, signal it
            let _ = kill(pid, Signal::SIGUSR1);
            unlock_pid_file(lock_fd);
            return Ok(());
        }
    }

    // Clean up stale PID file
    let _ = remove_pid_file();

    // Start new daemon
    let result = start_daemon();

    unlock_pid_file(lock_fd);
    result
}

/// Start the trash daemon (using double fork + exec method).
pub fn start_daemon() -> Result<()> {
    let current_exe = env::current_exe()?;
    let exe_path = CString::new(current_exe.as_os_str().as_bytes())?;

    match unsafe { fork()? } {
        ForkResult::Parent { .. } => Ok(()),
        ForkResult::Child => {
            setsid()?;

            match unsafe { fork()? } {
                ForkResult::Parent { .. } => std::process::exit(0),
                ForkResult::Child => {
                    // Standard daemon settings
                    env::set_current_dir("/").ok();
                    let _ = nix::unistd::close(0);
                    let _ = nix::unistd::close(1);
                    let _ = nix::unistd::close(2);

                    // Set environment variables to identify daemon mode
                    env::set_var(DAEMON_ENV_VAR, "1");

                    // Construct argv: [DAEMON_NAME, "..."]
                    // The second parameter can be anything
                    let argv0 = CString::new(DAEMON_NAME)?;
                    let argv1 = CString::new("Manager")?;  // Placeholder
                    let args = [&argv0, &argv1];

                    execvp(&exe_path, &args)?;
                    // execvp doesn't return when successful
                    unreachable!();
                }
            }
        }
    }
}

fn is_same_file(file: &std::fs::File, path: &Path) -> bool {
    if let (Ok(f_meta), Ok(p_meta)) = (file.metadata(), std::fs::metadata(path)) {
        let f_ino = (f_meta.ino(), f_meta.dev());
        let p_ino = (p_meta.ino(), p_meta.dev());
        f_ino == p_ino
    } else {
        false
    }
}

/// Daemon main loop – event‑driven via signalfd (no polling).
fn daemon_main() {
    // Lower CPU priority to avoid affecting foreground operations
    let _ = unsafe { nice(10) };

    // Create the set of signals we want to handle.
    let mut mask = SigSet::empty();
    mask.add(Signal::SIGUSR1);
    mask.add(Signal::SIGTERM);
    mask.add(Signal::SIGINT);

    // Block the signals so they are delivered through signalfd instead.
    sigprocmask(
        nix::sys::signal::SigmaskHow::SIG_BLOCK,
        Some(&mask),
        None,
    )
    .expect("Failed to block signals");

    // Create a signalfd that will wake up when any blocked signal arrives.
    let sfd = SignalFd::new(&mask).expect("Failed to create signalfd");

    let daemon_dir = get_daemon_dir().expect("Failed to get daemon dir");

    let log_path = daemon_dir.join(DAEMON_LOG_FILE);
    let mut log_writer = std::io::BufWriter::new(
        std::fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(&log_path)
            .expect("Failed to open daemon log"),
    );

    // Write PID file (existing logic).
    let _ = write_pid_file(nix::unistd::getpid());

    // Main event loop.
    loop {
        match sfd.read_signal() {
            Ok(Some(siginfo)) => {
                let signo = siginfo.ssi_signo as i32;
                let signal = Signal::try_from(signo).unwrap_or(Signal::SIGUSR1);

                if signal == Signal::SIGUSR1 {
                    rotate_log_if_needed(&daemon_dir);

                    if !is_same_file(log_writer.get_ref(), &log_path) {
                        log_writer = std::io::BufWriter::new(
                            std::fs::OpenOptions::new()
                                .create(true)
                                .append(true)
                                .open(&log_path)
                                .expect("Failed to reopen log after rotation"),
                        );
                    }

                    let msg = format!(
                        "[{} {}] Received SIGUSR1, starting maintenance\n",
                        DAEMON_NAME,
                        Local::now().format("%Y-%m-%d-%H:%M:%S")
                    );
                    let _ = log_writer.write_all(msg.as_bytes());
                    let _ = log_writer.flush();

                    let (compressed, fully_deleted, expired) =
                        perform_maintenance().unwrap_or((0, 0, 0));

                    let msg = format!(
                        "[{} {}] Maintenance: compressed {} groups, cleaned {} fully-deleted archives, deleted {} expired archives\n",
                        DAEMON_NAME,
                        Local::now().format("%Y-%m-%d-%H:%M:%S"),
                        compressed,
                        fully_deleted,
                        expired
                    );
                    let _ = log_writer.write_all(msg.as_bytes());
                    let _ = log_writer.flush();
                } else if signal == Signal::SIGTERM || signal == Signal::SIGINT {
                    // Termination signal – exit the loop.
                    break;
                }
            }
            Ok(None) => {
                // Should not happen; sleep briefly to avoid a busy loop just in case.
                std::thread::sleep(std::time::Duration::from_millis(100));
            }
            Err(e) => {
                // signalfd read error – log and retry after a short delay.
                eprintln!("trashd: signalfd read error: {}", e);
                std::thread::sleep(std::time::Duration::from_secs(1));
            }
        }
    }

    // Clean up PID file before exiting.
    let _ = remove_pid_file();
}

/// Run the main loop of the daemon directly
pub fn daemon_main_direct() {
    daemon_main();
}


