/*
** speedtest1_wrapper.c — Wrapper to run speedtest1 with azqlite VFS
*/

#include "sqlite3.h"
#include <stdio.h>

/* Public API for VFS registration */
#include "azqlite.h"

/* Include speedtest1 as a library */
#define main speedtest1_main
#include "speedtest1.c"
#undef main

int main(int argc, char **argv) {
  /* Register azqlite VFS as default */
  int rc = azqlite_vfs_register(1);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "Failed to register azqlite VFS: %d\n", rc);
    return 1;
  }
  
  /* Run speedtest1 */
  return speedtest1_main(argc, argv);
}
