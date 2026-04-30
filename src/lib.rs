//! SafeRm - Safe deletion tool library

pub mod daemon;

use anyhow::{Context, Result};
use chrono::{Local, NaiveDateTime};
use serde::{Deserialize, Serialize};
use std::fs;
use std::io::Write;
use std::sync::Mutex;
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::os::unix::fs::MetadataExt;
use sha2::{Sha256, Digest};
use hmac::{Hmac, Mac};
use base64::{Engine as _, engine::general_purpose::STANDARD};
use rayon::prelude::*;
use once_cell::sync::Lazy;
use tempfile::NamedTempFile;

/// Global counter for generating unique file suffixes when restoring
/// conflicts arise. Key is the original destination path (without suffix).
static UNIQUE_CTR: Lazy<Mutex<HashMap<PathBuf, u64>>> =
    Lazy::new(|| Mutex::new(HashMap::new()));

type HmacSha256 = Hmac<Sha256>;

pub const VERSION: &str = "1.0.0";

const SALT: &[u8] = b"SafeRmTrashCacheV1";

pub(crate) const RUBBISH_BIN_NAME: &str = ".rubbishbin";
pub(crate) const OLD_DIR: &str = ".old";
pub(crate) const INFO_FILE: &str = "info";
pub(crate) const RUBBISH_SUBDIR: &str = "rubbish";
pub(crate) const MANIFEST_SUFFIX: &str = ".manifest.json";
pub(crate) const CACHE_DIR: &str = ".cache";
pub(crate) const CACHE_FILE_NAME: &str = "items.json";

pub const DAEMON_NAME: &str = "trashd";
pub const DAEMON_ENV_VAR: &str = "TRASHD_DAEMON";
pub const DAEMON_DIR: &str = ".daemon";
pub const DAEMON_PID_FILE: &str = "trashd.pid";
pub const DAEMON_LOG_FILE: &str = "daemon.log";
pub const COMPRESS_DAYS: i64 = 3;
pub const CLEANUP_DAYS: i64 = 60;
pub const COMPRESS_THRESHOLD: usize = 20;
pub const ARCHIVE_CAPACITY: usize = 50;

/// Restore item information
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RestoreItem {
    pub id: usize,
    pub timestamp: String,
    pub original_path: PathBuf,
    pub filename: String,
    pub is_archived: bool,
    pub archive_file: Option<PathBuf>,
    pub index_dir: Option<PathBuf>,
}

/// Manifest entry for an archived item
#[derive(Debug, Clone, Serialize, Deserialize)]
struct ArchiveItem {
    timestamp: String,
    original_path: String,
    filename: String,
    deleted: bool,
}

/// Lightweight manifest for a .tar.gz archive
#[derive(Debug, Clone, Serialize, Deserialize)]
struct ArchiveManifest {
    archive: String,      // archive file name for documentation
    items: Vec<ArchiveItem>,
}

/// Cache structure for fast loading
#[derive(Debug, Clone, Serialize, Deserialize)]
struct TrashCache {
    items: Vec<RestoreItem>,
    dir_ctime: i64,
    item_count: usize,
    signature: String,  // base64 encoded HMAC-SHA256
}

impl TrashCache {
    /// Verify if the signature is valid
    fn verify(&self) -> bool {
        let key = match derive_key() {
            Ok(k) => k,
            Err(_) => return false,
        };
        
        // Recalculate signature (exclude signature field itself)
        let data = serde_json::json!({
            "items": self.items,
            "dir_ctime": self.dir_ctime,
            "item_count": self.item_count,
        }).to_string();
        
        let mut mac = match HmacSha256::new_from_slice(&key) {
            Ok(m) => m,
            Err(_) => return false,
        };
        mac.update(data.as_bytes());
        let expected = mac.finalize().into_bytes();
        
        // Decode stored signature
        let stored = match STANDARD.decode(&self.signature) {
            Ok(s) => s,
            Err(_) => return false,
        };
        
        expected.as_slice() == stored.as_slice()
    }
    
    /// Create new cache with signature
    fn new(items: Vec<RestoreItem>, dir_ctime: i64) -> Result<Self> {
        let key = derive_key()?;
        let item_count = items.len();

        let data = serde_json::json!({
            "items": items,
            "dir_ctime": dir_ctime,
            "item_count": item_count,
        }).to_string();

        let mut mac = HmacSha256::new_from_slice(&key)
            .map_err(|_| anyhow::anyhow!("Failed to create HMAC"))?;
        mac.update(data.as_bytes());
        let signature = mac.finalize().into_bytes();
        let signature_b64 = STANDARD.encode(&signature);

        Ok(Self {
            items,
            dir_ctime,
            item_count,
            signature: signature_b64,
        })
    }
}

/// Get the rubbish bin path
pub fn get_rubbish_bin_path() -> Result<PathBuf> {
    let home = dirs::home_dir().context("Cannot get home directory")?;
    Ok(home.join(RUBBISH_BIN_NAME))
}

/// Get daemon directory path (inside rubbish bin)
pub fn get_daemon_dir() -> Result<PathBuf> {
    let rubbish_bin = get_rubbish_bin_path()?;
    let daemon_dir = rubbish_bin.join(DAEMON_DIR);
    create_dir_all(&daemon_dir)?;
    Ok(daemon_dir)
}

/// Get current timestamp string
pub fn get_timestamp() -> String {
    Local::now().format("%Y-%m-%d-%H:%M:%S").to_string()
}

/// Parse timestamp string
fn parse_timestamp(s: &str) -> Option<NaiveDateTime> {
    NaiveDateTime::parse_from_str(s, "%Y-%m-%d-%H:%M:%S").ok()
}

/// Check if path is rubbish bin or its parent
/// Returns: 0=safe, 1=is rubbish bin or child, 2=is parent of rubbish bin
pub fn is_rubbishbin_or_parent(path: &Path) -> Result<i32> {
    let rubbish_bin = get_rubbish_bin_path()?;
    let canonical_path = fs::canonicalize(path).unwrap_or_else(|_| path.to_path_buf());
    let canonical_rubbish = fs::canonicalize(&rubbish_bin).unwrap_or(rubbish_bin);

    if canonical_path == canonical_rubbish {
        return Ok(1);
    }

    if canonical_path.starts_with(&canonical_rubbish) {
        return Ok(1);
    }

    if canonical_rubbish.starts_with(&canonical_path) {
        return Ok(2);
    }

    Ok(0)
}

/// Check if path is dangerous
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum PathDanger {
    Safe,
    IsDot,
    IsDotDot,
    EndsWithDot,
    EndsWithDotDot,
    IsCwd,
    IsRoot,
}

impl PathDanger {
    pub fn check(path: &Path) -> Self {
        let s = path.to_string_lossy();

        if s == "." || s == "./" {
            return PathDanger::IsDot;
        }
        if s == ".." || s == "../" {
            return PathDanger::IsDotDot;
        }

        if let Some(file_name) = path.file_name() {
            let name = file_name.to_string_lossy();
            if name == "." {
                return PathDanger::EndsWithDot;
            }
            if name == ".." {
                return PathDanger::EndsWithDotDot;
            }
        }

        if let Ok(resolved) = fs::canonicalize(path) {
            if let Ok(cwd) = std::env::current_dir() {
                if resolved == cwd {
                    return PathDanger::IsCwd;
                }
            }
            if resolved == Path::new("/") {
                return PathDanger::IsRoot;
            }
        }

        PathDanger::Safe
    }

    pub fn description(&self) -> &'static str {
        match self {
            PathDanger::Safe => "Safe to operate",
            PathDanger::IsDot => "Cannot remove directory",
            PathDanger::IsDotDot => "Cannot remove directory",
            PathDanger::EndsWithDot => "Directory ends with '.' (points to self)",
            PathDanger::EndsWithDotDot => "Directory ends with '..' (points to parent)",
            PathDanger::IsCwd => "Is current working directory",
            PathDanger::IsRoot => "Is root directory",
        }
    }

    pub fn is_safe(&self) -> bool {
        matches!(self, PathDanger::Safe)
    }
}

/// Safety check
pub fn check_safety_constraints(path: &Path) -> Result<()> {
    match is_rubbishbin_or_parent(path)? {
        1 => anyhow::bail!(
            "Cannot delete rubbish bin or its subdirectories '{}'",
            path.display()
        ),
        2 => anyhow::bail!(
            "Cannot delete parent directory of rubbish bin '{}'",
            path.display()
        ),
        _ => {}
    }

    let danger = PathDanger::check(path);
    if !danger.is_safe() {
        anyhow::bail!("{} '{}'", danger.description(), path.display());
    }

    Ok(())
}

/// Create directory recursively
fn create_dir_all(path: &Path) -> Result<()> {
    fs::create_dir_all(path)
        .with_context(|| format!("Failed to create directory: {}", path.display()))?;
    Ok(())
}

/// Get absolute path
fn get_absolute_path(path: &Path) -> Result<PathBuf> {
    let abs = fs::canonicalize(path).or_else(|_| {
        let cwd = std::env::current_dir()?;
        Ok::<_, anyhow::Error>(cwd.join(path))
    })?;
    Ok(abs)
}

/// Copy file metadata
pub fn copy_metadata(src: &Path, dst: &Path) -> Result<()> {
    let metadata =
        fs::metadata(src).with_context(|| format!("Failed to get metadata: {}", src.display()))?;

    let permissions = metadata.permissions();
    fs::set_permissions(dst, permissions)
        .with_context(|| format!("Failed to set permissions: {}", dst.display()))?;

    Ok(())
}

/// Move file or directory
pub fn move_file_or_dir(src: &Path, dst: &Path) -> Result<()> {
    if fs::rename(src, dst).is_ok() {
        return Ok(());
    }

    copy_recursive(src, dst)?;
    remove_recursive(src)?;

    Ok(())
}

fn copy_recursive(src: &Path, dst: &Path) -> Result<()> {
    let metadata = fs::metadata(src)?;

    if metadata.is_dir() {
        fs::create_dir_all(dst)?;
        for entry in fs::read_dir(src)? {
            let entry = entry?;
            let src_path = entry.path();
            let dst_path = dst.join(entry.file_name());
            copy_recursive(&src_path, &dst_path)?;
        }
    } else {
        fs::copy(src, dst)?;
        copy_metadata(src, dst)?;
    }

    Ok(())
}

fn remove_recursive(path: &Path) -> Result<()> {
    if path.is_dir() {
        fs::remove_dir_all(path)?;
    } else {
        fs::remove_file(path)?;
    }
    Ok(())
}

/// Generate a candidate destination path with a numeric suffix
fn unique_name(original: &Path, index: u64) -> PathBuf {
    let parent = original.parent().unwrap_or(Path::new("."));
    let stem = original.file_stem().unwrap_or_default();
    let ext = original.extension();

    if let Some(ext) = ext {
        parent.join(format!(
            "{} ({}).{}",
            stem.to_string_lossy(),
            index,
            ext.to_string_lossy()
        ))
    } else {
        parent.join(format!("{} ({})", stem.to_string_lossy(), index))
    }
}

/// Move `src` to `dst`. If `dst` already exists, automatically append
fn move_to_unique_path(src: &Path, dst: &Path) -> Result<PathBuf> {
    let mut map = UNIQUE_CTR.lock().unwrap();

    if !dst.exists() {
        match fs::rename(src, dst) {
            Ok(()) => return Ok(dst.to_path_buf()),
            Err(e) => {
                drop(map);
                if e.kind() == std::io::ErrorKind::CrossesDevices {
                    move_file_or_dir(src, dst)?;
                    return Ok(dst.to_path_buf());
                } else {
                    return Err(e.into());
                }
            }
        }
    }

    loop {
        let counter = map.entry(dst.to_path_buf()).or_insert(0);
        *counter += 1;
        let candidate = unique_name(dst, *counter);

        if !candidate.exists() {
            match fs::rename(src, &candidate) {
                Ok(()) => {
                    return Ok(candidate);
                }
                Err(e) if e.kind() == std::io::ErrorKind::CrossesDevices => {
                    drop(map);
                    move_file_or_dir(src, &candidate)?;
                    return Ok(candidate);
                }
                Err(_) => continue,
            }
        }
    }
}

/// Get cache file path
pub fn get_cache_path() -> Result<PathBuf> {
    let rubbish_bin = get_rubbish_bin_path()?;
    let cache_dir = rubbish_bin.join(CACHE_DIR);
    create_dir_all(&cache_dir)?;
    Ok(cache_dir.join(CACHE_FILE_NAME))
}

/// Write to a file atomically: write to a temp file then rename.
fn atomic_write(path: &Path, content: &[u8]) -> Result<()> {
    let dir = path.parent().context("No parent directory")?;
    let mut tmp = NamedTempFile::new_in(dir)?;

    tmp.write_all(content)?;
    tmp.persist(path)?;

    Ok(())
}

static CACHED_KEY: Lazy<Vec<u8>> = Lazy::new(|| {
    derive_key_inner().expect("Failed to derive cache key")
});

fn derive_key_inner() -> Result<Vec<u8>> {
    let hostname = whoami::fallible::hostname()
        .unwrap_or_else(|_| "unknown".to_string());

    let devicename = whoami::fallible::devicename()
        .unwrap_or_else(|_| "unknown".to_string());

    let distro = whoami::distro();

    let sys_info = format!(
        "{}In{}Is{}With{}As{}At{}Use",
        distro,
        devicename,
        std::env::consts::OS,
        std::env::consts::ARCH,
        whoami::username(),
        hostname,
    );

    let mut hasher = Sha256::new();
    hasher.update(sys_info.as_bytes());
    hasher.update(SALT);

    Ok(hasher.finalize().to_vec())
}

/// Derive encryption key from user environment (system information + SALT)
fn derive_key() -> Result<Vec<u8>> {
    Ok(CACHED_KEY.clone())
}

/// Load cache from file if valid
fn load_cache() -> Result<Option<TrashCache>> {
    let cache_path = get_cache_path()?;

    if !cache_path.exists() {
        return Ok(None);
    }

    let cache_data = fs::read_to_string(&cache_path)?;
    
    // Try to parse the cache.
    // If the format is incompatible, delete it and return None
    let cache: TrashCache = match serde_json::from_str(&cache_data) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("Cache parse error (will be discarded): {}", e);
            let _ = fs::remove_file(&cache_path);
            return Ok(None);
        }
    };

    // Verify signature first (defense against tampering)
    if !cache.verify() {
        eprintln!("Cache signature invalid, discarding");
        let _ = fs::remove_file(&cache_path);
        return Ok(None);
    }

    // Then check ctime (defense against accidental changes)
    let rubbish_bin = get_rubbish_bin_path()?;
    let metadata = fs::metadata(&rubbish_bin)?;

    if cache.dir_ctime == metadata.ctime() {
        Ok(Some(cache))
    } else {
        // Directory changed, cache outdated
        let _ = fs::remove_file(&cache_path);
        Ok(None)
    }
}

/// Save cache to file
fn save_cache(items: &[RestoreItem]) -> Result<()> {
    let cache_path = get_cache_path()?;
    let rubbish_bin = get_rubbish_bin_path()?;
    let metadata = fs::metadata(&rubbish_bin)?;
    let cache = TrashCache::new(items.to_vec(), metadata.ctime())?;
    let cache_data = serde_json::to_string(&cache)?;

    atomic_write(&cache_path, cache_data.as_bytes())?;
    Ok(())
}

/// Scan and save cache
pub fn refresh_cache() -> Result<()> {
    let items = scan_directories()?;
    save_cache(&items)?;
    Ok(())
}

/// Load valid cache (signature + ctime check).
fn load_valid_cache() -> Option<TrashCache> {
    load_cache().ok().flatten()
}

/// Mutate cache items and write back, updating dir_ctime automatically.
fn mutate_cache<F>(mutator: F)
where
    F: FnOnce(&mut Vec<RestoreItem>),
{
    if let Some(mut cache) = load_valid_cache() {
        mutator(&mut cache.items);
        // Re-save will recompute dir_ctime and signature
        let _ = save_cache(&cache.items);
    }
}

/// Invalidate cache (delete cache file)
pub fn invalidate_cache() -> Result<()> {
    let cache_path = get_cache_path()?;
    if cache_path.exists() {
        fs::remove_file(&cache_path)?;
    }
    Ok(())
}

/// Scan restore items from rubbish bin (with cache support)
pub fn scan_restore_items() -> Result<Vec<RestoreItem>> {
    // Try to load from cache first
    if let Some(cache) = load_cache()? {
        return Ok(cache.items);
    }

    // Cache miss or expired, scan directories
    let items = scan_directories()?;

    // Save to cache
    let _ = save_cache(&items);

    Ok(items)
}

/// Scan directories and build item list
fn scan_directories() -> Result<Vec<RestoreItem>> {
    let rubbish_bin = get_rubbish_bin_path()?;
    let mut items = Vec::new();
    let mut id = 1;

    if !rubbish_bin.exists() {
        return Ok(items);
    }

    for entry in fs::read_dir(&rubbish_bin)? {
        let entry = entry?;
        let path = entry.path();
        let name = entry.file_name().to_string_lossy().to_string();

        if name == OLD_DIR || name.starts_with('.') {
            continue;
        }

        if path.is_dir() && parse_timestamp(&name).is_some() {
            scan_timestamp_dir(&path, &name, false, None, &mut items, &mut id)?;
        }
    }

    let old_dir = rubbish_bin.join(OLD_DIR);
    if old_dir.exists() {
        for entry in fs::read_dir(&old_dir)? {
            let entry = entry?;
            let path = entry.path();
            let name = entry.file_name().to_string_lossy().to_string();

            if name.ends_with(".tar.gz") {
                scan_archive(&path, &name, &mut items, &mut id)?;
            }
        }
    }

    items.sort_by(|a, b| b.timestamp.cmp(&a.timestamp));

    for (i, item) in items.iter_mut().enumerate() {
        item.id = i + 1;
    }

    Ok(items)
}

fn scan_timestamp_dir(
    dir: &Path,
    timestamp: &str,
    is_archived: bool,
    archive_file: Option<&Path>,
    items: &mut Vec<RestoreItem>,
    id: &mut usize,
) -> Result<()> {
    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        let index_dir = entry.path(); // e.g. .../timestamp/1

        if index_dir.is_dir() {
            let info_file = index_dir.join(INFO_FILE);
            if info_file.exists() {
                let original_path = fs::read_to_string(&info_file)
                    .unwrap_or_default()
                    .trim()
                    .to_string();

                let rubbish_dir = index_dir.join(RUBBISH_SUBDIR);
                if let Ok(entries) = fs::read_dir(&rubbish_dir) {
                    for rubbish_entry in entries.flatten() {
                        let filename = 
                            rubbish_entry.file_name().to_string_lossy().to_string();

                        items.push(RestoreItem {
                            id: *id,
                            timestamp: timestamp.to_string(),
                            original_path: PathBuf::from(&original_path),
                            filename,
                            is_archived,
                            archive_file: archive_file.map(|p| p.to_path_buf()),
                            index_dir: if is_archived {
                                None
                            } else {
                                // Store the actual directory
                                Some(index_dir.clone())
                            },
                        });
                        *id += 1;
                    }
                }
            }
        }
    }
    Ok(())
}

/// Returns the manifest path corresponding to the given archive path.
/// The manifest file is placed in the same directory as the archive,
/// with MANIFEST_SUFFIX appended to its original file name.
fn manifest_path(archive_path: &Path) -> PathBuf {
    let mut name = archive_path
        .file_name()
        .expect("archive path should have a file name")
        .to_os_string();
    name.push(MANIFEST_SUFFIX);
    archive_path.with_file_name(name)
}

/// Generate a manifest file next to the archive
fn generate_manifest(archive_path: &Path, dirs: &[&Path]) -> Result<()> {
    let mut items = Vec::new();

    for dir in dirs {
        let timestamp = dir
            .file_name()
            .and_then(|n| n.to_str())
            .unwrap_or("unknown")
            .to_string();

        // Scan each index subdirectory inside timestamp dir
        for entry in fs::read_dir(dir)? {
            let entry = entry?;
            let index_dir = entry.path();
            if !index_dir.is_dir() {
                continue;
            }

            let info_file = index_dir.join(INFO_FILE);
            if !info_file.exists() {
                continue;
            }

            let original_path = fs::read_to_string(&info_file)?
                .trim()
                .to_string();

            let rubbish_dir = index_dir.join(RUBBISH_SUBDIR);
            if let Ok(rubbish_entries) = fs::read_dir(&rubbish_dir) {
                for rubbish_entry in rubbish_entries.flatten() {
                    let filename = 
                        rubbish_entry.file_name().to_string_lossy().to_string();
                    items.push(ArchiveItem {
                        timestamp: timestamp.clone(),
                        original_path: original_path.clone(),
                        filename,
                        deleted: false,
                    });
                }
            }
        }
    }

    let manifest = ArchiveManifest {
        archive: archive_path
            .file_name()
            .and_then(|n| n.to_str())
            .unwrap_or("archive.tar.gz")
            .to_string(),
        items,
    };

    let manifest_path = manifest_path(archive_path);
    let manifest_data = serde_json::to_string_pretty(&manifest)?;
    fs::write(&manifest_path, manifest_data)?;

    Ok(())
}

fn scan_archive(
    archive_path: &Path,
    _archive_name: &str,
    items: &mut Vec<RestoreItem>,
    id: &mut usize,
) -> Result<()> {
    let manifest_path = manifest_path(archive_path);

    if manifest_path.exists() {
        // Fast path: read manifest directly
        let manifest_data = fs::read_to_string(&manifest_path)?;
        let manifest: ArchiveManifest = serde_json::from_str(&manifest_data)?;

        for entry in manifest.items {
            if entry.deleted {
                continue;   // skip deleted items
            }

            items.push(RestoreItem {
                id: *id,
                timestamp: entry.timestamp,
                original_path: PathBuf::from(entry.original_path),
                filename: entry.filename,
                is_archived: true,
                archive_file: Some(archive_path.to_path_buf()),
                index_dir: None, // archived items have no active index directory
            });
            *id += 1;
        }
        return Ok(());
    }

    // Fallback: decompress and scan (old archive without manifest)
    // After scanning, generate the manifest for future use.
    let temp_dir = tempfile::tempdir()?;
    {
        use flate2::read::GzDecoder;
        use tar::Archive;

        let file = fs::File::open(archive_path)?;
        let decoder = GzDecoder::new(file);
        let mut archive = Archive::new(decoder);

        // Unpack to temp directory to inspect
        archive.unpack(temp_dir.path())?;
    }

    // Now scan the unpacked directory and build manifest entries
    let mut manifest_items = Vec::new();
    for entry in fs::read_dir(temp_dir.path())? {
        let entry = entry?;
        let timestamp_dir = entry.path();
        let timestamp = entry.file_name().to_string_lossy().to_string();

        if !timestamp_dir.is_dir() {
            continue;
        }

        for index_entry in fs::read_dir(&timestamp_dir)? {
            let index_entry = index_entry?;
            let index_dir = index_entry.path();
            if !index_dir.is_dir() {
                continue;
            }

            let info_file = index_dir.join(INFO_FILE);
            let original_path = if info_file.exists() {
                fs::read_to_string(&info_file)?.trim().to_string()
            } else {
                String::new()
            };

            let rubbish_dir = index_dir.join(RUBBISH_SUBDIR);
            if let Ok(rubbish_entries) = fs::read_dir(&rubbish_dir) {
                for rubbish_entry in rubbish_entries.flatten() {
                    let filename = 
                        rubbish_entry.file_name().to_string_lossy().to_string();

                    items.push(RestoreItem {
                        id: *id,
                        timestamp: timestamp.clone(),
                        original_path: PathBuf::from(&original_path),
                        filename: filename.clone(),
                        is_archived: true,
                        archive_file: Some(archive_path.to_path_buf()),
                        index_dir: None,
                    });
                    *id += 1;

                    manifest_items.push(ArchiveItem {
                        timestamp: timestamp.clone(),
                        original_path: original_path.clone(),
                        filename,
                        deleted: false,
                    });
                }
            }
        }
    }

    // Write manifest for next time
    let manifest = ArchiveManifest {
        archive: archive_path
            .file_name()
            .and_then(|n| n.to_str())
            .unwrap_or("archive.tar.gz")
            .to_string(),
        items: manifest_items,
    };
    let manifest_data = serde_json::to_string_pretty(&manifest)?;
    fs::write(&manifest_path, manifest_data)?;

    Ok(())
}

/// Restore item to original location (internal, no cache invalidation)
fn restore_item_internal(item: &RestoreItem) -> Result<()> {
    let rubbish_bin = get_rubbish_bin_path()?;

    // Keep TempDir alive until move is done (fixes premature cleanup)
    let (src_path, maybe_index_dir, _temp_dir_holder) = if item.is_archived {
        let temp_dir = tempfile::tempdir()?;
        let extracted = extract_from_archive(item, temp_dir.path())?;
        (extracted, None, Some(temp_dir))
    } else {
        let index_dir = match &item.index_dir {
            Some(dir) => dir.clone(),
            None => find_index_dir(&rubbish_bin, &item.timestamp, &item.filename)?,
        };
        let src = index_dir.join(RUBBISH_SUBDIR).join(&item.filename);
        (src, Some(index_dir), None)
    };

    // Resolve absolute destination (handles possible relative paths)
    let mut original = item.original_path.clone();
    if original.is_relative() {
        original = std::env::current_dir()?.join(&original);
    }
    let dst_path = original.join(&item.filename);

    // Ensure parent directory exists
    if let Some(parent) = dst_path.parent() {
        create_dir_all(parent)?;
    }

    // Move with automatic conflict renaming
    let _final_dst = move_to_unique_path(&src_path, &dst_path)?;

    // Remove from manifest after successful move (archived items only)
    if item.is_archived {
        remove_archive_item_from_manifest(item)?;
    }

    // Clean up empty directories (non-archived items)
    if let Some(index_dir) = maybe_index_dir {
        cleanup_empty_index_dir(&index_dir)?;
        cleanup_empty_timestamp_dir(&rubbish_bin, &item.timestamp)?;
    }

    Ok(())
}

/// Restore item to original location (with cache invalidation)
pub fn restore_item(item: &RestoreItem, _all_items: &[RestoreItem]) -> Result<()> {
    restore_item_internal(item)?;
    mutate_cache(|items| {
        items.retain(|i| !(
            i.timestamp == item.timestamp &&
            i.original_path == item.original_path &&
            i.filename == item.filename &&
            i.is_archived == item.is_archived
        ));
    });
    Ok(())
}

fn extract_from_archive(item: &RestoreItem, temp_dir: &Path) -> Result<PathBuf> {
    use flate2::read::GzDecoder;
    use tar::Archive;

    let archive_path = item
        .archive_file
        .as_ref()
        .context("Archive file path is empty")?;

    let file = fs::File::open(archive_path)?;
    let decoder = GzDecoder::new(file);
    let mut archive = Archive::new(decoder);

    let mut rubbish_prefix: Option<PathBuf> = None;
    let mut found = false;       // have we seen the target entry?
    let mut finished = false;    // have we extracted all entries under the prefix?

    // Single-pass scan: locate the target and extract all items under its rubbish/ prefix
    for entry in archive.entries()? {
        let mut entry = entry?;
        let path = entry.path()?.into_owned();

        // Skip remaining entries once the required prefix has been fully extracted
        if finished {
            continue;
        }

        if !found {
            // Locate the entry that matches our item
            if path.to_string_lossy().contains(&format!("{}/", item.timestamp))
                && path.to_string_lossy().contains(&format!("/{}/", RUBBISH_SUBDIR))
                && path.file_name().map_or(false, |n| n == item.filename.as_str())
            {
                let prefix = path.parent().unwrap().to_path_buf();
                // Extract the matched entry itself (file or directory)
                let relative = path.strip_prefix(&prefix)?;
                entry.unpack(&temp_dir.join(relative))?;
                rubbish_prefix = Some(prefix);
                found = true;
            }
            continue;
        }

        // Found — extract all remaining entries that belong to the same prefix
        let prefix = rubbish_prefix.as_ref().unwrap();
        if path.starts_with(prefix) {
            let relative = path.strip_prefix(prefix)?;
            entry.unpack(&temp_dir.join(relative))?;
        } else {
            // The archive stores entries depth‑first; any path outside the prefix
            // means we have finished extracting this rubbish/ directory.
            finished = true;
        }
    }

    if !found {
        anyhow::bail!("File not found in archive");
    }

    let final_path = temp_dir.join(&item.filename);
    if !final_path.exists() {
        anyhow::bail!(
            "Extraction succeeded but target not found: {}",
            item.filename
        );
    }

    Ok(final_path)
}

fn find_index_dir(rubbish_bin: &Path, timestamp: &str, filename: &str) -> Result<PathBuf> {
    let timestamp_dir = rubbish_bin.join(timestamp);

    for entry in fs::read_dir(&timestamp_dir)? {
        let entry = entry?;
        let index_dir = entry.path();
        let rubbish_dir = index_dir.join(RUBBISH_SUBDIR);
        let file_path = rubbish_dir.join(filename);

        if file_path.exists() {
            return Ok(index_dir);
        }
    }

    anyhow::bail!("File index directory not found")
}

/// Delete item permanently from rubbish bin (internal, no cache invalidation)
fn delete_item_internal(item: &RestoreItem) -> Result<()> {
    let rubbish_bin = get_rubbish_bin_path()?;

    if item.is_archived {
        delete_from_archive(item)?;
    } else {
        // Prefer the cached index_dir, fall back to scanning for old cache entries
        let index_dir = match &item.index_dir {
            Some(dir) => dir.clone(),
            None => find_index_dir(&rubbish_bin, &item.timestamp, &item.filename)?,
        };
        let file_path = index_dir.join(RUBBISH_SUBDIR).join(&item.filename);

        if file_path.is_dir() {
            fs::remove_dir_all(&file_path)?;
        } else {
            fs::remove_file(&file_path)?;
        }

        cleanup_empty_index_dir(&index_dir)?;
        cleanup_empty_timestamp_dir(&rubbish_bin, &item.timestamp)?;
    }

    Ok(())
}

/// Delete item permanently from rubbish bin (with cache invalidation)
pub fn delete_item(item: &RestoreItem, _all_items: &[RestoreItem]) -> Result<()> {
    delete_item_internal(item)?;
    mutate_cache(|items| {
        items.retain(|i| !(
            i.timestamp == item.timestamp &&
            i.original_path == item.original_path &&
            i.filename == item.filename &&
            i.is_archived == item.is_archived
        ));
    });
    Ok(())
}

/// Delete multiple items (batch operation, single cache invalidation)
pub fn delete_items_batch(items: &[&RestoreItem]) -> Result<usize> {
    let success = process_batch(items, |item| delete_item_internal(item))?;

    mutate_cache(|cache_items| {
        for item in items {
            cache_items.retain(|i| !(
                i.timestamp == item.timestamp &&
                i.original_path == item.original_path &&
                i.filename == item.filename &&
                i.is_archived == item.is_archived
            ));
        }
    });
    Ok(success)
}

/// Restore multiple items (batch operation, single cache invalidation)
pub fn restore_items_batch(items: &[&RestoreItem]) -> Result<usize> {
    let success = process_batch(items, |item| restore_item_internal(item))?;

    mutate_cache(|cache_items| {
        for item in items {
            cache_items.retain(|i| !(
                i.timestamp == item.timestamp &&
                i.original_path == item.original_path &&
                i.filename == item.filename &&
                i.is_archived == item.is_archived
            ));
        }
    });
    Ok(success)
}

/// Clean up empty index directory (e.g., 2026-04-04-07:51:27/1)
fn cleanup_empty_index_dir(index_dir: &Path) -> Result<()> {
    if !index_dir.exists() {
        return Ok(());
    }

    let rubbish_dir = index_dir.join(RUBBISH_SUBDIR);

    // Check if rubbish directory exists and is empty
    if rubbish_dir.exists() {
        let has_files = fs::read_dir(&rubbish_dir)?.any(|e| {
            e.map(|entry| entry.file_type().map(|ft| ft.is_file()).unwrap_or(false))
                .unwrap_or(false)
        });

        if !has_files {
            // Remove the entire index_dir (including info file and empty rubbish dir)
            let _ = fs::remove_dir_all(index_dir);
        }
    } else {
        // If rubbish dir doesn't exist, remove the index_dir
        let _ = fs::remove_dir_all(index_dir);
    }

    Ok(())
}

/// Clean up empty timestamp directory (e.g., 2026-04-04-07:51:27)
fn cleanup_empty_timestamp_dir(rubbish_bin: &Path, timestamp: &str) -> Result<()> {
    let timestamp_dir = rubbish_bin.join(timestamp);

    if !timestamp_dir.exists() {
        return Ok(());
    }

    // Check if timestamp_dir has any subdirectories
    let has_subdirs = fs::read_dir(&timestamp_dir)?.any(|e| {
        e.map(|entry| entry.file_type().map(|ft| ft.is_dir()).unwrap_or(false))
            .unwrap_or(false)
    });

    if !has_subdirs {
        // Remove the entire timestamp_dir
        let _ = fs::remove_dir_all(&timestamp_dir);
    }

    Ok(())
}

fn delete_from_archive(item: &RestoreItem) -> Result<()> {
    let archive_path = item
        .archive_file
        .as_ref()
        .context("Archive file path is empty")?;

    let manifest_path = manifest_path(archive_path); 

    // Ensure manifest exists (should always exist for archives created after the update)
    if !manifest_path.exists() {
        anyhow::bail!(
            "Archive manifest for {} not found, please refresh scan first",
            archive_path.display()
        );
    }

    // Load manifest
    let manifest_data = fs::read_to_string(&manifest_path)?;
    let mut manifest: ArchiveManifest = serde_json::from_str(&manifest_data)?;

    // Find and mark the item as deleted
    let mut found = false;
    for entry in &mut manifest.items {
        if entry.timestamp == item.timestamp
            && entry.original_path == item.original_path.to_string_lossy()
            && entry.filename == item.filename
            && !entry.deleted
        {
            entry.deleted = true;
            found = true;
            break;
        }
    }

    if !found {
        anyhow::bail!("Item not found in archive manifest or already deleted");
    }

    // Write back manifest
    let new_data = serde_json::to_string_pretty(&manifest)?;
    fs::write(&manifest_path, new_data)?;

    Ok(())
}

/// After successfully restoring a file from an archive, remove its entry
/// from the manifest entirely, so it no longer appears in any scan.
fn remove_archive_item_from_manifest(item: &RestoreItem) -> Result<()> {
    if !item.is_archived {
        return Ok(());
    }

    let archive_path = item.archive_file.as_ref()
        .context("Archive path missing")?;
    let manifest_path = manifest_path(archive_path);

    if !manifest_path.exists() {
        // Manifest missing; nothing to clean up, but not an error.
        return Ok(());
    }

    let data = fs::read_to_string(&manifest_path)?;
    let mut manifest: ArchiveManifest = serde_json::from_str(&data)?;

    // Keep all items except the one being restored
    let original_len = manifest.items.len();
    manifest.items.retain(|entry| {
        !(entry.timestamp == item.timestamp
            && entry.original_path == item.original_path.to_string_lossy()
            && entry.filename == item.filename)
    });

    if manifest.items.len() == original_len {
        eprintln!("Warning: restored item not found in manifest, may already be removed");
    } else {
        let new_data = serde_json::to_string_pretty(&manifest)?;
        fs::write(&manifest_path, new_data)?;
    }

    Ok(())
}

/// Safely delete file to rubbish bin
pub fn safe_delete_file(path: &Path, timestamp: &str, index: usize) -> Result<()> {
    let rubbish_bin = get_rubbish_bin_path()?;
    let timestamp_dir = rubbish_bin.join(timestamp);
    let index_dir = timestamp_dir.join(index.to_string());
    let info_file = index_dir.join(INFO_FILE);
    let rubbish_dir = index_dir.join(RUBBISH_SUBDIR);

    create_dir_all(&rubbish_dir)?;

    let abs_path = get_absolute_path(path)?;
    let original_dir = abs_path.parent().context("Cannot get parent directory")?;

    fs::write(&info_file, original_dir.to_string_lossy().as_bytes())?;

    let filename = path.file_name().context("Cannot get filename")?;
    let dest_path = rubbish_dir.join(filename);

    move_file_or_dir(path, &dest_path)?;

    // Incrementally add to cache
    let new_item = RestoreItem {
        id: 0, // will be reassigned in mutate_cache
        timestamp: timestamp.to_string(),
        original_path: original_dir.to_path_buf(),
        filename: filename.to_string_lossy().to_string(),
        is_archived: false,
        archive_file: None,
        index_dir: Some(index_dir.clone()), // store the exact path
    };

    mutate_cache(|items| {
        items.push(new_item.clone());
        items.sort_by(|a, b| b.timestamp.cmp(&a.timestamp));
        for (idx, it) in items.iter_mut().enumerate() {
            it.id = idx + 1;
        }
    });

    Ok(())
}

/// Check whether every item in an archive's manifest is marked as deleted.
fn is_archive_fully_deleted(archive_path: &Path) -> Result<bool> {
    let manifest_path = manifest_path(archive_path);
    if !manifest_path.exists() {
        // Without a manifest we conservatively assume the archive is not fully deleted.
        return Ok(false);
    }

    let data = fs::read_to_string(&manifest_path)?;
    let manifest: ArchiveManifest = serde_json::from_str(&data)?;

    if manifest.items.is_empty() {
        // An empty manifest (should not normally occur) implies fully deleted.
        return Ok(true);
    }

    Ok(manifest.items.iter().all(|entry| entry.deleted))
}

/// Remove archives whose every item has been logically deleted.
/// This allows space reclamation sooner than the normal CLEANUP_DAYS‑day expiry.
pub fn cleanup_fully_deleted_archives() -> Result<usize> {
    let rubbish_bin = get_rubbish_bin_path()?;
    let old_dir = rubbish_bin.join(OLD_DIR);

    if !old_dir.exists() {
        return Ok(0);
    }

    let mut deleted = 0;

    let regex = 
        regex::Regex::new(r"^\d{4}-\d{2}-\d{2}-\d{2}:\d{2}:\d{2}(?:_\d+)?\.tar\.gz$")
            .unwrap();

    for entry in fs::read_dir(&old_dir)? {
        let entry = entry?;
        let name = entry.file_name().to_string_lossy().to_string();

        if regex.is_match(&name) {
            let path = entry.path();
            if is_archive_fully_deleted(&path).unwrap_or(false) {
                if fs::remove_file(&path).is_ok() {
                    // Also delete the accompanying manifest file.
                    let manifest_path = manifest_path(&path);
                    let _ = fs::remove_file(manifest_path);
                    deleted += 1;
                }
            }
        }
    }

    Ok(deleted)
}

/// Process a batch of items with automatic parallelism
/// - Active (un-archived) items are processed fully in parallel
/// - Archived items are grouped by their archive file; different
///   archives are processed in parallel while items inside the same
///   archive are processed sequentially
fn process_batch<F>(items: &[&RestoreItem], op: F) -> Result<usize>
where
    F: Fn(&RestoreItem) -> Result<()> + Sync,
{
    let (active, archived): (Vec<&RestoreItem>, Vec<&RestoreItem>) =
        items.iter().copied().partition(|i| !i.is_archived);

    // Active items – fully parallel
    let active_success: usize = active
        .par_iter()
        .map(|item| if op(item).is_ok() { 1 } else { 0 })
        .sum();

    // Group archived items by their archive file
    let mut groups: HashMap<&Path, Vec<&RestoreItem>> = HashMap::new();
    for item in archived {
        if let Some(archive_path) = item.archive_file.as_deref() {
            groups.entry(archive_path).or_default().push(item);
        }
    }

    // Process each archive group in parallel, serial within a group
    let archived_success: usize = groups
        .par_iter()
        .map(|(_archive, group)| {
            group.iter().map(|item| if op(item).is_ok() { 1 } else { 0 }).sum::<usize>()
        })
        .sum();

    Ok(active_success + archived_success)
}

/// Process a batch of items in parallel while reporting progress.
/// - `op(item)` is called for each item; must be thread-safe (`Sync`).
/// - `progress(current, total)` is called each time an item is finished.
///   It may be called from multiple threads concurrently.
/// Process a batch of items in parallel, calling `progress(current, total)` after
/// every single item finishes. Returns the number of successful operations.
fn process_batch_with_progress<F, P>(
    items: &[&RestoreItem],
    op: F,
    progress: P,
) -> Result<usize>
where
    F: Fn(&RestoreItem) -> Result<()> + Sync,
    P: Fn(usize, usize) + Sync,
{
    use std::sync::atomic::{AtomicUsize, Ordering};

    let total = items.len();
    let done = AtomicUsize::new(0);

    // Helper that updates the counter and fires the progress callback
    let execute = |item: &RestoreItem| {
        let ok = op(item).is_ok();
        let current = done.fetch_add(1, Ordering::Relaxed) + 1;
        progress(current, total);
        ok
    };

    // Partition into active / archived items
    let (active, archived): (Vec<&RestoreItem>, Vec<&RestoreItem>) =
        items.iter().copied().partition(|i| !i.is_archived);

    // Active items – fully parallel
    let active_success: usize = active
        .par_iter()
        .map(|item| if execute(item) { 1 } else { 0 })
        .sum();

    // Group archived items by their archive file
    let mut groups: HashMap<&Path, Vec<&RestoreItem>> = HashMap::new();
    for item in archived {
        if let Some(archive_path) = item.archive_file.as_deref() {
            groups.entry(archive_path).or_default().push(item);
        }
    }

    // Process each archive group in parallel, serial within a group
    let archived_success: usize = groups
        .par_iter()
        .map(|(_archive, group)| {
            group
                .iter()
                .map(|item| if execute(item) { 1 } else { 0 })
                .sum::<usize>()
        })
        .sum();

    Ok(active_success + archived_success)
}

/// Batch restore with real-time progress callback
/// `progress(current, total)` is invoked after each item finishes.
pub fn restore_items_batch_with_progress<P: Fn(usize, usize) + Sync>(
    items: &[&RestoreItem],
    progress: P,
) -> Result<usize> {
    let success = 
        process_batch_with_progress(items,
            |item| restore_item_internal(item), progress)?;

    // Remove successfully restored items from cache
    mutate_cache(|cache_items| {
        for item in items {
            cache_items.retain(|i| !(
                i.timestamp == item.timestamp &&
                i.original_path == item.original_path &&
                i.filename == item.filename &&
                i.is_archived == item.is_archived
            ));
        }
    });
    Ok(success)
}

/// Batch permanent deletion with real-time progress callback.
/// `progress(current, total)` is invoked after each item finishes.
pub fn delete_items_batch_with_progress<P: Fn(usize, usize) + Sync>(
    items: &[&RestoreItem],
    progress: P,
) -> Result<usize> {
    let success = 
        process_batch_with_progress(items,
            |item| delete_item_internal(item), progress)?;

    mutate_cache(|cache_items| {
        for item in items {
            cache_items.retain(|i| !(
                i.timestamp == item.timestamp &&
                i.original_path == item.original_path &&
                i.filename == item.filename &&
                i.is_archived == item.is_archived
            ));
        }
    });
    Ok(success)
}
