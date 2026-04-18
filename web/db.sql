-- db.sql — schema of out.db, extracted from builder.c
--
-- The compiler (polc) writes out.db in two tiers:
--
--   * RUNTIME tables  — always emitted. Enough for a datapath/simulator
--                       to answer "does this packet pass?". No symbolic
--                       names, no per-entity rows — just hashes and bitvecs.
--
--   * DEBUG tables    — emitted only when polc is run with --debug.
--                       Contain the human-facing symbolic info: entity
--                       names, label key/value strings, per-entity IPs,
--                       eid↔label membership, DNF selectors, variable defs.
--
-- The Inventory tab needs entity + entity_ip + label + eid_label, which
-- are ALL debug tables. A non-debug out.db will have no rows the
-- Inventory tab can render — the UI must detect this and prompt the
-- user to rebuild with --debug.
--
-- Encoding notes (from builder.c):
--   * EID hashes and IPs are stored as 64-bit signed INTEGER. Bit pattern
--     is preserved; SQLite may display large values as negative. Display
--     EIDs in hex in the UI.
--   * IPs are host-order uint32 packed into INTEGER. Convert to dotted
--     quad in the UI:  a.b.c.d  where  a = (ip >> 24) & 0xff, etc.
--   * `proto_mask` is a bitmask: bit 0 = TCP, bit 1 = UDP.
--
-- =====================================================================
-- RUNTIME TABLES  (always present)
-- =====================================================================

-- Compiler metadata: source filename, compile timestamp, mode flags.
CREATE TABLE meta (
  key   TEXT PRIMARY KEY,
  value TEXT NOT NULL
);

-- Enforcement Identities. One row per distinct label-set.
-- `ordinal` is a human-friendly 0-based index in parse order.
-- `bitset_hex` is the 512-bit label membership bitset as a compact hex string.
CREATE TABLE eid (
  hash       INTEGER PRIMARY KEY,
  ordinal    INTEGER UNIQUE NOT NULL,
  bitset_hex TEXT NOT NULL
);

-- Datapath's first lookup: IP → EID.
-- In non-debug DBs this is the ONLY place asset IPs appear.
CREATE TABLE ipcache (
  ip       INTEGER PRIMARY KEY,
  eid_hash INTEGER NOT NULL
);
CREATE INDEX idx_ipcache_eid ON ipcache(eid_hash);

-- Rules. src_line/src_col/source are NULL unless --debug.
CREATE TABLE rule (
  id         INTEGER PRIMARY KEY,
  action     TEXT    NOT NULL CHECK (action IN
                 ('ALLOW','BLOCK','OVERRIDE-ALLOW','OVERRIDE-BLOCK')),
  proto_mask INTEGER NOT NULL,   -- bit 0 = TCP, bit 1 = UDP
  src_line   INTEGER,
  src_col    INTEGER,
  source     TEXT,
  resolved   INTEGER NOT NULL CHECK (resolved IN (0,1))
);

-- Interned rule-bitvectors. `reserved` flags the two boundary ids:
-- id 0 = all-zeros (no rules match), id 1 = all-ones (wildcard).
CREATE TABLE bagvec (
  id       INTEGER PRIMARY KEY,
  reserved INTEGER NOT NULL CHECK (reserved IN (0,1))
);
CREATE TABLE bagvec_bit (
  bag_id  INTEGER NOT NULL,
  rule_id INTEGER NOT NULL,
  PRIMARY KEY (bag_id, rule_id)
);
CREATE INDEX idx_bagvec_bit_rule ON bagvec_bit(rule_id);

-- Four bag maps — the runtime enforcement indexes.
CREATE TABLE bag_src   ( eid_hash INTEGER PRIMARY KEY, bag_id INTEGER NOT NULL );
CREATE TABLE bag_dst   ( eid_hash INTEGER PRIMARY KEY, bag_id INTEGER NOT NULL );
CREATE TABLE bag_port  ( port     INTEGER PRIMARY KEY, bag_id INTEGER NOT NULL );
CREATE TABLE bag_proto (
  proto  TEXT PRIMARY KEY CHECK (proto IN ('TCP','UDP')),
  bag_id INTEGER NOT NULL
);

-- =====================================================================
-- DEBUG TABLES  (only present when compiled with --debug)
-- The Inventory tab needs these.
-- =====================================================================

-- Label dictionary: label_id → (key, value).
-- Label ids are 1..511 (max 511 distinct labels enforced by the compiler).
CREATE TABLE label (
  id    INTEGER PRIMARY KEY,
  key   TEXT    NOT NULL,
  value TEXT    NOT NULL,
  UNIQUE (key, value)
);
CREATE INDEX idx_label_key ON label(key);

-- One row per named asset in the source policy.
-- `name` is the Asset column in the Inventory tab.
CREATE TABLE entity (
  name     TEXT PRIMARY KEY,
  eid_hash INTEGER NOT NULL,
  src_line INTEGER,
  src_col  INTEGER
);
CREATE INDEX idx_entity_eid ON entity(eid_hash);

-- One row per (entity, ip). IPs are globally unique across the inventory.
CREATE TABLE entity_ip (
  entity_name TEXT    NOT NULL,
  ip          INTEGER NOT NULL,
  UNIQUE (ip)
);
CREATE INDEX idx_entity_ip_entity ON entity_ip(entity_name);

-- Symbolic EID ↔ label membership. Feeds the Labels chip column.
CREATE TABLE eid_label (
  eid_hash INTEGER NOT NULL,
  label_id INTEGER NOT NULL,
  PRIMARY KEY (eid_hash, label_id)
);
CREATE INDEX idx_eid_label_label ON eid_label(label_id);

-- Variable definitions (source-level $var / @var) — not needed for
-- the Inventory tab; included for schema completeness.
CREATE TABLE var_label (
  name     TEXT PRIMARY KEY,
  src_line INTEGER,
  src_col  INTEGER
);
CREATE TABLE var_label_member (
  var_name TEXT    NOT NULL,
  label_id INTEGER NOT NULL,
  ordinal  INTEGER NOT NULL,
  PRIMARY KEY (var_name, ordinal)
);
CREATE TABLE var_port (
  name     TEXT PRIMARY KEY,
  src_line INTEGER,
  src_col  INTEGER
);
CREATE TABLE var_port_member (
  var_name TEXT    NOT NULL,
  port     INTEGER NOT NULL,
  ordinal  INTEGER NOT NULL,
  PRIMARY KEY (var_name, ordinal)
);

-- Denormalized rule → port list (inverse of bag_port).
CREATE TABLE rule_port (
  rule_id INTEGER NOT NULL,
  port    INTEGER NOT NULL,
  PRIMARY KEY (rule_id, port)
);

-- DNF — symbolic selector representation. Used by the Rules tab.
CREATE TABLE rule_dnf_term (
  id        INTEGER PRIMARY KEY,
  rule_id   INTEGER NOT NULL,
  side      TEXT    NOT NULL CHECK (side IN ('src','dst')),
  term_ord  INTEGER NOT NULL,
  undefined INTEGER NOT NULL CHECK (undefined IN (0,1)),
  UNIQUE (rule_id, side, term_ord)
);
CREATE INDEX idx_dnf_rule_side ON rule_dnf_term(rule_id, side);

CREATE TABLE rule_dnf_term_label (
  term_id  INTEGER NOT NULL,
  label_id INTEGER NOT NULL,
  PRIMARY KEY (term_id, label_id)
);
CREATE INDEX idx_dnf_term_label_label ON rule_dnf_term_label(label_id);

-- Precomputed answers: which EIDs matched this rule's src/dst DNF.
CREATE TABLE rule_src_eid (
  rule_id  INTEGER NOT NULL,
  eid_hash INTEGER NOT NULL,
  PRIMARY KEY (rule_id, eid_hash)
);
CREATE INDEX idx_rule_src_eid_eid ON rule_src_eid(eid_hash);

CREATE TABLE rule_dst_eid (
  rule_id  INTEGER NOT NULL,
  eid_hash INTEGER NOT NULL,
  PRIMARY KEY (rule_id, eid_hash)
);
CREATE INDEX idx_rule_dst_eid_eid ON rule_dst_eid(eid_hash);

-- =====================================================================
-- QUERIES THE INVENTORY TAB WILL USE
-- =====================================================================
--
-- Detect whether the DB was built with --debug (i.e. has the tables we need):
--
--   SELECT name FROM sqlite_master
--    WHERE type='table' AND name IN ('entity','entity_ip','label','eid_label');
--   -- Expect 4 rows. Fewer → warn the user and offer ipcache-only fallback.
--
-- Status line counters:
--
--   SELECT COUNT(*) FROM rule;                     -- "rules: N"
--   SELECT COUNT(*) FROM entity;                   -- "inventory: N"  (debug)
--   SELECT COUNT(DISTINCT eid_hash) FROM ipcache;  -- fallback for non-debug
--
-- Main table — one row per asset, with IPs and labels aggregated:
--
--   SELECT
--     e.name                                           AS asset,
--     GROUP_CONCAT(DISTINCT ei.ip)                     AS ip_ints,
--     e.eid_hash                                       AS eid,
--     GROUP_CONCAT(DISTINCT l.key || ':' || l.value)   AS labels
--   FROM       entity     e
--   LEFT JOIN  entity_ip  ei ON ei.entity_name = e.name
--   LEFT JOIN  eid_label  el ON el.eid_hash    = e.eid_hash
--   LEFT JOIN  label      l  ON l.id           = el.label_id
--   GROUP BY   e.name, e.eid_hash
--   ORDER BY   e.name;
--
-- Convert the aggregated ip_ints / labels strings to arrays in JS,
-- and convert each IP int to dotted-quad for display.
--
-- Label-filter dropdown (distinct key:value pairs, for the checkbox list):
--
--   SELECT l.key, l.value
--   FROM   label l
--   JOIN   eid_label el ON el.label_id = l.id
--   GROUP  BY l.key, l.value
--   ORDER  BY l.key, l.value;
