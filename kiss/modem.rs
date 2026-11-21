use std::env;
use std::io::{self, Read, Write};
use std::ops::Range;
use std::os::linux::net::SocketAddrExt;
use std::os::unix::{
    ffi::OsStrExt,
    net::{SocketAddr, UnixDatagram},
};
use std::path::Path;
use std::thread::{self, ScopedJoinHandle};
use std::time::{Duration, Instant};

use rustix::event::{PollFd, PollFlags, Timespec, poll};

use crate::codec::{Decoder, encode, encoded_max};
use crate::pty::{PortConfig, Pty, RuntimeDirectory};
use crate::radio::{
    DEFAULT_POWER_DBM, Profile, Radio, TX_TIMEOUT_MAX_MS, TxOutcome, airtime_ms, power_supported,
};

const PORTS: usize = 2;
const MAX_PACKET: usize = 255;
const MAX_OUTPUT: usize = 4096;
const MAX_CONTROL_PAYLOAD: usize = 30;
const REPLY_ROOM: usize = encoded_max(MAX_CONTROL_PAYLOAD) + encoded_max(2);
const TX_WAIT: Duration = Duration::from_secs(2);
const TX_MARGIN_MS: u32 = 500;
const POLL_INTERVAL: Timespec = Timespec {
    tv_sec: 0,
    tv_nsec: 2_000_000,
};

const DATA: u8 = 0x00;
const TX_DELAY: u8 = 0x01;
const SET_HARDWARE: u8 = 0x06;
const SET_RADIO: u8 = 0x09;
const SET_TX_POWER: u8 = 0x0a;
const GET_RADIO: u8 = 0x0b;
const GET_TX_POWER: u8 = 0x0c;
const IS_CHANNEL_BUSY: u8 = 0x0e;
const GET_AIRTIME: u8 = 0x0f;
const GET_VERSION: u8 = 0x11;
const GET_STATS: u8 = 0x12;
const GET_DEVICE_NAME: u8 = 0x16;
const PING: u8 = 0x17;
const SET_SIGNAL_REPORT: u8 = 0x19;
const GET_SIGNAL_REPORT: u8 = 0x1a;
const SET_SYNC_WORD: u8 = 0x1b;
const SET_PREAMBLE: u8 = 0x1c;
const GET_PHY_EXTRA: u8 = 0x1d;
const OK: u8 = 0xf0;
const ERROR: u8 = 0xf1;
const TX_DONE: u8 = 0xf8;
const RX_META: u8 = 0xf9;

const BAD_LENGTH: u8 = 1;
const BAD_PARAMETER: u8 = 2;
const UNAVAILABLE: u8 = 3;
const UNKNOWN_COMMAND: u8 = 5;
const TX_BUSY: u8 = 7;

struct Pending {
    payload: Vec<u8>,
    power_dbm: i8,
    earliest: Instant,
    deadline: Instant,
}

struct Client {
    profile: Profile,
    configured: bool,
    signal_report: bool,
    power_dbm: i8,
    preamble_override: u16,
    tx_delay: Duration,
    pending: Option<Pending>,
    received: u32,
    sent: u32,
    tx_errors: u32,
    output: Vec<u8>,
    written: usize,
}

impl Client {
    fn new(profile: Profile) -> Self {
        Self {
            profile,
            configured: false,
            signal_report: true,
            power_dbm: DEFAULT_POWER_DBM,
            preamble_override: 0,
            tx_delay: Duration::ZERO,
            pending: None,
            received: 0,
            sent: 0,
            tx_errors: 0,
            output: Vec::with_capacity(MAX_OUTPUT),
            written: 0,
        }
    }

    fn output_room(&self) -> usize {
        MAX_OUTPUT - (self.output.len() - self.written)
    }

    fn clear_output(&mut self) {
        self.output.clear();
        self.written = 0;
    }

    fn queue_frame(&mut self, frame_type: u8, data: &[u8]) {
        let capacity = encoded_max(data.len());
        assert!(capacity <= self.output_room());
        if self.output.len() + capacity > MAX_OUTPUT {
            self.output.copy_within(self.written.., 0);
            self.output.truncate(self.output.len() - self.written);
            self.written = 0;
        }
        encode(&mut self.output, frame_type, data);
    }

    fn reply(&mut self, opcode: u8, data: &[u8]) {
        let mut payload = [0; MAX_CONTROL_PAYLOAD];
        payload[0] = opcode;
        payload[1..1 + data.len()].copy_from_slice(data);
        self.queue_frame(SET_HARDWARE, &payload[..1 + data.len()]);
    }

    fn error(&mut self, code: u8) {
        self.reply(ERROR, &[code]);
    }

    fn tx_done(&mut self, success: bool) {
        self.reply(TX_DONE, &[u8::from(success)]);
        if success {
            self.sent = self.sent.wrapping_add(1);
        } else {
            self.tx_errors = self.tx_errors.wrapping_add(1);
        }
    }

    fn finish_pending(&mut self, success: bool) {
        if self.pending.take().is_some() {
            self.tx_done(success);
        }
    }

    fn handle_frame(
        &mut self,
        rf: usize,
        access: &mut RadioAccess<'_>,
        frame: &[u8],
        enable_transmit: bool,
        now: Instant,
    ) {
        let Some((&command, data)) = frame.split_first() else {
            return;
        };
        match command {
            DATA => {
                if data.is_empty() || data.len() > MAX_PACKET {
                    self.error(BAD_LENGTH);
                } else if self.pending.is_some() || access.transmitting(rf) {
                    self.error(TX_BUSY);
                } else if !enable_transmit || !self.configured {
                    self.tx_done(false);
                } else {
                    let earliest = now + self.tx_delay;
                    self.pending = Some(Pending {
                        payload: data.to_vec(),
                        power_dbm: self.power_dbm,
                        earliest,
                        deadline: earliest + TX_WAIT,
                    });
                }
            }
            TX_DELAY if data.len() == 1 => {
                self.tx_delay = Duration::from_millis(u64::from(data[0]) * 10);
            }
            SET_HARDWARE => self.hardware_command(rf, access, data),
            _ => {}
        }
    }

    fn hardware_command(&mut self, rf: usize, access: &mut RadioAccess<'_>, command: &[u8]) {
        let Some((&opcode, data)) = command.split_first() else {
            self.error(BAD_LENGTH);
            return;
        };
        let expected_len = match opcode {
            SET_RADIO => 10,
            SET_PREAMBLE => 2,
            SET_SYNC_WORD | SET_TX_POWER | SET_SIGNAL_REPORT | GET_AIRTIME => 1,
            _ => 0,
        };
        if data.len() != expected_len {
            self.error(BAD_LENGTH);
            return;
        }

        let response = opcode | 0x80;
        let profile = self.profile;
        match opcode {
            SET_RADIO | SET_SYNC_WORD | SET_PREAMBLE => {
                self.apply_profile(rf, access, opcode, data);
            }
            SET_TX_POWER => {
                let power_dbm = data[0] as i8;
                if power_supported(power_dbm) {
                    self.power_dbm = power_dbm;
                    self.reply(OK, &[]);
                } else {
                    self.error(BAD_PARAMETER);
                }
            }
            SET_SIGNAL_REPORT => {
                self.signal_report = data[0] != 0;
                self.reply(OK, &[]);
            }
            GET_RADIO => {
                let mut config = [0; 10];
                config[..4].copy_from_slice(&profile.frequency_hz.to_le_bytes());
                config[4..8].copy_from_slice(&profile.bandwidth_hz.to_le_bytes());
                config[8] = profile.spreading_factor;
                config[9] = profile.coding_rate;
                self.reply(response, &config);
            }
            GET_PHY_EXTRA => {
                let [lo, hi] = profile.preamble_symbols.to_le_bytes();
                self.reply(response, &[profile.sync_word, lo, hi]);
            }
            GET_TX_POWER => self.reply(response, &[self.power_dbm as u8]),
            GET_SIGNAL_REPORT => self.reply(response, &[u8::from(self.signal_report)]),
            GET_AIRTIME => self.reply(response, &airtime_ms(&profile, data[0]).to_le_bytes()),
            IS_CHANNEL_BUSY => match access {
                RadioAccess::Sending(_) => self.reply(response, &[1]),
                RadioAccess::Listening(radio) => match receive_busy(radio) {
                    Ok(busy) => self.reply(response, &[u8::from(busy)]),
                    Err(_) => self.error(UNAVAILABLE),
                },
            },
            GET_VERSION => self.reply(response, &[2, 0]),
            GET_DEVICE_NAME => self.reply(response, [b"SX130x RF0", b"SX130x RF1"][rf]),
            GET_STATS => {
                let mut stats = [0; 12];
                for (value, field) in [self.received, self.sent, self.tx_errors]
                    .into_iter()
                    .zip(stats.chunks_exact_mut(4))
                {
                    field.copy_from_slice(&value.to_le_bytes());
                }
                self.reply(response, &stats);
            }
            PING => self.reply(response, &[]),
            1..=GET_PHY_EXTRA => self.error(UNAVAILABLE),
            _ => self.error(UNKNOWN_COMMAND),
        }
    }

    fn apply_profile(&mut self, rf: usize, access: &mut RadioAccess<'_>, opcode: u8, data: &[u8]) {
        let RadioAccess::Listening(radio) = access else {
            self.error(TX_BUSY);
            return;
        };
        if self.pending.is_some() {
            self.error(TX_BUSY);
            return;
        }

        let mut profile = self.profile;
        let mut preamble = self.preamble_override;
        match opcode {
            SET_RADIO => {
                profile.frequency_hz = u32::from_le_bytes(data[..4].try_into().unwrap());
                profile.bandwidth_hz = u32::from_le_bytes(data[4..8].try_into().unwrap());
                profile.spreading_factor = data[8];
                profile.coding_rate = data[9];
            }
            SET_SYNC_WORD => profile.sync_word = data[0],
            SET_PREAMBLE => preamble = u16::from_le_bytes(data.try_into().unwrap()),
            _ => unreachable!(),
        }
        profile.preamble_symbols = match preamble {
            0 if profile.spreading_factor <= 8 => 32,
            0 => 16,
            explicit => explicit,
        };
        if let Err(error) = radio.reconfigure(rf, &profile) {
            let code = match error.raw_os_error() {
                Some(libc::EINVAL) => BAD_PARAMETER,
                Some(libc::EAGAIN | libc::EBUSY) => TX_BUSY,
                _ => UNAVAILABLE,
            };
            self.error(code);
            return;
        }
        self.profile = profile;
        self.preamble_override = preamble;
        self.configured |= opcode == SET_RADIO;
        self.reply(OK, &[]);
    }
}

struct Port<'dir> {
    tty: Pty<'dir>,
    connected: bool,
    decoder: Decoder,
    input: [u8; 256],
    unread: Range<usize>,
    client: Client,
}

impl<'dir> Port<'dir> {
    fn new(tty: Pty<'dir>, profile: Profile) -> Self {
        Self {
            tty,
            connected: false,
            decoder: Decoder::default(),
            input: [0; 256],
            unread: 0..0,
            client: Client::new(profile),
        }
    }

    fn probe_connection(&mut self) -> io::Result<()> {
        if !self.connected {
            let mut probe = [PollFd::new(&self.tty, PollFlags::empty())];
            // Exclude disconnected PTYs from the timed poll because HUP stays ready.
            match poll(&mut probe, Some(&Timespec::default())) {
                Ok(ready) => self.connected = ready == 0,
                Err(rustix::io::Errno::INTR) => {}
                Err(error) => return Err(error.into()),
            }
        }
        Ok(())
    }

    fn poll_fd(&self) -> PollFd<'_> {
        let mut events = PollFlags::empty();
        if self.unread.is_empty() && self.client.output_room() >= REPLY_ROOM {
            events |= PollFlags::IN;
        }
        if self.client.written < self.client.output.len() {
            events |= PollFlags::OUT;
        }
        PollFd::new(&self.tty, events)
    }

    fn disconnect(&mut self) {
        self.connected = false;
        self.decoder = Decoder::default();
        self.unread = 0..0;
        self.client.clear_output();
        self.client.pending = None;
        self.tty.discard();
    }

    fn flush(&mut self) -> io::Result<()> {
        let queued = &self.client.output[self.client.written..];
        if queued.is_empty() {
            return Ok(());
        }
        match self.tty.write(queued) {
            Ok(count) => {
                self.client.written += count;
                if self.client.written == self.client.output.len() {
                    self.client.clear_output();
                }
            }
            Err(error) if error.raw_os_error() == Some(libc::EIO) => self.disconnect(),
            Err(error) if temporary_io_error(&error) => {}
            Err(error) => return Err(error),
        }
        Ok(())
    }

    fn service(
        &mut self,
        rf: usize,
        events: PollFlags,
        access: &mut RadioAccess<'_>,
        enable_transmit: bool,
        now: Instant,
    ) -> io::Result<()> {
        if events.intersects(PollFlags::HUP | PollFlags::ERR) {
            self.disconnect();
            return Ok(());
        }
        if events.contains(PollFlags::NVAL) {
            return Err(io::Error::from_raw_os_error(libc::EBADF));
        }
        if events.contains(PollFlags::OUT) {
            self.flush()?;
        }
        if !self.connected {
            return Ok(());
        }
        if events.contains(PollFlags::IN) {
            match self.tty.read(&mut self.input) {
                Ok(0) => self.disconnect(),
                Ok(length) => self.unread = 0..length,
                Err(error) if error.raw_os_error() == Some(libc::EIO) => self.disconnect(),
                Err(error) if temporary_io_error(&error) => {}
                Err(error) => return Err(error),
            }
        }
        while self.client.output_room() >= REPLY_ROOM {
            let Some(index) = self.unread.next() else {
                break;
            };
            if let Some(frame) = self.decoder.push(self.input[index]) {
                self.client
                    .handle_frame(rf, access, frame, enable_transmit, now);
            }
        }
        Ok(())
    }

    fn forward_rx(&mut self, rf: usize, radio: &mut Radio) -> io::Result<()> {
        let Some(packet) = radio.next(rf)? else {
            return Ok(());
        };
        let client = &mut self.client;
        if !self.connected || !client.configured {
            return Ok(());
        }
        client.received = client.received.wrapping_add(1);
        let metadata = if client.signal_report {
            encoded_max(3)
        } else {
            0
        };
        // Keep room for both a control response and the completion of a queued send.
        if client.output_room() >= encoded_max(packet.payload().len()) + metadata + REPLY_ROOM {
            client.queue_frame(DATA, packet.payload());
            if client.signal_report {
                let metrics = [
                    signal_byte(packet.snr_db * 4.0),
                    signal_byte(packet.rssi_dbm),
                ];
                client.reply(RX_META, &metrics);
            }
        }
        Ok(())
    }
}

fn temporary_io_error(error: &io::Error) -> bool {
    matches!(
        error.kind(),
        io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted
    )
}

fn signal_byte(signal: f32) -> u8 {
    signal.min(127.0).round() as i8 as u8
}

enum RadioAccess<'radio> {
    Listening(&'radio mut Radio),
    Sending(usize),
}

impl RadioAccess<'_> {
    fn transmitting(&self, rf: usize) -> bool {
        matches!(self, Self::Sending(sender) if *sender == rf)
    }
}

fn receive_busy(radio: &mut Radio) -> io::Result<bool> {
    for rf in 0..PORTS {
        if radio.activity(rf)? {
            return Ok(true);
        }
    }
    Ok(false)
}

struct Modem<'dir> {
    ports: [Port<'dir>; PORTS],
    enable_transmit: bool,
    turn: usize,
}

impl Modem<'_> {
    fn poll_ports(&mut self, mut access: RadioAccess<'_>) -> io::Result<()> {
        for port in &mut self.ports {
            port.probe_connection()?;
        }
        let mut events = [PollFlags::empty(); PORTS];
        {
            let mut descriptors = self.ports.each_ref().map(Port::poll_fd);
            let mut active_rf = [0; PORTS];
            let mut active_count = 0;
            for (rf, port) in self.ports.iter().enumerate() {
                if port.connected {
                    descriptors.swap(active_count, rf);
                    active_rf[active_count] = rf;
                    active_count += 1;
                }
            }
            match poll(&mut descriptors[..active_count], Some(&POLL_INTERVAL)) {
                Ok(_) => {}
                Err(rustix::io::Errno::INTR) => return Ok(()),
                Err(error) => return Err(error.into()),
            }
            for (fd, &rf) in descriptors[..active_count].iter().zip(&active_rf) {
                events[rf] = fd.revents();
            }
        }
        let now = Instant::now();
        for (rf, (port, flags)) in self.ports.iter_mut().zip(events).enumerate() {
            port.service(rf, flags, &mut access, self.enable_transmit, now)?;
        }
        Ok(())
    }

    fn expire_pending(&mut self, transmitting: Option<usize>, now: Instant) {
        for (rf, port) in self.ports.iter_mut().enumerate() {
            if transmitting != Some(rf)
                && port
                    .client
                    .pending
                    .as_ref()
                    .is_some_and(|tx| now >= tx.deadline)
            {
                port.client.finish_pending(false);
            }
        }
    }

    fn next_tx(&mut self, radio: &mut Radio, now: Instant) -> io::Result<Option<(usize, u32)>> {
        for offset in 0..PORTS {
            let rf = (self.turn + offset) % PORTS;
            let client = &mut self.ports[rf].client;
            let Some(pending) = client.pending.as_ref() else {
                continue;
            };
            if now < pending.earliest {
                continue;
            }
            let airtime = airtime_ms(&client.profile, pending.payload.len() as u8);
            if airtime == 0 || airtime > TX_TIMEOUT_MAX_MS - TX_MARGIN_MS {
                client.finish_pending(false);
                continue;
            }
            if receive_busy(radio)? {
                return Ok(None);
            }
            return Ok(Some((
                rf,
                (2 * airtime + TX_MARGIN_MS).min(TX_TIMEOUT_MAX_MS),
            )));
        }
        Ok(None)
    }

    fn send(
        &mut self,
        radio: &mut Radio,
        rf: usize,
        timeout_ms: u32,
        shutdown: &mut impl FnMut() -> io::Result<bool>,
    ) -> io::Result<bool> {
        let client = &self.ports[rf].client;
        let pending = client.pending.as_ref().expect("scheduled transmission");
        let profile = client.profile;
        let payload = pending.payload.clone();
        let power_dbm = pending.power_dbm;
        thread::scope(|scope| {
            // During TX the PTY loop has no borrow of the board, so it can only service KISS state.
            let worker = thread::Builder::new().spawn_scoped(scope, move || {
                radio.send(&profile, power_dbm, &payload, timeout_ms)
            });
            let worker = match worker {
                Ok(worker) => worker,
                Err(error) => {
                    eprintln!("Start RF0 transmission: {error}");
                    self.ports[rf].client.finish_pending(false);
                    return Ok(false);
                }
            };
            let stopping = self.poll_while_sending(rf, &worker, shutdown);
            let outcome = worker
                .join()
                .map_err(|_| io::Error::other("RF0 transmit worker panicked"));
            let stopping = stopping?;
            let outcome = outcome?;
            if !stopping {
                self.complete_tx(rf, outcome)?;
            }
            Ok(stopping)
        })
    }

    fn poll_while_sending(
        &mut self,
        rf: usize,
        worker: &ScopedJoinHandle<'_, TxOutcome>,
        shutdown: &mut impl FnMut() -> io::Result<bool>,
    ) -> io::Result<bool> {
        while !shutdown()? {
            self.poll_ports(RadioAccess::Sending(rf))?;
            if worker.is_finished() {
                return Ok(false);
            }
            self.expire_pending(Some(rf), Instant::now());
        }
        Ok(true)
    }

    fn complete_tx(&mut self, rf: usize, outcome: TxOutcome) -> io::Result<()> {
        if !outcome.tx_started
            && outcome
                .result
                .as_ref()
                .is_err_and(|error| error.raw_os_error() == Some(libc::EAGAIN))
        {
            return Ok(());
        }
        let port = &mut self.ports[rf];
        // A detected hangup clears the pending slot. The replacement client owns no completion.
        port.client
            .finish_pending(outcome.result.is_ok() && outcome.receivers_rearmed);
        port.flush()?;
        self.turn = (rf + 1) % PORTS;
        if !outcome.receivers_rearmed {
            outcome.result?;
            return Err(io::Error::other("RF0 send did not restore both receivers"));
        }
        Ok(())
    }

    fn run(
        &mut self,
        radio: &mut Radio,
        mut shutdown: impl FnMut() -> io::Result<bool>,
    ) -> io::Result<()> {
        while !shutdown()? {
            self.poll_ports(RadioAccess::Listening(radio))?;
            for (rf, port) in self.ports.iter_mut().enumerate() {
                port.forward_rx(rf, radio)?;
            }
            let now = Instant::now();
            self.expire_pending(None, now);
            if shutdown()? {
                break;
            }
            if let Some((rf, timeout_ms)) = self.next_tx(radio, now)? {
                if shutdown()? || self.send(radio, rf, timeout_ms, &mut shutdown)? {
                    break;
                }
            }
        }
        Ok(())
    }
}

fn runtime_error(path: &Path, error: io::Error) -> io::Error {
    let hint = match error.raw_os_error() {
        Some(libc::EWOULDBLOCK) => " Another sx130x-kiss process may be using this directory.",
        Some(libc::ENOENT) => " Create the directory as the modem user.",
        Some(libc::EACCES) => " Check the modem user's directory permissions.",
        Some(libc::ELOOP | libc::ENOTDIR | libc::EEXIST) => " Supply the real path of a directory.",
        _ => "",
    };
    io::Error::new(
        error.kind(),
        format!(
            "cannot use runtime directory {}: {error}.{hint}",
            path.display()
        ),
    )
}

fn port_error(path: &Path, error: io::Error) -> io::Error {
    io::Error::new(
        error.kind(),
        format!("cannot use port {}: {error}", path.display()),
    )
}

fn notify_ready() -> io::Result<()> {
    let Some(socket) = env::var_os("NOTIFY_SOCKET") else {
        return Ok(());
    };
    let address = match socket.as_bytes() {
        [b'@', name @ ..] => SocketAddr::from_abstract_name(name)?,
        [b'/', ..] => SocketAddr::from_pathname(Path::new(&socket))?,
        _ => {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "NOTIFY_SOCKET must be an absolute path or an abstract socket name",
            ));
        }
    };
    UnixDatagram::unbound()?.send_to_addr(b"READY=1", &address)?;
    Ok(())
}

pub(crate) fn run(
    ports: &[PortConfig; PORTS],
    spi_device: &Path,
    enable_transmit: bool,
    shutdown: impl FnMut() -> io::Result<bool>,
) -> io::Result<()> {
    if let Some(port) = ports.iter().find(|port| port.derived_path) {
        let directory = port.directory();
        RuntimeDirectory::create(directory).map_err(|error| runtime_error(directory, error))?;
    }
    let [rf0, rf1] = ports;
    let open_directory = |port: &PortConfig| {
        let directory = port.directory();
        RuntimeDirectory::open(directory).map_err(|error| runtime_error(directory, error))
    };
    let first_directory = open_directory(rf0)?;
    let second_directory = open_directory(rf1)?;
    let same_directory = first_directory.same_inode(&second_directory);
    if same_directory && rf0.name() == rf1.name() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!(
                "rf0 and rf1 require distinct port paths ({} and {})",
                rf0.path.display(),
                rf1.path.display(),
            ),
        ));
    }
    let require_private = |port: &PortConfig| port.derived_path && port.group.is_none();
    let first_private = if same_directory {
        (rf0.derived_path || rf1.derived_path) && rf0.group.is_none() && rf1.group.is_none()
    } else {
        require_private(rf0)
    };
    first_directory
        .lock(first_private)
        .map_err(|error| runtime_error(rf0.directory(), error))?;
    let rf1_directory = if same_directory {
        &first_directory
    } else {
        second_directory
            .lock(require_private(rf1))
            .map_err(|error| runtime_error(rf1.directory(), error))?;
        &second_directory
    };
    // Clear both links before a new PTY can reuse an old slave number.
    for (port, directory) in ports.iter().zip([&first_directory, rf1_directory]) {
        directory
            .clear_stale_link(port)
            .map_err(|error| port_error(&port.path, error))?;
    }
    let profiles = [Profile::default(); PORTS];
    let mut modem = Modem {
        ports: [
            Port::new(
                first_directory
                    .open_port(rf0.name(), rf0.group)
                    .map_err(|error| port_error(&rf0.path, error))?,
                profiles[0],
            ),
            Port::new(
                rf1_directory
                    .open_port(rf1.name(), rf1.group)
                    .map_err(|error| port_error(&rf1.path, error))?,
                profiles[1],
            ),
        ],
        enable_transmit,
        turn: 0,
    };
    let mut radio = Radio::open(spi_device, &profiles).map_err(|error| {
        let hint = match error.raw_os_error() {
            Some(libc::EACCES | libc::EPERM) => {
                " Give the modem user read and write access to the SPI device."
            }
            Some(libc::EBUSY) => {
                " Stop other concentrator software and run the board reset helper."
            }
            Some(libc::ENOENT | libc::ENODEV) => {
                " Check the device path, SPI setup, and concentrator connection."
            }
            _ => "",
        };
        io::Error::new(
            error.kind(),
            format!(
                "cannot open SPI device {}: {error}.{hint}",
                spi_device.display()
            ),
        )
    })?;
    notify_ready()
        .map_err(|error| io::Error::new(error.kind(), format!("cannot notify systemd: {error}")))?;
    {
        let mut stdout = io::stdout().lock();
        stdout.write_all(b"SX130X_KISS_READY")?;
        for (name, port) in ["rf0", "rf1"].into_iter().zip(ports) {
            write!(stdout, " {name}=")?;
            stdout.write_all(port.path.as_os_str().as_bytes())?;
        }
        writeln!(stdout, " tx_enabled={}", u8::from(enable_transmit))?;
        stdout.flush()?;
    }
    let result = modem.run(&mut radio, shutdown);
    result.and(radio.close())
}
