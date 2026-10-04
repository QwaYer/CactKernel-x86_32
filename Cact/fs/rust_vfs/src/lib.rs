//! Rust core of the CactOS VFS.
//!
//! This crate owns the mount table, symlinks, node lifetime and path
//! resolution.  C filesystems are unchanged: they register through the C
//! `vfs_ops_t` table and the core calls into them.  The exported `#[no_mangle]`
//! symbols are the same ones `Cact/fs/vfs/vfs.c` used to define, so every C
//! caller and the Rust `sched`/`rust_drm`/`rust_mm` externs bind as before.
//!
//! This is the first stage of moving the VFS to Rust: it replaces the C core
//! verbatim.  The dentry cache, inode/superblock split and mount tree grow on
//! top of it without changing these symbols or the struct layouts.

#![no_std]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]

pub mod abi;

mod addr;
mod dcache;
mod file;
mod ops;
mod pipe;
mod sb;
mod vfs;

pub use abi::*;
