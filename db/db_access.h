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

#ifndef __INCLUDED_DB_AUTH_H
#define __INCLUDED_DB_AUTH_H

#include <plhash.h>

int gbl_enable_auth_cache;
uint64_t gbl_auth_gen;

void check_access_controls(struct dbenv *);
int check_sql_access(struct sqlthdstate *, struct sqlclntstate *);
/* Validate write access to database pointed by cursor pCur */
int access_control_check_sql_write(struct BtCursor *, struct sql_thread *);
/* Validate wead access to database pointed by cursor pCur */
int access_control_check_sql_read(struct BtCursor *, struct sql_thread *);

#endif /* !__INCLUDED_DB_AUTH_H */
