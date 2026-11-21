mod codec;
mod modem;
mod pty;
mod radio;

use std::{
    env,
    ffi::CString,
    fs::File,
    io::{self, Read},
    mem::{MaybeUninit, size_of},
    os::fd::FromRawFd,
    path::PathBuf,
    process::ExitCode,
};

use clap::{Parser, ValueEnum};
use rustix::{
    fs::{Gid, Mode},
    process::umask,
};

use crate::pty::PortConfig;

#[derive(Parser)]
#[command(
    version,
    about,
    after_help = concat!(
        "Receive with explicit settings\n",
        "  sx130x-kiss --spi-device /dev/spidev0.0 \\\n",
        "    --runtime-dir \"$HOME/.local/run/sx130x-kiss\" --mode receive-only"
    )
)]
struct Options {
    /// SPI device for the concentrator.
    #[arg(long, value_name = "PATH", default_value = "/dev/spidev0.0")]
    spi_device: PathBuf,

    /// Absolute directory for ports without an explicit path.
    ///
    /// Defaults to $XDG_RUNTIME_DIR/sx130x-kiss or $HOME/.local/run/sx130x-kiss.
    #[arg(long, value_name = "DIR")]
    runtime_dir: Option<PathBuf>,

    /// Absolute rf0 link path. Pre-create its parent directory.
    #[arg(long, value_name = "PATH")]
    rf0_path: Option<PathBuf>,

    /// Absolute rf1 link path. Pre-create its parent directory.
    #[arg(long, value_name = "PATH")]
    rf1_path: Option<PathBuf>,

    /// Unix group whose members may access rf0.
    #[arg(long, value_name = "GROUP")]
    rf0_group: Option<String>,

    /// Unix group whose members may access rf1.
    #[arg(long, value_name = "GROUP")]
    rf1_group: Option<String>,

    /// Choose whether KISS clients can transmit through RF0.
    #[arg(long, value_enum, default_value_t = OperatingMode::ReceiveOnly)]
    mode: OperatingMode,
}

#[derive(Clone, Copy, PartialEq, Eq, ValueEnum)]
enum OperatingMode {
    /// Receive packets on both ports.
    ReceiveOnly,
    /// Let KISS clients receive packets and send through RF0.
    ReceiveAndTransmit,
}

fn default_runtime_directory() -> io::Result<PathBuf> {
    env::var_os("XDG_RUNTIME_DIR")
        .map(PathBuf::from)
        .filter(|path| path.is_absolute())
        .or_else(|| {
            env::var_os("HOME")
                .map(PathBuf::from)
                .filter(|path| path.is_absolute())
                .map(|home| home.join(".local/run"))
        })
        .map(|base| base.join(env!("CARGO_PKG_NAME")))
        .ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::InvalidInput,
                "set XDG_RUNTIME_DIR or HOME to an absolute path, or pass --runtime-dir with an absolute path",
            )
        })
}

fn resolve_group(port: &str, name: &str) -> io::Result<Gid> {
    let c_name = CString::new(name).map_err(io::Error::other)?;
    // SAFETY: Startup is single threaded and the group ID is copied before the next lookup.
    let gid = unsafe {
        libc::getgrnam(c_name.as_ptr())
            .as_ref()
            .map(|entry| entry.gr_gid)
    };
    gid.filter(|gid| *gid != libc::gid_t::MAX)
        .map(Gid::from_raw)
        .ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("cannot resolve Unix group {name:?} for --{port}-group"),
            )
        })
}

fn shutdown_signals() -> io::Result<File> {
    // SAFETY: The mask has storage for a sigset_t and is initialized by
    // sigemptyset before use. Only this function takes ownership of the new fd.
    unsafe {
        let mut mask = MaybeUninit::<libc::sigset_t>::zeroed();
        if libc::sigemptyset(mask.as_mut_ptr()) != 0 {
            return Err(io::Error::last_os_error());
        }
        let mut mask = mask.assume_init();
        if libc::sigaddset(&mut mask, libc::SIGINT) != 0
            || libc::sigaddset(&mut mask, libc::SIGTERM) != 0
        {
            return Err(io::Error::last_os_error());
        }
        // TX workers inherit this mask, so shutdown cannot interrupt the handoff.
        let error = libc::pthread_sigmask(libc::SIG_BLOCK, &mask, std::ptr::null_mut());
        if error != 0 {
            return Err(io::Error::from_raw_os_error(error));
        }
        let fd = libc::signalfd(-1, &mask, libc::SFD_NONBLOCK | libc::SFD_CLOEXEC);
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(File::from_raw_fd(fd))
    }
}

fn run(options: Options) -> io::Result<()> {
    umask(Mode::RWXG | Mode::RWXO);
    let runtime_dir = if options.rf0_path.is_some() && options.rf1_path.is_some() {
        None
    } else {
        let directory = options
            .runtime_dir
            .map_or_else(default_runtime_directory, Ok)?;
        if !directory.is_absolute() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!(
                    "the runtime directory {} must be an absolute path",
                    directory.display()
                ),
            ));
        }
        Some(directory)
    };
    let config = |port, path: Option<PathBuf>, group: Option<String>| {
        let derived_path = path.is_none();
        let path = path.unwrap_or_else(|| {
            runtime_dir
                .as_ref()
                .expect("a default port needs a runtime directory")
                .join(port)
        });
        let group = group
            .as_deref()
            .map(|name| resolve_group(port, name))
            .transpose()?;
        PortConfig::new(path, group, derived_path)
    };
    let ports = [
        config("rf0", options.rf0_path, options.rf0_group)?,
        config("rf1", options.rf1_path, options.rf1_group)?,
    ];
    let mut signals = shutdown_signals()?;
    let mut signal = [0; size_of::<libc::signalfd_siginfo>()];
    modem::run(
        &ports,
        &options.spi_device,
        options.mode == OperatingMode::ReceiveAndTransmit,
        || match signals.read(&mut signal) {
            Ok(_) => Ok(true),
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => Ok(false),
            Err(error) => Err(error),
        },
    )
}

fn main() -> ExitCode {
    match run(Options::parse()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("{}: {error}", env!("CARGO_PKG_NAME"));
            ExitCode::FAILURE
        }
    }
}
