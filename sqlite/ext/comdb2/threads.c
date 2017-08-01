/*
   Copyright 2017 Bloomberg Finance L.P.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 */

#if (!defined(SQLITE_CORE) || defined(SQLITE_BUILDING_FOR_COMDB2)) &&          \
    !defined(SQLITE_OMIT_VIRTUALTABLE)

#if defined(SQLITE_BUILDING_FOR_COMDB2) && !defined(SQLITE_CORE)
#define SQLITE_CORE 1
#endif

#include <assert.h>
#include "comdb2.h"
#include "comdb2systblInt.h"
#include "thrman.h"
#include "list.h"

/*
  comdb2_threads:
  Query various attributes of all the threads create by Comdb2.

  TODO: Check user permissions
*/

extern LISTC_T(struct thr_handle) gbl_threads;

typedef struct systbl_threads_cursor {
    sqlite3_vtab_cursor base;  /* Base class - must be first */
    sqlite3_int64 rowid;       /* Row ID */
    struct thr_handle *thread; /* Current thread. */
    time_t time;               /* Current time. */
} systbl_threads_cursor;

/* Column numbers (always keep the below table definition in sync). */
enum {
    THREADS_COLUMN_TID,
    THREADS_COLUMN_TYPE,
    THREADS_COLUMN_UPTIME,
    THREADS_COLUMN_STATUS,
};

static int systblThreadsConnect(sqlite3 *db, void *pAux, int argc,
                                const char *const *argv, sqlite3_vtab **ppVtab,
                                char **pErr)
{
    int rc;

    rc = sqlite3_declare_vtab(db, "CREATE TABLE comdb2_threads(\"thread_id\", "
                                  "\"type\", \"uptime\", \"status\")");

    if (rc == SQLITE_OK) {
        if ((*ppVtab = sqlite3_malloc(sizeof(sqlite3_vtab))) == 0) {
            return SQLITE_NOMEM;
        }
        memset(*ppVtab, 0, sizeof(*ppVtab));
    }

    return 0;
}

static int systblThreadsBestIndex(sqlite3_vtab *tab,
                                  sqlite3_index_info *pIdxInfo)
{
    return SQLITE_OK;
}

static int systblThreadsDisconnect(sqlite3_vtab *pVtab)
{
    sqlite3_free(pVtab);
    return SQLITE_OK;
}

static int systblThreadsOpen(sqlite3_vtab *p, sqlite3_vtab_cursor **ppCursor)
{
    systbl_threads_cursor *cur = sqlite3_malloc(sizeof(systbl_threads_cursor));
    if (cur == 0) {
        return SQLITE_NOMEM;
    }
    memset(cur, 0, sizeof(*cur));
    *ppCursor = &cur->base;

    /* Unlocked in systblThreadsClose() */
    thrman_lock();
    cur->thread = LISTC_TOP(&gbl_threads);

    /* Get the current time */
    time(&cur->time);

    return SQLITE_OK;
}

static int systblThreadsClose(sqlite3_vtab_cursor *cur)
{
    thrman_unlock();
    sqlite3_free(cur);
    return SQLITE_OK;
}

static int systblThreadsFilter(sqlite3_vtab_cursor *pVtabCursor, int idxNum,
                               const char *idxStr, int argc,
                               sqlite3_value **argv)
{
    systbl_threads_cursor *pCur = (systbl_threads_cursor *)pVtabCursor;
    pCur->rowid = 0;
    return SQLITE_OK;
}

static int systblThreadsNext(sqlite3_vtab_cursor *cur)
{
    systbl_threads_cursor *pCur = (systbl_threads_cursor *)cur;
    pCur->thread = thrman_next_thread(pCur->thread);
    pCur->rowid++;
    return SQLITE_OK;
}

static int systblThreadsEof(sqlite3_vtab_cursor *cur)
{
    systbl_threads_cursor *pCur = (systbl_threads_cursor *)cur;
    return (pCur->thread == NULL) ? 1 : 0;
}

/*
  Calculates and presents the uptime in a user friendly format.
  The returned string must be freed by the caller.
*/
static const char *uptime(time_t start, time_t end)
{
    const char *err_time = "--m --s";
    char buf[14];
    int diff;
    int rc;

    diff = end - start;
    rc = snprintf(buf, sizeof(buf), "%dm %ds", (int)(diff / 60), diff % 60);

    return strdup((rc == -1) ? err_time : buf);
}

static int systblThreadsColumn(sqlite3_vtab_cursor *cur, sqlite3_context *ctx,
                               int pos)
{
    /* We already have a lock on gbl_threads. */
    systbl_threads_cursor *pCur = (systbl_threads_cursor *)cur;
    struct thr_handle *thread = pCur->thread;

    switch (pos) {
    case THREADS_COLUMN_TID:
        sqlite3_result_int64(ctx, thrman_get_tid(thread));
        break;
    case THREADS_COLUMN_TYPE:
        sqlite3_result_text(ctx, thrman_type2a(thrman_get_type(thread)), -1,
                            NULL);
        break;
    case THREADS_COLUMN_UPTIME: {
        sqlite3_result_text(ctx, uptime(thrman_get_when(thread), pCur->time),
                            -1, free);
        break;
    }
    case THREADS_COLUMN_STATUS:
        sqlite3_result_text(ctx, thrman_get_where(thread), -1, NULL);
        break;
    default: assert(0);
    };

    return SQLITE_OK;
}

static int systblThreadsRowid(sqlite3_vtab_cursor *cur, sqlite_int64 *pRowid)
{
    systbl_threads_cursor *pCur = (systbl_threads_cursor *)cur;
    *pRowid = pCur->rowid;

    return SQLITE_OK;
}

const sqlite3_module systblThreadsModule = {
    0,                       /* iVersion */
    0,                       /* xCreate */
    systblThreadsConnect,    /* xConnect */
    systblThreadsBestIndex,  /* xBestIndex */
    systblThreadsDisconnect, /* xDisconnect */
    0,                       /* xDestroy */
    systblThreadsOpen,       /* xOpen - open a cursor */
    systblThreadsClose,      /* xClose - close a cursor */
    systblThreadsFilter,     /* xFilter - configure scan constraints */
    systblThreadsNext,       /* xNext - advance a cursor */
    systblThreadsEof,        /* xEof - check for end of scan */
    systblThreadsColumn,     /* xColumn - read data */
    systblThreadsRowid,      /* xRowid - read data */
    0,                       /* xUpdate */
    0,                       /* xBegin */
    0,                       /* xSync */
    0,                       /* xCommit */
    0,                       /* xRollback */
    0,                       /* xFindMethod */
    0,                       /* xRename */
};

#endif /* (!defined(SQLITE_CORE) || defined(SQLITE_BUILDING_FOR_COMDB2))       \
          && !defined(SQLITE_OMIT_VIRTUALTABLE) */
