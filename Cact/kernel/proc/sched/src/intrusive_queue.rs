//! Pure generic intrusive singly-linked FIFO queue.
//!
//! A node stores its own link, so a `TaskStruct` is never copied and can sit in
//! a queue without any allocation.  The queue owns no memory and only needs the
//! `Link` accessor, which the kernel implements for `TaskStruct` (mapping the
//! link to its `queue_next` field) and the host tests implement for a plain
//! struct.
//!
//! No hardware access, no globals and no FFI, so this is the single source of
//! truth for the MLFQ queue mechanics and is unit-tested on the host by
//! `Kernel-Unit-Tests-for-Cact/` (P2.1).

use core::ptr;

/// A node that can be threaded onto an [`IntrusiveQueue`].
pub trait Link {
    /// The next node in the queue, or null.
    fn next(&self) -> *mut Self;
    /// Overwrite the next-node link.
    fn set_next(&mut self, next: *mut Self);
}

/// An intrusive FIFO.  Invariants: `count` equals the number of nodes reachable
/// from `head`; `tail` is null iff `head` is null; a popped/removed node's link
/// is cleared.
pub struct IntrusiveQueue<T: Link> {
    head: *mut T,
    tail: *mut T,
    count: u32,
}

impl<T: Link> IntrusiveQueue<T> {
    pub const fn new() -> Self {
        Self { head: ptr::null_mut(), tail: ptr::null_mut(), count: 0 }
    }

    pub fn is_empty(&self) -> bool {
        self.head.is_null()
    }

    pub fn count(&self) -> u32 {
        self.count
    }

    /// Append `node` at the tail.
    pub fn push(&mut self, node: &mut T) {
        let p: *mut T = node;
        node.set_next(ptr::null_mut());
        if self.tail.is_null() {
            self.head = p;
        } else {
            // SAFETY: `self.tail` is non-null and, by the invariant, points at a
            // live node still owned by this queue; this only rewrites its link.
            unsafe { (*self.tail).set_next(p) };
        }
        self.tail = p;
        self.count += 1;
    }

    /// Remove and return the head node, or null when empty.
    pub fn pop(&mut self) -> *mut T {
        if self.head.is_null() {
            return ptr::null_mut();
        }
        let t = self.head;
        // SAFETY: `t` is the non-null head, a live node owned by this queue.
        let next = unsafe { (*t).next() };
        self.head = next;
        if self.head.is_null() {
            self.tail = ptr::null_mut();
        }
        // SAFETY: `t` is that live node; clearing its link keeps the invariant.
        unsafe { (*t).set_next(ptr::null_mut()) };
        self.count -= 1;
        t
    }

    /// Unlink `node` from anywhere in the queue.  Returns `true` if it was
    /// present.  A null `node` is a no-op.
    pub fn remove(&mut self, node: *mut T) -> bool {
        if node.is_null() {
            return false;
        }
        let mut prev: *mut T = ptr::null_mut();
        let mut cur = self.head;
        while !cur.is_null() {
            if cur == node {
                // SAFETY: `node` is non-null and owned by this queue.
                let node_next = unsafe { (*node).next() };
                if prev.is_null() {
                    self.head = node_next;
                } else {
                    // SAFETY: `prev` was reached by walking this queue's chain, so
                    // it is a live node still owned by the queue.
                    unsafe { (*prev).set_next(node_next) };
                }
                if self.tail == node {
                    self.tail = prev;
                }
                self.count -= 1;
                // SAFETY: `node` is live and owned by this queue.
                unsafe { (*node).set_next(ptr::null_mut()) };
                return true;
            }
            prev = cur;
            // SAFETY: `cur` was reached by walking this queue's chain.
            cur = unsafe { (*cur).next() };
        }
        false
    }

    /// Drop every node (their links are cleared).
    pub fn clear(&mut self) {
        while !self.pop().is_null() {}
    }
}
