/*!
Querying IMTL Version
 */

use std::fmt;

use crate::sys;

/// A structure that contains information about the version in use.
#[derive(Copy, Clone, Eq, PartialEq, Hash, Debug)]
pub struct Version {
    /// major version
    pub major: u8,
    /// minor version
    pub minor: u8,
    /// update version (patchlevel)
    pub patch: u8,
}

impl Version {
    /// Convert const numbers to Version.
    pub fn from_const() -> Version {
        Version {
            major: sys::MTL_VERSION_MAJOR as u8,
            minor: sys::MTL_VERSION_MINOR as u8,
            patch: sys::MTL_VERSION_LAST as u8,
        }
    }
}

impl fmt::Display for Version {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "{}.{}.{}", self.major, self.minor, self.patch)
    }
}

pub fn version() -> Version {
    Version::from_const()
}
