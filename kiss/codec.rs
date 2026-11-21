use std::mem;

const FRAME_CAPACITY: usize = 512;
const FEND: u8 = 0xc0;
const FESC: u8 = 0xdb;
const TFEND: u8 = 0xdc;
const TFESC: u8 = 0xdd;

enum State {
    Waiting,
    Frame,
    Escaped,
}

pub(crate) struct Decoder {
    frame: [u8; FRAME_CAPACITY],
    length: usize,
    state: State,
}

impl Default for Decoder {
    fn default() -> Self {
        Self {
            frame: [0; FRAME_CAPACITY],
            length: 0,
            state: State::Waiting,
        }
    }
}

impl Decoder {
    pub(crate) fn push(&mut self, byte: u8) -> Option<&[u8]> {
        if byte == FEND {
            let length = mem::take(&mut self.length);
            let state = mem::replace(&mut self.state, State::Frame);
            return (matches!(state, State::Frame) && length != 0).then_some(&self.frame[..length]);
        }
        let byte = match self.state {
            State::Waiting => return None,
            State::Frame if byte == FESC => {
                self.state = State::Escaped;
                return None;
            }
            State::Frame => byte,
            State::Escaped => match byte {
                TFEND => FEND,
                TFESC => FESC,
                _ => {
                    self.state = State::Waiting;
                    return None;
                }
            },
        };
        if let Some(slot) = self.frame.get_mut(self.length) {
            *slot = byte;
            self.length += 1;
            self.state = State::Frame;
        } else {
            self.state = State::Waiting;
        }
        None
    }
}

pub(crate) const fn encoded_max(payload_len: usize) -> usize {
    4 + 2 * payload_len
}

pub(crate) fn encode(out: &mut Vec<u8>, frame_type: u8, payload: &[u8]) {
    assert!(
        payload.len() < FRAME_CAPACITY,
        "KISS payload exceeds frame limit"
    );
    out.push(FEND);
    for byte in std::iter::once(frame_type).chain(payload.iter().copied()) {
        match byte {
            FEND => out.extend_from_slice(&[FESC, TFEND]),
            FESC => out.extend_from_slice(&[FESC, TFESC]),
            _ => out.push(byte),
        }
    }
    out.push(FEND);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn escapes_type_and_payload_bytes() {
        let payload = [FESC, 0x53, FEND];
        let mut wire = Vec::new();
        encode(&mut wire, FEND, &payload);
        assert_eq!(
            wire,
            [FEND, FESC, TFEND, FESC, TFESC, 0x53, FESC, TFEND, FEND]
        );

        let mut decoder = Decoder::default();
        for &byte in &wire[..wire.len() - 1] {
            assert!(decoder.push(byte).is_none());
        }
        assert_eq!(
            decoder.push(FEND),
            Some([FEND, FESC, 0x53, FEND].as_slice())
        );
    }

    #[test]
    fn limits_decoded_frame_and_recovers_after_overflow() {
        let payload = [FESC; FRAME_CAPACITY - 1];
        let mut wire = Vec::new();
        encode(&mut wire, FESC, &payload);
        assert_eq!(wire.len(), encoded_max(payload.len()));

        let mut decoder = Decoder::default();
        for &byte in &wire[..wire.len() - 1] {
            assert!(decoder.push(byte).is_none());
        }
        assert_eq!(decoder.push(FEND), Some([FESC; FRAME_CAPACITY].as_slice()));

        for _ in 0..=FRAME_CAPACITY {
            assert!(decoder.push(0x31).is_none());
        }
        assert!(decoder.push(FEND).is_none());
        assert!(decoder.push(0x06).is_none());
        assert_eq!(decoder.push(FEND), Some([0x06].as_slice()));
    }

    #[test]
    #[should_panic(expected = "KISS payload exceeds frame limit")]
    fn refuses_an_oversized_outgoing_frame() {
        encode(&mut Vec::new(), FESC, &[FESC; FRAME_CAPACITY]);
    }

    #[test]
    fn discards_damaged_frames_and_noise() {
        let damaged = [
            0x91, FESC, TFEND, FEND, FEND, 0x06, FESC, 0x00, 0x17, FEND, 0x06, FESC, FEND,
        ];
        let mut decoder = Decoder::default();
        for byte in damaged {
            assert!(decoder.push(byte).is_none());
        }
        assert!(decoder.push(0x06).is_none());
        assert_eq!(decoder.push(FEND), Some([0x06].as_slice()));
    }
}
