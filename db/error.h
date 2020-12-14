/*
   Copyright 2020, Bloomberg Finance L.P.

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

typedef enum comdb2_error_type {
    CDB2_ERR_IS_RETRIABLE = 1 << 0,
    CDB2_ERR_IS_IGNORABLE = 1 << 1,
    CDB2_ERR_RETRIED_BY_API = 1 << 2,
    CDB2_ERR_RETRIED_BY_REPLICANT = 1 << 3,
    CDB2_ERR_RETRIED_BY_MASTER = 1 << 4,
} comdb2_error_type_st;

typedef struct comdb2_error {
    int code;         // error code
    char *origin;     // module (core, csc2, berk, bdb, sqlite, plugins)
    char *message;    // error message
    char *reason;     // reason for failure
    char *where;      // file, line number
    time_t when;      // when did the error occur
    char sqlstate[5]; // sql state
    char *sql;        // sql string
    comdb2_error_type_st type;    // error properties
    LINKC_T(comdb2_error_st) lnk; // make it linkable
} comdb2_error_st;
