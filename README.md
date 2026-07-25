# ShibaDB

Embedded ACID key-value database in C11.

## Requirements

- C11 compiler (GCC, Clang, MSVC)
- CMake >= 3.20
- POSIX or Windows

## Build from source

```bash
git clone https://github.com/8w6s/shibadb.git
cd shibadb
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

## Run tests

```bash
ctest --test-dir build --output-on-failure
```

## System install

```bash
sudo cmake --install build
```

Installs headers to `<prefix>/include/shibadb.h`, the static library
to `<prefix>/lib/`, and the CMake / pkg-config files.

## Use in a CMake project

```cmake
find_package(shibadb CONFIG REQUIRED)
target_link_libraries(myapp PRIVATE shibadb::shibadb)
```

## Use with pkg-config

```bash
cc myapp.c $(pkg-config --cflags --libs shibadb) -o myapp
```

## License

See [LICENSE](LICENSE).
