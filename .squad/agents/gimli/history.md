# Gimli — History

## Project Context

**Project:** Azure Blob-backed SQLite (azqlite)
**Owner:** Quetzal Bradley
**Stack:** C, SQLite VFS API, Azure Blob Storage REST API, libcurl, OpenSSL — with Rust crate wrappers
**License:** MIT

### What This Project Does

A drop-in replacement for SQLite where all storage is backed by Azure Blob Storage. Implemented as a custom VFS layer in C. My role is to package this as Rust crates so Rust developers can use it via rusqlite.

### Key Architecture

- C source in `src/` — azqlite_vfs.c (VFS impl), azure_client.c/.h (Azure REST), azqlite.h (public API)
- SQLite source in `sqlite-autoconf-3520000/`
- Public C API: `azqlite_vfs_register()`, `azqlite_vfs_register_uri()`, `azqlite_vfs_register_with_config()`
- Dependencies: libcurl, OpenSSL (system libraries)
- URI-based per-file config: `file:db?azure_account=...&azure_container=...&azure_sas=...`

## Learnings

(None yet — first session)
