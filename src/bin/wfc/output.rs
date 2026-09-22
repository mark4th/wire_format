use std::ffi::OsString;
use std::fs::{self, OpenOptions};
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

pub struct OutputPaths {
    pub header: PathBuf,
    pub binary: PathBuf,
}

impl OutputPaths {
    pub fn new(stem: &Path) -> io::Result<Self> {
        let header = append_suffix(stem, ".h");
        let binary = append_suffix(stem, ".wfb");

        Ok(Self { header, binary })
    }
}

pub fn write_pair(paths: &OutputPaths, header: &str, binary: &[u8]) -> io::Result<()> {
    if let Some(parent) = paths
        .header
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
    {
        fs::create_dir_all(parent)?;
    }

    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |duration| duration.as_nanos());
    let suffix = format!(".wfc-{}-{nonce}", std::process::id());
    let header_temp = append_suffix(&paths.header, &format!("{suffix}.tmp"));
    let binary_temp = append_suffix(&paths.binary, &format!("{suffix}.tmp"));
    let header_backup = append_suffix(&paths.header, &format!("{suffix}.old"));
    let binary_backup = append_suffix(&paths.binary, &format!("{suffix}.old"));

    write_new(&header_temp, header.as_bytes())?;
    if let Err(error) = write_new(&binary_temp, binary) {
        remove_if_present(&header_temp);
        return Err(error);
    }

    let had_header = match move_existing(&paths.header, &header_backup) {
        Ok(value) => value,
        Err(error) => {
            remove_if_present(&header_temp);
            remove_if_present(&binary_temp);
            return Err(error);
        }
    };
    let had_binary = match move_existing(&paths.binary, &binary_backup) {
        Ok(value) => value,
        Err(error) => {
            restore(&paths.header, &header_backup, had_header);
            remove_if_present(&header_temp);
            remove_if_present(&binary_temp);
            return Err(error);
        }
    };

    if let Err(error) = fs::rename(&header_temp, &paths.header) {
        restore(&paths.header, &header_backup, had_header);
        restore(&paths.binary, &binary_backup, had_binary);
        remove_if_present(&binary_temp);
        return Err(error);
    }
    if let Err(error) = fs::rename(&binary_temp, &paths.binary) {
        remove_if_present(&paths.header);
        restore(&paths.header, &header_backup, had_header);
        restore(&paths.binary, &binary_backup, had_binary);
        remove_if_present(&binary_temp);
        return Err(error);
    }

    remove_if_present(&header_backup);
    remove_if_present(&binary_backup);
    Ok(())
}

fn append_suffix(path: &Path, suffix: &str) -> PathBuf {
    let mut value: OsString = path.as_os_str().to_owned();
    value.push(suffix);
    value.into()
}

fn write_new(path: &Path, contents: &[u8]) -> io::Result<()> {
    let mut file = OpenOptions::new().write(true).create_new(true).open(path)?;
    file.write_all(contents)?;
    file.sync_all()
}

fn move_existing(path: &Path, backup: &Path) -> io::Result<bool> {
    match fs::symlink_metadata(path) {
        Ok(metadata) if !metadata.file_type().is_file() => Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!(
                "refusing to replace non-regular output `{}`",
                path.display()
            ),
        )),
        Ok(_) => fs::rename(path, backup).map(|()| true),
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(false),
        Err(error) => Err(error),
    }
}

fn restore(path: &Path, backup: &Path, existed: bool) {
    if existed {
        let _ = fs::rename(backup, path);
    }
}

fn remove_if_present(path: &Path) {
    match fs::remove_file(path) {
        Ok(()) => {}
        Err(error) if error.kind() == io::ErrorKind::NotFound => {}
        Err(_) => {}
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn writes_and_replaces_both_outputs() {
        let directory = std::env::temp_dir().join(format!(
            "wfc-output-test-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let paths = OutputPaths::new(&directory.join("sample")).unwrap();

        write_pair(&paths, "old header", b"old binary").unwrap();
        write_pair(&paths, "new header", b"new binary").unwrap();
        assert_eq!(fs::read_to_string(&paths.header).unwrap(), "new header");
        assert_eq!(fs::read(&paths.binary).unwrap(), b"new binary");

        fs::remove_dir_all(directory).unwrap();
    }
}
