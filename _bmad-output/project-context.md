---
project_name: 'mycoflow-core'
user_name: 'Baris'
date: '2026-02-18'
sections_completed: ['technology_stack', 'language_rules', 'module_architecture', 'testing_rules', 'quality_rules', 'anti_patterns']
status: 'complete'
rule_count: 38
optimized_for_llm: true
---

# Project Context for AI Agents

_This file contains critical rules and patterns that AI agents must follow when implementing code in this project. Focus on unobvious details that agents might otherwise miss._

---

## Technology Stack & Versions

- **Language:** C11 (`-std=c11`, `-Wall -Wextra`)
- **Build system:** CMake 3.10+
- **Compiler (host):** GCC (cross: `aarch64-openwrt-linux-musl-gcc`)
- **Compiler (eBPF):** Clang (`-O2 -target bpf`)
- **Target platform:** OpenWrt (musl libc, aarch64 / MT7981B)
- **Always-linked libs:** libm (`-lm`), pthreads (`Threads::Threads`)
- **Conditional libs:** libubus + libubox + blobmsg_json (guard: `HAVE_UBUS`),
  libbpf + libelf + zlib (guard: `HAVE_LIBBPF`)
- **Test framework:** minunit.h (project-local minimal framework, NOT gtest/cmocka)
- **Simulation:** Docker Compose — `mycoflow-openwrt` + `mycoflow-client{1,2}`
  on bridge net `10.10.10.0/24`
- **LuCI frontend:** JavaScript (OpenWrt LuCI framework v2 view API)
- **Scripting:** Lua / ucode (`myco_bridge.lua`, `myco_bridge.uc`)

## Critical Implementation Rules

### Language-Specific Rules (C11)

- **Header guards:** Always use `#ifndef MYCO_{MODULE}_H` / `#define MYCO_{MODULE}_H` /
  `#endif /* MYCO_{MODULE}_H */` — include the comment on `#endif`
- **Type naming:** All typedef'd types end with `_t` (e.g., `myco_config_t`, `persona_t`,
  `log_level_t`). Enums follow `TYPE_VALUE` uppercase pattern (e.g., `PERSONA_INTERACTIVE`,
  `LOG_ERROR`)
- **`myco_types.h` is the single source of truth** for all shared structs, enums, and extern
  declarations. Never duplicate type definitions in individual module headers
- **Global state lives in `main.c` only** — declared there, `extern`'d in `myco_types.h`.
  Never define globals in module `.c` files
- **Signal-safe globals** use `volatile sig_atomic_t` (e.g., `g_stop`, `g_reload`)
- **String formatting safety:** Use `snprintf` (never `sprintf`) for any buffer writes,
  especially when constructing shell commands with interface names or host addresses
- **No `system()` with untrusted input** — shell commands (`tc`, `ping`) must validate
  interface names and hostnames before passing to `system()`; prefer `execvp`/`execv` families
  where feasible
- **C11 standard, no GNU extensions** — do not use `__attribute__` GCC-isms unless wrapped
  in a feature macro; code must compile with `-Wall -Wextra` warning-clean
- **Math functions** require explicit `-lm` link; `fabs()`, `pow()`, etc. are in `<math.h>`

### Module Architecture & OpenWrt-Specific Rules

- **Module file pattern:** Every module is a `myco_{name}.c` + `myco_{name}.h` pair in `src/`.
  New modules must be added to `SOURCES` in `src/CMakeLists.txt` and have their own test
  executable registered with `add_executable` + `add_test`
- **Module header includes only `myco_types.h`** — never include other module headers in a
  `.h` file; cross-module dependencies go in the `.c` file only
- **Conditional compilation for platform-specific code:** Wrap OpenWrt-only code with
  `#ifdef HAVE_UBUS` and provide a no-op `static inline` stub in the `#else` branch
  (see `myco_ubus.h` as the canonical pattern)
- **ubus API surface** (8 methods): `myco.status`, `myco.persona` (list/add/delete),
  `myco.policy` (get/set/boost/throttle) — adding new methods requires updating the
  rpcd ACL at `luci-app-mycoflow/root/usr/share/rpcd/acl.d/luci-app-mycoflow.json`
- **JSON fallback (`myco_dump_json`):** Always keep the non-ubus JSON dump path working —
  it is the fallback used in Docker simulation where ubus is unavailable
- **Thread safety:** All shared globals (`g_last_metrics`, `g_last_policy`, `g_last_persona`,
  `g_persona_override`, etc.) must be accessed under `g_state_mutex` from any thread other
  than the main loop; the ubus thread is the primary second thread
- **Config hierarchy:** UCI → environment variables → compiled-in defaults. Never hard-code
  values that belong in config; always respect `config_reload()` for hot-reload via SIGHUP
- **Resource budget (hard limits):** CPU target <20% (peak 40%), RAM <64 MB — cap collection
  buffers and use LRU eviction in any new sensing or flow-table code
- **eBPF object file** `mycoflow.bpf.o` is built by CMake via `add_custom_command` with
  clang; it is a separate artefact, not linked into the daemon binary
- **OpenWrt Makefile** (`luci-app-mycoflow/Makefile`) uses `$(TOPDIR)/feeds/luci/luci.mk`
  — follow OpenWrt SDK conventions, not standard GNU make patterns
- **Install paths on-router:** daemon binary → `/usr/sbin/mycoflowd`; LuCI view JS →
  `/www/luci-static/resources/view/mycoflow.js`; menu → `/usr/share/luci/menu.d/`

### Testing Rules

- **Test framework is minunit.h** (project-local, `src/minunit.h`) — NOT gtest, cmocka, or
  Unity. Each test file must declare `int tests_run;` and use `mu_assert` / `mu_run_test`
- **Test file naming:** `test_{module}.c` in `src/tests/`. Each test file compiles into its
  own executable (e.g., `test_ewma`, `test_control`)
- **CMakeLists registration:** Every new test executable needs both `add_executable` and
  `add_test` in `src/CMakeLists.txt`. Link only the module(s) under test plus `myco_log.c`
  and `myco_config.c` if needed — not the full `SOURCES` list
- **Module isolation:** Tests compile individual `.c` files directly (e.g.,
  `test_ewma.c myco_ewma.c`) — do not link the full daemon. Modules under test must not
  have undefined symbol dependencies beyond what's explicitly linked
- **Run tests with CTest:** `cmake --build build && ctest --test-dir build` — do not run
  test executables directly unless debugging a single test
- **Dummy-metric harness:** `cfg.dummy_metrics = 1` exercises the control loop without live
  network — use this flag in tests that exercise `control_decide` or `act_apply`
- **No mocking framework** — test doubles are done by compiling stub `.c` files or using
  the `dummy_metrics` config flag; do not introduce an external mock library

### Code Quality & Style Rules

- **File header comment block:** Every `.c` and `.h` file starts with:
  `/* MycoFlow — Bio-Inspired Reflexive QoS System\n * {filename} — {one-line description}\n */`
- **Function naming:** `{module}_{verb}` snake_case (e.g., `config_load`, `sense_collect`,
  `persona_update`, `act_apply`). Public API functions declared in `.h`; file-local helpers
  are `static`
- **Section dividers** use the pattern `/* ── Section Name ───── */` (em-dash + spaces) —
  keep this style in `myco_types.h` and long headers for readability
- **No global `#include` dumping** — each `.c` includes only what it uses; system headers
  go in `.c` files, not `.h` files (except those already pulled in via `myco_types.h`)
- **Logging:** always `log_msg(level, "MODULE", "fmt", ...)` — never `printf`/`fprintf` in
  daemon code (only `fprintf(stderr, ...)` acceptable in `main()` before log is initialised)
- **Error returns:** `int` (0 = success, -1 = error) or typed sentinel (`NULL`,
  `PERSONA_UNKNOWN`) — be consistent with what the module already uses
- **`clamp_double` and `now_monotonic_s`** are declared in `myco_types.h` and defined in
  `main.c` — use them; do not reimplement clamping or monotonic time in modules
- **No dynamic allocation in the hot path** — the control loop runs at ≤2 Hz on a
  resource-constrained router; prefer stack allocation and fixed-size arrays

### Critical Don't-Miss Rules

**Anti-Patterns to Avoid:**
- **Never refactor to a single-file daemon** — the project was intentionally split into
  `myco_{module}.c` files; don't consolidate back into `main.c`
- **Never use `sleep()` for the control loop interval** — use `nanosleep()` via the
  existing `sleep_interval(double seconds)` helper in `main.c`
- **Never skip the `g_stop` check** inside any long-running or re-entrant loop — the
  daemon must exit cleanly on SIGTERM/SIGINT
- **Never write to shared globals without the mutex** from a non-main thread — even a
  single `int` write can race with the ubus thread reading `g_last_policy`

**Edge Cases:**
- **EWMA filter must be initialised before first use** — call `ewma_init(&filter, alpha)`
  at startup; reading an uninitialised filter returns undefined values
- **Persona k-of-m requires `history_len` to reach `m` before voting** — do not act on
  persona decisions until the history buffer is full
- **`myco_dump_json()` is always compiled in** (no `#ifdef`) — it is the Docker/dev fallback
  and must remain functional even when `HAVE_UBUS` is defined
- **Config field `no_tc = 1`** disables actual `tc` calls (dry-run mode) — always check
  this flag in `act_apply` before executing shell commands; test code must set it

**Security Rules:**
- Validate `egress_iface` and `probe_host` from config before use in shell commands —
  alphanumeric + `.` + `-` + `_` only; reject anything else
- ubus ACL is least-privilege: read-only methods (`status`, `persona list`) must not require
  write permissions in the ACL JSON

**Performance Gotchas:**
- `sense_collect()` includes a blocking ICMP ping — keep `sample_hz` ≤ 2 and never call
  sense from the ubus thread
- Flow table (`flow_table_t`) has fixed capacity — inserting beyond it silently drops
  entries; always check return value and enforce the LRU cap

---

## Usage Guidelines

**For AI Agents:**
- Read this file before implementing any code in this project
- Follow ALL rules exactly as documented — especially conditional compilation stubs,
  mutex usage, and `no_tc` flag in tests
- When in doubt, prefer the more restrictive option
- Update this file if new patterns or modules emerge

**For Humans:**
- Keep this file lean and focused on agent needs
- Update when technology stack or module structure changes
- Remove rules that become obvious over time

Last Updated: 2026-02-18
