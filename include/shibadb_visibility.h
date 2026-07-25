#ifndef SHIBADB_VISIBILITY_H
#define SHIBADB_VISIBILITY_H

#if defined(_WIN32) && defined(SDB_SHARED)
#if defined(SDB_BUILDING_LIBRARY)
#define SDB_API __declspec(dllexport)
#else
#define SDB_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define SDB_API __attribute__((visibility("default")))
#else
#define SDB_API
#endif

#endif
