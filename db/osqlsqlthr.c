/*
   Copyright 2015 Bloomberg Finance L.P.

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

/**
 *
 *  Interface with sqlglue.c
 *
 *  Each sqlthread executing an osql request has associated an osqlstate_t
 *  structure.  The structure is registered in a checkboard during the
 *  osql request lifetime.
 *
 *  Sqlthreads will call insrec/updrec/delrec functions.
 *   - For blocksql and socksql modes, each call results in a net transfer
 *     to the master.
 *   - For recom, snapisol and serial mode, each call will update the local
 *shadow tables (which
 *     will be transferred to the master once the transaction is committed).
 *
 */

#include <poll.h>
#include <util.h>
#include <unistd.h>
#include "sql.h"
#include "osqlsqlthr.h"
#include "osqlshadtbl.h"
#include "osqlcomm.h"
#include "osqlcheckboard.h"
#include <bdb_api.h>
#include "comdb2.h"
#include "genid.h"
#include "comdb2uuid.h"
#include "nodemap.h"
#include <bpfunc.h>

#include "debug_switches.h"
#include <net_types.h>
#include <trigger.h>
#include <logmsg.h>
#include "views.h"

/* don't retry commits, fail transactions during master swings !
   we need blockseq */

extern int gbl_partial_indexes;
extern int gbl_expressions_indexes;

int gbl_survive_n_master_swings = 600;
int gbl_master_retry_poll_ms = 100;

static int osql_send_usedb_logic(struct BtCursor *pCur, struct sql_thread *thd,
                                 int nettype);
static int osql_send_delrec_logic(struct BtCursor *pCur, struct sql_thread *thd,
                                  int nettype);
int osql_send_delidx_logic(struct BtCursor *pCur, struct sql_thread *thd,
                           int nettype);
static int osql_send_insrec_logic(struct BtCursor *pCur, struct sql_thread *thd,
                                  char *pData, int nData, int nettype,
                                  int flags);
int osql_send_insidx_logic(struct BtCursor *pCur, struct sql_thread *thd,
                           int nettype);
static int osql_send_updrec_logic(struct BtCursor *pCur, struct sql_thread *thd,
                                  char *pData, int nData, int nettype,
                                  int flags);
static int osql_qblobs(struct BtCursor *pCur, struct sql_thread *thd,
                       int *updCols, blob_buffer_t *blobs, int maxblobs,
                       int is_update);
static int osql_updcols(struct BtCursor *pCur, struct sql_thread *thd,
                        int *updCols);
static int osql_send_recordgenid_logic(struct BtCursor *pCur,
                                       struct sql_thread *thd,
                                       unsigned long long genid, int nettype);
static int osql_send_updstat_logic(struct BtCursor *pCur,
                                   struct sql_thread *thd, char *pData,
                                   int nData, int nStat, int nettype);
static int osql_send_commit_logic(struct sqlclntstate *clnt, int is_retry,
                                  int nettype);
static int osql_send_abort_logic(struct sqlclntstate *clnt, int nettype);
static int check_osql_capacity(struct sql_thread *thd);
static int access_control_check_sql_write(struct BtCursor *pCur,
                                          struct sql_thread *thd);

/*
   artificially limit the size of the transaction;
   apparently this avoids some bugs and makes the db happy
   - less deadlocks
   - no timeouts
   - no replication bugout on 4.2
*/
int g_osql_max_trans = 50000;

/**
 * Set maximum osql transaction size
 *
 */
inline void set_osql_maxtransfer(int limit)
{
    g_osql_max_trans = limit;
}

/**
 * Get maximum osql transaction size
 *
 */
inline int get_osql_maxtransfer(void)
{
    return g_osql_max_trans;
}

/**
 * Set the maximum time throttling offload-sql requests
 *
 */
inline void set_osql_maxthrottle_sec(int limit)
{
    gbl_osql_max_throttle_sec = limit;
}

inline int get_osql_maxthrottle_sec(void)
{
    return gbl_osql_max_throttle_sec;
}

/**
 * Process a sqlite index delete request
 * Index is provided by thd->clnt->idxDelete
 * Returns SQLITE_OK if successful.
 *
 */
int osql_delidx(struct BtCursor *pCur, struct sql_thread *thd, int is_update)
{
    int rc = 0;

    if (!gbl_expressions_indexes || !pCur->db->ix_expr)
        return SQLITE_OK;

    if (thd->clnt->dbtran.mode == TRANLEVEL_SOSQL) {
        rc = osql_send_delidx_logic(pCur, thd, NET_OSQL_SOCK_RPL);
        if (rc) {
            logmsg(LOGMSG_ERROR,
                   "%s:%d %s - failed to send socksql row rc=%d\n", __FILE__,
                   __LINE__, __func__, rc);
            return rc;
        }
    }
    return osql_save_index(pCur, thd, is_update, 1);
}

/**
 * Process a sqlite delete row request
 * BtCursor points to the record to be deleted.
 * Returns SQLITE_OK if successful.
 *
 */
int osql_delrec(struct BtCursor *pCur, struct sql_thread *thd)
{
    int rc = 0;

    if ((rc = access_control_check_sql_write(pCur, thd)))
        return rc;

    rc = osql_delidx(pCur, thd, 0);
    if (rc != SQLITE_OK)
        return rc;

    if (thd->clnt->dbtran.mode == TRANLEVEL_SOSQL) {
        rc = osql_send_delrec_logic(pCur, thd, NET_OSQL_SOCK_RPL);
        if (rc) {
            logmsg(LOGMSG_ERROR,
                   "%s:%d %s - failed to send socksql row rc=%d\n", __FILE__,
                   __LINE__, __func__, rc);
            return rc;
        }
    }
    return osql_save_delrec(pCur, thd);
}

/**
 * Process an insert or update to sqlite_stat1.
 * This is handled specially to allow us to save the previous data to llmeta.
 * Returns SQLITE_OK if successul.
 *
 */

int osql_updstat(struct BtCursor *pCur, struct sql_thread *thd,
                        char *pData, int nData, int nStat)
{
    return osql_send_updstat_logic(pCur, thd, pData, nData, nStat,
                                   NET_OSQL_SOCK_RPL);
}

/**
 * Process a sqlite index insert request
 * Index is provided by thd->clnt->idxInsert
 * Returns SQLITE_OK if successful.
 *
 */
int osql_insidx(struct BtCursor *pCur, struct sql_thread *thd, int is_update)
{
    int rc = 0;

    if (!gbl_expressions_indexes || !pCur->db->ix_expr)
        return SQLITE_OK;

    if (thd->clnt->dbtran.mode == TRANLEVEL_SOSQL) {
        rc = osql_send_insidx_logic(pCur, thd, NET_OSQL_SOCK_RPL);
        if (rc) {
            logmsg(LOGMSG_ERROR,
                   "%s:%d %s - failed to send socksql row rc=%d\n", __FILE__,
                   __LINE__, __func__, rc);
            return rc;
        }
    }
    return osql_save_index(pCur, thd, is_update, 0);
}

/**
 * Process a sqlite insert row request
 * Row is provided by (pData, nData, blobs, maxblobs)
 * Returns SQLITE_OK if successful.
 *
 */
int osql_insrec(struct BtCursor *pCur, struct sql_thread *thd, char *pData,
                int nData, blob_buffer_t *blobs, int maxblobs, int flags)
{
    int rc = 0;

    if ((rc = access_control_check_sql_write(pCur, thd)))
        return rc;

    rc = osql_insidx(pCur, thd, 0);
    if (rc != SQLITE_OK)
        return rc;

    rc = osql_qblobs(pCur, thd, NULL, blobs, maxblobs, 0);
    if (rc != SQLITE_OK)
        return rc;

    if (thd->clnt->dbtran.mode == TRANLEVEL_SOSQL) {
        rc = osql_send_insrec_logic(pCur, thd, pData, nData,
                                    NET_OSQL_SOCK_RPL, flags);
        if (rc) {
            logmsg(LOGMSG_ERROR,
                   "%s:%d %s - failed to send socksql row rc=%d\n", __FILE__,
                   __LINE__, __func__, rc);
            return rc;
        }
    }
    return osql_save_insrec(pCur, thd, pData, nData, flags);
}

/**
 * Process a sqlite update row request
 * New row is provided by (pData, nData, blobs, maxblobs)
 * BtCursor points to the old row.
 * Returns SQLITE_OK if successful.
 *
 */
int osql_updrec(struct BtCursor *pCur, struct sql_thread *thd, char *pData,
                int nData, int *updCols, blob_buffer_t *blobs, int maxblobs,
                int flags)
{
    int rc = 0;

    if ((rc = access_control_check_sql_write(pCur, thd)))
        return rc;

    rc = osql_delidx(pCur, thd, 1);
    if (rc != SQLITE_OK)
        return rc;

    rc = osql_insidx(pCur, thd, 1);
    if (rc != SQLITE_OK)
        return rc;

    rc = osql_qblobs(pCur, thd, updCols, blobs, maxblobs, 1);
    if (rc != SQLITE_OK)
        return rc;

    if (updCols) {
        rc = osql_updcols(pCur, thd, updCols);
        if (rc != SQLITE_OK)
            return rc;
    }

    if (thd->clnt->dbtran.mode == TRANLEVEL_SOSQL) {
        rc = osql_send_updrec_logic(pCur, thd, pData, nData, NET_OSQL_SOCK_RPL,
                                    flags);
        if (rc) {
            logmsg(LOGMSG_ERROR,
                   "%s:%d %s - failed to send socksql row rc=%d\n", __FILE__,
                   __LINE__, __func__, rc);
            return rc;
        }
    }
    return osql_save_updrec(pCur, thd, pData, nData, flags);
}

/**
 * Process a sqlite clear table request
 * Currently not implemented due to non-transactional vs slowness unresolved
 * issues.
 *
 */
int osql_cleartable(struct sql_thread *thd, char *dbname)
{

    logmsg(LOGMSG_ERROR, "cleartable not implemented!\n");
    return -1;
}

int nettypetouuidnettype(int nettype)
{
    switch (nettype) {
    case NET_OSQL_SOCK_RPL:
        return NET_OSQL_SOCK_RPL_UUID;
    case NET_OSQL_SERIAL_RPL:
        return NET_OSQL_SERIAL_RPL_UUID;
    default:
        logmsg(LOGMSG_ERROR, "Unsupported nettype %d\n", nettype);
        return nettype;
    }
}

int osql_serial_send_readset(struct sqlclntstate *clnt, int nettype)
{
    osqlstate_t *osql = &clnt->osql;
    int rc = 0;

    if (nettype == NET_OSQL_SERIAL_RPL) {
        if (clnt->arr->size == 0)
            return 0;
    } else {
        if (clnt->selectv_arr->size == 0)
            return 0;
    }

#if 0
   uuidstr_t us;
   printf("osql_serial_send_readset rqid %llx uuid %s\n", (unsigned long long) osql->rqid, comdb2uuidstr(osql->uuid, us));
#endif

    if (osql->rqid == OSQL_RQID_USE_UUID)
        nettype = nettypetouuidnettype(nettype);

    if (nettype == NET_OSQL_SERIAL_RPL || nettype == NET_OSQL_SERIAL_RPL_UUID)
        rc = osql_send_serial(osql->host, osql->rqid, osql->uuid, clnt->arr,
                              clnt->arr->file, clnt->arr->offset, nettype,
                              osql->logsb);
    else
        rc = osql_send_serial(osql->host, osql->rqid, osql->uuid,
                              clnt->selectv_arr, clnt->selectv_arr->file,
                              clnt->selectv_arr->offset, nettype, osql->logsb);
    return rc;
}

/**
 * Called when all rows are retrieved
 * Informs block process that the sql processing is over
 * and it can start processing bloplog
 *
 */
int osql_block_commit(struct sql_thread *thd)
{

    return osql_send_commit_logic(thd->clnt, 0, NET_OSQL_BLOCK_RPL);
}

/**
 * Starts a sosql session, which creates a blockprocessor peer
 * Returns ok if the packet is sent successful to the master
 * if keep_rqid, this is a retry and we want to
 * keep the same rqid
 *
 */
int osql_sock_start(struct sqlclntstate *clnt, int type, int keep_rqid)
{
    osqlstate_t *osql = &clnt->osql;
    struct sql_thread *thd = pthread_getspecific(query_info_key);
    int rc = 0;
    int retries = 0;
    int flags = 0;

    if (!thd) {
        logmsg(LOGMSG_ERROR, "%s:%d Bug, not sql thread !\n", __func__, __LINE__);
        cheap_stack_trace();
    }

    /* first, get an id */
    if (!keep_rqid) {
        if (gbl_noenv_messages) {
            osql->rqid = OSQL_RQID_USE_UUID;
            comdb2uuid(osql->uuid);

            uuidstr_t us;
        } else {
            osql->rqid = comdb2fastseed();
            comdb2uuid_clear(osql->uuid);
            assert(osql->rqid);
        }
    }

    /* lets reset error, this could be a retry */
    osql->xerr.errval = 0;
    osql->xerr.errstr[0] = '\0';

#ifdef DEBUG
    if (gbl_debug_sql_opcodes) {
        uuidstr_t us;
        logmsg(LOGMSG_USER, "%p gets rqid %llx %s tid=0x%x\n", clnt, osql->rqid,
                comdb2uuidstr(osql->uuid, us), pthread_self());
    }
#endif

retry:
    if (thd && bdb_lock_desired(thedb->bdb_env)) {
        int sleepms = 100 * clnt->deadlock_recovered;
        if (sleepms > 1000)
            sleepms = 1000;

        if (gbl_master_swing_osql_verbose)
            logmsg(LOGMSG_ERROR, 
                    "%s:%d bdb lock desired, recover deadlock with sleepms=%d\n",
                    __func__, __LINE__, sleepms);

        rc = recover_deadlock(thedb->bdb_env, thd, NULL, sleepms);
        if (rc != 0) {
            logmsg(LOGMSG_ERROR, "recover_deadlock returned %d\n", rc);
            return SQLITE_BUSY;
        }

        if (gbl_master_swing_osql_verbose)
            logmsg(LOGMSG_USER, "%s recovered deadlock\n", __func__);
        clnt->deadlock_recovered++;
        if (clnt->deadlock_recovered > 100) {
            sql_debug_logf(clnt, __func__, __LINE__,
                           "deadlock_recovered is %d, returning SQLITE_BUSY\n",
                           clnt->deadlock_recovered);
            return SQLITE_BUSY;
        }
    }

    /* register the session */
    osql->host = thedb->master;

    /* protect against no master */
    if (osql->host == NULL) {
        /* wait up to 50 seconds for a new master */
        if (retries < 100) {
            retries++;

            logmsg(LOGMSG_WARN, "Retrying to find the master retries=%d \n",
                    retries);
            poll(NULL, 0, 500);
        } else {
            logmsg(LOGMSG_ERROR, "%s: no master for %llu!\n", __func__, osql->rqid);
            errstat_set_rc(&osql->xerr, ERR_NOMASTER);
            errstat_set_str(&osql->xerr, "No master available");
            return SQLITE_ABORT;
        }
    }

    /* TODO: review the initialization of osqlstate_t */

    if (!keep_rqid) {
        /* register this new member */
        rc = osql_register_sqlthr(clnt, type);
        if (rc) {
            sql_debug_logf(clnt, __func__, __LINE__,
                           "osql_register_sqlthr returns %d\n", rc);
            return rc;
        }
    } else {
        /* this is a replay with same rqid, already registered */
        /* sets to the same node */
        osql_reuse_sqlthr(clnt, osql->host);
    }

    /* socksql: check if this is a verify retry, and if we got enough of those
       to trigger a self-deadlock check on the master */

    if ((type == OSQL_SOCK_REQ || type == OSQL_SOCK_REQ_COST) &&
        clnt->verify_retries > gbl_osql_verify_ext_chk)
        flags |= OSQL_FLAGS_CHECK_SELFLOCK;
    else
        flags = 0;

    /* send request to blockprocessor */
    rc = osql_comm_send_socksqlreq(osql->host, clnt->sql, strlen(clnt->sql) + 1,
                                   osql->rqid, osql->uuid, clnt->tzname, type,
                                   flags);

    if (rc != 0 && retries < 100) {
        retries++;
        logmsg(LOGMSG_WARN, "Retrying to find the master (2) retries=%d \n",
                retries);
        goto retry;
    }

    if (rc && osql->host) {
        sql_debug_logf(clnt, __func__, __LINE__,
                       "Tried to talk to %s and got %d returning SQLITE_BUSY\n",
                       osql->host, rc);
        logmsg(LOGMSG_ERROR,
               "Tried to talk to %s and got rc=%d - returning SQLITE_BUSY\n",
               osql->host, rc);
        rc = SQLITE_BUSY;
    }

    if (!keep_rqid && rc != 0) {
        osql_unregister_sqlthr(clnt);
    }

    return rc;
}

int gbl_master_swing_sock_restart_sleep = 0;
/**
 * Restart a broken socksql connection by opening
 * a new blockproc on the provided master and
 * sending the cache rows to resume the current.
 * If keep_session is set, the same rqid is used for the replay
 *
 */
int osql_sock_restart(struct sqlclntstate *clnt, int maxretries,
                      int keep_session)
{
    int rc = 0;
    int retries = 0;
    int sentops = 0;
    int bdberr = 0;
    osqlstate_t *osql = &clnt->osql;
    struct sql_thread *thd = pthread_getspecific(query_info_key);

    retries = 0;

    if (!thd) {
        logmsg(LOGMSG_ERROR, "%s:%d Bug, not sql thread !\n", __func__, __LINE__);
        cheap_stack_trace();
    }

again:

    sentops = 0;

    /* we need to check if we need bdb write lock here to prevent a master
       upgrade
       blockade
     */
    if (thd && bdb_lock_desired(thedb->bdb_env)) {
        int sleepms = 100 * clnt->deadlock_recovered;
        if (sleepms > 1000)
            sleepms = 1000;

        logmsg(LOGMSG_ERROR, 
                "%s:%d bdb lock desired, recover deadlock with sleepms=%d\n",
                __func__, __LINE__, sleepms);

        rc = recover_deadlock(thedb->bdb_env, thd, NULL, sleepms);

        if (rc != 0) {
            logmsg(LOGMSG_ERROR, "recover_deadlock returned %d\n", rc);

            osql_unregister_sqlthr(clnt);

            return SQLITE_BUSY;
        }

        logmsg(LOGMSG_DEBUG, "%s recovered deadlock\n", __func__);

        clnt->deadlock_recovered++;

        if (clnt->deadlock_recovered > 100) {
            osql_unregister_sqlthr(clnt);

            return SQLITE_BUSY;
        }
    }

    if (osql->tablename) {
        free(osql->tablename);
        osql->tablename = NULL;
        osql->tablenamelen = 0;
    }

    if (!keep_session) {
        if (gbl_master_swing_osql_verbose)
            logmsg(LOGMSG_USER, "%lu Starting %llx\n", pthread_self(),
                   clnt->osql.rqid);
        /* unregister this osql thread from checkboard */
        rc = osql_unregister_sqlthr(clnt);
        if (rc)
            return SQLITE_INTERNAL;
    } else {
        if (gbl_master_swing_osql_verbose)
            logmsg(LOGMSG_USER,
                   "0x%lu Restarting clnt->osql.rqid=%llx against %s\n",
                   pthread_self(), clnt->osql.rqid, thedb->master);
        /* we should reset this ! */
        rc = osql_reuse_sqlthr(clnt, thedb->master);
        if (rc)
            return SQLITE_INTERNAL;
    }

    rc = osql_sock_start(clnt, (clnt->dbtran.mode == TRANLEVEL_SOSQL)
                                   ? OSQL_SOCK_REQ
                                   : OSQL_RECOM_REQ,
                         keep_session);
    if (rc) {
        sql_debug_logf(clnt, __func__, __LINE__, "osql_sock_start returns %d\n",
                       rc);
        goto error;
    }

    /* process messages from cache */
    rc = osql_shadtbl_process(clnt, &sentops, &bdberr, 1);
    if (rc == SQLITE_TOOBIG) {
        logmsg(LOGMSG_ERROR, "%s: transaction too big %d\n", __func__, sentops);
        return rc;
    }
    if (rc == ERR_SC) {
        logmsg(LOGMSG_ERROR, "%s: schema change error\n", __func__);
        return rc;
    }

    if (keep_session && gbl_master_swing_sock_restart_sleep) {
        sleep(gbl_master_swing_sock_restart_sleep);
    }

    /* selectv skip optimization, not an error */
    if (unlikely(rc == -2 || rc == -3))
        rc = 0;

    if (rc)
        goto error;

    if (0) {
    error:
        retries++;
        if (retries < maxretries) {
            logmsg(LOGMSG_ERROR, 
                "Error in restablishing the sosql session, rc=%d, retries=%d\n",
                rc, retries);

            /* if we're shaking really badly, back off */
            if (retries > 1)
                sleep(retries);

            goto again;
        }

        sql_debug_logf(clnt, __func__, __LINE__,
                       "failed %d times to restart socksql session\n", retries);
        logmsg(LOGMSG_ERROR,
               "%s:%d %s failed %d times to restart socksql session\n",
               __FILE__, __LINE__, __func__, retries);
        return -1;
    }

    return 0;
}

int gbl_random_blkseq_replays;

/**
 * Terminates a sosql session
 * Block processor is informed that all the rows are sent
 * It waits for the block processor to report back the return code
 * Returns the result of block processor commit
 *
 */
int osql_sock_commit(struct sqlclntstate *clnt, int type)
{
    osqlstate_t *osql = &clnt->osql;
    int rc = 0, rc2;
    int rcout = 0;
    int retries = 0;
    int bdberr = 0;
    int timeout = 0;

    /* temp hook for sql transactions */
    /* is it distributed? */
    if (clnt->dbtran.mode == TRANLEVEL_SOSQL && clnt->dbtran.dtran)
    {
        rc = fdb_trans_commit(clnt);
        if (rc) {
            logmsg(LOGMSG_ERROR, "%s distributed failure rc=%d\n", __func__, rc);

            rc2 = osql_sock_abort(clnt, type);

            if (rc2) {
                logmsg(LOGMSG_ERROR, "%s osql_sock_abort failed with rc=%d\n",
                        __func__, rc2);
            }

            return SQLITE_ABORT;
        }
    }

    osql->timings.commit_start = osql_log_time();

/* send results of sql processing to block master */
/* if (thd->clnt->query_stats)*/

retry:

    /* Release our locks.  Only table locks should be held at this point. */
    if (clnt->dbtran.cursor_tran) {
        rc = bdb_free_curtran_locks(thedb->bdb_env, clnt->dbtran.cursor_tran,
                                    &bdberr);
        if (rc) {
            rcout = rc;
            sql_debug_logf(clnt, __func__, __LINE__,
                           "got %d and setting rcout to %d\n", rc, rcout);
            goto err;
        }
    }

    rc = osql_send_commit_logic(clnt, retries, req2netrpl(type));
    if (rc) {
        logmsg(LOGMSG_ERROR, "%s:%d: failed to send commit to master rc was %d\n", __FILE__,
                __LINE__, rc);
        rcout = SQLITE_CLIENT_CHANGENODE;
        goto err;
    }

    /* if there was a sqlite error, don't wait for block processor */
    if (osql->xerr.errval == 0) {

        /* trap */
        if (!osql->rqid) {
            logmsg(LOGMSG_ERROR, "%s: !rqid %p %lu???\n", __func__, clnt,
                   pthread_self());
            /*cheap_stack_trace();*/
            abort();
        }

        if (osql->running_ddl)
            timeout = bdb_attr_get(thedb->bdb_attr,
                                   BDB_ATTR_SOSQL_DDL_MAX_COMMIT_WAIT_SEC);
        else
            timeout = bdb_attr_get(thedb->bdb_attr,
                                   BDB_ATTR_SOSQL_MAX_COMMIT_WAIT_SEC);

        /* waits for a sign */
        rc = osql_chkboard_wait_commitrc(osql->rqid, osql->uuid, timeout,
                                         &osql->xerr);
        if (rc) {
            rcout = SQLITE_CLIENT_CHANGENODE;
            logmsg(LOGMSG_ERROR, "%s line %d setting rcout to (%d) from %d\n", 
                    __func__, __LINE__, rcout, rc);
        }
        else {

            if (gbl_random_blkseq_replays && ((rand() % 50) == 0)) {
                logmsg(LOGMSG_ERROR, "%s line %d forcing random blkseq retry\n",
                       __func__, __LINE__);
                osql->xerr.errval = ERR_NOMASTER;
            }
            /* we got a return from block processor \
               propagate the result/error
             */
            if (osql->xerr.errval) {
                /* lets check to see if a master swing happened and we need to
                 * retry */
                sql_debug_logf(clnt, __func__, __LINE__,
                               "returns xerr.errval=%d\n", osql->xerr.errval);
                if (osql->xerr.errval == ERR_NOMASTER ||
                    osql->xerr.errval == ERR_NOT_DURABLE ||
                    osql->xerr.errval == 999) {
                    if (bdb_attr_get(thedb->bdb_attr,
                                     BDB_ATTR_SC_RESUME_AUTOCOMMIT) &&
                        !clnt->in_client_trans && osql->running_ddl) {
                        clnt->osql.xerr.errval = ERR_SC;
                        errstat_cat_str(&(clnt->osql.xerr),
                                        "Master node downgrading - new "
                                        "master will resume schemachange");
                        logmsg(LOGMSG_INFO,
                               "%s: Master node downgrading - new master "
                               "will resume schemachange\n",
                               __func__);
                        rcout = SQLITE_ABORT;
                        goto err;
                    }
                    if (retries++ < gbl_survive_n_master_swings) {
                        sql_debug_logf(
                            clnt, __func__, __LINE__,
                            "lost "
                            "connection to master, retrying %d in %d "
                            "msec\n",
                            retries, gbl_master_retry_poll_ms);

                        poll(NULL, 0, gbl_master_retry_poll_ms);

                        rc = osql_sock_restart(
                            clnt, 1,
                            1 /*no new rqid*/); /* retry at higher level */
                        if (rc != SQLITE_TOOBIG && rc != ERR_SC) {
                            if (gbl_master_swing_sock_restart_sleep) {
                                sleep(gbl_master_swing_sock_restart_sleep);
                            }
                            goto retry;
                        }
                    }
                }
                /* transaction failed on the master, abort here as well */
                if (rc != SQLITE_TOOBIG) {
                    if (osql->xerr.errval == -109 /* SQLHERR_MASTER_TIMEOUT */) {
                        sql_debug_logf(
                            clnt, __func__, __LINE__,
                            "got %d and "
                            "setting rcout to MASTER_TIMEOUT, errval is "
                            "%d setting rcout to -109\n",
                            rc, osql->xerr.errval);
                        rcout = -109;
                    } else if (osql->xerr.errval == ERR_NOT_DURABLE) {
                        /* Ask the client to change nodes */
                        sql_debug_logf(
                            clnt, __func__, __LINE__,
                            "got %d and "
                            "setting rcout to CHANGENODE, errval is %d\n",
                            rc, osql->xerr.errval);
                        rcout = SQLITE_CLIENT_CHANGENODE;
                    } else {
                        sql_debug_logf(
                            clnt, __func__, __LINE__,
                            "got %d and "
                            "setting rcout to SQLITE_ABORT, errval is "
                            "%d\n",
                            rc, osql->xerr.errval);
                        // SQLITE_ABORT comes out as a "4" in the client,
                        // which is translated to 4 'null key constraint'.
                        rcout = SQLITE_ABORT;
                    }
                } else {
                    sql_debug_logf(
                        clnt, __func__, __LINE__,
                        "got %d and "
                        "setting rcout to SQLITE_TOOBIG, errval is %d\n",
                        rc, osql->xerr.errval);
                    rcout = SQLITE_TOOBIG;
                }
            } else {
                sql_debug_logf(clnt, __func__, __LINE__, "got %d from %s\n", rc,
                               osql->host);
            }
        }
        if (clnt->client_understands_query_stats && clnt->dbglog)
            append_debug_logs_from_master(clnt->dbglog,
                                          clnt->master_dbglog_cookie);
    } else {
        rcout = SQLITE_ERROR;
        logmsg(LOGMSG_ERROR, "%s line %d set rcout to %d\n", __func__, __LINE__, rcout);
    }

#if 0
      printf("Unregistering rqid=%llu tmp=%llu\n", osql->rqid, osql_log_time());
#endif

err:
    /* unregister this osql thread from checkboard */
    rc = osql_unregister_sqlthr(clnt);
    if (rc && !rcout) {
        logmsg(LOGMSG_ERROR, "%s line %d setting rout to SQLITE_INTERNAL (%d) rc is %d\n", 
                __func__, __LINE__, SQLITE_INTERNAL, rc);
        rcout = SQLITE_INTERNAL;
    }
#if 0
   printf("Unregistered rqid=%llu tmp=%llu\n", osql->rqid, osql_log_time());
#endif

    osql->timings.commit_end = osql_log_time();

#if 0
   printf( "recv=%llu disp=%llu fin=%llu commit_prep=%llu, commit_start=%llu commit_end=%llu\n",
         thd->clnt->osql.timings.query_received,
         thd->clnt->osql.timings.query_dispatched,
         thd->clnt->osql.timings.query_finished,
         thd->clnt->osql.timings.commit_prep,
         thd->clnt->osql.timings.commit_start,
         thd->clnt->osql.timings.commit_end
         );
#endif

   /* mark socksql as non-retriable if seletv are present
      also don't retry distributed transactions
    */
   if (clnt->osql.xerr.errval == (ERR_BLOCK_FAILED + ERR_VERIFY) &&
           clnt->dbtran.mode == TRANLEVEL_SOSQL && !clnt->dbtran.dtran) {
       int bdberr = 0;
       int iirc = 0;
       iirc = osql_shadtbl_has_selectv(clnt, &bdberr);
       if (iirc != 0) /* if error or has selectv rows */
       {
           if (iirc < 0) {
               logmsg(LOGMSG_ERROR, 
                       "%s: osql_shadtbl_has_selectv failed rc=%d bdberr=%d\n",
                       __func__, rc, bdberr);
           }
           osql_set_replay(__FILE__, __LINE__, clnt, OSQL_RETRY_LAST);
       }
   }

   /* DDLs are also non-retriable */
   if (osql->xerr.errval == (ERR_BLOCK_FAILED + ERR_VERIFY) &&
       osql->running_ddl) {
       logmsg(LOGMSG_DEBUG, "%s: marking DDL transaction as non-retriable\n",
              __func__);
       osql_set_replay(__FILE__, __LINE__, clnt, OSQL_RETRY_LAST);
   }

   osql_shadtbl_close(clnt);

   if (clnt->dbtran.mode == TRANLEVEL_SOSQL) {
       /* we also need to free the tran object */
       rc = trans_abort_shadow((void **)&clnt->dbtran.shadow_tran, &bdberr);
       if (rc)
           logmsg(LOGMSG_ERROR, 
                   "%s:%d failed to abort shadow tran for socksql rc=%d\n",
                   __FILE__, __LINE__, rc);
   }

   return rcout;
}

/**
 * Terminates a sosql session
 * It notifies the block processor to abort the request
 *
 */
int osql_sock_abort(struct sqlclntstate *clnt, int type)
{
    int rcout = 0;
    int rc = 0;
    int irc = 0;
    int bdberr = 0;

    /* am I talking already with the master? rqid != 0 */
    if (clnt->osql.rqid != 0) {
        /* send results of sql processing to block master */
        rc = osql_send_abort_logic(clnt, req2netrpl(type));
        if (rc) {
            logmsg(LOGMSG_ERROR, "%s: failed to send abort rc=%d\n", __func__, rc);
            /* we still need to unregister the clnt */
            rcout = SQLITE_INTERNAL;
        }

        /* unregister this osql thread from checkboard */
        rc = osql_unregister_sqlthr(clnt);
        if (rc) {
            logmsg(LOGMSG_ERROR, "%s: failed to unregister clnt rc=%d\n", __func__,
                    rc);
            rcout = SQLITE_INTERNAL;
        }
    }

    clnt->osql.sentops = 0;  /* reset statement size counter*/
    clnt->osql.tran_ops = 0; /* reset transaction size counter*/

    osql_shadtbl_close(clnt);

    /* we also need to free the tran object, if we fail to dispatch after a
       begin
       for example.
     */
    rc = trans_abort_shadow((void **)&clnt->dbtran.shadow_tran, &bdberr);
    if (rc) {
        logmsg(LOGMSG_ERROR, 
                "%s:%d failed to abort shadow tran for socksql rc=%d\n",
                __FILE__, __LINE__, rc);
    }

    if (clnt->osql.tablename) {
        free(clnt->osql.tablename);
        clnt->osql.tablename = NULL;
        clnt->osql.tablenamelen = 0;
    }

    return rcout;
}

/********************** INTERNALS
 * ***********************************************/

int gbl_osql_random_restart = 0;

static int should_restart(struct sqlclntstate *clnt, int rc)
{
    if (rc == OSQL_SEND_ERROR_WRONGMASTER &&
        (clnt->dbtran.mode == TRANLEVEL_SOSQL ||
         clnt->dbtran.mode == TRANLEVEL_RECOM)) {
        return 1;
    }

    if (gbl_osql_random_restart && (rand() % 100) == 0) {
        logmsg(LOGMSG_USER, "Forcing random-restart\n");
        return 1;
    }

    return 0;
}

#define RESTART_SOCKSQL_KEEP_RQID(keep_rqid)                                   \
    restarted = 0;                                                             \
    if (should_restart(clnt, rc)) {                                            \
        restarted = 1;                                                         \
        rc = osql_sock_restart(clnt, gbl_survive_n_master_swings, keep_rqid);  \
        if (rc) {                                                              \
            logmsg(LOGMSG_ERROR,                                               \
                   "%s: failed to restart socksql session rc=%d\n", __func__,  \
                   rc);                                                        \
        }                                                                      \
    }                                                                          \
    if (rc) {                                                                  \
        logmsg(LOGMSG_ERROR,                                                   \
               "%s: error writting record to master in offload mode rc=%d!\n", \
               __func__, rc);                                                  \
        if (rc != SQLITE_TOOBIG && rc != ERR_SC)                               \
            rc = SQLITE_INTERNAL;                                              \
    } else {                                                                   \
        rc = SQLITE_OK;                                                        \
    }

#define RESTART_SOCKSQL RESTART_SOCKSQL_KEEP_RQID(0)

static int osql_send_usedb_logic_int(char *tablename, struct sqlclntstate *clnt,
                                     int nettype)
{
    osqlstate_t *osql = &clnt->osql;
    int rc = 0;
    int restarted;

    if (clnt->ddl_tables && hash_find_readonly(clnt->ddl_tables, tablename)) {
        return SQLITE_DDL_MISUSE;
    }
    if (clnt->dml_tables && !hash_find_readonly(clnt->dml_tables, tablename))
        hash_add(clnt->dml_tables, strdup(tablename));

    int tablenamelen = strlen(tablename) + 1; /*including trailing 0*/
    if (osql->tablename) {
        if (osql->tablenamelen == tablenamelen &&
            !strncmp(tablename, osql->tablename, osql->tablenamelen))
            /* we've already sent this, skip */
            return SQLITE_OK;

        /* free the cache */
        free(osql->tablename);
        osql->tablename = NULL;
        osql->tablenamelen = 0;
    }

    do {
        rc = osql_send_usedb(osql->host, osql->rqid, osql->uuid, tablename,
                             nettype, osql->logsb,
                             comdb2_table_version(tablename));
        RESTART_SOCKSQL;
    } while (restarted && rc == 0);

    if (rc == SQLITE_OK) {
        /* cache the sent tablename */
        osql->tablename = strdup(tablename);
        if (osql->tablename)
            osql->tablenamelen = tablenamelen;
    }

    return rc;
}

static int osql_send_usedb_logic(struct BtCursor *pCur, struct sql_thread *thd,
                                 int nettype)
{
    return osql_send_usedb_logic_int(pCur->db->tablename, thd->clnt,
                                     nettype);
}

static int osql_send_delrec_logic(struct BtCursor *pCur, struct sql_thread *thd,
                                  int nettype)
{

    struct sqlclntstate *clnt = thd->clnt;
    osqlstate_t *osql = &clnt->osql;
    unsigned long long rqid = thd->clnt->osql.rqid;
    int restarted;
    int rc = 0;

    /* limit transaction*/
    if ((rc = check_osql_capacity(thd)))
        return rc;

    do {
        rc = osql_send_usedb_logic(pCur, thd, nettype);
        if (rc == SQLITE_OK) {
            rc = osql_send_delrec(
                osql->host, osql->rqid, osql->uuid, pCur->genid,
                (gbl_partial_indexes && pCur->db->ix_partial) ? clnt->del_keys
                                                              : -1ULL,
                nettype, osql->logsb);
        }
        RESTART_SOCKSQL;
    } while (restarted && rc == 0);

    return rc;
}

static int osql_send_updstat_logic(struct BtCursor *pCur,
                                   struct sql_thread *thd, char *pData,
                                   int nData, int nStat, int nettype)
{
    struct sqlclntstate *clnt = thd->clnt;
    osqlstate_t *osql = &clnt->osql;
    int rc = 0;
    int restarted;

    do {
        rc = osql_send_updstat(osql->host, osql->rqid, osql->uuid, pCur->genid,
                               pData, nData, nStat, nettype, osql->logsb);
        RESTART_SOCKSQL;
    } while (restarted && rc == 0);

    return rc;
}

int osql_send_insidx_logic(struct BtCursor *pCur, struct sql_thread *thd,
                                  int nettype)
{
    struct sqlclntstate *clnt = thd->clnt;
    osqlstate_t *osql = &clnt->osql;
    int restarted;
    int rc = 0;
    int i;

    /* limit transaction*/
    if ((rc = check_osql_capacity(thd)))
        return rc;

restart:
    for (i = 0; i < pCur->db->nix; i++) {
        /* only send add keys when told */
        if (gbl_partial_indexes && pCur->db->ix_partial &&
            !(clnt->ins_keys & (1ULL << i)))
            continue;
        rc = osql_send_index(osql->host, osql->rqid, osql->uuid, pCur->genid, 0,
                             i, (char *)thd->clnt->idxInsert[i],
                             getkeysize(pCur->db, i), nettype, osql->logsb);
        RESTART_SOCKSQL;
        if (restarted && rc == 0)
            goto restart;
        if (rc)
            break;
    }
    return rc;
}

int osql_send_delidx_logic(struct BtCursor *pCur, struct sql_thread *thd,
                           int nettype)
{
    struct sqlclntstate *clnt = thd->clnt;
    osqlstate_t *osql = &clnt->osql;
    int restarted;
    int rc = 0;
    int i;

    /* limit transaction*/
    if ((rc = check_osql_capacity(thd)))
        return rc;

restart:
    for (i = 0; i < pCur->db->nix; i++) {
        /* only send delete keys when told */
        if (gbl_partial_indexes && pCur->db->ix_partial &&
            !(clnt->del_keys & (1ULL << i)))
            continue;

        rc = osql_send_index(osql->host, osql->rqid, osql->uuid, pCur->genid, 1,
                             i, (char *)thd->clnt->idxDelete[i],
                             getkeysize(pCur->db, i), nettype, osql->logsb);
        RESTART_SOCKSQL;
        if (restarted && rc == 0)
            goto restart;
        if (rc)
            break;
    }
    return rc;
}

static int osql_send_insrec_logic(struct BtCursor *pCur, struct sql_thread *thd,
                                  char *pData, int nData, int nettype,
                                  int flags)
{

    struct sqlclntstate *clnt = thd->clnt;
    osqlstate_t *osql = &clnt->osql;
    int restarted;
    int rc = 0;

    /* limit transaction*/
    if ((rc = check_osql_capacity(thd)))
        return rc;

    do {
        rc = osql_send_usedb_logic(pCur, thd, nettype);
        if (rc == SQLITE_OK) {
            rc = osql_send_insrec(
                osql->host, osql->rqid, osql->uuid, pCur->genid,
                (gbl_partial_indexes && pCur->db->ix_partial) ? clnt->ins_keys
                                                              : -1ULL,
                pData, nData, nettype, osql->logsb, flags);
        }
        RESTART_SOCKSQL;
    } while (restarted && rc == 0);

    return rc;
}

static int osql_send_updcols_logic(struct BtCursor *pCur,
                                   struct sql_thread *thd, int *updCols,
                                   int nettype)
{
    struct sqlclntstate *clnt = thd->clnt;
    osqlstate_t *osql = &clnt->osql;
    int restarted;
    int rc = 0;

    do {
        rc = osql_send_updcols(osql->host, osql->rqid, osql->uuid, pCur->genid,
                               nettype, &updCols[1], updCols[0], osql->logsb);
        RESTART_SOCKSQL;
    } while (restarted && rc == 0);

    return rc;
}

static int osql_send_qblobs_logic(struct BtCursor *pCur, struct sql_thread *thd,
                                  int *updCols, blob_buffer_t *blobs,
                                  int nettype)
{
    struct sqlclntstate *clnt = thd->clnt;
    osqlstate_t *osql = &clnt->osql;
    int rc = 0;
    int restarted;
    int i;
    int idx;
    int ncols;

    int actualblobs = pCur->db->schema->numblobs;

restart:
    for (i = 0; i < actualblobs; i++) {
        /* NOTE: the blobs are NOT clustered:
           create table t1(id int not null, b1 blob, b2 blob);
           insert into t1 (id, b2) values(0, x'11')
           so we need to run through all the defined blobs.
           we only need to send the non-null blobs, and the master
           will fix up those we missed.
         */

        if (!blobs[i].exists)
            continue;

        /* Send length of -2 if this isn't being used in this update. */
        if (updCols && gbl_osql_blob_optimization && blobs[i].length > 0) {
            idx = get_schema_blob_field_idx(pCur->db->tablename, ".ONDISK", i);
            /* AZ is pCur->db->schema not set to ondisk so we can instead call
             * get_schema_blob_field_idx_sc(pCur->db->schema,i); */
            ncols = updCols[0];
            if (idx >= 0 && idx < ncols && -1 == updCols[idx + 1]) {

                /* Put a token on the network if this isn't going to be used */
                rc = osql_send_qblob(osql->host, osql->rqid, osql->uuid, i,
                                     pCur->genid, nettype, NULL, -2,
                                     osql->logsb);
                RESTART_SOCKSQL;
                if (restarted && rc == 0)
                    goto restart;
                if (rc)
                    break;
                continue;
            }
        }

        rc = osql_send_qblob(osql->host, osql->rqid, osql->uuid, i, pCur->genid,
                             nettype, blobs[i].data, blobs[i].length,
                             osql->logsb);
        RESTART_SOCKSQL;
        if (restarted && rc == 0)
            goto restart;
        if (rc)
            break;
    }

    return rc;
}

static int osql_updcols(struct BtCursor *pCur, struct sql_thread *thd,
                        int *updCols)
{
    int rc = 0;

    if (thd->clnt->dbtran.mode == TRANLEVEL_SOSQL) {
        rc = osql_send_updcols_logic(pCur, thd, updCols, NET_OSQL_SOCK_RPL);
        if (rc) {
            logmsg(LOGMSG_ERROR,
                   "%s:%d %s - failed to send socksql row rc=%d\n", __FILE__,
                   __LINE__, __func__, rc);
            return rc;
        }
    }
    return osql_save_updcols(pCur, thd, updCols);
}

static int osql_qblobs(struct BtCursor *pCur, struct sql_thread *thd,
                       int *updCols, blob_buffer_t *blobs, int maxblobs,
                       int is_update)
{
    if (thd->clnt->dbtran.mode == TRANLEVEL_SOSQL) {
        int rc = osql_send_qblobs_logic(pCur, thd, updCols, blobs,
                                        NET_OSQL_SOCK_RPL);
        if (rc) {
            logmsg(LOGMSG_ERROR,
                   "%s:%d %s - failed to send socksql row rc=%d\n", __FILE__,
                   __LINE__, __func__, rc);
            return rc;
        }
    }
    return osql_save_qblobs(pCur, thd, blobs, maxblobs, is_update);
}

static int osql_send_updrec_logic(struct BtCursor *pCur, struct sql_thread *thd,
                                  char *pData, int nData, int nettype,
                                  int flags)
{

    struct sqlclntstate *clnt = thd->clnt;
    osqlstate_t *osql = &clnt->osql;
    int restarted;
    int rc = 0;

    if ((rc = check_osql_capacity(thd)))
        return rc;

    do {
        rc = osql_send_usedb_logic(pCur, thd, nettype);
        if (rc == SQLITE_OK) {

            rc = osql_send_updrec(
                osql->host, osql->rqid, osql->uuid, pCur->genid,
                (gbl_partial_indexes && pCur->db->ix_partial) ? clnt->ins_keys
                                                              : -1ULL,
                (gbl_partial_indexes && pCur->db->ix_partial) ? clnt->del_keys
                                                              : -1ULL,
                pData, nData, nettype, osql->logsb);
        }
        RESTART_SOCKSQL;
    } while (restarted && rc == 0);

    return rc;
}

static int osql_send_commit_logic(struct sqlclntstate *clnt, int is_retry,
                                  int nettype)
{
    osqlstate_t *osql = &clnt->osql;
    int rc = 0;
    int restarted = 0;
    snap_uid_t snap_info, *snap_info_p = NULL;

    /* reset the tablename */
    if (osql->tablename) {

        /* free the cache */
        free(osql->tablename);
        osql->tablename = NULL;
        osql->tablenamelen = 0;
    }
    osql->tran_ops = 0; /* reset transaction size counter*/

    extern int gbl_always_send_cnonce;
    int send_cnonce = gbl_always_send_cnonce ? 1 : has_high_availability(clnt);
    if (osql->rqid == OSQL_RQID_USE_UUID && send_cnonce &&
        get_cnonce(clnt, &snap_info) == 0 && !clnt->trans_has_sp) {

        /* pass to master the state of verify retry.
         * if verify retry is on and error is retryable, don't write to
         * blkseq on master because replicant will retry */
        snap_info.replicant_can_retry = replicant_can_retry(clnt);
        snap_info.effects = clnt->effects;
        comdb2uuidcpy(snap_info.uuid, osql->uuid);
        snap_info_p = &snap_info;
    }

retry:
    if (osql->rqid == OSQL_RQID_USE_UUID) {
        rc = osql_send_commit_by_uuid(osql->host, osql->uuid, osql->sentops,
                                      &osql->xerr, nettype, osql->logsb,
                                      clnt->query_stats, snap_info_p);
    } else {
        rc = osql_send_commit(osql->host, osql->rqid, osql->uuid, osql->sentops,
                              &osql->xerr, nettype, osql->logsb,
                              clnt->query_stats, NULL);
    }

    RESTART_SOCKSQL_KEEP_RQID(is_retry);

    if (rc == SQLITE_OK && restarted) {
        /* we need to reset the commit here */
        goto retry;
    }

    return rc;
}

static int osql_send_abort_logic(struct sqlclntstate *clnt, int nettype)
{
    osqlstate_t *osql = &clnt->osql;
    struct errstat xerr = {0};
    int rc = 0;
    uuidstr_t us;

    if (errstat_get_rc(&osql->xerr) != SQLITE_TOOBIG)
        errstat_set_rc(&xerr, SQLITE_ABORT);
    else
        errstat_set_rc(&xerr, SQLITE_TOOBIG);
    snprintf(&xerr.errstr[0], sizeof(xerr.errstr) - 1,
             "sql session %llu %s rollback", osql->rqid,
             comdb2uuidstr(osql->uuid, us));

    if (osql->rqid == OSQL_RQID_USE_UUID)
        rc = osql_send_commit_by_uuid(osql->host, osql->uuid, 0, &xerr, nettype,
                                      osql->logsb, clnt->query_stats, NULL);
    else
        rc = osql_send_commit(osql->host, osql->rqid, osql->uuid, 0, &xerr,
                              nettype, osql->logsb, clnt->query_stats, NULL);
    /* no need to restart an abort, master drop the transaction anyway
    RESTART_SOCKSQL; */

    return rc;
}

static int check_osql_capacity(struct sql_thread *thd)
{
    struct sqlclntstate *clnt = thd->clnt;
    osqlstate_t *osql = &thd->clnt->osql;

    osql->sentops++;
    osql->tran_ops++;

    if (clnt->osql_max_trans && osql->tran_ops > clnt->osql_max_trans) {
        /* This trace is used by ALMN 1779 to alert database owners.. please do
         * not change without reference to that almn. */
        logmsg(LOGMSG_ERROR, "check_osql_capacity: transaction size %d too big "
                        "(limit is %d) [\"%s\"]\n",
                osql->tran_ops, clnt->osql_max_trans,
                (thd->clnt && thd->clnt->sql)
                    ? thd->clnt->sql
                    : "not_set");

        errstat_set_rc(&osql->xerr, SQLITE_TOOBIG);
        errstat_set_str(&osql->xerr, "transaction too big\n");

        return SQLITE_TOOBIG;
    }

    return SQLITE_OK;
}

int osql_query_dbglog(struct sql_thread *thd, int queryid)
{

    struct sqlclntstate *clnt = thd->clnt;
    osqlstate_t *osql = &clnt->osql;
    int rc;
    int restarted;
    unsigned long long new_cookie;

    /* Why a new cookie?  The net send could be local (if no
       cluster, or the query somehow ends up running on
       the master) and we don't want to clobber the existing dbglog. */
    if (thd->clnt->master_dbglog_cookie == 0)
        thd->clnt->master_dbglog_cookie = get_id(thedb->bdb_env);
    new_cookie = thd->clnt->master_dbglog_cookie;
    do {
        rc = osql_send_dbglog(osql->host, osql->rqid, osql->uuid, new_cookie,
                              queryid, NET_OSQL_SOCK_RPL);
        /* not sure if we want to restart this */
        RESTART_SOCKSQL;
    } while (restarted && rc == 0);
    return rc;
}

#if 0
static int osql_send_cleartable_osqlsosql(struct sql_thread *thd, char *dbname) {

   osqlstate_t          *osql = &thd->clnt->osql;
   netinfo_type         *netinfo_ptr = (netinfo_type*)theosql->handle_sibling;
   osql_clrtbl_rpl_t    clr_rpl;
   int                  rc = 0;

   /* limit transaction*/
   if(rc = check_osql_capacity (thd)) return rc;

   rc = osql_send_usedb_int(thd, dbname, NET_OSQL_BLOCK_RPL);
   if (rc == SQLITE_OK) {

      clr_rpl.hd.type = OSQL_CLRTBL;
      clr_rpl.hd.sid = osql->rqid;

      if(osql->logsb) {
         sbuf2printf(thd->clnt->osql.logsb, "[%llu] send OSQL_CLRTBL %s\n", osql->rqid, dbname);
         sbuf2flush(thd->clnt->osql.logsb);
      }

      if(offload_net_send(netinfo_ptr, osql->host, NET_OSQL_BLOCK_RPL, &clr_rpl, sizeof(clr_rpl), 1)) {
         rc = SQLITE_INTERNAL;
         fprintf(stderr,
               "%s: error writting record to master in offload mode!\n", __func__);
      } else
         rc = SQLITE_OK;
   }

   return rc;
}

static int osql_send_cleartable_recom(struct sql_thread *thd, char *dbname) {
    fprintf(stderr, "cleartable not implemented yet\n");
    return -1;
}
int osql_cleartable(struct sql_thread *thd, char *dbname) {
    if(thd->clnt->dbtran.mode == TRANLEVEL_OSQL ||
          thd->clnt->dbtran.mode == TRANLEVEL_SOSQL)
        return osql_send_cleartable_osqlsosql(thd, dbname);
    else
        return osql_send_cleartable_recom(thd, dbname);
}

#endif

/**
 * Record a genid with the current transaction so we can
 * verify its existence upon commit
 *
 */
int osql_record_genid(struct BtCursor *pCur, struct sql_thread *thd,
                      unsigned long long genid)
{
    /* skip synthetic genids */
    if (is_genid_synthetic(genid)) {
        return 0;
    }

    if (thd->clnt->dbtran.mode == TRANLEVEL_SOSQL) {
        int rc =
            osql_send_recordgenid_logic(pCur, thd, genid, NET_OSQL_SOCK_RPL);
        if (rc) {
            logmsg(LOGMSG_ERROR,
                   "%s:%d %s - failed to send socksql row rc=%d\n", __FILE__,
                   __LINE__, __func__, rc);
            return rc;
        }
    }
    return osql_save_recordgenid(pCur, thd, genid);
}

static int osql_send_recordgenid_logic(struct BtCursor *pCur,
                                       struct sql_thread *thd,
                                       unsigned long long genid, int nettype)
{
    struct sqlclntstate *clnt = thd->clnt;
    osqlstate_t *osql = &clnt->osql;
    int restarted;
    int rc = 0;

    do {
        rc = osql_send_usedb_logic(pCur, thd, nettype);
        if (rc == SQLITE_OK) {

            if (osql->rqid == OSQL_RQID_USE_UUID)
                nettype = NET_OSQL_SOCK_RPL_UUID;

            rc = osql_send_recordgenid(osql->host, osql->rqid, osql->uuid,
                                       genid, nettype, osql->logsb);
            if (gbl_master_swing_sock_restart_sleep) {
                usleep(gbl_master_swing_sock_restart_sleep * 1000);
            }
        }
        RESTART_SOCKSQL;
    } while (restarted && rc == 0);

    return rc;
}

int osql_dbq_consume(struct sqlclntstate *clnt, const char *spname,
                     genid_t genid)
{
    Q4SP(qname, spname);
    osqlstate_t *osql = &clnt->osql;
    int rc = osql_send_usedb_logic_int(qname, clnt, NET_OSQL_SOCK_RPL);
    if (rc != SQLITE_OK)
        return rc;
    return osql_send_dbq_consume(osql->host, osql->rqid, osql->uuid, genid,
                                 NET_OSQL_SOCK_RPL, osql->logsb);
}

int osql_dbq_consume_logic(struct sqlclntstate *clnt, const char *spname,
                           genid_t genid)
{
    int rc;
    if ((rc = osql_save_dbq_consume(clnt, spname, genid)) != 0) {
        return rc;
    }
    if (clnt->dbtran.mode == TRANLEVEL_SOSQL) {
        rc = osql_dbq_consume(clnt, spname, genid);
    }
    return rc;
}

extern int gbl_allow_user_schema;

static int access_control_check_sql_write(struct BtCursor *pCur,
                                          struct sql_thread *thd)
{
    int rc = 0;
    int bdberr = 0;

    if (gbl_uses_accesscontrol_tableXnode) {
        rc = bdb_access_tbl_write_by_mach_get(
            pCur->db->dbenv->bdb_env, NULL, pCur->db->tablename,
            nodeix(thd->clnt->origin), &bdberr);
        if (rc <= 0) {
            char msg[1024];
            snprintf(
                msg, sizeof(msg), "Write access denied to %s from %d bdberr=%d",
                pCur->db->tablename, nodeix(thd->clnt->origin), bdberr);
            logmsg(LOGMSG_WARN, "%s\n", msg);
            errstat_set_rc(&thd->clnt->osql.xerr, SQLITE_ACCESS);
            errstat_set_str(&thd->clnt->osql.xerr, msg);

            return SQLITE_ABORT;
        }
    }

    /* Check read access if its not user schema. */
    /* Check it only if engine is open already. */
    if (gbl_uses_password &&
        (thd->clnt->no_transaction == 0)) {
        rc = bdb_check_user_tbl_access(
            pCur->db->dbenv->bdb_env, thd->clnt->user,
            pCur->db->tablename, ACCESS_WRITE, &bdberr);
        if (rc != 0) {
            char msg[1024];
            snprintf(msg, sizeof(msg),
                     "Write access denied to %s for user %s bdberr=%d",
                     pCur->db->tablename, thd->clnt->user, bdberr);
            logmsg(LOGMSG_WARN, "%s\n", msg);
            errstat_set_rc(&thd->clnt->osql.xerr, SQLITE_ACCESS);
            errstat_set_str(&thd->clnt->osql.xerr, msg);

            return SQLITE_ABORT;
        }
    }

    return 0;
}

int access_control_check_sql_read(struct BtCursor *pCur, struct sql_thread *thd)
{
    int rc = 0;
    int bdberr = 0;

    if (pCur->cursor_class == CURSORCLASS_TEMPTABLE)
        return 0;

    if (gbl_uses_accesscontrol_tableXnode) {
        rc = bdb_access_tbl_read_by_mach_get(
            pCur->db->dbenv->bdb_env, NULL, pCur->db->tablename,
            nodeix(thd->clnt->origin), &bdberr);
        if (rc <= 0) {
            char msg[1024];
            snprintf(
                msg, sizeof(msg), "Read access denied to %s from %d bdberr=%d",
                pCur->db->tablename, nodeix(thd->clnt->origin), bdberr);
            logmsg(LOGMSG_WARN, "%s\n", msg);
            errstat_set_rc(&thd->clnt->osql.xerr, SQLITE_ACCESS);
            errstat_set_str(&thd->clnt->osql.xerr, msg);

            return SQLITE_ABORT;
        }
    }

    /* Check read access if its not user schema. */
    /* Check it only if engine is open already. */
    if (gbl_uses_password && thd->clnt->no_transaction == 0) {
        rc = bdb_check_user_tbl_access(
            pCur->db->dbenv->bdb_env, thd->clnt->user,
            pCur->db->tablename, ACCESS_READ, &bdberr);
        if (rc != 0) {
            char msg[1024];
            snprintf(msg, sizeof(msg),
                     "Read access denied to %s for user %s bdberr=%d",
                     pCur->db->tablename, thd->clnt->user, bdberr);
            logmsg(LOGMSG_WARN, "%s\n", msg);
            errstat_set_rc(&thd->clnt->osql.xerr, SQLITE_ACCESS);
            errstat_set_str(&thd->clnt->osql.xerr, msg);

            return SQLITE_ABORT;
        }
    }

    return 0;
}

/**
*
* Process a schema change request
* Returns SQLITE_OK if successful.
*
*/
int osql_schemachange_logic(struct schema_change_type *sc,
                            struct sql_thread *thd, int usedb)
{
    struct sqlclntstate *clnt = thd->clnt;
    osqlstate_t *osql = &clnt->osql;
    int restarted;
    int rc = 0;
    unsigned long long rqid = thd->clnt->osql.rqid;
    unsigned long long version = 0;

    osql->running_ddl = 1;

    if (clnt->dml_tables && hash_find_readonly(clnt->dml_tables, sc->table)) {
        return SQLITE_DDL_MISUSE;
    }
    if (clnt->ddl_tables) {
        if (hash_find_readonly(clnt->ddl_tables, sc->table)) {
            return SQLITE_DDL_MISUSE;
        } else
            hash_add(clnt->ddl_tables, strdup(sc->table));
    }

    if (!bdb_attr_get(thedb->bdb_attr, BDB_ATTR_SC_RESUME_AUTOCOMMIT) ||
        clnt->in_client_trans) {
        sc->rqid = osql->rqid;
        comdb2uuidcpy(sc->uuid, osql->uuid);
    }

    if (thd->clnt->dbtran.mode == TRANLEVEL_SOSQL) {
        if (usedb) {
            if (getdbidxbyname(sc->table) < 0) { // view
                char *viewname = timepart_newest_shard(sc->table, &version);
                if (viewname) {
                    free(viewname);
                } else
                    usedb = 0;
            } else {
                version = comdb2_table_version(sc->table);
            }
        }
        do {
            rc = 0;
            if (usedb) {
                if (osql->tablename) {
                    /* free the cached tablename so that we send a new usedb for
                     * the next op */
                    free(osql->tablename);
                    osql->tablename = NULL;
                    osql->tablenamelen = 0;
                }
                rc = osql_send_usedb(osql->host, osql->rqid, osql->uuid,
                                     sc->table, NET_OSQL_SOCK_RPL, osql->logsb,
                                     version);
            }
            if (rc == SQLITE_OK) {
                rc = osql_send_schemachange(osql->host, rqid,
                                            thd->clnt->osql.uuid, sc,
                                            NET_OSQL_SOCK_RPL, osql->logsb);
            }
            RESTART_SOCKSQL;
        } while (restarted && rc == 0);
    }

    rc = osql_save_schemachange(thd, sc, usedb);
    if (rc) {
        logmsg(LOGMSG_ERROR,
               "%s:%d %s - failed to cache socksql schemachange rc=%d\n",
               __FILE__, __LINE__, __func__, rc);
        return rc;
    }

    return rc;
}

/**
 *
 * Process a bpfunc request
 * Returns SQLITE_OK if successful.
 *
 */
int osql_bpfunc_logic(struct sql_thread *thd, BpfuncArg *arg)
{
    struct sqlclntstate *clnt = thd->clnt;
    osqlstate_t *osql = &clnt->osql;
    unsigned long long rqid = thd->clnt->osql.rqid;
    int restarted;
    int rc;

    if (thd->clnt->dbtran.mode == TRANLEVEL_SOSQL) {
        do {
            rc = osql_send_bpfunc(osql->host, rqid, thd->clnt->osql.uuid, arg,
                                  NET_OSQL_SOCK_RPL, osql->logsb);
            RESTART_SOCKSQL;
        } while (restarted && rc == 0);
        if (rc) {
            logmsg(LOGMSG_ERROR,
                   "%s:%d %s - failed to send socksql bpfunc rc=%d\n", __FILE__,
                   __LINE__, __func__, rc);
            return rc;
        }
    }

    rc = osql_save_bpfunc(thd, arg);
    if (rc) {
        logmsg(LOGMSG_ERROR,
               "%s:%d %s - failed to cache socksql bpfunc rc=%d\n", __FILE__,
               __LINE__, __func__, rc);
        return rc;
    }
    return rc;
}
