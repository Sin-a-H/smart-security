#include "blackbox.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

static sqlite3 *g_db = NULL;
static pthread_mutex_t g_db_mutex = PTHREAD_MUTEX_INITIALIZER;

int blackbox_init(const char *db_path)
{
    int rc = sqlite3_open(db_path, &g_db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[blackbox] failed to open %s: %s\n", db_path, sqlite3_errmsg(g_db));
        return -1;
    }

    const char *schema =
        "CREATE TABLE IF NOT EXISTS detections ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  timestamp INTEGER NOT NULL,"
        "  person_count INTEGER NOT NULL,"
        "  cpu_temp_c REAL"
        ");"
        "CREATE TABLE IF NOT EXISTS lifetime_stats ("
        "  id INTEGER PRIMARY KEY CHECK (id = 1),"
        "  total_detections INTEGER NOT NULL DEFAULT 0"
        ");"
        "INSERT OR IGNORE INTO lifetime_stats (id, total_detections) VALUES (1, 0);";

    char *errmsg = NULL;
    rc = sqlite3_exec(g_db, schema, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[blackbox] schema creation failed: %s\n", errmsg);
        sqlite3_free(errmsg);
        return -1;
    }

    /* WAL mode gives better concurrent read/write behavior for a long-running process. */
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);

    return 0;
}

int blackbox_record_detection(time_t timestamp, int person_count, double cpu_temp_c)
{
    if (!g_db) return -1;

    pthread_mutex_lock(&g_db_mutex);

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(g_db,
        "INSERT INTO detections (timestamp, person_count, cpu_temp_c) VALUES (?, ?, ?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[blackbox] insert prepare failed: %s\n", sqlite3_errmsg(g_db));
        pthread_mutex_unlock(&g_db_mutex);
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)timestamp);
    sqlite3_bind_int(stmt, 2, person_count);
    sqlite3_bind_double(stmt, 3, cpu_temp_c);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "[blackbox] insert failed: %s\n", sqlite3_errmsg(g_db));
        pthread_mutex_unlock(&g_db_mutex);
        return -1;
    }

    /* Update lifetime total (persisted, unbounded counter). */
    char update_sql[128];
    snprintf(update_sql, sizeof(update_sql),
             "UPDATE lifetime_stats SET total_detections = total_detections + %d WHERE id = 1",
             person_count);
    sqlite3_exec(g_db, update_sql, NULL, NULL, NULL);

    /* Circular buffer: cap the detections table at BLACKBOX_MAX_ROWS by
     * deleting the oldest rows once we exceed it, so the on-disk table
     * doesn't grow unboundedly on a board that runs for months. */
    char trim_sql[256];
    snprintf(trim_sql, sizeof(trim_sql),
             "DELETE FROM detections WHERE id NOT IN "
             "(SELECT id FROM detections ORDER BY id DESC LIMIT %d)",
             BLACKBOX_MAX_ROWS);
    sqlite3_exec(g_db, trim_sql, NULL, NULL, NULL);

    pthread_mutex_unlock(&g_db_mutex);
    return 0;
}

long blackbox_get_total_detections(void)
{
    if (!g_db) return 0;

    pthread_mutex_lock(&g_db_mutex);
    long total = 0;
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(g_db, "SELECT total_detections FROM lifetime_stats WHERE id = 1",
                                 -1, &stmt, NULL);
    if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        total = (long)sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
    return total;
}

void blackbox_close(void)
{
    pthread_mutex_lock(&g_db_mutex);
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
    }
    pthread_mutex_unlock(&g_db_mutex);
}
