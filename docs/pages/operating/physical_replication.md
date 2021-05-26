* The cluster is always considered at tier 0.

## comdb2_physreps
   * dbname string
   * host string
   * tier int
   * lsn string
   * last_keepalive datetime

create table comdb2_physreps(dbname cstring(20), host cstring(100), tier int, lsn cstring(60), last_keepalive datetime)$$
create unique index "idx1" on comdb2_physreps(dbname, host);

## comdb2_physrep_connections
   * dbname string
   * host string
   * source_dbname string
   * source_host string

create table comdb2_physrep_connections(dbname cstring(20), host cstring(100), source_dbname cstring(20), source_host cstring(100))$$
create unique index "idx1" on comdb2_physrep_connections(dbname, host, source_dbname, source_host);

## Stored procedures
  Following are the stored procedures that are executed by a replicant
  on the cluster (tier 0):

  * register_replicant()     : a request to register a new replicant (deprecated)
  * register_replicant_v1()  : a request to register a new replicant
  * confirm_registration()   : confirms that the replicant is now connected
  * keepalive()              : relays that the replicant ia alive by periodically
                               sending its lsn
## Tunables
  * replicate_from
  * elect_highest_committed_gen

## Metrics
  *

## Algorithm

create table comdb2_physreps(dbname cstring(20), host cstring(100), tier int, lsn cstring(60), last_keepalive datetime, state cstring(10))$$
create unique index "idx1" on comdb2_physreps(dbname, host);
create table comdb2_physrep_connections(dbname cstring(20), host cstring(100), source_dbname cstring(20), source_host cstring(100))$$
create unique index "idx1" on comdb2_physrep_connections(dbname, host, source_dbname, source_host);
