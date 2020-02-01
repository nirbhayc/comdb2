/*
** 2010 July 12
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This file contains an implementation of the "dbstat" virtual table.
**
** The dbstat virtual table is used to extract low-level formatting
** information from an SQLite database in order to implement the
** "sqlitex_analyzer" utility.  See the ../tool/spaceanal.tcl script
** for an example implementation.
*/

#if (defined(SQLITE_ENABLE_DBSTAT_VTAB) || defined(SQLITE_TEST)) \
    && !defined(SQLITE_OMIT_VIRTUALTABLE)
#include "sqliteInt.h"   /* Requires access to internal data structures */

/*
** Page paths:
** 
**   The value of the 'path' column describes the path taken from the 
**   root-node of the b-tree structure to each page. The value of the 
**   root-node path is '/'.
**
**   The value of the path for the left-most child page of the root of
**   a b-tree is '/000/'. (Btrees store content ordered from left to right
**   so the pages to the left have smaller keys than the pages to the right.)
**   The next to left-most child of the root page is
**   '/001', and so on, each sibling page identified by a 3-digit hex 
**   value. The children of the 451st left-most sibling have paths such
**   as '/1c2/000/, '/1c2/001/' etc.
**
**   Overflow pages are specified by appending a '+' character and a 
**   six-digit hexadecimal value to the path to the cell they are linked
**   from. For example, the three overflow pages in a chain linked from 
**   the left-most cell of the 450th child of the root page are identified
**   by the paths:
**
**      '/1c2/000+000000'         // First page in overflow chain
**      '/1c2/000+000001'         // Second page in overflow chain
**      '/1c2/000+000002'         // Third page in overflow chain
**
**   If the paths are sorted using the BINARY collation sequence, then
**   the overflow pages associated with a cell will appear earlier in the
**   sort-order than its child page:
**
**      '/1c2/000/'               // Left-most child of 451st child of root
*/
#define VTAB_SCHEMA                                                         \
  "CREATE TABLE xx( "                                                       \
  "  name       STRING,           /* Name of table or index */"             \
  "  path       INTEGER,          /* Path to page from root */"             \
  "  pageno     INTEGER,          /* Page number */"                        \
  "  pagetype   STRING,           /* 'internal', 'leaf' or 'overflow' */"   \
  "  ncell      INTEGER,          /* Cells on page (0 for overflow) */"     \
  "  payload    INTEGER,          /* Bytes of payload on this page */"      \
  "  unused     INTEGER,          /* Bytes of unused space on this page */" \
  "  mx_payload INTEGER,          /* Largest payload size of all cells */"  \
  "  pgoffset   INTEGER,          /* Offset of page in file */"             \
  "  pgsize     INTEGER           /* Size of the page */"                   \
  ");"


typedef struct StatTable StatTable;
typedef struct StatCursor StatCursor;
typedef struct StatPage StatPage;
typedef struct StatCell StatCell;

struct StatCell {
  int nLocal;                     /* Bytes of local payload */
  u32 iChildPg;                   /* Child node (or 0 if this is a leaf) */
  int nOvfl;                      /* Entries in aOvfl[] */
  u32 *aOvfl;                     /* Array of overflow page numbers */
  int nLastOvfl;                  /* Bytes of payload on final overflow page */
  int iOvfl;                      /* Iterates through aOvfl[] */
};

struct StatPage {
  u32 iPgno;
  DbPage *pPg;
  int iCell;

  char *zPath;                    /* Path to this page */

  /* Variables populated by statDecodePage(): */
  u8 flags;                       /* Copy of flags byte */
  int nCell;                      /* Number of cells on page */
  int nUnused;                    /* Number of unused bytes on page */
  StatCell *aCell;                /* Array of parsed cells */
  u32 iRightChildPg;              /* Right-child page number (or 0) */
  int nMxPayload;                 /* Largest payload of any cell on this page */
};

struct StatCursor {
  sqlitex_vtab_cursor base;
  sqlitex_stmt *pStmt;            /* Iterates through set of root pages */
  int isEof;                      /* After pStmt has returned SQLITE_DONEX */

  StatPage aPage[32];
  int iPage;                      /* Current entry in aPage[] */

  /* Values to return. */
  char *zName;                    /* Value of 'name' column */
  char *zPath;                    /* Value of 'path' column */
  u32 iPageno;                    /* Value of 'pageno' column */
  char *zPagetype;                /* Value of 'pagetype' column */
  int nCell;                      /* Value of 'ncell' column */
  int nPayload;                   /* Value of 'payload' column */
  int nUnused;                    /* Value of 'unused' column */
  int nMxPayload;                 /* Value of 'mx_payload' column */
  i64 iOffset;                    /* Value of 'pgOffset' column */
  int szPage;                     /* Value of 'pgSize' column */
};

struct StatTable {
  sqlitex_vtab base;
  sqlitex *db;
  int iDb;                        /* Index of database to analyze */
};

#ifndef get2byte
# define get2byte(x)   ((x)[0]<<8 | (x)[1])
#endif

/*
** Connect to or create a statvfs virtual table.
*/
static int statConnect(
  sqlitex *db,
  void *pAux,
  int argc, const char *const*argv,
  sqlitex_vtab **ppVtab,
  char **pzErr
){
  StatTable *pTab = 0;
  int rc = SQLITE_OK;
  int iDb;

  if( argc>=4 ){
    iDb = sqlitexFindDbName(db, argv[3]);
    if( iDb<0 ){
      *pzErr = sqlitex_mprintf("no such database: %s", argv[3]);
      return SQLITE_ERROR;
    }
  }else{
    iDb = 0;
  }
  rc = sqlitex_declare_vtab(db, VTAB_SCHEMA);
  if( rc==SQLITE_OK ){
    pTab = (StatTable *)sqlitex_malloc64(sizeof(StatTable));
    if( pTab==0 ) rc = SQLITE_NOMEM;
  }

  assert( rc==SQLITE_OK || pTab==0 );
  if( rc==SQLITE_OK ){
    memset(pTab, 0, sizeof(StatTable));
    pTab->db = db;
    pTab->iDb = iDb;
  }

  *ppVtab = (sqlitex_vtab*)pTab;
  return rc;
}

/*
** Disconnect from or destroy a statvfs virtual table.
*/
static int statDisconnect(sqlitex_vtab *pVtab){
  sqlitex_free(pVtab);
  return SQLITE_OK;
}

/*
** There is no "best-index". This virtual table always does a linear
** scan of the binary VFS log file.
*/
static int statBestIndex(sqlitex_vtab *tab, sqlitex_index_info *pIdxInfo){

  /* Records are always returned in ascending order of (name, path). 
  ** If this will satisfy the client, set the orderByConsumed flag so that 
  ** SQLite does not do an external sort.
  */
  if( ( pIdxInfo->nOrderBy==1
     && pIdxInfo->aOrderBy[0].iColumn==0
     && pIdxInfo->aOrderBy[0].desc==0
     ) ||
      ( pIdxInfo->nOrderBy==2
     && pIdxInfo->aOrderBy[0].iColumn==0
     && pIdxInfo->aOrderBy[0].desc==0
     && pIdxInfo->aOrderBy[1].iColumn==1
     && pIdxInfo->aOrderBy[1].desc==0
     )
  ){
    pIdxInfo->orderByConsumed = 1;
  }

  pIdxInfo->estimatedCost = 10.0;
  return SQLITE_OK;
}

/*
** Open a new statvfs cursor.
*/
static int statOpen(sqlitex_vtab *pVTab, sqlitex_vtab_cursor **ppCursor){
  StatTable *pTab = (StatTable *)pVTab;
  StatCursor *pCsr;
  int rc;

  pCsr = (StatCursor *)sqlitex_malloc64(sizeof(StatCursor));
  if( pCsr==0 ){
    rc = SQLITE_NOMEM;
  }else{
    char *zSql;
    memset(pCsr, 0, sizeof(StatCursor));
    pCsr->base.pVtab = pVTab;

    zSql = sqlitex_mprintf(
        "SELECT 'sqlite_master' AS name, 1 AS rootpage, 'table' AS type"
        "  UNION ALL  "
        "SELECT name, rootpage, type"
        "  FROM \"%w\".sqlite_master WHERE rootpage!=0"
        "  ORDER BY name", pTab->db->aDb[pTab->iDb].zName);
    if( zSql==0 ){
      rc = SQLITE_NOMEM;
    }else{
      rc = sqlitex_prepare_v2(pTab->db, zSql, -1, &pCsr->pStmt, 0);
      sqlitex_free(zSql);
    }
    if( rc!=SQLITE_OK ){
      sqlitex_free(pCsr);
      pCsr = 0;
    }
  }

  *ppCursor = (sqlitex_vtab_cursor *)pCsr;
  return rc;
}

static void statClearPage(StatPage *p){
  int i;
  if( p->aCell ){
    for(i=0; i<p->nCell; i++){
      sqlitex_free(p->aCell[i].aOvfl);
    }
    sqlitex_free(p->aCell);
  }
  sqlitexPagerUnref(p->pPg);
  sqlitex_free(p->zPath);
  memset(p, 0, sizeof(StatPage));
}

static void statResetCsr(StatCursor *pCsr){
  int i;
  sqlitex_reset(pCsr->pStmt);
  for(i=0; i<ArraySize(pCsr->aPage); i++){
    statClearPage(&pCsr->aPage[i]);
  }
  pCsr->iPage = 0;
  sqlitex_free(pCsr->zPath);
  pCsr->zPath = 0;
}

/*
** Close a statvfs cursor.
*/
static int statClose(sqlitex_vtab_cursor *pCursor){
  StatCursor *pCsr = (StatCursor *)pCursor;
  statResetCsr(pCsr);
  sqlitex_finalize(pCsr->pStmt);
  sqlitex_free(pCsr);
  return SQLITE_OK;
}

static void getLocalPayload(
  int nUsable,                    /* Usable bytes per page */
  u8 flags,                       /* Page flags */
  int nTotal,                     /* Total record (payload) size */
  int *pnLocal                    /* OUT: Bytes stored locally */
){
  int nLocal;
  int nMinLocal;
  int nMaxLocal;
 
  if( flags==0x0D ){              /* Table leaf node */
    nMinLocal = (nUsable - 12) * 32 / 255 - 23;
    nMaxLocal = nUsable - 35;
  }else{                          /* Index interior and leaf nodes */
    nMinLocal = (nUsable - 12) * 32 / 255 - 23;
    nMaxLocal = (nUsable - 12) * 64 / 255 - 23;
  }

  nLocal = nMinLocal + (nTotal - nMinLocal) % (nUsable - 4);
  if( nLocal>nMaxLocal ) nLocal = nMinLocal;
  *pnLocal = nLocal;
}

static int statDecodePage(Btree *pBt, StatPage *p){
  int nUnused;
  int iOff;
  int nHdr;
  int isLeaf;
  int szPage;

  u8 *aData = sqlitexPagerGetData(p->pPg);
  u8 *aHdr = &aData[p->iPgno==1 ? 100 : 0];

  p->flags = aHdr[0];
  p->nCell = get2byte(&aHdr[3]);
  p->nMxPayload = 0;

  isLeaf = (p->flags==0x0A || p->flags==0x0D);
  nHdr = 12 - isLeaf*4 + (p->iPgno==1)*100;

  nUnused = get2byte(&aHdr[5]) - nHdr - 2*p->nCell;
  nUnused += (int)aHdr[7];
  iOff = get2byte(&aHdr[1]);
  while( iOff ){
    nUnused += get2byte(&aData[iOff+2]);
    iOff = get2byte(&aData[iOff]);
  }
  p->nUnused = nUnused;
  p->iRightChildPg = isLeaf ? 0 : sqlitexGet4byte(&aHdr[8]);
  szPage = sqlite3BtreeGetPageSize(pBt);

  if( p->nCell ){
    int i;                        /* Used to iterate through cells */
    int nUsable;                  /* Usable bytes per page */

    sqlite3BtreeEnter(pBt);
    nUsable = szPage - sqlite3BtreeGetReserveNoMutex(pBt);
    sqlite3BtreeLeave(pBt);
    p->aCell = sqlitex_malloc64((p->nCell+1) * sizeof(StatCell));
    if( p->aCell==0 ) return SQLITE_NOMEM;
    memset(p->aCell, 0, (p->nCell+1) * sizeof(StatCell));

    for(i=0; i<p->nCell; i++){
      StatCell *pCell = &p->aCell[i];

      iOff = get2byte(&aData[nHdr+i*2]);
      if( !isLeaf ){
        pCell->iChildPg = sqlitexGet4byte(&aData[iOff]);
        iOff += 4;
      }
      if( p->flags==0x05 ){
        /* A table interior node. nPayload==0. */
      }else{
        u32 nPayload;             /* Bytes of payload total (local+overflow) */
        int nLocal;               /* Bytes of payload stored locally */
        iOff += getVarint32(&aData[iOff], nPayload);
        if( p->flags==0x0D ){
          u64 dummy;
          iOff += sqlitexGetVarint(&aData[iOff], &dummy);
        }
        if( nPayload>(u32)p->nMxPayload ) p->nMxPayload = nPayload;
        getLocalPayload(nUsable, p->flags, nPayload, &nLocal);
        pCell->nLocal = nLocal;
        assert( nLocal>=0 );
        assert( nPayload>=(u32)nLocal );
        assert( nLocal<=(nUsable-35) );
        if( nPayload>(u32)nLocal ){
          int j;
          int nOvfl = ((nPayload - nLocal) + nUsable-4 - 1) / (nUsable - 4);
          pCell->nLastOvfl = (nPayload-nLocal) - (nOvfl-1) * (nUsable-4);
          pCell->nOvfl = nOvfl;
          pCell->aOvfl = sqlitex_malloc64(sizeof(u32)*nOvfl);
          if( pCell->aOvfl==0 ) return SQLITE_NOMEM;
          pCell->aOvfl[0] = sqlitexGet4byte(&aData[iOff+nLocal]);
          for(j=1; j<nOvfl; j++){
            int rc;
            u32 iPrev = pCell->aOvfl[j-1];
            DbPage *pPg = 0;
            rc = sqlitexPagerGet(sqlite3BtreePager(pBt), iPrev, &pPg);
            if( rc!=SQLITE_OK ){
              assert( pPg==0 );
              return rc;
            } 
            pCell->aOvfl[j] = sqlitexGet4byte(sqlitexPagerGetData(pPg));
            sqlitexPagerUnref(pPg);
          }
        }
      }
    }
  }

  return SQLITE_OK;
}

/*
** Populate the pCsr->iOffset and pCsr->szPage member variables. Based on
** the current value of pCsr->iPageno.
*/
static void statSizeAndOffset(StatCursor *pCsr){
  StatTable *pTab = (StatTable *)((sqlitex_vtab_cursor *)pCsr)->pVtab;
  Btree *pBt = pTab->db->aDb[pTab->iDb].pBt;
  Pager *pPager = sqlite3BtreePager(pBt);
  sqlitex_file *fd;
  sqlitex_int64 x[2];

  /* The default page size and offset */
  pCsr->szPage = sqlite3BtreeGetPageSize(pBt);
  pCsr->iOffset = (i64)pCsr->szPage * (pCsr->iPageno - 1);

  /* If connected to a ZIPVFS backend, override the page size and
  ** offset with actual values obtained from ZIPVFS.
  */
  fd = sqlite3PagerFile(pPager);
  x[0] = pCsr->iPageno;
  if( fd->pMethods!=0 && sqlitexOsFileControl(fd, 230440, &x)==SQLITE_OK ){
    pCsr->iOffset = x[0];
    pCsr->szPage = (int)x[1];
  }
}

/*
** Move a statvfs cursor to the next entry in the file.
*/
static int statNext(sqlitex_vtab_cursor *pCursor){
  int rc;
  int nPayload;
  char *z;
  StatCursor *pCsr = (StatCursor *)pCursor;
  StatTable *pTab = (StatTable *)pCursor->pVtab;
  Btree *pBt = pTab->db->aDb[pTab->iDb].pBt;
  Pager *pPager = sqlite3BtreePager(pBt);

  sqlitex_free(pCsr->zPath);
  pCsr->zPath = 0;

statNextRestart:
  if( pCsr->aPage[0].pPg==0 ){
    rc = sqlitex_step(pCsr->pStmt);
    if( rc==SQLITE_ROW ){
      int nPage;
      u32 iRoot = (u32)sqlitex_column_int64(pCsr->pStmt, 1);
      sqlitexPagerPagecount(pPager, &nPage);
      if( nPage==0 ){
        pCsr->isEof = 1;
        return sqlitex_reset(pCsr->pStmt);
      }
      rc = sqlitexPagerGet(pPager, iRoot, &pCsr->aPage[0].pPg);
      pCsr->aPage[0].iPgno = iRoot;
      pCsr->aPage[0].iCell = 0;
      pCsr->aPage[0].zPath = z = sqlitex_mprintf("/");
      pCsr->iPage = 0;
      if( z==0 ) rc = SQLITE_NOMEM;
    }else{
      pCsr->isEof = 1;
      return sqlitex_reset(pCsr->pStmt);
    }
  }else{

    /* Page p itself has already been visited. */
    StatPage *p = &pCsr->aPage[pCsr->iPage];

    while( p->iCell<p->nCell ){
      StatCell *pCell = &p->aCell[p->iCell];
      if( pCell->iOvfl<pCell->nOvfl ){
        int nUsable;
        sqlite3BtreeEnter(pBt);
        nUsable = sqlite3BtreeGetPageSize(pBt) - 
                        sqlite3BtreeGetReserveNoMutex(pBt);
        sqlite3BtreeLeave(pBt);
        pCsr->zName = (char *)sqlitex_column_text(pCsr->pStmt, 0);
        pCsr->iPageno = pCell->aOvfl[pCell->iOvfl];
        pCsr->zPagetype = "overflow";
        pCsr->nCell = 0;
        pCsr->nMxPayload = 0;
        pCsr->zPath = z = sqlitex_mprintf(
            "%s%.3x+%.6x", p->zPath, p->iCell, pCell->iOvfl
        );
        if( pCell->iOvfl<pCell->nOvfl-1 ){
          pCsr->nUnused = 0;
          pCsr->nPayload = nUsable - 4;
        }else{
          pCsr->nPayload = pCell->nLastOvfl;
          pCsr->nUnused = nUsable - 4 - pCsr->nPayload;
        }
        pCell->iOvfl++;
        statSizeAndOffset(pCsr);
        return z==0 ? SQLITE_NOMEM : SQLITE_OK;
      }
      if( p->iRightChildPg ) break;
      p->iCell++;
    }

    if( !p->iRightChildPg || p->iCell>p->nCell ){
      statClearPage(p);
      if( pCsr->iPage==0 ) return statNext(pCursor);
      pCsr->iPage--;
      goto statNextRestart; /* Tail recursion */
    }
    pCsr->iPage++;
    assert( p==&pCsr->aPage[pCsr->iPage-1] );

    if( p->iCell==p->nCell ){
      p[1].iPgno = p->iRightChildPg;
    }else{
      p[1].iPgno = p->aCell[p->iCell].iChildPg;
    }
    rc = sqlitexPagerGet(pPager, p[1].iPgno, &p[1].pPg);
    p[1].iCell = 0;
    p[1].zPath = z = sqlitex_mprintf("%s%.3x/", p->zPath, p->iCell);
    p->iCell++;
    if( z==0 ) rc = SQLITE_NOMEM;
  }


  /* Populate the StatCursor fields with the values to be returned
  ** by the xColumn() and xRowid() methods.
  */
  if( rc==SQLITE_OK ){
    int i;
    StatPage *p = &pCsr->aPage[pCsr->iPage];
    pCsr->zName = (char *)sqlitex_column_text(pCsr->pStmt, 0);
    pCsr->iPageno = p->iPgno;

    rc = statDecodePage(pBt, p);
    if( rc==SQLITE_OK ){
      statSizeAndOffset(pCsr);

      switch( p->flags ){
        case 0x05:             /* table internal */
        case 0x02:             /* index internal */
          pCsr->zPagetype = "internal";
          break;
        case 0x0D:             /* table leaf */
        case 0x0A:             /* index leaf */
          pCsr->zPagetype = "leaf";
          break;
        default:
          pCsr->zPagetype = "corrupted";
          break;
      }
      pCsr->nCell = p->nCell;
      pCsr->nUnused = p->nUnused;
      pCsr->nMxPayload = p->nMxPayload;
      pCsr->zPath = z = sqlitex_mprintf("%s", p->zPath);
      if( z==0 ) rc = SQLITE_NOMEM;
      nPayload = 0;
      for(i=0; i<p->nCell; i++){
        nPayload += p->aCell[i].nLocal;
      }
      pCsr->nPayload = nPayload;
    }
  }

  return rc;
}

static int statEof(sqlitex_vtab_cursor *pCursor){
  StatCursor *pCsr = (StatCursor *)pCursor;
  return pCsr->isEof;
}

static int statFilter(
  sqlitex_vtab_cursor *pCursor, 
  int idxNum, const char *idxStr,
  int argc, sqlitex_value **argv
){
  StatCursor *pCsr = (StatCursor *)pCursor;

  statResetCsr(pCsr);
  return statNext(pCursor);
}

static int statColumn(
  sqlitex_vtab_cursor *pCursor, 
  sqlitex_context *ctx, 
  int i
){
  StatCursor *pCsr = (StatCursor *)pCursor;
  switch( i ){
    case 0:            /* name */
      sqlitex_result_text(ctx, pCsr->zName, -1, SQLITEX_TRANSIENT);
      break;
    case 1:            /* path */
      sqlitex_result_text(ctx, pCsr->zPath, -1, SQLITEX_TRANSIENT);
      break;
    case 2:            /* pageno */
      sqlitex_result_int64(ctx, pCsr->iPageno);
      break;
    case 3:            /* pagetype */
      sqlitex_result_text(ctx, pCsr->zPagetype, -1, SQLITEX_STATIC);
      break;
    case 4:            /* ncell */
      sqlitex_result_int(ctx, pCsr->nCell);
      break;
    case 5:            /* payload */
      sqlitex_result_int(ctx, pCsr->nPayload);
      break;
    case 6:            /* unused */
      sqlitex_result_int(ctx, pCsr->nUnused);
      break;
    case 7:            /* mx_payload */
      sqlitex_result_int(ctx, pCsr->nMxPayload);
      break;
    case 8:            /* pgoffset */
      sqlitex_result_int64(ctx, pCsr->iOffset);
      break;
    default:           /* pgsize */
      assert( i==9 );
      sqlitex_result_int(ctx, pCsr->szPage);
      break;
  }
  return SQLITE_OK;
}

static int statRowid(sqlitex_vtab_cursor *pCursor, sqlite_int64 *pRowid){
  StatCursor *pCsr = (StatCursor *)pCursor;
  *pRowid = pCsr->iPageno;
  return SQLITE_OK;
}

/*
** Invoke this routine to register the "dbstat" virtual table module
*/
int sqlitexDbstatRegister(sqlitex *db){
  static sqlitex_module dbstat_module = {
    0,                            /* iVersion */
    statConnect,                  /* xCreate */
    statConnect,                  /* xConnect */
    statBestIndex,                /* xBestIndex */
    statDisconnect,               /* xDisconnect */
    statDisconnect,               /* xDestroy */
    statOpen,                     /* xOpen - open a cursor */
    statClose,                    /* xClose - close a cursor */
    statFilter,                   /* xFilter - configure scan constraints */
    statNext,                     /* xNext - advance a cursor */
    statEof,                      /* xEof - check for end of scan */
    statColumn,                   /* xColumn - read data */
    statRowid,                    /* xRowid - read data */
    0,                            /* xUpdate */
    0,                            /* xBegin */
    0,                            /* xSync */
    0,                            /* xCommit */
    0,                            /* xRollback */
    0,                            /* xFindMethod */
    0,                            /* xRename */
  };
  return sqlitex_create_module(db, "dbstat", &dbstat_module, 0);
}
#endif /* SQLITE_ENABLE_DBSTAT_VTAB */
