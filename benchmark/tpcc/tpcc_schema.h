/*
** tpcc_schema.h - TPC-C schema definitions and SQL statements
**
** Implements a simplified TPC-C benchmark schema with 8 core tables:
**   - warehouse, district, customer, item, stock
**   - orders, order_line, new_order, history
*/

#ifndef TPCC_SCHEMA_H
#define TPCC_SCHEMA_H

/* Schema creation SQL */
static const char *TPCC_CREATE_TABLES = 
  "CREATE TABLE IF NOT EXISTS warehouse ("
  "  w_id       INTEGER PRIMARY KEY,"
  "  w_name     TEXT,"
  "  w_street_1 TEXT,"
  "  w_street_2 TEXT,"
  "  w_city     TEXT,"
  "  w_state    TEXT,"
  "  w_zip      TEXT,"
  "  w_tax      REAL,"
  "  w_ytd      REAL"
  ");"
  
  "CREATE TABLE IF NOT EXISTS district ("
  "  d_id       INTEGER,"
  "  d_w_id     INTEGER,"
  "  d_name     TEXT,"
  "  d_street_1 TEXT,"
  "  d_street_2 TEXT,"
  "  d_city     TEXT,"
  "  d_state    TEXT,"
  "  d_zip      TEXT,"
  "  d_tax      REAL,"
  "  d_ytd      REAL,"
  "  d_next_o_id INTEGER,"
  "  PRIMARY KEY (d_w_id, d_id)"
  ");"
  
  "CREATE TABLE IF NOT EXISTS customer ("
  "  c_id           INTEGER,"
  "  c_d_id         INTEGER,"
  "  c_w_id         INTEGER,"
  "  c_first        TEXT,"
  "  c_middle       TEXT,"
  "  c_last         TEXT,"
  "  c_street_1     TEXT,"
  "  c_street_2     TEXT,"
  "  c_city         TEXT,"
  "  c_state        TEXT,"
  "  c_zip          TEXT,"
  "  c_phone        TEXT,"
  "  c_since        TEXT,"
  "  c_credit       TEXT,"
  "  c_credit_lim   REAL,"
  "  c_discount     REAL,"
  "  c_balance      REAL,"
  "  c_ytd_payment  REAL,"
  "  c_payment_cnt  INTEGER,"
  "  c_delivery_cnt INTEGER,"
  "  c_data         TEXT,"
  "  PRIMARY KEY (c_w_id, c_d_id, c_id)"
  ");"
  
  "CREATE TABLE IF NOT EXISTS item ("
  "  i_id    INTEGER PRIMARY KEY,"
  "  i_im_id INTEGER,"
  "  i_name  TEXT,"
  "  i_price REAL,"
  "  i_data  TEXT"
  ");"
  
  "CREATE TABLE IF NOT EXISTS stock ("
  "  s_i_id       INTEGER,"
  "  s_w_id       INTEGER,"
  "  s_quantity   INTEGER,"
  "  s_dist_01    TEXT,"
  "  s_dist_02    TEXT,"
  "  s_dist_03    TEXT,"
  "  s_dist_04    TEXT,"
  "  s_dist_05    TEXT,"
  "  s_dist_06    TEXT,"
  "  s_dist_07    TEXT,"
  "  s_dist_08    TEXT,"
  "  s_dist_09    TEXT,"
  "  s_dist_10    TEXT,"
  "  s_ytd        INTEGER,"
  "  s_order_cnt  INTEGER,"
  "  s_remote_cnt INTEGER,"
  "  s_data       TEXT,"
  "  PRIMARY KEY (s_w_id, s_i_id)"
  ");"
  
  "CREATE TABLE IF NOT EXISTS orders ("
  "  o_id         INTEGER,"
  "  o_d_id       INTEGER,"
  "  o_w_id       INTEGER,"
  "  o_c_id       INTEGER,"
  "  o_entry_d    TEXT,"
  "  o_carrier_id INTEGER,"
  "  o_ol_cnt     INTEGER,"
  "  o_all_local  INTEGER,"
  "  PRIMARY KEY (o_w_id, o_d_id, o_id)"
  ");"
  
  "CREATE TABLE IF NOT EXISTS new_order ("
  "  no_o_id  INTEGER,"
  "  no_d_id  INTEGER,"
  "  no_w_id  INTEGER,"
  "  PRIMARY KEY (no_w_id, no_d_id, no_o_id)"
  ");"
  
  "CREATE TABLE IF NOT EXISTS order_line ("
  "  ol_o_id        INTEGER,"
  "  ol_d_id        INTEGER,"
  "  ol_w_id        INTEGER,"
  "  ol_number      INTEGER,"
  "  ol_i_id        INTEGER,"
  "  ol_supply_w_id INTEGER,"
  "  ol_delivery_d  TEXT,"
  "  ol_quantity    INTEGER,"
  "  ol_amount      REAL,"
  "  ol_dist_info   TEXT,"
  "  PRIMARY KEY (ol_w_id, ol_d_id, ol_o_id, ol_number)"
  ");"
  
  "CREATE TABLE IF NOT EXISTS history ("
  "  h_c_id   INTEGER,"
  "  h_c_d_id INTEGER,"
  "  h_c_w_id INTEGER,"
  "  h_d_id   INTEGER,"
  "  h_w_id   INTEGER,"
  "  h_date   TEXT,"
  "  h_amount REAL,"
  "  h_data   TEXT"
  ");"
  
  "CREATE INDEX IF NOT EXISTS idx_customer_name ON customer(c_w_id, c_d_id, c_last);"
  "CREATE INDEX IF NOT EXISTS idx_orders_customer ON orders(o_w_id, o_d_id, o_c_id);";

/* TPC-C constants */
#define TPCC_NUM_ITEMS      100000  /* Fixed: 100,000 items */
#define TPCC_DISTRICTS_PER_WH   10  /* 10 districts per warehouse */
#define TPCC_CUSTOMERS_PER_DIST 3000 /* 3,000 customers per district */
#define TPCC_INITIAL_ORDERS    3000 /* Initial orders per district */

/* Transaction mix percentages (TPC-C spec) */
#define TPCC_MIX_NEWORDER     45
#define TPCC_MIX_PAYMENT      43
#define TPCC_MIX_ORDERSTATUS   4
#define TPCC_MIX_DELIVERY      4
#define TPCC_MIX_STOCKLEVEL    4

#endif /* TPCC_SCHEMA_H */
