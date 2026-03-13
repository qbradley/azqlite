//! Smoke test for sqlite-objs on Linux.
//!
//! Verifies the full compile → link → FFI chain works by:
//!   1. Registering the sqlite-objs VFS (C FFI + system libs)
//!   2. Opening an in-memory database via rusqlite (bundled SQLite)
//!   3. Running basic CRUD operations

use sqlite_objs::SqliteObjsVfs;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("=== sqlite-objs Docker Smoke Test ===");

    // Step 1: Register VFS in URI mode (lightest — no Azure config needed).
    // This exercises the full C compilation and linking chain:
    //   Rust FFI → sqlite_objs C lib → libcurl → OpenSSL → SQLite
    println!("[1/3] Registering sqlite-objs VFS in URI mode...");
    SqliteObjsVfs::register_uri(false)?;
    println!("      OK — VFS registered");

    // Step 2: Open an in-memory database using the *default* VFS.
    // We don't need Azure credentials — this just proves rusqlite +
    // libsqlite3-sys link correctly alongside sqlite-objs-sys.
    println!("[2/3] Opening in-memory SQLite database...");
    let conn = rusqlite::Connection::open_in_memory()?;
    println!("      OK — database opened");

    // Step 3: CRUD round-trip
    println!("[3/3] Running CRUD operations...");
    conn.execute(
        "CREATE TABLE smoke_test (id INTEGER PRIMARY KEY, name TEXT NOT NULL)",
        [],
    )?;
    conn.execute("INSERT INTO smoke_test (name) VALUES (?1)", ["docker-test"])?;
    let name: String = conn.query_row("SELECT name FROM smoke_test WHERE id = 1", [], |row| {
        row.get(0)
    })?;
    assert_eq!(name, "docker-test", "query returned unexpected value");
    println!("      OK — CRUD round-trip passed");

    println!();
    println!("SUCCESS — sqlite-objs compiles and links on this platform.");
    Ok(())
}
