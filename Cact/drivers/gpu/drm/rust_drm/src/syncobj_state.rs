//! The fence-free state a syncobj's behaviour reduces to.
//!
//! CactKernel's DRM core holds no fence objects, so what a client can observe of
//! a syncobj is only whether it is signalled (the **binary** flavour) or which
//! point it has reached (the **timeline** flavour).  Every fence-shaped question
//! the ioctls ask — "is the fence at `point` there yet", "what does a transfer
//! put on the destination", "when does a registered eventfd fire" — is decided
//! here, on values rather than on device state, so the rules can be unit-tested
//! on the host (`Kernel-Unit-Tests-for-Cact`) instead of only through a live
//! device.

/// One syncobj's observable state.  `timeline` is the flavour and is fixed at
/// creation; `signaled` and `point` are what moves.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SyncState {
    pub timeline: bool,
    /// The binary "signalled" bit; always false on a timeline object.
    pub signaled: bool,
    /// The point a timeline object has reached; only ever moves forward.
    pub point: u64,
}

/// Why a transfer cannot land on a destination.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TransferError {
    /// `dst_point` asks for a shape the destination is not: a point belongs to a
    /// timeline object, the signalled bit to a binary one.
    WrongShape,
}

impl SyncState {
    /// Has this object got as far as the fence at `point` would?
    ///
    /// A binary object's state *is* its signalled bit.  A timeline object's is
    /// the point it reached, and `point == 0` asks for "any point at all" — the
    /// answer Linux gives with the chain's head fence.
    pub const fn reached(&self, point: u64) -> bool {
        if self.timeline {
            if point == 0 {
                self.point > 0
            } else {
                self.point >= point
            }
        } else {
            self.signaled
        }
    }

    /// Can `point` be asked of this object at all?  A binary object has no
    /// points, so a non-zero one is a bad argument — on Linux the sequence
    /// lookup on a non-chain fence fails.
    pub const fn takes_point(&self, point: u64) -> bool {
        self.timeline || point == 0
    }

    /// Signal a binary object (`DRM_IOCTL_SYNCOBJ_SIGNAL`, and what a transfer of
    /// a signalled source leaves behind).
    pub fn signal(&mut self) {
        self.signaled = true;
    }

    /// Reset a binary object (`DRM_IOCTL_SYNCOBJ_RESET`).
    pub fn reset(&mut self) {
        self.signaled = false;
    }

    /// Move a timeline point forward.  Returns whether it moved: a point never
    /// goes back, so a signal at or below it changes nothing.
    pub fn advance(&mut self, point: u64) -> bool {
        if point > self.point {
            self.point = point;
            true
        } else {
            false
        }
    }

    /// Apply a transfer onto this object.  `dst_point != 0` asks for the timeline
    /// shape, where the point moves to `dst_point`; `dst_point == 0` asks for the
    /// binary shape, which becomes signalled.  Returns whether the destination
    /// changed.
    ///
    /// The caller has already established that the source carries the state
    /// being moved: with no pending fences to carry, there is nothing else a
    /// transfer could put here.
    pub fn take_transfer(&mut self, dst_point: u64) -> Result<bool, TransferError> {
        if dst_point != 0 {
            if !self.timeline {
                return Err(TransferError::WrongShape);
            }
            Ok(self.advance(dst_point))
        } else {
            if self.timeline {
                return Err(TransferError::WrongShape);
            }
            let changed = !self.signaled;
            self.signal();
            Ok(changed)
        }
    }
}
