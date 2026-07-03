include(FetchContent)

FetchContent_Declare(
        Corrosion
        GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
        GIT_TAG v0.6
)

set(Rust_TOOLCHAIN "nightly")
set(Rust_RUSTUP_INSTALL_MISSING_TARGET ON)
# Set any global configuration variables such as `Rust_TOOLCHAIN` before this line!
FetchContent_MakeAvailable(Corrosion)

corrosion_set_cargo_flags("-Z build-std=std,panic_abort" "-Z build-std-features=\"optimize_for_size\"")

# Import targets defined in a package or workspace manifest `Cargo.toml` file
corrosion_import_crate(MANIFEST_PATH ./Cargo.toml
        PROFILE release
)