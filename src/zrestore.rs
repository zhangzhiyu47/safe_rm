//! zrestore - Interactive rubbish bin management tool (TUI)

use safe_rm::VERSION;
use anyhow::Result;
use clap::Parser;
use crossterm::{
    event::{self, DisableMouseCapture, EnableMouseCapture, Event, KeyCode},
    execute,
    terminal::{disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen, size},
};
use ratatui::{
    backend::{Backend, CrosstermBackend},
    layout::{Alignment, Constraint, Direction, Layout, Rect},
    style::{Color, Modifier, Style},
    text::{Line, Span, Text},
    widgets::{Block, Borders, Cell, Clear, Paragraph, Row, Table, TableState, Wrap},
    Frame, Terminal,
};
use std::io;
use std::thread;
use std::ffi::OsStr;
use std::path::PathBuf;
use std::time::{Duration, Instant};
use std::collections::HashSet;
use notify::{Config, EventKind, RecommendedWatcher, RecursiveMode, Watcher};
use crossbeam_channel::{unbounded, Sender, TryRecvError};
use std::sync::Arc;
use std::sync::mpsc::{self, Receiver};
use std::sync::atomic::{AtomicU64, Ordering, AtomicBool};

const DEBOUNCE_MS: u64                 = 500;
const CACHE_COOLDOWN_MS: u64           = 2 * 1000;
const PROGRESS_UPDATE_INTERVAL_MS: u64 = 100;
const SIMILARITY: i32                  = 20;

#[derive(Parser)]
#[command(name = "zrestore")]
#[command(about = "Interactive rubbish bin management tool (TUI)")]
#[command(version = VERSION)]
#[command(help_template = "\
  {name} v{version} - {about}

{usage-heading} {usage}

{all-args}

\x1b[1;4mExamples:\x1b[0m
  {bin}       Start interactive TUI
  {bin} -l    List all restorable items (plain text)
  {bin} -h    Show this help
")]
struct Args {
    /// List all restorable items (plain text)
    #[arg(short = 'l', long = "list")]
    list: bool,
}

#[derive(Debug, Clone, Copy, PartialEq)]
enum Language { English, Chinese }

impl Language {
    fn toggle(&self) -> Self {
        match self { Language::English => Language::Chinese, Language::Chinese => Language::English }
    }
    fn title(&self) -> &'static str { match self { Language::English => " Trash Manager ", Language::Chinese => " 回收站管理器 " } }
    fn item_list(&self) -> &'static str { match self { Language::English => " Items ", Language::Chinese => " 项目列表 " } }
    fn item_details(&self) -> &'static str { match self { Language::English => " Details ", Language::Chinese => " 详情 " } }
    fn help(&self) -> &'static str { match self { Language::English => " Help ", Language::Chinese => " 帮助 " } }
    fn confirm_restore(&self) -> &'static str { match self { Language::English => " Confirm Restore ", Language::Chinese => " 确认恢复 " } }
    fn confirm_delete(&self) -> &'static str { match self { Language::English => " Permanently Delete ", Language::Chinese => " 永久删除 " } }
    fn empty_bin(&self) -> &'static str { match self { Language::English => " Empty Trash ", Language::Chinese => " 清空回收站 " } }
    fn total(&self) -> &'static str { match self { Language::English => "Total", Language::Chinese => "共" } }
    fn showing(&self) -> &'static str { match self { Language::English => "Showing", Language::Chinese => "显示" } }
    fn selected(&self) -> &'static str { match self { Language::English => "Selected", Language::Chinese => "选中" } }
    fn search(&self) -> &'static str { match self { Language::English => "Search", Language::Chinese => "搜索" } }
    fn none(&self) -> &'static str { match self { Language::English => "None", Language::Chinese => "无" } }
    fn exact(&self) -> &'static str { match self { Language::English => "[Exact]", Language::Chinese => "[精确]" } }
    fn fuzzy(&self) -> &'static str { match self { Language::English => "[Fuzzy]", Language::Chinese => "[模糊]" } }
    fn archived(&self) -> &'static str { match self { Language::English => "Archived", Language::Chinese => "已归档" } }
    fn active(&self) -> &'static str { match self { Language::English => "Active", Language::Chinese => "活跃" } }
    fn id(&self) -> &'static str { match self { Language::English => "ID", Language::Chinese => "ID" } }
    fn deleted_at(&self) -> &'static str { match self { Language::English => "Deleted", Language::Chinese => "删除时间" } }
    fn status(&self) -> &'static str { match self { Language::English => "Status", Language::Chinese => "状态" } }
    fn original_path(&self) -> &'static str { match self { Language::English => "Source", Language::Chinese => "原路径" } }
    fn filename(&self) -> &'static str { match self { Language::English => "Filename", Language::Chinese => "文件名" } }
    fn no_item_selected(&self) -> &'static str { match self { Language::English => "No item selected", Language::Chinese => "没有选择项目" } }
    fn archive_file(&self) -> &'static str { match self { Language::English => "Archive", Language::Chinese => "归档" } }
    fn yes(&self) -> &'static str { match self { Language::English => "Yes(Y)", Language::Chinese => "是(Y)" } }
    fn no(&self) -> &'static str { match self { Language::English => "No(N)", Language::Chinese => "否(N)" } }
    fn items(&self) -> &'static str { match self { Language::English => "items", Language::Chinese => "项" } }
    fn restore_success(&self) -> &'static str { match self { Language::English => "Restored", Language::Chinese => "恢复成功" } }
    fn no_items_selected(&self) -> &'static str { match self { Language::English => "No items selected", Language::Chinese => "没有选中的项目" } }
    fn select_all_msg(&self) -> &'static str { match self { Language::English => "Selected all", Language::Chinese => "已全选" } }
    fn deselect_all_msg(&self) -> &'static str { match self { Language::English => "Deselected all", Language::Chinese => "已取消全选" } }
    fn fuzzy_mode(&self) -> &'static str { match self { Language::English => "Fuzzy mode", Language::Chinese => "模糊匹配" } }
    fn exact_mode(&self) -> &'static str { match self { Language::English => "Exact mode", Language::Chinese => "精确匹配" } }

    fn confirm_restore_msg(&self, count: usize) -> String {
        match self { Language::English => format!("Restore {} items?", count), Language::Chinese => format!("恢复 {} 个项目?", count) }
    }
    fn confirm_restore_all_msg(&self, count: usize) -> String {
        match self { Language::English => format!("Restore all {} items?", count), Language::Chinese => format!("恢复全部 {} 个项目?", count) }
    }
    fn confirm_delete_msg(&self, count: usize) -> String {
        match self { Language::English => format!("Delete {} items permanently!", count), Language::Chinese => format!("永久删除 {} 个项目!", count) }
    }
    fn empty_bin_msg(&self, count: usize) -> String {
        match self { Language::English => format!("Delete all {} items!", count), Language::Chinese => format!("删除全部 {} 个项目!", count) }
    }

    fn help_text(&self) -> Text<'static> {
        let title = match self { Language::English => " Keyboard Shortcuts ", Language::Chinese => " 键盘快捷键 " };
        let nav = match self { Language::English => "Navigation:", Language::Chinese => "导航:" };
        let act = match self { Language::English => "Actions:", Language::Chinese => "操作:" };
        let other = match self { Language::English => "Other:", Language::Chinese => "其他:" };
        let close = match self { Language::English => "Press any key to close", Language::Chinese => "按任意键关闭" };

        let lines = vec![
            Line::from(""),
            Line::from(vec![Span::styled(title, Style::default().fg(Color::Black).bg(Color::Cyan).add_modifier(Modifier::BOLD))]),
            Line::from(""),
            Line::from(vec![Span::styled(nav, Style::default().fg(Color::Yellow).add_modifier(Modifier::BOLD))]),
            Line::from(match self { Language::English => "  ↑/↓/k/j    Move up/down", Language::Chinese => "  ↑/↓/k/j    上下移动" }),
            Line::from(match self { Language::English => "  PgUp/PgDn  Page up/down", Language::Chinese => "  PgUp/PgDn  翻页" }),
            Line::from(match self { Language::English => "  Home/End   First/last", Language::Chinese => "  Home/End   跳到首尾" }),
            Line::from(""),
            Line::from(vec![Span::styled(act, Style::default().fg(Color::Yellow).add_modifier(Modifier::BOLD))]),
            Line::from(match self { Language::English => "  Space      Toggle selection", Language::Chinese => "  Space      选择/取消选择" }),
            Line::from(match self { Language::English => "  a/A        Select all/None", Language::Chinese => "  a/A        全选/取消全选" }),
            Line::from(match self { Language::English => "  Enter      Restore current", Language::Chinese => "  Enter      恢复当前项" }),
            Line::from(match self { Language::English => "  r          Restore selected", Language::Chinese => "  r          恢复选中项" }),
            Line::from(match self { Language::English => "  R          Restore all visible", Language::Chinese => "  R          恢复所有可见" }),
            Line::from(match self { Language::English => "  d          Delete selected", Language::Chinese => "  d          删除选中项" }),
            Line::from(match self { Language::English => "  D          Delete all visible", Language::Chinese => "  D          删除所有可见" }),
            Line::from(""),
            Line::from(vec![Span::styled(other, Style::default().fg(Color::Yellow).add_modifier(Modifier::BOLD))]),
            Line::from(match self { Language::English => "  /          Search", Language::Chinese => "  /          搜索" }),
            Line::from(match self { Language::English => "  Tab        Toggle exact/fuzzy", Language::Chinese => "  Tab        切换精确/模糊" }),
            Line::from(match self { Language::English => "  L          Switch language", Language::Chinese => "  L          切换语言" }),
            Line::from(match self { Language::English => "  ?/h/F1     Show help", Language::Chinese => "  ?/h/F1     显示帮助" }),
            Line::from(match self { Language::English => "  q          Quit", Language::Chinese => "  q          退出" }),
            Line::from(""),
            Line::from(vec![Span::styled(close, Style::default().fg(Color::Green))]),
        ];
        Text::from(lines)
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
enum SearchMode { Exact, Fuzzy }

#[derive(Debug, Clone, Copy, PartialEq)]
enum StatusType { Success, Warning, Error }

#[derive(Debug, Clone)]
struct ConfirmDialog {
    title: String,
    message: String,
    is_delete: bool,
    callback: ConfirmCallback,
}

#[derive(Debug, Clone)]
#[allow(dead_code)]
enum ConfirmCallback {
    RestoreSingle(usize),
    RestoreSelected,
    RestoreAll,
    DeleteSingle(usize),
    DeleteSelected,
    DeleteAll,
}

/// Scan results
#[derive(Debug)]
enum ScanResult {
    Success(Vec<safe_rm::RestoreItem>),
    Failed(String),
}

#[derive(Debug)]
struct OpResult {
    message: String,
    status: StatusType,
    need_refresh: bool,
}

struct App {
    all_items: Vec<safe_rm::RestoreItem>,
    filtered_indices: Vec<usize>,
    selected: HashSet<usize>,
    table_state: TableState,
    search_mode: SearchMode,
    language: Language,
    filter: String,
    show_help: bool,
    show_search: bool,
    show_confirm: Option<ConfirmDialog>,
    status_message: Option<(String, StatusType, Instant)>,
    should_quit: bool,
    need_refresh: bool,
    scroll_offset: usize,
    is_loading: bool,
    loading_message: String,
    loading_spinner_idx: usize,
    op_in_progress: bool,
    op_message: String,
    op_spinner_idx: usize,
    op_total: usize,
    op_done: usize,
    op_done_rx: Option<Receiver<OpResult>>,
    op_progress_rx: Option<Receiver<(usize, usize)>>,
    initial_loading: bool,
    initial_scan_rx: Option<Receiver<ScanResult>>,
    last_scan_finish: Option<Instant>,
    scan_in_progress: Arc<AtomicBool>,
    debounce_timer: Option<Instant>,
}

impl App {
    fn new() -> Self {
        Self {
            all_items: Vec::new(),
            filtered_indices: Vec::new(),
            selected: HashSet::new(),
            table_state: TableState::default(),
            search_mode: SearchMode::Exact,
            language: Language::English,
            filter: String::new(),
            show_help: false,
            show_search: false,
            show_confirm: None,
            status_message: None,
            should_quit: false,
            need_refresh: false,
            scroll_offset: 0,
            is_loading: true,
            loading_message: "Loading rubbish bin".to_string(),
            loading_spinner_idx: 0,
            initial_loading: true,
            initial_scan_rx: None,
            op_in_progress: false,
            op_message: String::new(),
            op_spinner_idx: 0,
            op_total: 0,
            op_done: 0,
            op_done_rx: None,
            op_progress_rx: None,
            last_scan_finish: None,
            scan_in_progress: Arc::new(AtomicBool::new(false)),
            debounce_timer: None,
        }
    }

    fn start_loading(&mut self, msg: &str) {
        self.is_loading = true;
        self.loading_message = msg.to_string();
    }

    fn stop_loading(&mut self) {
        self.is_loading = false;
        self.loading_message.clear();
    }

    fn selected_item(&self) -> Option<&safe_rm::RestoreItem> {
        self.table_state.selected()
            .and_then(|i| self.filtered_indices.get(i))
            .and_then(|&idx| self.all_items.get(idx))
    }

    fn selected_id(&self) -> Option<usize> {
        self.selected_item().map(|item| item.id)
    }

    fn update_filter(&mut self) {
        self.filtered_indices.clear();
        for (idx, item) in self.all_items.iter().enumerate() {
            let matches = if self.filter.is_empty() {
                true
            } else {
                match self.search_mode {
                    SearchMode::Exact => {
                        item.filename.to_lowercase().contains(&self.filter.to_lowercase()) ||
                        item.original_path.to_string_lossy().to_lowercase().contains(&self.filter.to_lowercase())
                    }
                    SearchMode::Fuzzy => {
                        fuzzy_match(&self.filter, &item.filename) > SIMILARITY ||
                        fuzzy_match(&self.filter,
                            &item.original_path.to_string_lossy()) > SIMILARITY
                    }
                }
            };
            if matches { self.filtered_indices.push(idx); }
        }
        if !self.filtered_indices.is_empty() {
            self.table_state.select(Some(0));
        } else {
            self.table_state.select(None);
        }
        self.scroll_offset = 0;
    }

    fn refresh_items(&mut self, new_items: Vec<safe_rm::RestoreItem>) {
        let selected_id = self.selected_item().map(|item| item.id);

        self.all_items = new_items;

        self.all_items.sort_by(|a, b| {
            if a.is_archived != b.is_archived {
                a.is_archived.cmp(&b.is_archived)
            } else {
                b.timestamp.cmp(&a.timestamp)
            }
        });

        self.update_filter();

        if let Some(mut id) = selected_id {
            loop {
                if let Some(pos) = self
                    .filtered_indices
                    .iter()
                    .position(|&idx| self.all_items[idx].id == id)
                {
                    self.table_state.select(Some(pos));
                    return;
                }
                if id <= 1 {
                    break;
                }
                id -= 1;
            }
        }

        if !self.filtered_indices.is_empty() {
            self.table_state.select(Some(0));
        } else {
            self.table_state.select(None);
        }
    }

    fn toggle_selection(&mut self) {
        if let Some(id) = self.selected_id() {
            if self.selected.contains(&id) { self.selected.remove(&id); } else { self.selected.insert(id); }
        }
    }

    fn select_all(&mut self) {
        for &idx in &self.filtered_indices {
            if let Some(item) = self.all_items.get(idx) { self.selected.insert(item.id); }
        }
    }

    fn select_none(&mut self) { self.selected.clear(); }

    fn set_status(&mut self, msg: String, status_type: StatusType) {
        self.status_message = Some((msg, status_type, Instant::now()));
    }

    fn clear_status(&mut self) {
        if let Some((_, _, time)) = &self.status_message {
            if time.elapsed() > Duration::from_secs(3) { self.status_message = None; }
        }
    }
}

fn fuzzy_match(pattern: &str, text: &str) -> i32 {
    if pattern.is_empty() {
        return 100;
    }
    let pattern_lower = pattern.to_lowercase();
    let text_lower = text.to_lowercase();

    if text_lower.contains(&pattern_lower) {
        return 100;
    }

    let distance = levenshtein_distance(&pattern_lower, &text_lower);
    let max_len = pattern.len().max(text.len());
    if max_len == 0 {
        return 100;
    }
    let score = 100 - (distance * 100 / max_len as i32);
    score.max(0)
}

#[allow(clippy::needless_range_loop)]
fn levenshtein_distance(a: &str, b: &str) -> i32 {
    let a_chars: Vec<char> = a.chars().collect();
    let b_chars: Vec<char> = b.chars().collect();
    let m = a_chars.len();
    let n = b_chars.len();
    if m == 0 { return n as i32; }
    if n == 0 { return m as i32; }
    let mut dp: Vec<Vec<i32>> = vec![vec![0; n + 1]; m + 1];
    for i in 0..=m { dp[i][0] = i as i32; }
    for j in 0..=n { dp[0][j] = j as i32; }
    for i in 1..=m {
        for j in 1..=n {
            let cost = if a_chars[i - 1] == b_chars[j - 1] { 0 } else { 1 };
            dp[i][j] = (dp[i - 1][j] + 1).min(dp[i][j - 1] + 1).min(dp[i - 1][j - 1] + cost);
        }
    }
    dp[m][n]
}

fn main() -> Result<()> {
    let args = Args::parse();
    if args.list { return run_list_mode(); }
    run_tui_mode()
}

fn run_list_mode() -> Result<()> {
    let items = safe_rm::scan_restore_items()?;
    if items.is_empty() {
        println!("Rubbish bin is empty");
        return Ok(());
    }

    // Get terminal width
    let (cols, _) = size()?;
    let cols = cols as usize;

    println!("\nRestorable items:");

    // Calculate column widths based on terminal size
    let id_width = 6;
    let time_width = 20;
    let status_width = 10;
    let min_filename_width = 20;
    let min_path_width = 15;

    let remaining = cols.saturating_sub(id_width + time_width + status_width + 4);
    let filename_width = (remaining * 60 / 100).max(min_filename_width);
    let path_width = (remaining * 40 / 100).max(min_path_width);

    // Print header
    println!("{:<id_width$} {:<time_width$} {:<status_width$} {:<path_width$} {}",
             "ID", "Deleted At", "Status", "Original Path", "Filename",
             id_width = id_width, time_width = time_width, status_width = status_width, path_width = path_width);
    println!("{}", "-".repeat(cols.min(120)));

    for item in &items {
        let status = if item.is_archived { "Archived" } else { "Active" };
        let original_path = item.original_path.to_string_lossy();
        let path_display = truncate_str(&original_path, path_width);
        let filename_display = truncate_str(&item.filename, filename_width);

        println!("{:<id_width$} {:<time_width$} {:<status_width$} {:<path_width$} {}",
                 item.id, item.timestamp, status, path_display, filename_display,
                 id_width = id_width, time_width = time_width, status_width = status_width, path_width = path_width);
    }

    println!("\nTotal: {} items", items.len());
    Ok(())
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

fn run_tui_mode() -> Result<()> {
    enable_raw_mode()?;
    let mut stdout = io::stdout();
    execute!(stdout, EnterAlternateScreen, EnableMouseCapture)?;
    let backend = CrosstermBackend::new(stdout);
    let mut terminal = Terminal::new(backend)?;

    let mut app = App::new();

    let (init_tx, init_rx) = mpsc::channel();
    app.initial_scan_rx = Some(init_rx);
    thread::spawn(move || {
        let result = match safe_rm::scan_restore_items() {
            Ok(items) => ScanResult::Success(items),
            Err(e) => ScanResult::Failed(e.to_string()),
        };
        let _ = init_tx.send(result);
    });

    let res = run_app(&mut terminal, &mut app);

    disable_raw_mode()?;
    execute!(terminal.backend_mut(), LeaveAlternateScreen, DisableMouseCapture)?;
    terminal.show_cursor()?;
    res
}

/// Start the cache file listener
fn spawn_cache_watcher(tx: Sender<()>) -> Result<()> {
    let cache_path = match safe_rm::get_cache_path() {
        Ok(p) => p,
        Err(e) => {
            eprintln!("Failed to get cache path: {}", e);
            return Err(e.into());
        }
    };

    let cache_dir = cache_path.parent().unwrap().to_path_buf();
    let cache_file_name = cache_path
        .file_name()
        .unwrap()
        .to_string_lossy()
        .to_string();

    // Create watcher channel
    let (watcher_tx, watcher_rx) = unbounded();

    // Create watcher
    let mut watcher = RecommendedWatcher::new(
        watcher_tx,
        Config::default().with_poll_interval(Duration::from_secs(1)),
    )?;

    watcher.watch(&cache_dir, RecursiveMode::NonRecursive)?;

    // Start listening thread
    thread::spawn(move || {
        for res in watcher_rx {
            match res {
                Ok(event) => {
                    let is_cache_event = event.paths.iter().any(|p: &PathBuf| {
                        p.file_name()
                            .map(|n: &OsStr| n.to_string_lossy() == cache_file_name)
                            .unwrap_or(false)
                    });

                    if is_cache_event {
                        match event.kind {
                            EventKind::Modify(_) | EventKind::Remove(_) | EventKind::Create(_) => {
                                // Notify the main thread to refresh when cache files change
                                tx.send(()).ok();
                            }
                            _ => {}
                        }
                    }
                }
                Err(e) => {
                    // Listening error detected, silently ignored (doesn't affect main functionality)
                    eprintln!("Cache watcher error: {:?}", e);
                }
            }
        }
    });

    Ok(())
}

/// Scan rubbish bin in a background thread and return results via channel once done
fn spawn_scanner_with_flag(tx: Sender<ScanResult>, flag: Arc<AtomicBool>) {
    thread::spawn(move || {
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            match safe_rm::scan_restore_items() {
                Ok(items) => ScanResult::Success(items),
                Err(e) => ScanResult::Failed(e.to_string()),
            }
        }));
        let _ = match result {
            Ok(scan_result) => tx.send(scan_result),
            Err(_) => tx.send(ScanResult::Failed("Scanner panicked".into())),
        };
        flag.store(false, Ordering::Relaxed);
    });
}

fn run_app<B: Backend>(
        terminal: &mut Terminal<B>, 
        app: &mut App) -> Result<()> 
{
    let (cache_tx, cache_rx) = unbounded::<()>();
    let (scan_tx, scan_rx) = unbounded::<ScanResult>();

    if let Err(e) = spawn_cache_watcher(cache_tx) {
        eprintln!("Failed to start cache watcher: {}", e);
    }

    let mut last_frame = Instant::now();
    let frame_duration = Duration::from_millis(80);

    loop {
        if app.initial_loading {
            if let Some(rx) = &app.initial_scan_rx {
                match rx.try_recv() {
                    Ok(ScanResult::Success(items)) => {
                        app.all_items = items;
                        app.filtered_indices = (0..app.all_items.len()).collect();
                        app.update_filter();
                        app.is_loading = false;
                        app.initial_loading = false;
                        app.initial_scan_rx = None;
                        app.last_scan_finish = Some(Instant::now());
                    }
                    Ok(ScanResult::Failed(err)) => {
                        app.set_status(format!("Failed to load trash: {}", err), StatusType::Error);
                        app.is_loading = false;
                        app.initial_loading = false;
                        app.initial_scan_rx = None;
                        app.last_scan_finish = Some(Instant::now());
                    }
                    Err(std::sync::mpsc::TryRecvError::Empty) => {}
                    Err(std::sync::mpsc::TryRecvError::Disconnected) => {
                        app.set_status("Initial scanner disconnected".to_string(), StatusType::Error);
                        app.is_loading = false;
                        app.initial_loading = false;
                        app.initial_scan_rx = None;
                        app.last_scan_finish = Some(Instant::now());
                    }
                }
            }
        }

        if app.is_loading && last_frame.elapsed() >= frame_duration {
            app.loading_spinner_idx = (app.loading_spinner_idx + 1) % 10;
            last_frame = Instant::now();
        }

        match cache_rx.try_recv() {
            Ok(()) => {
                app.debounce_timer = Some(Instant::now());
            }
            Err(TryRecvError::Empty) => {}
            Err(TryRecvError::Disconnected) => {}
        }

         match scan_rx.try_recv() {
            Ok(ScanResult::Success(items)) => {
                app.refresh_items(items);
                app.selected.clear();
                app.stop_loading();
                app.last_scan_finish = Some(Instant::now());
            }
            Ok(ScanResult::Failed(err)) => {
                app.stop_loading();
                app.last_scan_finish = Some(Instant::now());
                app.set_status(format!("Load failed: {}", err), StatusType::Error);
            }
            Err(TryRecvError::Empty) => {}
            Err(TryRecvError::Disconnected) => {
                app.stop_loading();
                app.last_scan_finish = Some(Instant::now());
                app.set_status("Scanner disconnect".to_string(), StatusType::Error);
            }
        }

        // Stabilization trigger: 
        // If no new events occur within DEBOUNCE_MS ms,
        // a scan will be initiated uniformly
        if let Some(timer_start) = app.debounce_timer {
            if timer_start.elapsed() >= Duration::from_millis(DEBOUNCE_MS) {
                if !app.scan_in_progress.load(Ordering::Relaxed) {
                    let cooldown_passed = app.last_scan_finish
                        .map(|t| t.elapsed() 
                            >= Duration::from_millis(CACHE_COOLDOWN_MS))
                        .unwrap_or(true);
                    if cooldown_passed {
                        app.scan_in_progress.store(true, Ordering::Relaxed);
                        app.start_loading("Updating cache");
                        spawn_scanner_with_flag(scan_tx.clone(),
                            app.scan_in_progress.clone());
                    }
                }
                app.debounce_timer = None;
            }
        }

        if let Some(rx) = &app.op_progress_rx {
            loop {
                match rx.try_recv() {
                    Ok((current, total)) => {
                        app.op_done = current;
                        app.op_total = total;
                    }
                    Err(std::sync::mpsc::TryRecvError::Empty) => break,
                    Err(std::sync::mpsc::TryRecvError::Disconnected) => {
                        app.op_progress_rx = None;
                        break;
                    }
                }
            }
        }

        if let Some(rx) = &app.op_done_rx {
            if let Ok(result) = rx.try_recv() {
                app.op_in_progress = false;
                app.op_done_rx = None;
                app.op_progress_rx = None;
                app.set_status(result.message, result.status);
                if result.need_refresh {
                    app.need_refresh = true;
                }
            }
        }

        if app.op_in_progress && last_frame.elapsed() >= frame_duration {
            app.op_spinner_idx = (app.op_spinner_idx + 1) % 10;
            last_frame = Instant::now();
        }

        terminal.draw(|f| ui(f, app))?;
        app.clear_status();

        if event::poll(Duration::from_millis(50))? {
            let event = event::read()?;
            handle_input(app, event)?;
        }

        if app.need_refresh && !app.is_loading {
            app.debounce_timer = Some(Instant::now());
            app.need_refresh = false;
        }

        if app.should_quit {
            return Ok(());
        }
    }
}

fn handle_input(app: &mut App, event: Event) -> Result<()> {
    if app.show_help {
        app.show_help = false;
        return Ok(());
    }

    // Handle mouse events
    if let Event::Mouse(mouse_event) = event {
        use crossterm::event::MouseEventKind;
        match mouse_event.kind {
            MouseEventKind::ScrollDown => {
                // Scroll down: Select the next item
                let i = app.table_state.selected().unwrap_or(0);
                if i < app.filtered_indices.len().saturating_sub(1) {
                    app.table_state.select(Some(i + 1));
                }
                return Ok(());
            }
            MouseEventKind::ScrollUp => {
                // Scroll up: Select the previous item
                let i = app.table_state.selected().unwrap_or(0);
                if i > 0 {
                    app.table_state.select(Some(i - 1));
                }
                return Ok(());
            }
            _ => {}
        }
    }

    // Only handle keyboard events
    let key = if let Event::Key(key) = event {
        key
    } else {
        return Ok(());
    };

    if let Some(dialog) = &mut app.show_confirm {
        match key.code {
            KeyCode::Char('y') | KeyCode::Char('Y') => {
                let callback = dialog.callback.clone();
                app.show_confirm = None;
                execute_confirm_action(app, callback)?;
            }
            KeyCode::Char('n') | KeyCode::Char('N') | KeyCode::Esc => {
                app.show_confirm = None;
            }
            KeyCode::Enter => {
                if !dialog.is_delete {
                    let callback = dialog.callback.clone();
                    app.show_confirm = None;
                    execute_confirm_action(app, callback)?;
                } else {
                    app.show_confirm = None;
                }
            }
            _ => {}
        }
        return Ok(());
    }

    if app.show_search {
        match key.code {
            KeyCode::Esc => {
                app.show_search = false;
                app.filter.clear();
                app.update_filter();
            }
            KeyCode::Enter => {
                app.show_search = false;
                app.table_state.select(Some(0));
            }
            KeyCode::Tab => {
                app.search_mode = match app.search_mode {
                    SearchMode::Exact => {
                        app.set_status(app.language.fuzzy_mode().to_string(), StatusType::Success);
                        SearchMode::Fuzzy
                    }
                    SearchMode::Fuzzy => {
                        app.set_status(app.language.exact_mode().to_string(), StatusType::Success);
                        SearchMode::Exact
                    }
                };
                app.update_filter();
            }
            KeyCode::Backspace => {
                app.filter.pop();
                app.update_filter();
            }
            KeyCode::Char(c) => {
                app.filter.push(c);
                app.table_state.select(Some(0));
                app.update_filter();
            }
            _ => {}
        }
        return Ok(());
    }

    match key.code {
        KeyCode::Char('q') | KeyCode::Char('Q') => app.should_quit = true,
        KeyCode::Up | KeyCode::Char('k') => {
            let i = app.table_state.selected().unwrap_or(0);
            if i > 0 { app.table_state.select(Some(i - 1)); }
        }
        KeyCode::Down | KeyCode::Char('j') => {
            let i = app.table_state.selected().unwrap_or(0);
            if i < app.filtered_indices.len().saturating_sub(1) { app.table_state.select(Some(i + 1)); }
        }
        KeyCode::PageUp => {
            let i = app.table_state.selected().unwrap_or(0);
            app.table_state.select(Some(i.saturating_sub(10)));
        }
        KeyCode::PageDown => {
            let i = app.table_state.selected().unwrap_or(0);
            let max = app.filtered_indices.len().saturating_sub(1);
            app.table_state.select(Some((i + 10).min(max)));
        }
        KeyCode::Home => app.table_state.select(Some(0)),
        KeyCode::End => {
            let max = app.filtered_indices.len().saturating_sub(1);
            app.table_state.select(Some(max));
        }
        KeyCode::Char(' ') => app.toggle_selection(),
        KeyCode::Char('a') => {
            app.select_all();
            app.set_status(app.language.select_all_msg().to_string(), StatusType::Success);
        }
        KeyCode::Char('A') => {
            app.select_none();
            app.set_status(app.language.deselect_all_msg().to_string(), StatusType::Success);
        }
        KeyCode::Enter => {
            if let Some(id) = app.selected_id() {
                app.show_confirm = Some(ConfirmDialog {
                    title: app.language.confirm_restore().to_string(),
                    message: app.language.confirm_restore_msg(1),
                    is_delete: false,
                    callback: ConfirmCallback::RestoreSingle(id),
                });
            }
        }
        KeyCode::Char('r') => {
            if !app.selected.is_empty() {
                app.show_confirm = Some(ConfirmDialog {
                    title: app.language.confirm_restore().to_string(),
                    message: app.language.confirm_restore_msg(app.selected.len()),
                    is_delete: false,
                    callback: ConfirmCallback::RestoreSelected,
                });
            } else {
                app.set_status(app.language.no_items_selected().to_string(), StatusType::Warning);
            }
        }
        KeyCode::Char('R') => {
            app.show_confirm = Some(ConfirmDialog {
                title: app.language.confirm_restore().to_string(),
                message: app.language.confirm_restore_all_msg(app.filtered_indices.len()),
                is_delete: false,
                callback: ConfirmCallback::RestoreAll,
            });
        }
        KeyCode::Char('d') => {
            if !app.selected.is_empty() {
                app.show_confirm = Some(ConfirmDialog {
                    title: app.language.confirm_delete().to_string(),
                    message: app.language.confirm_delete_msg(
                        app.selected.len()),
                    is_delete: true,
                    callback: ConfirmCallback::DeleteSelected,
                });
            } else if let Some(id) = app.selected_id() {
                app.show_confirm = Some(ConfirmDialog {
                    title: app.language.confirm_delete().to_string(),
                    message: "Delete this item permanently?".to_string(),
                    is_delete: true,
                    callback: ConfirmCallback::DeleteSingle(id),
                });
            } else {
                app.set_status(
                    app.language.no_items_selected().to_string(),
                    StatusType::Warning);
            }
        }
        KeyCode::Char('D') => {
            if !app.filtered_indices.is_empty() {
                app.show_confirm = Some(ConfirmDialog {
                    title: app.language.empty_bin().to_string(),
                    message: app.language.empty_bin_msg(app.filtered_indices.len()),
                    is_delete: true,
                    callback: ConfirmCallback::DeleteAll,
                });
            }
        }
        KeyCode::Char('/') | KeyCode::Char('s') | KeyCode::Char('S') => {
            app.show_search = true;
            app.filter.clear();
        }
        KeyCode::Tab => {
            app.search_mode = match app.search_mode {
                SearchMode::Exact => {
                    app.set_status(app.language.fuzzy_mode().to_string(), StatusType::Success);
                    SearchMode::Fuzzy
                }
                SearchMode::Fuzzy => {
                    app.set_status(app.language.exact_mode().to_string(), StatusType::Success);
                    SearchMode::Exact
                }
            };
            app.update_filter();
        }
        KeyCode::Esc => {
            app.filter.clear();
            app.update_filter();
        }
        KeyCode::Char('?') | KeyCode::Char('h') | KeyCode::Char('H') | KeyCode::F(1) => {
            app.show_help = true;
        }
        KeyCode::Char('l') | KeyCode::Char('L') => {
            app.language = app.language.toggle();
            app.set_status(format!("{:?}", app.language), StatusType::Success);
        }
        _ => {}
    }
    Ok(())
}

fn execute_confirm_action(app: &mut App, callback: ConfirmCallback) -> Result<()> {
    match callback {
        ConfirmCallback::RestoreSingle(id) => {
            if let Some(item) = app.all_items.iter().find(|i| i.id == id) {
                match safe_rm::restore_item(item, &app.all_items) {
                    Ok(_) => { 
                        app.set_status(app.language.restore_success().to_string(), StatusType::Success);
                        app.need_refresh = true;
                    }
                    Err(e) => { app.set_status(format!("Restore failed: {}", e), StatusType::Error); }
                }
            }
        }
        ConfirmCallback::DeleteSingle(id) => {
            if let Some(item) = 
                    app.all_items.iter().find(|i| i.id == id)
            {
                let item = item.clone();
                
                match safe_rm::delete_item(&item, &[]) {
                    Ok(_) => {
                        app.set_status(
                            format!("Deleted: {}", item.filename),
                            StatusType::Success
                        );
                        app.need_refresh = true;
                    }
                    Err(e) => {
                        app.set_status(
                            format!("Delete failed: {}", e),
                            StatusType::Error
                        );
                    }
                }
            }
        }
        ConfirmCallback::RestoreSelected => {
            // Clone selected items to release immutable borrow on app.all_items
            let selected_ids: Vec<usize> = app.selected.iter().cloned().collect();
            let items: Vec<safe_rm::RestoreItem> = selected_ids
                .iter()
                .filter_map(|&id| app.all_items.iter().find(|i| i.id == id).cloned())
                .collect();

            if items.is_empty() {
                app.set_status(app.language.no_items_selected().to_string(), StatusType::Warning);
                return Ok(());
            }

            let total = items.len();
            let done = Arc::new(AtomicU64::new(0));
            let done_clone = Arc::clone(&done);

            let (prog_tx, prog_rx) = mpsc::channel();
            let (done_tx, done_rx) = mpsc::channel();

            // Timer thread: send progress every PROGRESS_UPDATE_INTERVAL_MS ms
            let prog_tx_timer = prog_tx.clone();
            thread::spawn(move || {
                loop {
                    std::thread::sleep(Duration::from_millis(PROGRESS_UPDATE_INTERVAL_MS));
                    let current = done_clone.load(Ordering::Relaxed) as usize;
                    if prog_tx_timer.send((current, total)).is_err() {
                        break;
                    }
                    if current >= total {
                        break;
                    }
                }
            });

            app.op_in_progress = true;
            app.op_message = app.language.confirm_restore_msg(total);
            app.op_total = total;
            app.op_done = 0;
            app.op_progress_rx = Some(prog_rx);
            app.op_done_rx = Some(done_rx);

            thread::spawn(move || {
                let item_refs: Vec<&safe_rm::RestoreItem> = items.iter().collect();
                // Progress callback: only increment counter
                let progress = |_current: usize, _total: usize| {
                    done.fetch_add(1, Ordering::Relaxed);
                };
                let success = safe_rm::restore_items_batch_with_progress(&item_refs, progress)
                    .unwrap_or(0);
                let msg = format!("Restored {}/{} items", success, total);
                let status = if success == total { StatusType::Success } else { StatusType::Warning };
                let _ = done_tx.send(OpResult { message: msg, status, need_refresh: true });
            });
        }
        ConfirmCallback::RestoreAll => {
            let items: Vec<safe_rm::RestoreItem> = app.filtered_indices
                .iter()
                .filter_map(|&idx| app.all_items.get(idx).cloned())
                .collect();

            if items.is_empty() {
                app.set_status("No items to restore".to_string(), StatusType::Warning);
                return Ok(());
            }

            let total = items.len();
            let done = Arc::new(AtomicU64::new(0));
            let done_clone = Arc::clone(&done);

            let (prog_tx, prog_rx) = mpsc::channel();
            let (done_tx, done_rx) = mpsc::channel();

            let prog_tx_timer = prog_tx.clone();
            thread::spawn(move || {
                loop {
                    std::thread::sleep(Duration::from_millis(PROGRESS_UPDATE_INTERVAL_MS));
                    let current = done_clone.load(Ordering::Relaxed) as usize;
                    if prog_tx_timer.send((current, total)).is_err() {
                        break;
                    }
                    if current >= total {
                        break;
                    }
                }
            });

            app.op_in_progress = true;
            app.op_message = app.language.confirm_restore_all_msg(total);
            app.op_total = total;
            app.op_done = 0;
            app.op_progress_rx = Some(prog_rx);
            app.op_done_rx = Some(done_rx);

            thread::spawn(move || {
                let item_refs: Vec<&safe_rm::RestoreItem> = items.iter().collect();
                let progress = |_current, _total| {
                    done.fetch_add(1, Ordering::Relaxed);
                };
                let success = safe_rm::restore_items_batch_with_progress(&item_refs, progress)
                    .unwrap_or(0);
                let msg = format!("Restored {}/{} items", success, total);
                let status = if success == total { StatusType::Success } else { StatusType::Warning };
                let _ = done_tx.send(OpResult { message: msg, status, need_refresh: true });
            });
        }
        ConfirmCallback::DeleteSelected => {
            let selected_ids: Vec<usize> = app.selected.iter().cloned().collect();
            let items: Vec<safe_rm::RestoreItem> = selected_ids
                .iter()
                .filter_map(|&id| app.all_items.iter().find(|i| i.id == id).cloned())
                .collect();

            if items.is_empty() {
                app.set_status(app.language.no_items_selected().to_string(), StatusType::Warning);
                return Ok(());
            }

            let total = items.len();
            let done = Arc::new(AtomicU64::new(0));
            let done_clone = Arc::clone(&done);

            let (prog_tx, prog_rx) = mpsc::channel();
            let (done_tx, done_rx) = mpsc::channel();

            let prog_tx_timer = prog_tx.clone();
            thread::spawn(move || {
                loop {
                    std::thread::sleep(Duration::from_millis(PROGRESS_UPDATE_INTERVAL_MS));
                    let current = done_clone.load(Ordering::Relaxed) as usize;
                    if prog_tx_timer.send((current, total)).is_err() {
                        break;
                    }
                    if current >= total {
                        break;
                    }
                }
            });

            app.op_in_progress = true;
            app.op_message = app.language.confirm_delete_msg(total);
            app.op_total = total;
            app.op_done = 0;
            app.op_progress_rx = Some(prog_rx);
            app.op_done_rx = Some(done_rx);

            thread::spawn(move || {
                let item_refs: Vec<&safe_rm::RestoreItem> = items.iter().collect();
                let progress = |_current, _total| {
                    done.fetch_add(1, Ordering::Relaxed);
                };
                let success = safe_rm::delete_items_batch_with_progress(&item_refs, progress)
                    .unwrap_or(0);
                let msg = format!("Deleted {}/{} items", success, total);
                let status = if success == total { StatusType::Success } else { StatusType::Warning };
                let _ = done_tx.send(OpResult { message: msg, status, need_refresh: true });
            });
        }
        ConfirmCallback::DeleteAll => {
            let items: Vec<safe_rm::RestoreItem> = app.filtered_indices
                .iter()
                .filter_map(|&idx| app.all_items.get(idx).cloned())
                .collect();

            if items.is_empty() {
                app.set_status("No items to delete".to_string(), StatusType::Warning);
                return Ok(());
            }

            let total = items.len();
            let done = Arc::new(AtomicU64::new(0));
            let done_clone = Arc::clone(&done);

            let (prog_tx, prog_rx) = mpsc::channel();
            let (done_tx, done_rx) = mpsc::channel();

            let prog_tx_timer = prog_tx.clone();
            thread::spawn(move || {
                loop {
                    std::thread::sleep(Duration::from_millis(PROGRESS_UPDATE_INTERVAL_MS));
                    let current = done_clone.load(Ordering::Relaxed) as usize;
                    if prog_tx_timer.send((current, total)).is_err() {
                        break;
                    }
                    if current >= total {
                        break;
                    }
                }
            });

            app.op_in_progress = true;
            app.op_message = app.language.empty_bin_msg(total);
            app.op_total = total;
            app.op_done = 0;
            app.op_progress_rx = Some(prog_rx);
            app.op_done_rx = Some(done_rx);

            thread::spawn(move || {
                let item_refs: Vec<&safe_rm::RestoreItem> = items.iter().collect();
                let progress = |_current, _total| {
                    done.fetch_add(1, Ordering::Relaxed);
                };
                let success = safe_rm::delete_items_batch_with_progress(&item_refs, progress)
                    .unwrap_or(0);
                let msg = format!("Deleted {}/{} items", success, total);
                let status = if success == total { StatusType::Success } else { StatusType::Warning };
                let _ = done_tx.send(OpResult { message: msg, status, need_refresh: true });
            });
        }
    }
    Ok(())
}

fn ui(f: &mut Frame, app: &App) {
    let area = f.area();

    // Calculate dynamic heights based on screen size
    let header_height = 3u16;
    let footer_height = 1u16;
    let preview_height = (area.height / 4).max(6).min(10);
    let list_height = area.height.saturating_sub(header_height + footer_height + preview_height);

    let main_chunks = Layout::default()
        .direction(Direction::Vertical)
        .margin(0)
        .constraints([
            Constraint::Length(header_height),
            Constraint::Min(list_height),
            Constraint::Length(footer_height),
            Constraint::Length(preview_height),
        ])
        .split(area);

    render_header(f, app, main_chunks[0]);
    render_list(f, app, main_chunks[1]);
    render_status_bar(f, app, main_chunks[2]);
    render_preview(f, app, main_chunks[3]);

    if app.show_help { render_help_dialog(f, app); }
    else if let Some(dialog) = 
        &app.show_confirm { 
            render_confirm_dialog(f, app, dialog); 
        }

    if app.op_in_progress {
        render_operation_overlay(f, area, app);
    }

    if app.is_loading {
        render_loading_overlay(f, area, 
            &app.loading_message, 
            app.loading_spinner_idx);
    }
}

fn render_loading_overlay(f: &mut Frame, area: Rect, msg: &str, spinner_idx: usize) {
    let popup_width = 26;
    let popup_height = 4;
    let popup_x = (area.width.saturating_sub(popup_width)) / 2;
    let popup_y = (area.height.saturating_sub(popup_height)) / 2;
    let popup_area = Rect::new(popup_x, popup_y, popup_width, popup_height);
    f.render_widget(Clear, popup_area);

    let spinner_chars = ['⠋', '⠙', '⠹', '⠸', '⠼', '⠴', '⠦', '⠧', '⠇', '⠏'];
    let spinner = spinner_chars[spinner_idx % spinner_chars.len()];

    let block = Block::default()
        .borders(Borders::ALL)
        .border_style(Style::default().fg(Color::Cyan))
        .title(" Loading ");
    let text = Text::from(vec![
        Line::from(vec![
            Span::styled(spinner.to_string(), Style::default().fg(Color::Yellow).add_modifier(Modifier::BOLD)),
            Span::raw(" "),
            Span::styled(msg, Style::default().add_modifier(Modifier::BOLD)),
        ]),
        Line::from(vec![
            Span::raw(" "),
            Span::styled("Please wait...", Style::default().fg(Color::Gray).add_modifier(Modifier::ITALIC)),
        ]),
    ]);
    let paragraph = Paragraph::new(text).block(block).alignment(Alignment::Center);
    f.render_widget(paragraph, popup_area);
}

fn render_operation_overlay(f: &mut Frame, area: Rect, app: &App) {
    let popup_width = 35;
    let popup_height = 4;
    let popup_x = (area.width.saturating_sub(popup_width)) / 2;
    let popup_y = (area.height.saturating_sub(popup_height)) / 2;
    let popup_area = Rect::new(popup_x, popup_y, popup_width, popup_height);
    f.render_widget(Clear, popup_area);

    let spinner_chars = ['⠋', '⠙', '⠹', '⠸', '⠼', '⠴', '⠦', '⠧', '⠇', '⠏'];
    let spinner = spinner_chars[app.op_spinner_idx % spinner_chars.len()];

    let title = if app.op_message.contains("Deleting") { " Deleting " } else { " Restoring " };
    let block = Block::default()
        .borders(Borders::ALL)
        .border_style(Style::default().fg(if title.contains("Deleting") { Color::Red } else { Color::Green }))
        .title(title);
    let progress_text = format!("Progress: {}/{}", app.op_done, app.op_total);
    let text = Text::from(vec![
        Line::from(vec![
            Span::raw(" "),
            Span::styled(spinner.to_string(), Style::default().fg(Color::Yellow).add_modifier(Modifier::BOLD)),
            Span::raw(" "),
            Span::styled(&app.op_message, Style::default().add_modifier(Modifier::BOLD)),
        ]),
        Line::from(vec![
            Span::raw(" "),
            Span::styled(progress_text, Style::default().fg(Color::Gray)),
        ]),
    ]);
    let paragraph = Paragraph::new(text).block(block).alignment(Alignment::Center);
    f.render_widget(paragraph, popup_area);
}

fn render_header(f: &mut Frame, app: &App, area: Rect) {
    let search_mode = match app.search_mode { SearchMode::Exact => app.language.exact(), SearchMode::Fuzzy => app.language.fuzzy() };
    let filter_text = if app.filter.is_empty() { app.language.none().to_string() } else { app.filter.clone() };
    let header = Paragraph::new(vec![
        Line::from(vec![
            Span::styled(app.language.title(), Style::default().fg(Color::Black).bg(Color::Cyan)),
            Span::raw("  "),
            Span::raw(format!("{}: {} {}  ", app.language.total(), app.all_items.len(), app.language.items())),
            Span::raw(format!("{}: {} {}  ", app.language.showing(), app.filtered_indices.len(), app.language.items())),
            Span::raw(format!("{}: {} {}", app.language.selected(), app.selected.len(), app.language.items())),
        ]),
        Line::from(vec![
            Span::raw(format!("{}: ", app.language.search())),
            Span::styled(filter_text, Style::default().fg(Color::Yellow)),
            Span::raw(" "),
            Span::styled(search_mode, Style::default().fg(Color::Green)),
        ]),
    ]).block(Block::default().borders(Borders::BOTTOM));
    f.render_widget(header, area);
}

fn render_list(f: &mut Frame, app: &App, area: Rect) {
    // Calculate column widths based on available space
    let total_width = area.width as usize;

    // Fixed widths for small columns
    let checkbox_width = 4usize;
    let id_width = 6usize;
    let time_width = 19usize;
    let status_width = 10usize;

    // Remaining space for path and filename
    let remaining = total_width.saturating_sub(checkbox_width + id_width + time_width + status_width + 6);

    // Give filename 60% of remaining space, path 40%
    let filename_width = (remaining * 60 / 100).max(15);
    let path_width = remaining.saturating_sub(filename_width).max(10);

    let header_cells: Vec<Cell> = vec![
        Cell::from(""),
        Cell::from(app.language.id()),
        Cell::from(app.language.deleted_at()),
        Cell::from(app.language.status()),
        Cell::from(app.language.original_path()),
        Cell::from(app.language.filename()),
    ].into_iter().map(|cell| cell.style(Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD))).collect();

    let header = Row::new(header_cells).height(1);

    // Calculate visible rows for lazy loading
    let visible_rows = (area.height as usize).max(1);
    let selected_idx = app.table_state.selected().unwrap_or(0);

    // Update scroll offset based on selection
    let scroll_offset = if selected_idx >= visible_rows {
        selected_idx.saturating_sub(visible_rows / 2)
    } else {
        0
    };

    let end_idx = (scroll_offset + visible_rows).min(app.filtered_indices.len());
    let visible_indices = &app.filtered_indices[scroll_offset..end_idx];

    let rows: Vec<Row> = visible_indices.iter().enumerate().map(|(i, &idx)| {
        let item = &app.all_items[idx];
        let actual_idx = scroll_offset + i;
        let is_selected = app.table_state.selected() == Some(actual_idx);
        let checkbox = if app.selected.contains(&item.id) { "[*]" } else { "[ ]" };
        let status = if item.is_archived { app.language.archived() } else { app.language.active() };
        let status_color = if item.is_archived { Color::Yellow } else { Color::Green };

        let original_path = item.original_path.to_string_lossy();
        let path_display = truncate_str(&original_path, path_width);
        let filename_display = truncate_str(&item.filename, filename_width);

        let cells = vec![
            Cell::from(checkbox),
            Cell::from(item.id.to_string()),
            Cell::from(item.timestamp.clone()),
            Cell::from(status).style(Style::default().fg(status_color)),
            Cell::from(path_display),
            Cell::from(filename_display),
        ];
        let style = if is_selected { Style::default().bg(Color::DarkGray).add_modifier(Modifier::BOLD) } else { Style::default() };
        Row::new(cells).style(style).height(1)
    }).collect();

    let constraints = vec![
        Constraint::Length(checkbox_width as u16),
        Constraint::Length(id_width as u16),
        Constraint::Length(time_width as u16),
        Constraint::Length(status_width as u16),
        Constraint::Length(path_width as u16),
        Constraint::Min(10),
    ];

    let table = Table::new(rows, constraints)
        .header(header)
        .block(Block::default().borders(Borders::ALL).title(app.language.item_list()));

    let mut state = app.table_state.clone();
    f.render_stateful_widget(table, area, &mut state);
}

fn render_status_bar(f: &mut Frame, app: &App, area: Rect) {
    let text = if let Some((msg, status_type, _)) = &app.status_message {
        let color = match status_type { StatusType::Success => Color::Green, StatusType::Warning => Color::Yellow, StatusType::Error => Color::Red };
        Line::from(Span::styled(msg.clone(), Style::default().fg(color)))
    } else {
        Line::from(vec![
            Span::raw("↑↓:Nav "), Span::raw("Sp:Sel "), Span::raw("a:All "), Span::raw("r:Rst "),
            Span::raw("d:Del "), Span::raw("/:Src "), Span::raw("L:Lang "), Span::raw("?:Help "), Span::raw("q:Quit"),
        ])
    };
    let status = Paragraph::new(text);
    f.render_widget(status, area);
}

fn render_preview(f: &mut Frame, app: &App, area: Rect) {
    let content = if let Some(item) = app.selected_item() {
        vec![
            Line::from(vec![Span::styled(format!("{}: ", app.language.filename()), Style::default().fg(Color::Cyan)), Span::raw(&item.filename)]),
            Line::from(vec![Span::styled(format!("{}: ", app.language.original_path()), Style::default().fg(Color::Cyan)), Span::raw(item.original_path.to_string_lossy().to_string())]),
            Line::from(vec![Span::styled(format!("{}: ", app.language.deleted_at()), Style::default().fg(Color::Cyan)), Span::raw(&item.timestamp)]),
            Line::from(vec![Span::styled(format!("{}: ", app.language.status()), Style::default().fg(Color::Cyan)), Span::raw(if item.is_archived { app.language.archived() } else { app.language.active() })]),
            if item.is_archived {
                Line::from(vec![Span::styled(format!("{}: ", app.language.archive_file()), Style::default().fg(Color::Cyan)), Span::raw(item.archive_file.as_ref().map(|p| p.to_string_lossy().to_string()).unwrap_or_default())])
            } else { Line::from("") },
        ]
    } else {
        vec![Line::from(app.language.no_item_selected())]
    };
    let preview = Paragraph::new(content).block(Block::default().borders(Borders::ALL).title(app.language.item_details())).wrap(Wrap { trim: true });
    f.render_widget(preview, area);
}

fn render_help_dialog(f: &mut Frame, app: &App) {
    // Smaller, more compact help dialog
    let area = centered_rect(50, 70, f.area());
    f.render_widget(Clear, area);

    let help_text = app.language.help_text();
    let help = Paragraph::new(help_text)
        .block(Block::default().borders(Borders::ALL).title(app.language.help()))
        .alignment(Alignment::Left);
    f.render_widget(help, area);
}

fn render_confirm_dialog(f: &mut Frame, app: &App, dialog: &ConfirmDialog) {
    let area = f.area();
    
    let msg_len = dialog.message.len() as u16;
    let popup_width = (msg_len + 5).max(35).min(area.width - 4);
    let popup_height = 6;
    
    let popup_x = (area.width.saturating_sub(popup_width)) / 2;
    let popup_y = (area.height.saturating_sub(popup_height)) / 2;
    let popup_area = Rect::new(popup_x, popup_y, popup_width, popup_height);
    
    f.render_widget(Clear, popup_area);

    let title_color = if dialog.is_delete { Color::Red } else { Color::Green };
    
    let content = format!(
        "\n{}\n\n{}    {}",
        dialog.message,
        app.language.yes(),
        app.language.no()
    );
    
    let dialog_widget = Paragraph::new(content)
        .block(
            Block::default()
                .borders(Borders::ALL)
                .border_style(Style::default().fg(title_color))
                .title(dialog.title.clone())
        )
        .alignment(Alignment::Center);

    f.render_widget(dialog_widget, popup_area);
}

fn centered_rect(percent_x: u16, percent_y: u16, r: Rect) -> Rect {
    let popup_layout = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Percentage((100 - percent_y) / 2), Constraint::Percentage(percent_y), Constraint::Percentage((100 - percent_y) / 2)])
        .split(r);
    Layout::default()
        .direction(Direction::Horizontal)
        .constraints([Constraint::Percentage((100 - percent_x) / 2), Constraint::Percentage(percent_x), Constraint::Percentage((100 - percent_x) / 2)])
        .split(popup_layout[1])[1]
}


