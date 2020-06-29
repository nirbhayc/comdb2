#include "comdb2.h"
#include "sql.h"
#include "bdb_api.h"
#include "bdb_access.h"
#include "nodemap.h"

extern int gbl_check_access_controls;
extern int gbl_upgrade_blocksql_2_socksql;

void delete_prepared_stmts(struct sqlthdstate *thd);

#if 0
int gbl_bpfunc_auth_gen = 1;
enum { AUTHENTICATE_READ = 1, AUTHENTICATE_WRITE = 2 };

int authenticate_cursor(BtCursor *pCur, int how);
int comdb2_authorizer_for_sqlite(
  void *pArg,        /* IN: NOT USED */
  int code,          /* IN: NOT USED */
  const char *zArg1, /* IN: NOT USED */
  const char *zArg2, /* IN: NOT USED */
  const char *zArg3, /* IN: NOT USED */
  const char *zArg4  /* IN: NOT USED */
#ifdef SQLITE_USER_AUTHENTICATION
  ,const char *zArg5 /* IN: NOT USED */
#endif
);
void comdb2_setup_authorizer_for_sqlite(
  sqlite3 *db,
  struct sql_authorizer_state *pAuthState,
  int bEnable
);
#endif

static void check_auth_enabled(struct dbenv *dbenv)
{
    int rc;
    int bdberr;

    rc = bdb_authentication_get(dbenv->bdb_env, NULL, &bdberr);
    if (rc) {
        gbl_uses_password = 0;
        logmsg(LOGMSG_WARN, "user authentication disabled (bdberr: %d)\n",
               bdberr);
        return;
    }

    gbl_uses_password = 1;
    gbl_upgrade_blocksql_2_socksql = 1;
    logmsg(LOGMSG_INFO, "user authentication enabled\n");
}

static void check_tableXnode_enabled(struct dbenv *dbenv)
{
    int rc;
    int bdberr;

    rc = bdb_accesscontrol_tableXnode_get(dbenv->bdb_env, NULL, &bdberr);
    if (rc) {
        gbl_uses_accesscontrol_tableXnode = 0;
        return;
    }

    if (bdb_access_create(dbenv->bdb_env, &bdberr)) {
        logmsg(LOGMSG_ERROR,
               "failed to enable tableXnode control (bdberr: %d\n)", bdberr);
        gbl_uses_accesscontrol_tableXnode = 0;
        return;
    }

    gbl_uses_accesscontrol_tableXnode = 1;
    logmsg(LOGMSG_INFO, "access control tableXnode enabled\n");
}

/* Check whether access controls have been enabled. */
void check_access_controls(struct dbenv *dbenv)
{
    check_auth_enabled(dbenv);
    check_tableXnode_enabled(dbenv);
}

/*
  If user password does not match, this function will write error
  response and return a non 0 rc.
*/
static inline int check_user_password(struct sqlclntstate *clnt)
{
    int rc = 0;
    int valid_user; // unused

    if (!gbl_uses_password)
        return 0;

    if (!clnt->current_user.have_name) {
        clnt->current_user.have_name = 1;
        strcpy(clnt->current_user.name, DEFAULT_USER);
    }

    if (!clnt->current_user.have_password) {
        clnt->current_user.have_password = 1;
        strcpy(clnt->current_user.password, DEFAULT_PASSWORD);
    }

    rc = bdb_user_password_check(clnt->current_user.name,
                                 clnt->current_user.password, &valid_user);

    if (rc != 0) {
        write_response(clnt, RESPONSE_ERROR_ACCESS, "access denied", 0);
        return 1;
    }
    return 0;
}

int check_sql_access(struct sqlthdstate *thd, struct sqlclntstate *clnt)
{
    int rc, bpfunc_auth_gen = gbl_bpfunc_auth_gen;

    /* Enable global access control flags, if not done already. */
    if (gbl_check_access_controls) {
        check_access_controls(thedb);
        gbl_check_access_controls = 0;
    }

    /* Free pass if our authentication gen is up-to-date. */
    if (clnt->authgen == bpfunc_auth_gen)
        return 0;

#if WITH_SSL
    /* Allow the user, if
       1) this is an SSL connection, and
       2) client sends a certificate, and
       3) client does not override the user
    */
    if (sslio_has_x509(clnt->sb) && clnt->current_user.is_x509_user)
        rc = 0;
    else
#endif
        rc = check_user_password(clnt);

    if (rc == 0) {
        if (thd->lastuser[0] != '\0' &&
            strcmp(thd->lastuser, clnt->current_user.name) != 0)
            delete_prepared_stmts(thd);
        strcpy(thd->lastuser, clnt->current_user.name);
        clnt->authgen = bpfunc_auth_gen;
    } else {
        clnt->authgen = 0;
    }

    return rc;
}

int access_control_check_sql_write(struct BtCursor *pCur,
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
            snprintf(msg, sizeof(msg),
                     "Write access denied to %s from %d bdberr=%d",
                     pCur->db->tablename, nodeix(thd->clnt->origin), bdberr);
            logmsg(LOGMSG_INFO, "%s\n", msg);
            errstat_set_rc(&thd->clnt->osql.xerr, SQLITE_ACCESS);
            errstat_set_str(&thd->clnt->osql.xerr, msg);

            return SQLITE_ABORT;
        }
    }

    /* Check read access if its not user schema. */
    /* Check it only if engine is open already. */
    if (gbl_uses_password && (thd->clnt->no_transaction == 0)) {
        rc = bdb_check_user_tbl_access(
            pCur->db->dbenv->bdb_env, thd->clnt->current_user.name,
            pCur->db->tablename, ACCESS_WRITE, &bdberr);
        if (rc != 0) {
            char msg[1024];
            snprintf(msg, sizeof(msg),
                     "Write access denied to %s for user %s bdberr=%d",
                     pCur->db->tablename, thd->clnt->current_user.name, bdberr);
            logmsg(LOGMSG_INFO, "%s\n", msg);
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
            snprintf(msg, sizeof(msg),
                     "Read access denied to %s from %d bdberr=%d",
                     pCur->db->tablename, nodeix(thd->clnt->origin), bdberr);
            logmsg(LOGMSG_INFO, "%s\n", msg);
            errstat_set_rc(&thd->clnt->osql.xerr, SQLITE_ACCESS);
            errstat_set_str(&thd->clnt->osql.xerr, msg);

            return SQLITE_ABORT;
        }
    }

    /* Check read access if its not user schema. */
    /* Check it only if engine is open already. */
    if (gbl_uses_password && thd->clnt->no_transaction == 0) {
        rc = bdb_check_user_tbl_access(
            pCur->db->dbenv->bdb_env, thd->clnt->current_user.name,
            pCur->db->tablename, ACCESS_READ, &bdberr);
        if (rc != 0) {
            char msg[1024];
            snprintf(msg, sizeof(msg),
                     "Read access denied to %s for user %s bdberr=%d",
                     pCur->db->tablename, thd->clnt->current_user.name, bdberr);
            logmsg(LOGMSG_INFO, "%s\n", msg);
            errstat_set_rc(&thd->clnt->osql.xerr, SQLITE_ACCESS);
            errstat_set_str(&thd->clnt->osql.xerr, msg);

            return SQLITE_ABORT;
        }
    }

    return 0;
}

// TODO(NC): unused?
int access_control_check_read(struct ireq *iq, tran_type *trans, int *bdberr)
{
    int rc = 0;

    if (gbl_uses_accesscontrol_tableXnode) {
        rc = bdb_access_tbl_read_by_mach_get(iq->dbenv->bdb_env, trans,
                                             iq->usedb->tablename,
                                             nodeix(iq->frommach), bdberr);
        if (rc <= 0) {
            reqerrstr(iq, ERR_ACCESS,
                      "Read access denied to %s from %s bdberr=%d\n",
                      iq->usedb->tablename, iq->corigin, *bdberr);
            return ERR_ACCESS;
        }
    }

    return 0;
}

int access_control_check_write(struct ireq *iq, tran_type *trans, int *bdberr)
{
    int rc = 0;

    if (gbl_uses_accesscontrol_tableXnode) {
        rc = bdb_access_tbl_write_by_mach_get(iq->dbenv->bdb_env, trans,
                                              iq->usedb->tablename,
                                              nodeix(iq->frommach), bdberr);
        if (rc <= 0) {
            reqerrstr(iq, ERR_ACCESS,
                      "Write access denied to %s from %s bdberr=%d\n",
                      iq->usedb->tablename, iq->corigin, *bdberr);
            return ERR_ACCESS;
        }
    }

    return 0;
}
