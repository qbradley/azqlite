# Azqlite Rust Bindings

Safe Rust bindings for azqlite - a SQLite VFS backed by Azure Blob Storage.

## Structure

This workspace contains two crates:

- **`azqlite-sys`** - Raw FFI bindings to the azqlite C library
- **`azqlite`** - Safe, idiomatic Rust API

## Building

Requirements:
- Rust 1.70 or later
- libcurl, OpenSSL (linked dynamically)
- C compiler (for building the C sources)

```sh
cd rust
cargo build
```

## Testing

```sh
cargo test
```

## Example

See `azqlite/examples/basic.rs`:

```sh
cargo run --example basic
```

## Usage

Add to your `Cargo.toml`:

```toml
[dependencies]
azqlite = { path = "path/to/rust/azqlite" }
rusqlite = "0.32"
```

Then in your code:

```rust
use azqlite::AzqliteVfs;
use rusqlite::Connection;

// Register VFS in URI mode
AzqliteVfs::register_uri(false)?;

// Open database with Azure credentials in URI
let conn = Connection::open_with_flags_and_vfs(
    "file:mydb.db?azure_account=myaccount&azure_container=databases&azure_sas=sv=2024...",
    OpenFlags::SQLITE_OPEN_READ_WRITE | OpenFlags::SQLITE_OPEN_CREATE | OpenFlags::SQLITE_OPEN_URI,
    "azqlite"
)?;
```

## License

MIT
