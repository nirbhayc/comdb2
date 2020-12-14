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

#include <sbuf2.h>
#include <plhash.h>
#include <sql.h>

/*
  x.    For each query in the top-N list
  x.x   Find all the tables involved in the given query
  x.x.x Track sqlite3FindTable()
  x.x.x Generate equivalent temporary table names for those tables
  x.x.x Create a mapping of orig table name to the new temporary table names
        later when executing the SELECT queries reroute base table name to
        the equivalent temporary table name (this is done to ease up from
        going over the SELECT query and replacing all the table names)
  x.x   For all tables
  x.x.x Create a temporary table
        > CREATE TEMPORARY TABLE $__t1 AS SELECT * FROM t1 LIMIT 100;
  x.x.x For each index of t1, go over sqlite_master and find a CREATE INDEX
        command and execute it against the temporary table
        > CREATE INDEX $__idx ON $__t1(cols);
  x.x   Enable autoexpert mode (this with trigger t1 -> $__t1 switch)
  x.x   Run the query and get the before cost
  x.x   Run query in expert mode against the temporary table
  x.x   Dump the recommendations
  x.x   Create the recommended indices
  x.x   Create recommended indices
  x.x   Run the query again and get the after cost
  x.x   Compare the costs
  x.x   If better than some specified threshold, go ahead and create indices
        against the original table
*/

extern int gbl_autoexpert;
extern pthread_mutex_t gbl_fingerprint_hash_mu;
extern hash_t *gbl_fingerprint_hash;

typedef struct query_sorter {
    const char *query;
    int count;
} query_sorter;

static int query_count_cmp_desc(const void *q1, const void *q2)
{
    return (((query_sorter *)q1)->count < ((query_sorter *)q2)->count);
}

static int get_top_N_queries(int n, char ***queries, int *num_queries)
{
    int fp_hash_size;
    int count;
    query_sorter *sorted_queries;
    char **top_queries;

    *num_queries = fp_hash_size = count = 0;

    logmsg(LOGMSG_USER, "autoexpert: retrieving top %d queries\n", n);

    Pthread_mutex_lock(&gbl_fingerprint_hash_mu);

    if (gbl_fingerprint_hash) {
        hash_info(gbl_fingerprint_hash, NULL, NULL, NULL, NULL, &fp_hash_size,
                  NULL, NULL);
        sorted_queries = calloc(fp_hash_size, sizeof(query_sorter));
        if (fp_hash_size > 0) {
            struct fingerprint_track *pEntry;
            void *cur;
            unsigned int bkt;
            pEntry = hash_first(gbl_fingerprint_hash, &cur, &bkt);
            for (int i = 0; pEntry; ++i) {
                if (pEntry->readOnly == 1) {
                    sorted_queries[i].query = strdup(pEntry->origSql);
                    sorted_queries[i].count = pEntry->count;
                    ++count;
                } else {
                    sorted_queries[i].query = strdup("");
                    sorted_queries[i].count = 0;
                }
                pEntry = hash_next(gbl_fingerprint_hash, &cur, &bkt);
            }
        }
    }
    Pthread_mutex_unlock(&gbl_fingerprint_hash_mu);

    if (count == 0) {
        return 0;
    }

    /* Sort all the queries based on their count */
    qsort(sorted_queries, fp_hash_size, sizeof(query_sorter),
          query_count_cmp_desc);

    /* Copy top-N queries */
    *num_queries = (n < count) ? n : count;
    top_queries = calloc(*num_queries, sizeof(char *));

    logmsg(LOGMSG_USER, "autoexpert: top %d queries are:\n", *num_queries);

    for (int i = 0; i < *num_queries; i++) {
        top_queries[i] = strdup(sorted_queries[i].query);
        logmsg(LOGMSG_USER, "%s\n", top_queries[i]);
    }

    *queries = top_queries;

    for (int i = 0; i < count; ++i) {
        free((char *)sorted_queries[i].query);
    }
    free(sorted_queries);

    return 0;
}

static int get_recommendations(struct sqlclntstate *clnt,
                               struct sqlthdstate *thd, char *query,
                               const char **outbuf)
{
    char *errmsg;
    int rc;

    clnt->sql = query;

    get_curtran(thedb->bdb_env, clnt);
    execute_expert_query(thd, clnt, outbuf, &errmsg, &rc);
    put_curtran(thedb->bdb_env, clnt);

    if (rc) {
        logmsg(
            LOGMSG_ERROR,
            "autoexpert: failed to execute query in expert mode (rc:%d, error: "
            "%s)\n",
            rc, errmsg);
    }

    return rc;
}

int exec_autoexpert(SBUF2 *sb)
{
    if (gbl_autoexpert) {
        struct sqlthdstate thd;
        struct sqlclntstate clnt;
        char **top_queries = NULL;
        const char *recommendations = NULL;
        int num_queries = 0;
        int rc;

        logmsg(LOGMSG_USER, "autoexpert-mode enabled!\n");

        start_internal_sql_clnt(&clnt);
        clnt.dbtran.mode = TRANLEVEL_SOSQL;
        clnt.thd = &thd;

        /* TODO (NC): introduce THRTYPE_autoexpert */
        sqlengine_thd_start(NULL, &thd, THRTYPE_ANALYZE);
        thd.sqlthd->clnt = &clnt;

        /* TODO (NC): make it a tunable */
        rc = get_top_N_queries(5, &top_queries, &num_queries);
        if (rc) {
            logmsg(LOGMSG_ERROR, "%s:%d failed to get top-n queries\n",
                   __func__, __LINE__);
        }

        for (int i = 0; i < num_queries; ++i) {
            logmsg(LOGMSG_USER,
                   "autoexpert: recommended indices for query '%s' are:\n",
                   top_queries[i]);
            get_recommendations(&clnt, &thd, top_queries[i], &recommendations);
            logmsg(LOGMSG_USER, "%s\n",
                   (recommendations) ? recommendations : "no recommendations");

            if (recommendations) {
                char *cur = (char *)recommendations;
                char *end = cur + strlen(recommendations);
                char *q;

                while (cur && (cur <= end)) {
                    char *next = strstr(cur, "\n");
                    if (next) {
                        ++next;
                        q = strndup(cur, next - cur - 1);
                    } else {
                        q = strdup(cur);
                    }

                    /* Be sure it's a CREATE INDEX command */
                    if ((memcmp(q, "CREATE INDEX", sizeof("CREATE INDEX") - 1)))
                        break;

                    logmsg(LOGMSG_USER, "autoexpert: executing %s\n", q);

                    int err;
                    bdb_state_type *bdb_state = thedb->bdb_env;
                    uint64_t seed = bdb_get_a_genid(bdb_state);
                    rc = bdb_llmeta_put_sc_queue(NULL, seed, q, &err);

                    rc = run_internal_sql_clnt(&clnt, q);
                    if (rc) {
                        logmsg(LOGMSG_ERROR,
                               "autoexpert: index creation failed (rc:%d)\n",
                               rc);
                    }
                    free(q);
                    cur = next;
                }
            }
            /* TODO (NC): freeme */
            // free(top_queries[i]);
        }

        end_internal_sql_clnt(&clnt);
        thd.sqlthd->clnt = NULL;
        sqlengine_thd_end(NULL, &thd);

        free(top_queries);
    }
    return 0;
}
