//! The KMS layer's Rust sources.
//!
//! This directory is the KMS half of the GPU stack: the modesetting object
//! model, framebuffers, properties and the KMS ioctl handlers.  The files reach
//! Rust through the main crate's `#[path]` module declaration
//! (`drm/rust_drm/src/lib.rs`), which is what makes one crate own both halves —
//! the two share `DrmDevice`/`DrmFile`, and two Rust staticlibs built from them
//! would duplicate those symbols at link time.
//!
//! The C that used to live here is being migrated module by module; the crate's
//! `meson.build` lists this directory as an input so edits here rebuild it.

pub mod atomic;
pub mod crtc;
pub mod framebuffer;
pub mod mode_ioctl;
pub mod mode_object;
pub mod property;
