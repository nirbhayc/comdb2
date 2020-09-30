/*
   Copyright 2020 Bloomberg Finance L.P.

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

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "comdb2.h"
#include "comdb2systbl.h"
#include "comdb2systblInt.h"
#include "ezsystables.h"
#include "sql.h"

typedef struct {
    char *seed;
    char *command;
    cdb2_client_datetime_t added;
} sc_queue_ent;

static int get_sc_queue_data(void **data, int *npoints)
{
    int rc, bdberr, row_count;
    sc_queue_row *rows;
    sc_queue_ent *sc_queue_ents = NULL;

    tran_type *trans = curtran_gettran();
    if (trans == NULL) {
        logmsg(LOGMSG_ERROR, "%s: cannot create transaction object\n",
               __func__);
        return SQLITE_INTERNAL;
    }

    rc = bdb_llmeta_get_sc_queue(trans, &rows, &row_count, &bdberr);
    if (rc || bdberr) {
        logmsg(LOGMSG_ERROR, "%s: failed to get all schema change queue\n",
               __func__);
        return SQLITE_INTERNAL;
    }

    sc_queue_ents = calloc(row_count, sizeof(sc_queue_ent));
    if (sc_queue_ents == NULL) {
        logmsg(LOGMSG_ERROR, "%s: failed to malloc\n", __func__);
        rc = SQLITE_NOMEM;
        goto cleanup;
    }

    for (int i = 0; i < row_count; i++) {
        dttz_t d;

        d = (dttz_t){.dttz_sec = rows[i].added / 1000,
                     .dttz_frac = rows[i].added - (rows[i].added / 1000 * 1000),
                     .dttz_prec = DTTZ_PREC_MSEC};
        dttz_to_client_datetime(
            &d, "UTC", (cdb2_client_datetime_t *)&(sc_queue_ents[i].added));

        char seed[20];
        sprintf(seed, "%0#16" PRIx64, flibc_htonll(rows[i].seed));
        sc_queue_ents[i].seed = strdup(seed);

        sc_queue_ents[i].command = strdup(rows[i].command);
    }

    *npoints = row_count;
    *data = sc_queue_ents;

cleanup:
    curtran_puttran(trans);
    free(rows);

    return rc;
}

static void free_sc_queue_data(void *p, int n)
{
    sc_queue_ent *sc_queue_ents = p;
    for (int i = 0; i < n; i++) {
        free(sc_queue_ents[i].seed);
        free(sc_queue_ents[i].command);
    }
    free(sc_queue_ents);
}

sqlite3_module systblScQueueModule = {
    .access_flag = CDB2_ALLOW_USER,
};

// clang-format off
int systblScQueueInit(sqlite3 *db)
{
    return create_system_table(
        db, "comdb2_sc_queue", &systblScQueueModule,
        get_sc_queue_data, free_sc_queue_data, sizeof(sc_queue_ent),
        CDB2_CSTRING, "seed", -1, offsetof(sc_queue_ent, seed),
        CDB2_CSTRING, "command", -1, offsetof(sc_queue_ent, command),
        CDB2_DATETIME, "added", -1, offsetof(sc_queue_ent, added),
        SYSTABLE_END_OF_FIELDS);
}
// clang-format off
