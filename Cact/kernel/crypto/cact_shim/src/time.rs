use core::cmp::Ordering;
use core::fmt;
use core::ops::{Add, AddAssign, Div, Mul, Sub, SubAssign};
use core::time::Duration as CoreDuration;

pub use core::time::Duration;

/// Kernel timer via FFI — wraps timer_ticks_get().
extern "C" {
    fn timer_ticks_get() -> u32;
}

fn ticks() -> u64 {
    unsafe { timer_ticks_get() as u64 }
}

const TICK_MS: u64 = 10;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Instant {
    ticks: u64,
}

impl Instant {
    pub fn now() -> Self {
        Instant { ticks: ticks() }
    }

    pub fn duration_since(&self, earlier: Self) -> Duration {
        Duration::from_millis(
            self.ticks.saturating_sub(earlier.ticks) * TICK_MS,
        )
    }

    pub fn elapsed(&self) -> Duration {
        Instant::now().duration_since(*self)
    }

    pub fn checked_add(&self, duration: Duration) -> Option<Self> {
        let ms = duration.as_millis() as u64;
        let t = self.ticks.checked_add(ms / TICK_MS)?;
        Some(Instant { ticks: t })
    }

    pub fn checked_sub(&self, duration: Duration) -> Option<Self> {
        let ms = duration.as_millis() as u64;
        let t = self.ticks.checked_sub(ms / TICK_MS)?;
        Some(Instant { ticks: t })
    }

    pub fn checked_duration_since(&self, earlier: Self) -> Option<Duration> {
        if self.ticks >= earlier.ticks {
            Some(self.duration_since(earlier))
        } else {
            None
        }
    }

    pub fn saturating_duration_since(&self, earlier: Self) -> Duration {
        self.checked_duration_since(earlier)
            .unwrap_or(Duration::ZERO)
    }
}

impl Add<Duration> for Instant {
    type Output = Instant;
    fn add(self, other: Duration) -> Instant {
        self.checked_add(other).expect("overflow")
    }
}

impl Sub<Duration> for Instant {
    type Output = Instant;
    fn sub(self, other: Duration) -> Instant {
        self.checked_sub(other).expect("underflow")
    }
}

impl Sub<Instant> for Instant {
    type Output = Duration;
    fn sub(self, other: Instant) -> Duration {
        self.duration_since(other)
    }
}

impl AddAssign<Duration> for Instant {
    fn add_assign(&mut self, other: Duration) {
        *self = *self + other;
    }
}

impl SubAssign<Duration> for Instant {
    fn sub_assign(&mut self, other: Duration) {
        *self = *self - other;
    }
}

/// SystemTime: ticks since boot as a SystemTime-compatible representation.
const TICKS_AT_UNIX_EPOCH: u64 = 0;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct SystemTime {
    ticks: u64,
}

pub const UNIX_EPOCH: SystemTime = SystemTime { ticks: 0 };

impl SystemTime {
    pub fn now() -> Self {
        SystemTime {
            ticks: ticks(),
        }
    }

    pub fn duration_since(&self, earlier: Self) -> Result<Duration, SystemTimeError> {
        if self.ticks >= earlier.ticks {
            Ok(Duration::from_millis(
                (self.ticks - earlier.ticks) * TICK_MS,
            ))
        } else {
            Err(SystemTimeError(
                Duration::from_millis((earlier.ticks - self.ticks) * TICK_MS),
            ))
        }
    }

    pub fn elapsed(&self) -> Result<Duration, SystemTimeError> {
        SystemTime::now().duration_since(*self)
    }

    pub fn checked_add(&self, duration: Duration) -> Option<Self> {
        let ms = duration.as_millis() as u64;
        Some(SystemTime {
            ticks: self.ticks.checked_add(ms / TICK_MS)?,
        })
    }

    pub fn checked_sub(&self, duration: Duration) -> Option<Self> {
        let ms = duration.as_millis() as u64;
        Some(SystemTime {
            ticks: self.ticks.checked_sub(ms / TICK_MS)?,
        })
    }
}

impl Add<Duration> for SystemTime {
    type Output = SystemTime;
    fn add(self, dur: Duration) -> SystemTime {
        self.checked_add(dur).expect("overflow")
    }
}

impl Sub<Duration> for SystemTime {
    type Output = SystemTime;
    fn sub(self, dur: Duration) -> SystemTime {
        self.checked_sub(dur).expect("underflow")
    }
}

impl Sub<SystemTime> for SystemTime {
    type Output = Duration;
    fn sub(self, other: SystemTime) -> Duration {
        self.duration_since(other)
            .unwrap_or(Duration::ZERO)
    }
}

#[derive(Clone, Debug)]
pub struct SystemTimeError(pub Duration);

impl SystemTimeError {
    pub fn duration(&self) -> Duration {
        self.0
    }
}

impl fmt::Display for SystemTimeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "time error")
    }
}

impl crate::error::Error for SystemTimeError {}
