/*
   Copyright 2015, 2017 Bloomberg Finance L.P.

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

#ifndef INCLUDED_THRMAN_H
#define INCLUDED_THRMAN_H

/* Forward declarations. */
struct thr_handle;
struct dbenv;
typedef struct sqlpool sqlpool_t;

enum thrtype {
    THRTYPE_UNKNOWN = -1,
    THRTYPE_APPSOCK = 0,
    THRTYPE_SQLPOOL,
    THRTYPE_SQL,
    THRTYPE_REQ,
    THRTYPE_CONSUMER,
    THRTYPE_PURGEBLKSEQ,
    THRTYPE_PREFAULT_HELPER,
    THRTYPE_VERIFY,
    THRTYPE_ANALYZE,
    THRTYPE_PUSHLOG,
    THRTYPE_SCHEMACHANGE,
    THRTYPE_BBIPC_WAITFT,
    THRTYPE_LOGDELHOLD,
    THRTYPE_OSQL,
    THRTYPE_COORDINATOR,
    THRTYPE_APPSOCK_POOL,
    THRTYPE_SQLENGINEPOOL,
    THRTYPE_APPSOCK_SQL,
    THRTYPE_MTRAP,
    THRTYPE_QSTAT,
    THRTYPE_PURGEFILES,
    THRTYPE_BULK_IMPORT,
    THRTYPE_TRIGGER,
    THRTYPE_STAT,
    THRTYPE_TIMER,
    THRTYPE_QFLUSH,
    THRTYPE_OSQL_HEARTBEAT,
    THRTYPE_PREFAULT_IO,
    THRTYPE_EXIT_HANDLER,
    THRTYPE_ASYNC_LOG,
    THRTYPE_COMPACT,
    THRTYPE_SCHEDULER,
    THRTYPE_WATCHDOG,
    THRTYPE_WATCHDOG_WATCHER,
    THRTYPE_DECOM_NODE,
    THRTYPE_READER,
    THRTYPE_WRITER,
    THRTYPE_CONNECT,
    THRTYPE_ACCEPT,
    THRTYPE_CONNECT_AND_ACCEPT,
    THRTYPE_HEARTBEAT_SEND,
    THRTYPE_HEARTBEAT_CHECK,
    /*...*/
    THRTYPE_MAX,
};

enum thrsubtype {
    THRSUBTYPE_UNKNOWN = -1,
    THRSUBTYPE_TOPLEVEL_SQL = 0,
    THRSUBTYPE_LUA_SQL = 1,
    /*...*/
    THRSUBTYPE_MAX,
};

void thrman_init(void);
struct thr_handle *thrman_register(enum thrtype type);
void thrman_unregister(void);

/* Setters */
void thrman_set_sqlpool(struct thr_handle *thr, sqlpool_t *sqlpool);
void thrman_setid(struct thr_handle *thr, const char *idstr);
void thrman_setfd(struct thr_handle *thr, int fd);
void thrman_set_subtype(struct thr_handle *thr, enum thrsubtype subtype);
void thrman_where(struct thr_handle *thr, const char *where);
void thrman_wheref(struct thr_handle *thr, const char *fmt, ...);
void thrman_origin(struct thr_handle *thr, const char *origin);

/* Getters */
struct thr_handle *thrman_self(void);
pthread_t thrman_get_tid(struct thr_handle *thr);
enum thrtype thrman_get_type(struct thr_handle *thr);
enum thrsubtype thrman_get_sub_type(struct thr_handle *thr);
const char *thrman_get_where(struct thr_handle *thr);
struct reqlogger *thrman_get_reqlogger(struct thr_handle *thr);
time_t thrman_get_when(struct thr_handle *thr);

/* Misc functions */
void thrman_change_type(struct thr_handle *thr, enum thrtype newtype);
char *thrman_describe(struct thr_handle *thr, char *buf, size_t szbuf);
const char *thrman_type2a(enum thrtype type);
void thrman_dump(void);
void thrman_stop_sql_connections(void);
void stop_threads(struct dbenv *dbenv);
void resume_threads(struct dbenv *dbenv);
int thrman_lock();
int thrman_unlock();
struct thr_handle *thrman_next_thread(struct thr_handle *thread);

#endif /* INCLUDED_THRMAN_H */
