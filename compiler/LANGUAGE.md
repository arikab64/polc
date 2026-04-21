# Specification for the gc Policy Language

**Version:** 1.1 (Target State)  
**Status:** Canonical Reference for `polc`

---

## 1. Lexical Structure

### 1.1 Character Set
Source files are encoded in UTF-8. The language is case-sensitive. Keywords are consistently uppercase; section headers are consistently lowercase.

### 1.2 Comments
Line comments begin with either `#` (shell-style) or `//` (C-style) and continue to the end of the line. Comments are treated as whitespace.

### 1.3 Identifiers and Literals
| Category | Terminal | Pattern / Description |
| :--- | :--- | :--- |
| **Name** | `NAME` | `[A-Za-z_][A-Za-z0-9_.-]*` |
| **Number** | `NUMBER` | `[0-9]+` (unsigned 16-bit range for ports) |
| **IP Address** | `IP` | `d.d.d.d` (IPv4 dotted-quad) |
| **Label Var** | `VAR_LABEL` | `$` followed by `NAME` |
| **Port Var** | `VAR_PORT` | `@` followed by `NAME` |

### 1.4 Keywords
*   **Actions:** `ALLOW`, `BLOCK`, `OVERRIDE-ALLOW`, `OVERRIDE-BLOCK`
*   **Operators:** `AND`, `OR`
*   **Protocols:** `TCP`, `UDP`
*   **Wildcard:** `ANY`

---

## 2. Program Structure

A `gc` program consists of three optional sections that must appear in the following order:

1.  **`vars:`** Macro definitions for labels and ports.
2.  **`inventory:`** Mapping of physical IP addresses to logical entity names and labels.
3.  **`policy:`** The set of enforcement rules.

---

## 3. Variable Expansion (`vars`)

Variables provide a macro expansion layer. They are resolved at parse-time and do not exist in the compiled IR. Redefinition of a variable is a compilation error.

### 3.1 Label Variables (`$`)
Assigned a single label (`key:value`) or a whitespace-separated list of labels enclosed in brackets `[...]`. When expanded in a selector, a list is treated as a disjunction (`OR`).

### 3.2 Port Variables (`@`)
Assigned a comma-separated list of port items. A port item may be a `NUMBER`, a range `NUMBER-NUMBER`, or the `ANY` keyword.

---

## 4. Inventory Definition

The inventory section populates the **Identity Cache (ipcache)**.

```ebnf
entity ::= NAME "[" IP+ "]" "=>" "[" (label | VAR_LABEL)+ "]" ";"
label  ::= NAME ":" (NAME | IP | "ANY")
```

*   **Enforcement Identity (EID):** Entities sharing an identical set of labels are collapsed into a single EID.
*   **Uniqueness:** IP addresses must be unique across the entire inventory.

---

## 5. Policy Rules

### 5.1 Rule Syntax
```ebnf
rule   ::= action side "->" side ":" port_spec ":" proto_spec ";"
action ::= "ALLOW" | "BLOCK" | "OVERRIDE-ALLOW" | "OVERRIDE-BLOCK"
```

### 5.2 Side Definition
A "side" (source or destination) defines the match criteria for a packet.

| Form | Internal Representation (`L`, `S_and`, `S_or`) |
| :--- | :--- |
| `ANY` | `ALL_EIDS`, `and-ANY`, `or-ANY` |
| `selector` | `selector`, `and-ANY`, `or-ANY` |
| `selector AND subnet` | `selector`, `subnet`, `or-ANY` |
| `selector OR subnet` | `selector`, `and-ANY`, `subnet` |
| `subnet` | `ALL_EIDS`, `subnet`, `or-ANY` |

*   **`ANY` Keyword:** A whole-side wildcard matching every packet.
*   **Subnet Clauses:** Boolean trees over CIDR leaves (`IP` or `IP/PREFIX`).

### 5.3 Ports and Protocols
*   **`port_spec`:** A comma-separated list of ports/ranges, or `ANY`. Port `0` is a numeric alias for `ANY` but cannot be mixed in lists.
*   **`proto_spec`:** `TCP`, `UDP`, or `ANY` (which expands to `TCP,UDP`).

---

## 6. Formal Semantics

### 6.1 The Match Function
A rule matches a packet `p` if the following predicate is true:
`match_side(src_side, src(p)) ∧ match_side(dst_side, dst(p)) ∧ dport(p) ∈ ports ∧ proto(p) ∈ protos`

The side match is defined as:
`match_side((L, S_and, S_or), addr) ≡ (eid(addr) ∈ L ∧ addr ∈ S_and) ∨ addr ∈ S_or`

### 6.2 EID Resolution and ANY_EID
The function `eid(addr)` returns the EID associated with an IP.
*   **Unresolved IPs:** If an IP is not in the inventory, it returns `ANY_EID`.
*   **Permission:** `ANY_EID` is a member of every possible label set `L`. Unresolved IPs match any label selector and can only be excluded via an `and-subnet`.

### 6.3 Sentinel Values
To maintain the match formula, the compiler inserts identity elements for missing clauses:
*   **`and-ANY`**: Range `0.0.0.1 - 255.255.255.255`. The identity for intersection (`AND`).
*   **`or-ANY`**: `0.0.0.0/32`. The identity for union (`OR`).

---

## 7. Grammar (EBNF)

```ebnf
program        = [ vars_sec ] [ inv_sec ] [ policy_sec ] ;

vars_sec       = "vars:" { var_def } ;
var_def        = VAR_LABEL "=" ( label_item | "[" { label_item } "]" ) ";"
               | VAR_PORT "=" port_val ";" ;

inv_sec        = "inventory:" { entity } ;
entity         = NAME "[" { IP } "]" "=>" "[" { label_ref } "]" ";" ;

policy_sec     = "policy:" { rule } ;
rule           = action side "->" side ":" port_spec ":" proto_spec ";" ;

side           = "ANY"
               | selector [ ( "AND" | "OR" ) subnet_clause ]
               | subnet_expr ;

selector       = and_expr { "OR" and_expr } ;
and_expr       = primary_sel { "AND" primary_sel } ;
primary_sel    = NAME ":" value | VAR_LABEL | "(" selector ")" | "[" { label_ref } "]" ;

subnet_clause  = cidr | "(" subnet_expr ")" ;
subnet_expr    = sub_and { "OR" sub_and } ;
sub_and        = sub_prim { "AND" sub_prim } ;
sub_prim       = cidr | "(" subnet_expr ")" ;

cidr           = IP [ "/" NUMBER ] ;
port_spec      = "ANY" | port_item { "," port_item } ;
proto_spec     = "ANY" | proto { "," proto } ;
```

---

## 8. Diagnostics

| Code | Severity | Description |
| :--- | :--- | :--- |
| `E001` | Error | Undefined variable usage. |
| `E002` | Error | Duplicate IP assignment in inventory. |
| `E003` | Error | Port range reversal or out of bounds (0-65535). |
| `W001` | Warning | Non-canonical CIDR (host bits set). |
| `W002` | Warning | Unresolved rule (selector matches no entities). |

---

## 9. Examples

### 9.1 Variables
```
vars:
  $production  = env:production;
  $web-tier    = [ app:web-front app:api-gateway ];
  @secure-web  = 443, 8443;
  @api-range   = 8000-8080;
```

### 9.2 Inventory
```
inventory:
  # Single IP with multiple labels including a variable
  web-prod-1 [10.0.0.1] => [ app:web-front $production role:server ];

  # Multi-IP entity
  api-prod-cluster [10.0.2.20 10.0.2.21] => [ app:api-backend $production ];
```

### 9.3 Policy Rules

**Label-based matching:**
```
# Simple label match
ALLOW app:web-front -> app:api-backend :80 :TCP;

# Using variables and boolean logic
ALLOW $web-tier AND $production -> role:database :@api-range :TCP;
```

**Subnet narrowing and broadening:**
```
# AND-subnet: Only match app:db if IP is within 10.0.0.0/16
ALLOW app:db AND 10.0.0.0/16 -> env:qa :80 :TCP;

# OR-subnet (Bypass): Match if EID is app:db OR if IP is 10.0.0.1
ALLOW app:db OR 10.0.0.1 -> env:qa :80 :TCP;
```

**Pure IP matching (No labels):**
```
# Match by source IP regardless of EID
ALLOW 192.168.1.50 -> 10.0.0.0/24 :ANY :ANY;
```

**Complex Subnets (Parentheses required):**
```
# Multiple IPs in an and-subnet
ALLOW app:api-backend AND (10.0.2.20 OR 10.0.2.21) -> role:database :5432 :TCP;
```

**Wildcards and Overrides:**
```
# Match everything to a specific destination
ALLOW ANY -> role:monitoring :ANY :UDP;

# Global block with high precedence
OVERRIDE-BLOCK $production -> env:staging :ANY :ANY;
```

