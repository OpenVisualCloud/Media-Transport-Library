extern crate bindgen;

use std::env;
use std::path::PathBuf;

fn main() {
    pkg_config::Config::new().probe("mtl").unwrap();
    println!("cargo:rerun-if-changed=build.rs");

    let mut builder = bindgen::Builder::default().header("wrapper.h");
    if env::var_os("CARGO_FEATURE_IPMX").is_some() {
        if pkg_config::get_variable("mtl", "ipmx").ok().as_deref() != Some("true") {
            panic!("feature ipmx needs an MTL built with enable_ipmx (mtl.pc ipmx=true)");
        }
        builder = builder.header("ipmx/wrapper_ipmx.h");
    }

    let bindings = builder
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .generate()
        .expect("Unable to generate bindings");

    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("mtl_bindings.rs"))
        .expect("Couldn't write bindings!");
}
