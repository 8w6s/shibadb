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
allowlist is [`abi/symbols-v1.txt`](../abi/symbols-v1.txt). Internal pager,
WAL, crypto, allocator, synchronization, and fault-injection symbols are not
exported.

The ABI test freezes public structure layouts and runtime version functions.
The `abi_symbols` test compares the shared library's actual exported symbol
table with the allowlist, in both directions: an entry exported but not listed
is a leaked internal symbol, and an entry listed but not exported is one that
lost its `SDB_API`. Symbols injected by a coverage or sanitizer runtime
(`__llvm_*`, `__gcov*`, `__asan_*`, and the like) are filtered out by prefix, so
instrumented presets are checked as strictly as a plain build. The test reports
only the differing symbols, not two full lists.

The extractor follows the library's object format rather than the host: ELF
dynamic tables and Mach-O (whose leading underscore is stripped) are read with
`nm`, and PE export directories with `objdump -p`, since a DLL carries no
dynamic symbol table for `nm -D` to read. The gate is therefore registered on
Linux, macOS, and MinGW builds; only MSVC is skipped, because the toolchain
ships neither tool. A read that yields no symbols at all fails the test rather
than passing vacuously against the allowlist.

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

