/*
 * blackbox.h
 * -----------------------------------------------------------------------
 * History storage (black box).
 *
 * Persists every detection event to a SQLite database on disk (survives reboots).
 *
 * The *table* is capped at BLACKBOX_MAX_ROWS: once full, inserting a new row
 * deletes the oldest one first, so the file doesn't grow unboundedly on a board,
 * while the *lifetime total count* (a separate running counter, persisted in
 * its own single-row table) keeps counting forever and is what /api/v1/detections/total reports.
 */

#ifndef BLACKBOX_H
#define BLACKBOX_H

#include <time.h>

#define BLACKBOX_MAX_ROWS 1000

/* Opens (creating if necessary) the SQLite DB at db_path and ensures
 * the schema exists. Returns 0 on success. Call once at startup. */
int blackbox_init(const char *db_path);

/* Records one detection event and increments the lifetime total.
 * Safe to call from any thread (uses its own internal locking around
 * the SQLite handle, since a single sqlite3* is not safe for
 * concurrent use from multiple threads without serialization). */
int blackbox_record_detection(time_t timestamp, int person_count, double cpu_temp_c);

/* Returns the lifetime total detection count (sum of person_count
 * across every recorded event, matching what /api/v1/detections/total
 * reports to stay consistent with the in-memory counter). */
long blackbox_get_total_detections(void);

/* Closes the database handle. Call at shutdown (best-effort; the
 * process is normally killed by systemd, so this mainly matters for
 * clean test runs). */
void blackbox_close(void);

#endif /* BLACKBOX_H */
