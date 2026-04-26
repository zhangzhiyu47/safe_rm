//! restore - Command line rubbish bin management tool

use safe_rm::VERSION;
use anyhow::Result;
use clap::Parser;
use std::io::{self, Write};
use crossterm::terminal::size;

#[derive(Parser)]
#[command(name = "restore")]
#[command(about = "Rubbish bin management tool")]
#[command(version = VERSION)]
#[command(help_template = "\
  {name} v{version} - {about}

{usage-heading} {usage}

{all-args}

\x1b[1;4mExamples:\x1b[0m
  {bin} -l        List all restorable items
  {bin} 1         Restore item with ID=1
  {bin} -a        Restore all items
  {bin} -d 1 2    Permanently delete items with ID=1,2
")]
struct Args {
    /// List all restorable items
    #[arg(short = 'l', long = "list")]
    list: bool,

    /// Restore all items
    #[arg(short = 'a', long = "all")]
    all: bool,

    /// Delete specified items (permanently)
    #[arg(short = 'd', long = "delete")]
    delete: bool,

    /// Delete all items (empty rubbish bin)
    #[arg(long = "delete-all")]
    delete_all: bool,

    /// Item IDs
    ids: Vec<usize>,
}

fn main() -> Result<()> {
    let args = Args::parse();

    // Scan items
    let items = safe_rm::scan_restore_items()?;

    // List mode
    if args.list || (!args.all && !args.delete && !args.delete_all && args.ids.is_empty()) {
        print_restore_list(&items);
        return Ok(());
    }

    // Restore all
    if args.all {
        restore_all(&items)?;
        return Ok(());
    }

    // Delete all
    if args.delete_all {
        delete_all(&items)?;
        return Ok(());
    }

    // Delete by IDs
    if args.delete {
        delete_by_ids(&items, &args.ids)?;
        return Ok(());
    }

    // Restore by IDs
    if !args.ids.is_empty() {
        restore_by_ids(&items, &args.ids)?;
    }

    Ok(())
}

/// Print restore list with adaptive screen size
fn print_restore_list(items: &[safe_rm::RestoreItem]) {
    if items.is_empty() {
        println!("Rubbish bin is empty, no items to restore");
        return;
    }

    // Get terminal width
    let cols = size().map(|(w, _)| w as usize).unwrap_or(100);

    println!("\nRestorable items:");

    // Calculate column widths based on terminal size
    let id_width = 6usize;
    let time_width = 20usize;
    let status_width = 10usize;
    let min_filename_width = 20usize;
    let min_path_width = 15usize;

    let remaining = cols.saturating_sub(id_width + time_width + status_width + 4);
    let filename_width = (remaining * 60 / 100).max(min_filename_width);
    let path_width = remaining.saturating_sub(filename_width).max(min_path_width);

    // Print header
    println!("{:<id_width$} {:<time_width$} {:<status_width$} {:<path_width$} {}",
             "ID", "Deleted At", "Status", "Source Path", "Filename",
             id_width = id_width, time_width = time_width, status_width = status_width, path_width = path_width);
    println!("{}", "-".repeat(cols.min(120)));

    for item in items {
        let status = if item.is_archived { "Archived" } else { "Active" };
        let original_path = item.original_path.to_string_lossy();
        let path_display = truncate_str(&original_path, path_width);
        let filename_display = truncate_str(&item.filename, filename_width);

        println!("{:<id_width$} {:<time_width$} {:<status_width$} {:<path_width$} {}",
                 item.id, item.timestamp, status, path_display, filename_display,
                 id_width = id_width, time_width = time_width, status_width = status_width, path_width = path_width);
    }

    println!("\nTotal: {} items", items.len());
}

fn truncate_str(s: &str, max_width: usize) -> String {
    if s.len() <= max_width {
        s.to_string()
    } else if max_width > 3 {
        format!("...{}", &s[s.len().saturating_sub(max_width - 3)..])
    } else {
        s.chars().take(max_width).collect()
    }
}

/// Restore all items
fn restore_all(items: &[safe_rm::RestoreItem]) -> Result<()> {
    if items.is_empty() {
        println!("Rubbish bin is empty, no items to restore");
        return Ok(());
    }

    if !confirm_action("restore", items.len())? {
        println!("Cancelled");
        return Ok(());
    }

    println!("Restoring all {} items...", items.len());

    // Convert to slice of references for batch operation
    let item_refs: Vec<&safe_rm::RestoreItem> = items.iter().collect();

    // Use batch operation for better performance
    match safe_rm::restore_items_batch(&item_refs) {
        Ok(success) => {
            println!("\nSuccessfully restored {}/{} items", success, items.len());
        }
        Err(e) => {
            eprintln!("\nBatch restore failed: {}", e);
        }
    }

    Ok(())
}

/// Delete all items
fn delete_all(items: &[safe_rm::RestoreItem]) -> Result<()> {
    if items.is_empty() {
        println!("Rubbish bin is empty");
        return Ok(());
    }

    println!(
        "\n\x1b[31mWarning: This will permanently delete all {} items, cannot be undone!\x1b[0m",
        items.len()
    );

    if !confirm_action("permanently delete", items.len())? {
        println!("Cancelled");
        return Ok(());
    }

    println!("Deleting all {} items...", items.len());

    // Convert to slice of references for batch operation
    let item_refs: Vec<&safe_rm::RestoreItem> = items.iter().collect();

    // Use batch operation for better performance
    match safe_rm::delete_items_batch(&item_refs) {
        Ok(success) => {
            println!("\nSuccessfully deleted {}/{} items", success, items.len());
        }
        Err(e) => {
            eprintln!("\nBatch delete failed: {}", e);
        }
    }

    Ok(())
}

/// Restore by IDs
fn restore_by_ids(items: &[safe_rm::RestoreItem], ids: &[usize]) -> Result<()> {
    if !confirm_action("restore", ids.len())? {
        println!("Cancelled");
        return Ok(());
    }

    println!("Restoring {} items...", ids.len());

    // Collect items to restore
    let items_to_restore: Vec<&safe_rm::RestoreItem> = ids
        .iter()
        .filter_map(|id| items.iter().find(|i| i.id == *id))
        .collect();

    // Use batch operation for better performance
    match safe_rm::restore_items_batch(&items_to_restore) {
        Ok(success) => {
            println!("\nSuccessfully restored {}/{} items", success, ids.len());
        }
        Err(e) => {
            eprintln!("\nBatch restore failed: {}", e);
        }
    }

    Ok(())
}

/// Delete by IDs
fn delete_by_ids(items: &[safe_rm::RestoreItem], ids: &[usize]) -> Result<()> {
    println!(
        "\n\x1b[31mWarning: This will permanently delete {} items, cannot be undone!\x1b[0m",
        ids.len()
    );

    if !confirm_action("permanently delete", ids.len())? {
        println!("Cancelled");
        return Ok(());
    }

    println!("Deleting {} items...", ids.len());

    // Collect items to delete
    let items_to_delete: Vec<&safe_rm::RestoreItem> = ids
        .iter()
        .filter_map(|id| items.iter().find(|i| i.id == *id))
        .collect();

    // Use batch operation for better performance
    match safe_rm::delete_items_batch(&items_to_delete) {
        Ok(success) => {
            println!("\nSuccessfully deleted {}/{} items", success, ids.len());
        }
        Err(e) => {
            eprintln!("\nBatch delete failed: {}", e);
        }
    }

    Ok(())
}

/// Confirm action
fn confirm_action(action: &str, count: usize) -> Result<bool> {
    print!("\nConfirm to {} {} items? [y/N]: ", action, count);
    io::stdout().flush()?;

    let mut buf = String::new();
    io::stdin().read_line(&mut buf)?;

    Ok(buf.trim().eq_ignore_ascii_case("y"))
}
