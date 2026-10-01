# C ABI v1

ShibaDB 1.0.0 establishes C ABI version 1. Call `sdb_abi_version` at runtime
and compare it with `SDB_ABI_VERSION` when loading the shared library
dynamically.

## Compatibility policy

Within ABI major version 1:

- existing exported functions, calling conventions, enum values, parameter
  types, and documented behavior will not be removed or changed;
- existing public structure sizes, alignment, field offsets, and meanings are
  frozen per supported platform ABI;
- reserved fields remain zero until assigned a documented meaning;
- new functions and new enum values may be added in backward-compatible minor
  releases;
- an incompatible change requires ABI version 2 and a new shared-library
  `SOVERSION`.

`sdb_database_options.struct_size` must be initialized through
`sdb_database_options_init`. ABI v1 accepts a larger future options structure
while reading only the v1 prefix. Reserved options must be zero.

The file-format version and C ABI version are independent. A future library
can retain ABI v1 while adding an explicitly migrated file format.

## Exported surface

Shared builds use hidden visibility by default. The authoritative ABI v1
allowlist is [`abi/symbols-v1.txt`](../abi/symbols-v1.txt) — 72 symbols.
Internal pager, WAL, crypto, allocator, synchronization, and fault-injection
symbols are not exported.

Two CTest cases enforce this, and they cover different halves:

- `abi` (`tests/test_abi.c`) freezes public structure layouts, sizes and field
  offsets at compile time via `_Static_assert`, plus the runtime version
  functions. It is a *compile-time* gate: it cannot see the symbol table.
- `abi_symbols` (`tests/check_symbols.cmake`) compares the shared library's
  exported symbol set against the allowlist, exactly and in **both**
  directions. A symbol exported but missing from the allowlist fails the build
  (unreviewed ABI growth); an allowlisted symbol that stopped being exported
  fails it too (ABI break).

Comparison normalises away platform noise so one allowlist serves every target:
the Mach-O leading underscore is stripped, and only *defined external* symbols
matching `^sdb_` are considered, which excludes synthesised runtime symbols
(`_init`, `_fini`, `__bss_start`, `__mh_execute_header`) and keeps the gate
correct even when it has to fall back to a plain `nm` that also lists hidden
local functions. The dumper is resolved in the order `llvm-nm`, `nm`, `dumpbin`
so Linux, macOS and MSVC need no per-platform wiring; pass `-DNM=` to override
for a cross-build.

Adding a public entry point therefore takes two edits, not one: declare it with
`SDB_API` and add the name to `abi/symbols-v1.txt`. Forgetting the second is
now a build failure rather than a silent widening of the ABI.

## CMake installation

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /desired/prefix
```

Consumers can choose either installed target:

```cmake
find_package(ShibaDB 1.0 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE ShibaDB::shared)
# or ShibaDB::core for the static library
```

The install also provides `shibadb.pc`:

```sh
cc app.c $(pkg-config --cflags --libs shibadb)
```

The shared library has `SOVERSION 1`; the build produces
`libshibadb.so.1`, the corresponding macOS install name, or `shibadb.dll`.

