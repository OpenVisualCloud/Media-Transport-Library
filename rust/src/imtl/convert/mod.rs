//!
//! A binding for `st_convert_api`
//!
//!
//! Note that you need to build with the
//! feature `convert` for this module to be enabled,
//! like so:
//!
//! ```bash
//! $ cargo build --features "convert"
//! ```
//!
//! If you want to use this with from inside your own
//! crate, you will need to add this in your Cargo.toml
//!
//! ```toml
//! [dependencies.imtl]
//! version = ...
//! default-features = false
//! features = ["convert"]
//! ```

use format;
use sys;
use sys::convert;

pub trait ToRFC {
    fn to_rfc4175() -> () {}
    fn from_rfc4175() -> () {}
}
