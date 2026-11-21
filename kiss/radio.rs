use std::ffi::{CString, c_char, c_int, c_uint};
use std::io;
use std::os::unix::ffi::OsStrExt;
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};

pub(crate) const DEFAULT_POWER_DBM: i8 = -9;
pub(crate) const TX_TIMEOUT_MAX_MS: u32 = 120_000;

// The three repr(C) types mirror sx1250_rx.h and sx1250_tx.h.
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct Profile {
    pub frequency_hz: u32,
    pub bandwidth_hz: u32,
    pub preamble_symbols: u16,
    pub spreading_factor: u8,
    pub coding_rate: u8,
    pub sync_word: u8,
    require_crc: bool,
    inverted_iq: bool,
    implicit_length: u8,
}

impl Default for Profile {
    fn default() -> Self {
        Self {
            frequency_hz: 868_100_000,
            bandwidth_hz: 125_000,
            preamble_symbols: 32,
            spreading_factor: 7,
            coding_rate: 5,
            sync_word: 0x12,
            require_crc: true,
            inverted_iq: false,
            implicit_length: 0,
        }
    }
}

#[repr(C)]
pub(crate) struct RxPacket {
    payload: [u8; 255],
    size: u8,
    coding_rate: u8,
    crc_present: bool,
    implicit_header: bool,
    pub rssi_dbm: f32,
    pub snr_db: f32,
}

impl RxPacket {
    pub(crate) fn payload(&self) -> &[u8] {
        &self.payload[..usize::from(self.size)]
    }
}

#[repr(C)]
struct HandoffReport {
    failed_stage: *const c_char,
    downtime_ns: i64,
    tx_started: bool,
    tx_done: bool,
    rf1_rearmed: bool,
    cold_recovery: bool,
    rf0_rearmed: bool,
}

unsafe extern "C" {
    fn sx1250_dual_open(path: *const c_char, profiles: *const Profile) -> c_int;
    fn sx1250_dual_close() -> c_int;
    fn sx1250_dual_next(rf: c_uint, packet: *mut RxPacket) -> c_int;
    fn sx1250_dual_activity(rf: c_uint) -> c_int;
    fn sx1250_dual_reconfigure(rf: c_uint, profile: *const Profile) -> c_int;
    fn sx1250_dual_send_with_power(
        profile: *const Profile,
        power_dbm: i8,
        payload: *const u8,
        length: usize,
        timeout_ms: c_uint,
        report: *mut HandoffReport,
    ) -> c_int;
    fn sx1250_lora_airtime_ms(profile: *const Profile, size: u8) -> u32;
    fn sx1250_tx_power_supported(power_dbm: i8) -> bool;
}

pub(crate) struct TxOutcome {
    pub result: io::Result<()>,
    pub tx_started: bool,
    pub receivers_rearmed: bool,
}

static OWNED: AtomicBool = AtomicBool::new(false);

pub(crate) struct Radio {
    open: bool,
}

impl Radio {
    pub(crate) fn open(device: &Path, profiles: &[Profile; 2]) -> io::Result<Self> {
        let path = CString::new(device.as_os_str().as_bytes())
            .map_err(|error| io::Error::new(io::ErrorKind::InvalidInput, error))?;
        if OWNED
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            return Err(io::Error::from_raw_os_error(libc::EBUSY));
        }
        // SAFETY: The atomic guard reserves the global session and both inputs live through the call.
        let result = check(unsafe { sx1250_dual_open(path.as_ptr(), profiles.as_ptr()) });
        if let Err(error) = result {
            OWNED.store(false, Ordering::Release);
            return Err(error);
        }
        Ok(Self { open: true })
    }

    pub(crate) fn next(&mut self, rf: usize) -> io::Result<Option<RxPacket>> {
        let rf = chain(rf)?;
        let mut packet = std::mem::MaybeUninit::uninit();
        // SAFETY: The session is exclusively borrowed and packet points to writable storage.
        if check(unsafe { sx1250_dual_next(rf, packet.as_mut_ptr()) })? == 0 {
            return Ok(None);
        }
        // SAFETY: The driver initializes the whole output packet when it returns 1.
        Ok(Some(unsafe { packet.assume_init() }))
    }

    pub(crate) fn activity(&mut self, rf: usize) -> io::Result<bool> {
        let rf = chain(rf)?;
        // SAFETY: The session is exclusively borrowed.
        check(unsafe { sx1250_dual_activity(rf) }).map(|status| status != 0)
    }

    pub(crate) fn reconfigure(&mut self, rf: usize, profile: &Profile) -> io::Result<()> {
        let rf = chain(rf)?;
        // SAFETY: The session is exclusively borrowed and the driver copies the profile.
        check(unsafe { sx1250_dual_reconfigure(rf, profile) }).map(|_| ())
    }

    pub(crate) fn send(
        &mut self,
        profile: &Profile,
        power_dbm: i8,
        payload: &[u8],
        timeout_ms: u32,
    ) -> TxOutcome {
        let mut report = HandoffReport {
            failed_stage: std::ptr::null(),
            downtime_ns: 0,
            tx_started: false,
            tx_done: false,
            rf1_rearmed: false,
            cold_recovery: false,
            rf0_rearmed: false,
        };
        // SAFETY: The session is exclusively borrowed and the pointers are valid through the call.
        let result = check(unsafe {
            sx1250_dual_send_with_power(
                profile,
                power_dbm,
                payload.as_ptr(),
                payload.len(),
                timeout_ms,
                &mut report,
            )
        });
        TxOutcome {
            result: result.map(|_| ()),
            tx_started: report.tx_started,
            receivers_rearmed: report.rf0_rearmed && report.rf1_rearmed,
        }
    }

    pub(crate) fn close(mut self) -> io::Result<()> {
        self.release()
    }

    fn release(&mut self) -> io::Result<()> {
        if !self.open {
            return Ok(());
        }
        // SAFETY: This Radio owns the global session until the driver releases it.
        let result = check(unsafe { sx1250_dual_close() });
        self.open = false;
        OWNED.store(false, Ordering::Release);
        result.map(|_| ())
    }
}

impl Drop for Radio {
    fn drop(&mut self) {
        let _ = self.release();
    }
}

pub(crate) fn airtime_ms(profile: &Profile, size: u8) -> u32 {
    // SAFETY: This calculation only reads the profile.
    unsafe { sx1250_lora_airtime_ms(profile, size) }
}

pub(crate) fn power_supported(power_dbm: i8) -> bool {
    // SAFETY: This check has no global state.
    unsafe { sx1250_tx_power_supported(power_dbm) }
}

fn check(status: c_int) -> io::Result<c_int> {
    if status < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(status)
    }
}

fn chain(rf: usize) -> io::Result<c_uint> {
    if rf < 2 {
        Ok(rf as c_uint)
    } else {
        Err(io::Error::from_raw_os_error(libc::EINVAL))
    }
}

#[cfg(all(test, feature = "sim"))]
mod tests {
    use super::*;

    #[test]
    fn radio_session_is_exclusive_until_drop() {
        let path = Path::new("/simulated-spi");
        let profiles = [Profile::default(); 2];
        let radio = Radio::open(path, &profiles).unwrap();
        let second = Radio::open(path, &profiles);
        assert!(matches!(second, Err(error) if error.raw_os_error() == Some(libc::EBUSY)));
        drop(radio);
        Radio::open(path, &profiles).unwrap().close().unwrap();
    }
}
