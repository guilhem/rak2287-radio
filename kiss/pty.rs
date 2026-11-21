use std::ffi::{CString, OsStr};
use std::fs::DirBuilder;
use std::io::{self, Read, Write};
use std::os::fd::{AsFd, BorrowedFd, OwnedFd};
use std::os::unix::{ffi::OsStrExt, fs::DirBuilderExt};
use std::path::{Path, PathBuf};

use rustix::fs::{self, AtFlags, FileType, FlockOperation, Gid, Mode, OFlags, Stat};
use rustix::io::Errno;
use rustix::process::geteuid;
use rustix::{pty, termios};

pub(crate) struct PortConfig {
    pub(crate) path: PathBuf,
    pub(crate) group: Option<Gid>,
    pub(crate) derived_path: bool,
}

impl PortConfig {
    pub(crate) fn new(path: PathBuf, group: Option<Gid>, derived_path: bool) -> io::Result<Self> {
        let name = path
            .as_os_str()
            .as_bytes()
            .rsplit(|&byte| byte == b'/')
            .next();
        if !path.is_absolute() || matches!(name, None | Some(b"" | b"." | b"..")) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!(
                    "port path {} must be an absolute path ending in a filename",
                    path.display()
                ),
            ));
        }
        Ok(Self {
            path,
            group,
            derived_path,
        })
    }

    pub(crate) fn directory(&self) -> &Path {
        self.path.parent().expect("port path has a directory")
    }

    pub(crate) fn name(&self) -> &OsStr {
        self.path.file_name().expect("port path has a filename")
    }
}

pub(crate) struct RuntimeDirectory {
    fd: OwnedFd,
    attributes: Stat,
}

impl RuntimeDirectory {
    pub(crate) fn create(path: &Path) -> io::Result<()> {
        DirBuilder::new().recursive(true).mode(0o700).create(path)
    }

    pub(crate) fn open(path: &Path) -> io::Result<Self> {
        // A trailing slash or "/." can make O_NOFOLLOW apply after the symlink.
        let path: PathBuf = path.components().collect();
        let fd = fs::open(
            &path,
            OFlags::RDONLY | OFlags::DIRECTORY | OFlags::NOFOLLOW | OFlags::CLOEXEC,
            Mode::empty(),
        )?;
        let attributes = fs::fstat(&fd)?;
        if attributes.st_uid != geteuid().as_raw() || attributes.st_mode & 0o022 != 0 {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "use a directory owned by the modem user with write access limited to that user",
            ));
        }
        Ok(Self { fd, attributes })
    }

    pub(crate) fn same_inode(&self, other: &Self) -> bool {
        self.attributes.st_dev == other.attributes.st_dev
            && self.attributes.st_ino == other.attributes.st_ino
    }

    fn is_private(&self) -> bool {
        self.attributes.st_mode & 0o077 == 0
    }

    pub(crate) fn lock(&self, require_private: bool) -> io::Result<()> {
        if require_private && !self.is_private() {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "use a private runtime directory with mode 0700",
            ));
        }
        fs::flock(&self.fd, FlockOperation::NonBlockingLockExclusive)?;
        Ok(())
    }

    pub(crate) fn clear_stale_link(&self, port: &PortConfig) -> io::Result<()> {
        let name = port.name();
        let attributes = match fs::statat(&self.fd, name, AtFlags::SYMLINK_NOFOLLOW) {
            Ok(attributes) => attributes,
            Err(Errno::NOENT) => return Ok(()),
            Err(error) => return Err(error.into()),
        };
        if FileType::from_raw_mode(attributes.st_mode) == FileType::Symlink
            && attributes.st_uid == geteuid().as_raw()
        {
            let target = fs::readlinkat(&self.fd, name, Vec::new())?;
            let is_pty = target
                .to_bytes()
                .strip_prefix(b"/dev/pts/")
                .is_some_and(|number| !number.is_empty() && number.iter().all(u8::is_ascii_digit));
            // A stale default link can point to a PTY number reused after a crash.
            let reserved = port.derived_path && port.group.is_none() && self.is_private();
            if is_pty && (reserved || matches!(fs::stat(&target), Err(Errno::NOENT))) {
                fs::unlinkat(&self.fd, name, AtFlags::empty())?;
                return Ok(());
            }
        }
        Err(io::Error::new(
            io::ErrorKind::AlreadyExists,
            "the port path is occupied",
        ))
    }

    pub(crate) fn open_port<'a>(
        &'a self,
        name: &'a OsStr,
        group: Option<Gid>,
    ) -> io::Result<Pty<'a>> {
        let flags = OFlags::RDWR | OFlags::NOCTTY | OFlags::NONBLOCK | OFlags::CLOEXEC;
        let master = fs::open("/dev/ptmx", flags, Mode::empty())?;
        let slave_path = pty::ptsname(&master, Vec::new())?;
        if let Some(gid) = group {
            fs::chown(&slave_path, None, Some(gid)).map_err(|error| {
                let error = io::Error::from(error);
                io::Error::new(error.kind(), format!(
                    "cannot set the PTY group to GID {}: {error}. Check the modem user's group memberships.",
                    gid.as_raw()
                ))
            })?;
        }
        // Apply access permissions before the slave can be opened.
        let mode = if group.is_some() { 0o660 } else { 0o600 };
        fs::chmod(&slave_path, Mode::from_raw_mode(mode))?;
        pty::unlockpt(&master)?;
        {
            let slave = fs::open(&slave_path, flags | OFlags::NOFOLLOW, Mode::empty())?;
            let mut mode = termios::tcgetattr(&slave)?;
            mode.make_raw();
            mode.control_modes |= termios::ControlModes::CLOCAL;
            mode.set_speed(termios::speed::B115200)?;
            termios::tcsetattr(&slave, termios::OptionalActions::Now, &mode)?;
        }

        fs::symlinkat(&slave_path, &self.fd, name)?;
        Ok(Pty {
            master,
            directory: self,
            name,
            slave_path,
        })
    }
}

pub(crate) struct Pty<'a> {
    master: OwnedFd,
    directory: &'a RuntimeDirectory,
    name: &'a OsStr,
    slave_path: CString,
}

impl Pty<'_> {
    pub(crate) fn discard(&self) {
        let _ = termios::tcflush(&self.master, termios::QueueSelector::IOFlush);
    }
}

impl AsFd for Pty<'_> {
    fn as_fd(&self) -> BorrowedFd<'_> {
        self.master.as_fd()
    }
}

impl Read for Pty<'_> {
    fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> {
        Ok(rustix::io::read(&self.master, buffer)?)
    }
}

impl Write for Pty<'_> {
    fn write(&mut self, buffer: &[u8]) -> io::Result<usize> {
        Ok(rustix::io::write(&self.master, buffer)?)
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

impl Drop for Pty<'_> {
    fn drop(&mut self) {
        let directory = &self.directory.fd;
        if fs::readlinkat(directory, self.name, Vec::new())
            .is_ok_and(|target| target == self.slave_path)
        {
            let _ = fs::unlinkat(directory, self.name, AtFlags::empty());
        }
    }
}
