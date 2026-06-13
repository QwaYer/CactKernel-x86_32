/// Stub. No threads in the kernel.
pub struct Thread;
pub fn current() -> Thread { Thread }
pub fn yield_now() {}
pub fn sleep(_dur: crate::time::Duration) {}
