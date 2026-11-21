use std::env;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::process::Command;

const HAL: &str = "deps/hal";
const USP_RADIO: &str =
    "deps/usp/smtc_rac_lib/smtc_radio_interface/radio_drivers/sx126x_driver/src";

fn main() {
    assert_eq!(
        env::var("CARGO_CFG_TARGET_OS").as_deref(),
        Ok("linux"),
        "sx130x-kiss requires Linux"
    );
    let hardware = cfg!(feature = "hardware");
    let fake_hal = cfg!(feature = "fake-hal");
    let sim = cfg!(feature = "sim");
    assert_eq!(
        usize::from(hardware) + usize::from(fake_hal) + usize::from(sim),
        1,
        "select exactly one radio backend: hardware, fake-hal or sim"
    );

    if sim {
        build_simulator();
    } else {
        build_native_driver(fake_hal);
    }
}

fn build_simulator() {
    let sources = ["kiss/tests/radio_scenario.c", "driver/sx1250_lora.c"];
    for input in sources.into_iter().chain([
        "driver/sx1250_board.h",
        "driver/sx1250_dual.h",
        "driver/sx1250_lora.h",
        "driver/sx1250_rx.h",
        "driver/sx1250_tx.h",
    ]) {
        println!("cargo::rerun-if-changed={input}");
    }
    cc::Build::new()
        .std("c11")
        .include("driver")
        .files(sources)
        .warnings_into_errors(true)
        .compile("sx1250_scenario");
}

fn build_native_driver(fake_hal: bool) {
    assert!(
        Path::new("deps/hal/libloragw/Makefile").is_file()
            && Path::new(USP_RADIO).join("sx126x.c").is_file(),
        "initialize the radio submodules with git submodule update --init deps/hal deps/usp"
    );
    let native = PathBuf::from(env::var_os("OUT_DIR").expect("Cargo sets OUT_DIR")).join("native");
    // Both Makefiles create artifacts beside their sources. Stage inputs for this Cargo target.
    match fs::remove_dir_all(&native) {
        Ok(()) => {}
        Err(error) if error.kind() == io::ErrorKind::NotFound => {}
        Err(error) => panic!("clear {}: {error}", native.display()),
    }
    for input in [
        "driver/Makefile",
        "deps/hal/VERSION",
        "deps/hal/target.cfg",
        "deps/hal/libloragw/Makefile",
        "deps/hal/libloragw/library.cfg",
    ] {
        stage_input(&native, Path::new(input));
    }
    for directory in ["driver", "deps/hal/libloragw/inc", USP_RADIO] {
        stage_sources(&native, directory);
    }

    let build = cc::Build::new();
    let compiler = make_tool("CC", &build.get_compiler().to_command());
    let archiver = make_tool("AR", &build.get_archiver());
    let make = |directory: &str, targets: &[&str]| {
        let mut command = Command::new("make");
        command
            .current_dir(native.join(directory))
            .env_remove("MAKEFLAGS")
            .env_remove("MFLAGS")
            .args([&compiler, &archiver])
            .args(targets);
        if let Some(flags) = env::var_os("CARGO_MAKEFLAGS") {
            command.env("MAKEFLAGS", flags);
        }
        let status = command
            .status()
            .expect("run GNU Make for the radio libraries");
        assert!(status.success(), "make in {directory} failed: {status}");
    };

    let mut driver_targets = vec![
        "HAL=../deps/hal",
        "USP=../deps/usp",
        "libsx1250_tx.a",
        "libsx1250_rx.a",
    ];
    if fake_hal {
        stage_input(&native, Path::new("driver/tests/fake_hal.c"));
        stage_input(&native, Path::new("driver/tests/fake_hal.h"));
        make("deps/hal/libloragw", &["inc/config.h"]);
        driver_targets.push("libsx1250_fake_hal.a");
    } else {
        stage_input(&native, Path::new("deps/hal/libtools/Makefile"));
        for directory in ["libloragw/src", "libtools/inc", "libtools/src"] {
            stage_sources(&native, &format!("{HAL}/{directory}"));
        }
        make("deps/hal/libtools", &["libtinymt32.a"]);
        make("deps/hal/libloragw", &["libloragw.a"]);
    }
    make("driver", &driver_targets);

    println!(
        "cargo::rustc-link-search=native={}",
        native.join("driver").display()
    );
    println!("cargo::rustc-link-lib=static=sx1250_tx");
    println!("cargo::rustc-link-lib=static=sx1250_rx");
    if fake_hal {
        println!("cargo::rustc-link-lib=static=sx1250_fake_hal");
    } else {
        for (directory, library) in [("libloragw", "loragw"), ("libtools", "tinymt32")] {
            println!(
                "cargo::rustc-link-search=native={}",
                native.join(HAL).join(directory).display()
            );
            println!("cargo::rustc-link-lib=static={library}");
        }
    }
    println!("cargo::rustc-link-lib=rt");
    println!("cargo::rustc-link-lib=m");
}

fn stage_sources(native: &Path, directory: &str) {
    let files = fs::read_dir(directory).unwrap_or_else(|error| panic!("read {directory}: {error}"));
    for file in files {
        let path = file.expect("read source directory entry").path();
        if matches!(
            path.extension().and_then(|extension| extension.to_str()),
            Some("c" | "h" | "var")
        ) && path != Path::new("deps/hal/libloragw/inc/config.h")
        {
            stage_input(native, &path);
        }
    }
}

fn stage_input(native: &Path, path: &Path) {
    println!("cargo::rerun-if-changed={}", path.display());
    let staged = native.join(path);
    fs::create_dir_all(staged.parent().expect("staged source directory"))
        .and_then(|()| fs::copy(path, &staged))
        .unwrap_or_else(|error| panic!("stage {}: {error}", path.display()));
}

fn make_tool(variable: &str, tool: &Command) -> String {
    let arguments = std::iter::once(tool.get_program())
        .chain(tool.get_args())
        .map(|argument| {
            let argument = argument.to_str().expect("C tools require UTF-8 arguments");
            format!("'{}'", argument.replace('$', "$$").replace('\'', "'\\''"))
        })
        .collect::<Vec<_>>()
        .join(" ");
    format!("{variable}={arguments}")
}
