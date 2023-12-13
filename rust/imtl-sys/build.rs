extern crate bindgen;

use std::env;
use std::path::PathBuf;

fn main() {
    pkg_config::Config::new().probe("mtl").unwrap();
    println!("cargo:rerun-if-changed=build.rs");

    let bindings = bindgen::Builder::default()
        .header("wrapper.h")
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .generate()
        .expect("Unable to generate bindings");

    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("mtl_bindings.rs"))
        .expect("Couldn't write bindings!");

    if cfg!(feature = "convert") {
        let bindings = bindgen::Builder::default()
            .header("wrapper_convert.h")
            .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
            .generate()
            .expect("Unable to generate bindings");

        bindings
            .write_to_file(out_path.join("mtl_convert_bindings.rs"))
            .expect("Couldn't write bindings!");
    }

    if cfg!(feature = "pipeline") {
        let bindings = bindgen::Builder::default()
            .header("wrapper_pipeline.h")
            .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
            .generate()
            .expect("Unable to generate bindings");

        bindings
            .write_to_file(out_path.join("mtl_pipeline_bindings.rs"))
            .expect("Couldn't write bindings!");
    }
}
