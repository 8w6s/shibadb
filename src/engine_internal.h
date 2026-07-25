#ifndef SHIBADB_ENGINE_INTERNAL_H
#define SHIBADB_ENGINE_INTERNAL_H

#include "file.h"
#include "shibadb_engine.h"

#if SDB_TESTING
sdb_file *sdb_database_file_for_testing(sdb_database *database);
void sdb_engine_crash_after_compact_phase_for_testing(unsigned phase);
void sdb_engine_crash_after_backup_phase_for_testing(unsigned phase);
void sdb_engine_backup_file_fail_after_for_testing(size_t boundary);
void sdb_engine_backup_file_clear_failure_for_testing(void);
#endif

#endif
