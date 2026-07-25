# C ABI v1

ShibaDB 0.1.0 establishes C ABI version 1. Call `sdb_abi_version` at runtime
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
The symbol test compares the actual dynamic symbol table byte-for-byte with
the allowlist.

## CMake installation

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /desired/prefix
```

Consumers can choose either installed target:

```cmake
find_package(ShibaDB 0.1 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE ShibaDB::shared)
# or ShibaDB::core for the static library
```

The install also provides `shibadb.pc`:

```sh
cc app.c $(pkg-config --cflags --libs shibadb)
```

The shared library has `SOVERSION 1`; the build produces
`libshibadb.so.1`, the corresponding macOS install name, or `shibadb.dll`.

