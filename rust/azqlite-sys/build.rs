use std::env;
use std::path::PathBuf;
use std::process::Command;

fn main() {
    let repo_root = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap())
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .to_path_buf();

    let src_dir = repo_root.join("src");
    let sqlite_dir = repo_root.join("sqlite-autoconf-3520000");

    // Find OpenSSL on macOS using pkg-config or Homebrew
    let openssl_include = if let Ok(output) = Command::new("pkg-config")
        .args(&["--cflags-only-I", "openssl"])
        .output()
    {
        String::from_utf8_lossy(&output.stdout)
            .trim()
            .strip_prefix("-I")
            .map(String::from)
    } else if let Ok(output) = Command::new("brew").args(&["--prefix", "openssl"]).output() {
        let prefix = String::from_utf8_lossy(&output.stdout).trim().to_string();
        Some(format!("{}/include", prefix))
    } else {
        None
    };

    // Compile SQLite amalgamation
    cc::Build::new()
        .file(sqlite_dir.join("sqlite3.c"))
        .include(&sqlite_dir)
        .define("SQLITE_THREADSAFE", "1")
        .define("SQLITE_ENABLE_FTS5", None)
        .define("SQLITE_ENABLE_JSON1", None)
        .define("_DARWIN_C_SOURCE", None)
        .warnings(false) // SQLite has many warnings we don't control
        .compile("sqlite3");

    // Compile azqlite VFS and Azure client
    let mut builder = cc::Build::new();
    builder
        .file(src_dir.join("azqlite_vfs.c"))
        .file(src_dir.join("azure_client.c"))
        .file(src_dir.join("azure_auth.c"))
        .file(src_dir.join("azure_error.c"))
        .include(&src_dir)
        .include(&sqlite_dir)
        .define("SQLITE_THREADSAFE", "1")
        .define("SQLITE_ENABLE_FTS5", None)
        .define("SQLITE_ENABLE_JSON1", None)
        .define("_DARWIN_C_SOURCE", None)
        .warnings(true)
        .extra_warnings(true)
        .flag("-std=c11");

    if let Some(inc) = openssl_include {
        builder.include(inc);
    }

    builder.compile("azqlite");

    // Find and link OpenSSL using pkg-config or Homebrew
    if let Ok(output) = Command::new("pkg-config")
        .args(&["--libs-only-L", "openssl"])
        .output()
    {
        let libs = String::from_utf8_lossy(&output.stdout);
        for flag in libs.trim().split_whitespace() {
            if let Some(path) = flag.strip_prefix("-L") {
                println!("cargo:rustc-link-search=native={}", path);
            }
        }
    } else if let Ok(output) = Command::new("brew").args(&["--prefix", "openssl"]).output() {
        let prefix = String::from_utf8_lossy(&output.stdout).trim().to_string();
        println!("cargo:rustc-link-search=native={}/lib", prefix);
    }

    // Link system libraries
    println!("cargo:rustc-link-lib=curl");
    println!("cargo:rustc-link-lib=ssl");
    println!("cargo:rustc-link-lib=crypto");
    println!("cargo:rustc-link-lib=pthread");
    println!("cargo:rustc-link-lib=m");

    // Tell cargo to recompile if C sources change
    println!("cargo:rerun-if-changed={}", src_dir.join("azqlite_vfs.c").display());
    println!("cargo:rerun-if-changed={}", src_dir.join("azure_client.c").display());
    println!("cargo:rerun-if-changed={}", src_dir.join("azure_auth.c").display());
    println!("cargo:rerun-if-changed={}", src_dir.join("azure_error.c").display());
    println!("cargo:rerun-if-changed={}", src_dir.join("azqlite.h").display());
    println!("cargo:rerun-if-changed={}", src_dir.join("azure_client.h").display());
    println!("cargo:rerun-if-changed={}", sqlite_dir.join("sqlite3.c").display());
}
