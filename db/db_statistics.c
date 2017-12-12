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

#include <assert.h>
#include <stdint.h>
#include "comdb2.h"
#include "comdb2_atomic.h"
#include "statistics.h"
#include "averager.h"
#include "net_int.h"

struct comdb2_statistics_store {
    int64_t bpool_hits;
    int64_t bpool_misses;
    int64_t commits;
    int64_t deadlocks;
    int64_t fstraps;
    int64_t lockwaits;
    int64_t preads;
    int64_t pwrites;
    int64_t repl_commits;
    int64_t repl_max_time_ms;
    int64_t repl_max_timeout_ms;
    int64_t repl_max_trx_size;
    int64_t repl_total_time_ms;
    int64_t repl_total_timeout_ms;
    int64_t repl_total_trx_size;
    int64_t retries;
    int64_t sql_cost;
    int64_t sql_count;
    int64_t start_time;
};

struct comdb2_statistics_store gbl_stats;

/*
  List of (almost) all comdb2 stats.
  Please keep'em sorted.
*/
comdb2_statistic gbl_statistics[] = {
    {"cache_hits", "Cache hits", STATISTIC_INTEGER, &gbl_stats.bpool_hits,
     NULL},
    {"cache_misses", "Cache misses", STATISTIC_INTEGER, &gbl_stats.bpool_misses,
     NULL},
    {"commits", "Number of commits", STATISTIC_INTEGER, &gbl_stats.commits,
     NULL},
    {"deadlocks", "Number of deadlocks", STATISTIC_INTEGER,
     &gbl_stats.deadlocks, NULL},
    {"fstrap", "Number of socket requests", STATISTIC_INTEGER,
     &gbl_stats.fstraps, NULL},
    {"lockwaits", "Number of lock waits", STATISTIC_INTEGER,
     &gbl_stats.lockwaits, NULL},
    {"preads", "Number of pread()'s", STATISTIC_INTEGER, &gbl_stats.preads,
     NULL},
    {"pwrites", "Number of pwrite()'s", STATISTIC_INTEGER, &gbl_stats.pwrites,
     NULL},
    {"repl_commits", "Total replication commits", STATISTIC_INTEGER,
     &gbl_stats.repl_commits, NULL},
    {"repl_max_timeout_ms", "Maximum replication timeout (in miliseconds)",
     STATISTIC_INTEGER, &gbl_stats.repl_max_timeout_ms},
    {"repl_max_trx_size", "Maximum replicated transaction size",
     STATISTIC_INTEGER, &gbl_stats.repl_max_trx_size, NULL},
    {"repl_total_time_ms", "Total replication time", STATISTIC_INTEGER,
     &gbl_stats.repl_total_time_ms, NULL},
    {"repl_total_timeout_ms", "Total replication timeout (in miliseconds)",
     STATISTIC_INTEGER, &gbl_stats.repl_total_timeout_ms, NULL},
    {"repl_total_trx_size", "Total replicated transaction size",
     STATISTIC_INTEGER, &gbl_stats.repl_total_trx_size, NULL},
    {"retries", "Number of retries", STATISTIC_INTEGER, &gbl_stats.retries,
     NULL},
    {"sql_cost", "Number of sql steps executed (cost)", STATISTIC_INTEGER,
     &gbl_stats.sql_cost, NULL},
    {"sql_count", "Number of sql queries executed", STATISTIC_INTEGER,
     &gbl_stats.sql_count, NULL},
    {"start_time", "Server start time (in seconds since the Epoch)",
     STATISTIC_INTEGER, &gbl_stats.start_time, NULL},
};

int gbl_statistics_count = sizeof(gbl_statistics) / sizeof(comdb2_statistic);

extern int n_commits;
extern long n_fstrap;

int refresh_statistics(void)
{
    int rc;
    const struct bdb_thread_stats *pstats;

    /* Check whether the server is exiting. */
    if (thedb->exiting || thedb->stopped)
        return 1;

    gbl_stats.commits = n_commits;
    gbl_stats.fstraps = n_fstrap;
    gbl_stats.retries = n_retries;
    gbl_stats.sql_cost = gbl_nsql_steps + gbl_nnewsql_steps;
    gbl_stats.sql_count = gbl_nsql + gbl_nnewsql;

    rc = bdb_get_lock_counters(thedb->bdb_env, &gbl_stats.deadlocks,
                               &gbl_stats.lockwaits);
    if (rc) {
        logmsg(LOGMSG_ERROR, "failed to refresh statistics (%s:%d)\n", __FILE__,
               __LINE__);
        return 1;
    }

    rc = bdb_get_bpool_counters(thedb->bdb_env, &gbl_stats.bpool_hits,
                                &gbl_stats.bpool_misses);
    if (rc) {
        logmsg(LOGMSG_ERROR, "failed to refresh statistics (%s:%d)\n", __FILE__,
               __LINE__);
        return 1;
    }

    pstats = bdb_get_process_stats();
    gbl_stats.preads = pstats->n_preads;
    gbl_stats.pwrites = pstats->n_pwrites;

    gbl_stats.repl_commits = thedb->num_txns;
    gbl_stats.repl_max_time_ms = thedb->max_reptime_ms;
    gbl_stats.repl_max_timeout_ms = thedb->max_timeout_ms;
    gbl_stats.repl_max_trx_size = thedb->biggest_txn;
    gbl_stats.repl_total_time_ms = thedb->total_reptime_ms;
    gbl_stats.repl_total_timeout_ms = thedb->total_timeouts_ms;
    gbl_stats.repl_total_trx_size = thedb->total_txn_sz;

    return 0;
}

int init_statistics(void)
{
    time_t t;

    memset(&gbl_stats, 0, sizeof(struct comdb2_statistics_store));

    t = time(NULL);
    gbl_stats.start_time = (int64_t)t;

    return 0;
}

const char *statistic_type(comdb2_statistic_type type)
{
    switch (type) {
    case STATISTIC_INTEGER:
        return "INTEGER";
    default:
        assert(0);
    }
}

comdb2_host_stat *get_net_host_stats(bdb_state_type *bdb_state,
                                     netinfo_type *netinfo_ptr,
                                     void *(*alloc_)(int), int *count)
{

    comdb2_host_stat *stats;
    host_node_type *ptr;
    int i;
    int size;

    i = 0;

    pthread_rwlock_rdlock(&(netinfo_ptr->lock));
    for (ptr = netinfo_ptr->head; ptr != NULL; ptr = ptr->next)
        i++;

    stats = alloc_(sizeof(comdb2_host_stat) * i);
    if (!stats) {
        pthread_rwlock_unlock(&(netinfo_ptr->lock));
        return 0;
    }

    *count = i;

    for (ptr = netinfo_ptr->head, i = 0; ptr != NULL; ptr = ptr->next, i++) {
        size = (ptr->hostname_len >= HOSTNAME_LEN) ? HOSTNAME_LEN - 1
                                                   : ptr->hostname_len;
        memcpy(stats[i].host, ptr->host, size);
        stats[i].host[size] = 0;
        stats[i].port = ptr->port;

        stats[i].bytes_written = ptr->stats.bytes_written;
        stats[i].bytes_read = ptr->stats.bytes_read;
        stats[i].throttle_waits = ptr->stats.throttle_waits;
        stats[i].reorders = ptr->stats.reorders;

        bdb_get_rep_avg_times(bdb_state, stats[i].host,
                              &stats[i].avg_wait_over_10secs,
                              &stats[i].avg_wait_over_1min);
    }
    pthread_rwlock_unlock(&(netinfo_ptr->lock));

    return stats;
}

void free_net_host_stats(comdb2_host_stat *stats, void (*free_)(void *))
{
    free_(stats);
}
