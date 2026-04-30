//! safe_rm - Safe deletion tool main program

use anyhow::Result;
use clap::Parser;
use crossterm::{
    cursor,
    event::{self, Event, KeyCode, KeyEvent, KeyModifiers},
    terminal, ExecutableCommand, QueueableCommand,
};
use safe_rm::daemon::{check_and_trigger_daemon, is_process_alive, read_pid_file, remove_pid_file};
use safe_rm::VERSION;
use safe_rm::{DAEMON_ENV_VAR, DAEMON_NAME};
use std::env;
use std::io::{self, Write};
use std::path::Path;
use rayon::prelude::*;

const VERIFY_STRING: &str = "I confirm deletion";

#[derive(Parser)]
#[command(name = "safe_rm")]
#[command(about = "Safe deletion tool with rubbish bin")]
#[command(version = VERSION)]
#[command(help_template = "\
  {name} v{version} - {about}

{usage-heading} {usage}

{all-args}

\x1b[1;4mExamples:\x1b[0m
  {bin} file.txt       Safe delete (move to rubbish bin)
  {bin} -f file.txt    Safe delete without output
  {bin} --remove-completely file.txt
                       Permanently delete
  {bin} --trigger      Trigger (or start) daemon to maintain rubbish bin
  {bin} --stop         Stop the running daemon
  {bin} --status       Check if daemon is running

\x1b[1;4mDaemon Policy:\x1b[0m
  - Keep latest 20 directories uncompressed
  - Compress directories older than 3 days
  - Delete archives older than 60 days

\x1b[1;4;31mWarning:\x1b[0m
  - If the file to be deleted is a symbolic link, the symbolic
    link file itself will be \x1b[31mpermanently removed\x1b[0m. It will not
    delete the file or directory that the symbolic link points to.
")]
struct Args {
    /// Force mode (no output)
    #[arg(short = 'f', long = "force")]
    force: bool,

    /// Permanently delete (with confirmation)
    #[arg(long = "remove-completely")]
    complete: bool,

    /// Recursive mode (always enabled)
    #[arg(short = 'r', long = "recursive", visible_alias = "R")]
    recursive: bool,

    /// Trigger (or start) daemon to maintain rubbish bin
    #[arg(long = "trigger")]
    trigger: bool,

    /// Stop the running daemon
    #[arg(long = "stop")]
    stop: bool,

    /// Check if daemon is running
    #[arg(long = "status")]
    status: bool,

    /// Files to delete
    files: Vec<String>,
}

/// Determine whether the current process should run as a daemon
fn is_daemon_mode() -> bool {
    // Both conditions must be met:
    //   1. the environment variable must exist
    //   2. the process name must be DAEMON_NAME
    env::var(DAEMON_ENV_VAR).is_ok()
        && env::args_os()
            .next()
            .and_then(|arg| Path::new(&arg).file_name().map(|n| n == DAEMON_NAME))
            .unwrap_or(false)
}

fn main() -> Result<()> {
    if is_daemon_mode() {
        safe_rm::daemon::daemon_main_direct();
        std::process::exit(0);
    }

    let args = Args::parse();

    // Print the help when there are no operation requests
    if args.files.is_empty() && !args.trigger && !args.stop && !args.status {
        println!("\x1b[1;41mNo valid args!\x1b[0m");
        println!("\x1b[1;41mType '-h' for help.\x1b[0m");
        return Ok(());
    }

    if args.complete {
        complete_remove(&args.files, args.force)?;
    } else if args.trigger {
        trigger_maintenance()?;
    } else if args.stop {
        stop_daemon()?;
    } else if args.status {
        check_status()?;
    } else {
        safe_delete(&args.files, args.force)?;
    }

    Ok(())
}

/// Safe delete (move to rubbish bin)
fn safe_delete(files: &[String], force: bool) -> Result<()> {
    let timestamp = safe_rm::get_timestamp();
    let mut success_count = 0;

    for (i, file_str) in files.iter().enumerate() {
        let clean_str = file_str.trim_end_matches('/');
        let path = Path::new(clean_str);

        // Safety check
        if let Err(e) = safe_rm::check_safety_constraints(path) {
            if !force {
                eprintln!("Error: {}", e);
            }
            continue;
        }

        // Check if file exists
        if !path.exists() {
            if !force {
                eprintln!(
                    "Error: Cannot access '{}': No such file or directory",
                    file_str
                );
            }
            continue;
        }

        // Symlinks are just pointers - delete them directly
        if path.symlink_metadata()?.file_type().is_symlink() {
            std::fs::remove_file(path)?;
            if !force {
                println!("Deleted symlink '{}' permanently", file_str);
            }
            continue;
        }

        // Normal files/directories move to rubbish bin
        if let Err(e) = 
                safe_rm::safe_delete_file(path, &timestamp, i + 1)
        {
            if !force {
                eprintln!("Error: Failed to delete '{}': {}", file_str, e);
            }
        } else {
            success_count += 1;
        }
    }

    // Trigger daemon for maintenance if any files were deleted
    if success_count > 0 {
        let _ = check_and_trigger_daemon();
    }

    Ok(())
}

/// Permanently delete (with typing verification)
fn complete_remove(files: &[String], force: bool) -> Result<()> {
    // Check if files exist
    let valid_files: Vec<&String> = files.iter().filter(|f| Path::new(f).exists()).collect();

    if valid_files.is_empty() {
        if !force {
            eprintln!("No valid files to delete");
        }
        return Ok(());
    }

    // Typing verification
    match typing_verification()? {
        VerificationResult::Cancelled => {
            println!();
            println!("Operation cancelled");
            return Ok(());
        }
        VerificationResult::Failed => {
            println!();
            println!("Verification failed, deletion cancelled");
            return Ok(());
        }
        VerificationResult::Success => {}
    }

    // Execute deletion
    println!();
    println!("Verification passed, deleting...");

    let delete_one = |file_str: &&String| {
        let path = Path::new(file_str);
        if path.is_dir() {
            let _ = std::fs::remove_dir_all(path);
        } else {
            let _ = std::fs::remove_file(path);
        }
    };

    if valid_files.len() > 20 {
        valid_files.par_iter().for_each(delete_one);
    } else {
        valid_files.iter().for_each(delete_one);
    }

    println!("Finished");
    Ok(())
}

#[derive(Debug, Clone, Copy)]
enum VerificationResult {
    Cancelled,
    Failed,
    Success,
}

/// Typing verification
fn typing_verification() -> Result<VerificationResult> {
    let mut stdout = io::stdout();

    // Enable raw mode
    terminal::enable_raw_mode()?;

    // Ensure terminal is restored on exit
    struct RawModeGuard;
    impl Drop for RawModeGuard {
        fn drop(&mut self) {
            let _ = terminal::disable_raw_mode();
        }
    }
    let _guard = RawModeGuard;

    // Print prompt
    println!("Please type the following text to confirm deletion (Ctrl+C to cancel):");
    stdout.execute(cursor::MoveToColumn(0))?;
    print!("{}", VERIFY_STRING);
    stdout.execute(cursor::MoveToColumn(0))?;
    stdout.flush()?;

    let verify_chars: Vec<char> = VERIFY_STRING.chars().collect();
    let mut status: Vec<i8> = vec![0; verify_chars.len()]; // 0=not entered, 1=correct, -1=wrong
    let mut pos = 0;
    let mut has_error = false;

    loop {
        if let Event::Key(KeyEvent {
            code, modifiers, ..
        }) = event::read()?
        {
            // Ctrl+C - cancel
            if code == KeyCode::Char('c') && modifiers.contains(KeyModifiers::CONTROL) {
                return Ok(VerificationResult::Cancelled);
            }

            match code {
                KeyCode::Enter => {
                    if pos == verify_chars.len() && !has_error {
                        return Ok(VerificationResult::Success);
                    } else {
                        return Ok(VerificationResult::Failed);
                    }
                }
                KeyCode::Backspace => {
                    if pos > 0 {
                        pos -= 1;

                        // Update error status
                        if status[pos] == -1 {
                            has_error = status.iter().take(pos).any(|&s| s == -1);
                        }
                        status[pos] = 0;

                        // Move cursor back, restore original text in white
                        stdout.queue(cursor::MoveLeft(1))?;
                        print!("\x1b[37m{}\x1b[0m", verify_chars[pos]);
                        stdout.queue(cursor::MoveLeft(1))?;
                        stdout.flush()?;
                    }
                }
                KeyCode::Char(c) => {
                    if pos < verify_chars.len() {
                        if c == verify_chars[pos] {
                            status[pos] = 1; // correct
                            print!("\x1b[32m{}\x1b[0m", c);
                        } else {
                            status[pos] = -1; // wrong
                            has_error = true;
                            print!("\x1b[31m{}\x1b[0m", c);
                        }
                        pos += 1;
                        stdout.flush()?;
                    }
                }
                _ => {}
            }
        }
    }
}

/// Stop daemon trashd
fn stop_daemon() -> Result<()> {
    match read_pid_file() {
        Some(pid) => {
            if is_process_alive(pid) {
                use nix::sys::signal::{kill, Signal};
                kill(pid, Signal::SIGTERM)?;
                println!("Daemon stopped (PID: {})", pid);
                Ok(())
            } else {
                println!("Daemon not running");
                let _ = remove_pid_file();
                Ok(())
            }
        }
        None => {
            println!("Daemon not running");
            Ok(())
        }
    }
}

/// Check the status of daemon
fn check_status() -> Result<()> {
    match read_pid_file() {
        Some(pid) => {
            if is_process_alive(pid) {
                println!("Status: up, PID: {}", pid);
                Ok(())
            } else {
                println!("Status: down, PID: None");
                let _ = remove_pid_file();
                Ok(())
            }
        }
        None => {
            println!("Status: down, PID: None");
            Ok(())
        }
    }
}

/// Trigger daemon maintains the rubbish bin
fn trigger_maintenance() -> Result<()> {
    match read_pid_file() {
        Some(pid) => {
            if is_process_alive(pid) {
                use nix::sys::signal::{kill, Signal};
                kill(pid, Signal::SIGUSR1)?;
                println!("Maintenance triggered (PID: {})", pid);
                Ok(())
            } else {
                println!("Daemon not running, starting it");
                check_and_trigger_daemon()?;
                println!("Daemon started and maintenance triggered");
                Ok(())
            }
        }
        None => {
            println!("Daemon not running, starting it");
            check_and_trigger_daemon()?;
            println!("Daemon started and maintenance triggered");
            Ok(())
        }
    }
}
