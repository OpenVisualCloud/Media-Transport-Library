#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]

include!(concat!(env!("OUT_DIR"), "/mtl_bindings.rs"));

#[cfg(feature = "convert")]
pub mod convert;

#[cfg(feature = "pipeline")]
pub mod pipeline;

#[cfg(test)]
mod tests {
    use super::*;
    use core::ffi::CStr;

    #[test]
    fn print_version() {
        unsafe {
            let version_slice = CStr::from_ptr(mtl_version());
            eprintln!("version: {}", version_slice.to_str().unwrap());
            assert_ne!(0, version_slice.to_bytes().len());
        }
    }

    #[test]
    fn simd_level() {
        unsafe {
            let cpu_level = mtl_get_simd_level();
            let name = mtl_get_simd_level_name(cpu_level as u32);

            let name_slice = CStr::from_ptr(name);
            println!(
                "simd level by cpu: {}({})",
                cpu_level,
                name_slice.to_str().unwrap()
            );
            assert!(cpu_level < mtl_simd_level_MTL_SIMD_LEVEL_MAX);
        }
    }
}
