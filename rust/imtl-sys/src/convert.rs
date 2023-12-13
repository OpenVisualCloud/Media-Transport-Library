include!(concat!(env!("OUT_DIR"), "/mtl_convert_bindings.rs"));

#[cfg(test)]
mod tests {
    use super::*;

    fn cvt_rfc4175_422be10_to_v210(w: u32, h: u32, cvt_level: mtl_simd_level) {
        let mut ret;
        let src_fb_size = w * h * 5 / 2;
        let dst_fb_size = w * h * 8 / 3;

        let mut src_buf: Vec<u8> = vec![0; src_fb_size as usize];
        let mut dst_buf: Vec<u8> = vec![0; dst_fb_size as usize];
        let mut rev_buf: Vec<u8> = vec![0; src_fb_size as usize];

        use rand::{thread_rng, Rng};
        thread_rng().fill(&mut src_buf[..]);
        unsafe {
            ret = st20_rfc4175_422be10_to_v210_simd(
                src_buf.as_mut_ptr() as *mut st20_rfc4175_422_10_pg2_be,
                dst_buf.as_mut_ptr(),
                w,
                h,
                cvt_level,
            );
            assert_eq!(0, ret);

            ret = st20_v210_to_rfc4175_422be10_simd(
                dst_buf.as_mut_ptr(),
                rev_buf.as_mut_ptr() as *mut st20_rfc4175_422_10_pg2_be,
                w,
                h,
                cvt_level,
            );
            assert_eq!(0, ret);
        }
        assert_eq!(src_buf, rev_buf);
        println!("src 100:{}, rev 100: {}", src_buf[100], rev_buf[100]);
    }

    #[test]
    fn rfc_422be_to_v210() {
        cvt_rfc4175_422be10_to_v210(1920, 1080, mtl_simd_level_MTL_SIMD_LEVEL_NONE);
        cvt_rfc4175_422be10_to_v210(1920, 1080, mtl_simd_level_MTL_SIMD_LEVEL_AVX512);
        cvt_rfc4175_422be10_to_v210(1920, 1080, mtl_simd_level_MTL_SIMD_LEVEL_AVX512_VBMI2);
    }
}
