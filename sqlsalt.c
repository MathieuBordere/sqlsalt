#if defined(SQLITE_AMALGAMATION) && !defined(SQLSALT_STATIC)
# define SQLSALT_STATIC
#endif
#ifdef SQLSALT_STATIC
# include "sqlite3.h"
#else
# include "sqlite3ext.h"
  SQLITE_EXTENSION_INIT1
#endif

#include <assert.h>
#include <string.h>

#ifdef SQLSALT_DEBUG
  #include <stdio.h>
  #define DEBUG_PRINTF(...) fprintf(stderr, __VA_ARGS__)
#else
  #define DEBUG_PRINTF(...) ((void)0)
#endif

/*
** Forward declaration of objects used by this utility
*/
typedef struct sqlite3_vfs sqlsaltVfs;

/* Access to a lower-level VFS that (might) implement dynamic loading,
** access to randomness, etc.
*/
#define ORIGVFS(p)  ((sqlite3_vfs*)((p)->pAppData))
#define ORIGFILE(p) ((sqlite3_file*)(((sqlsaltFile*)(p))+1))

/*
** Methods for sqlsaltFile
*/
static int sqlsaltClose(sqlite3_file*);
static int sqlsaltRead(sqlite3_file*, void*, int iAmt, sqlite3_int64 iOfst);
static int sqlsaltWrite(sqlite3_file*,const void*,int iAmt, sqlite3_int64 iOfst);
static int sqlsaltTruncate(sqlite3_file*, sqlite3_int64 size);
static int sqlsaltSync(sqlite3_file*, int flags);
static int sqlsaltFileSize(sqlite3_file*, sqlite3_int64 *pSize);
static int sqlsaltLock(sqlite3_file*, int);
static int sqlsaltUnlock(sqlite3_file*, int);
static int sqlsaltCheckReservedLock(sqlite3_file*, int *pResOut);
static int sqlsaltFileControl(sqlite3_file*, int op, void *pArg);
static int sqlsaltSectorSize(sqlite3_file*);
static int sqlsaltDeviceCharacteristics(sqlite3_file*);
static int sqlsaltShmMap(sqlite3_file*, int iPg, int pgsz, int, void volatile**);
static int sqlsaltShmLock(sqlite3_file*, int offset, int n, int flags);
static void sqlsaltShmBarrier(sqlite3_file*);
static int sqlsaltShmUnmap(sqlite3_file*, int deleteFlag);
static int sqlsaltFetch(sqlite3_file*, sqlite3_int64 iOfst, int iAmt, void **pp);
static int sqlsaltUnfetch(sqlite3_file*, sqlite3_int64 iOfst, void *p);

/*
** Methods for sqlsaltVfs
*/
static int sqlsaltOpen(sqlite3_vfs*, const char *, sqlite3_file*, int , int *);
static int sqlsaltDelete(sqlite3_vfs*, const char *zName, int syncDir);
static int sqlsaltAccess(sqlite3_vfs*, const char *zName, int flags, int *);
static int sqlsaltFullPathname(sqlite3_vfs*, const char *zName, int, char *zOut);
static void *sqlsaltDlOpen(sqlite3_vfs*, const char *zFilename);
static void sqlsaltDlError(sqlite3_vfs*, int nByte, char *zErrMsg);
static void (*sqlsaltDlSym(sqlite3_vfs *pVfs, void *p, const char*zSym))(void);
static void sqlsaltDlClose(sqlite3_vfs*, void*);
static int sqlsaltRandomness(sqlite3_vfs*, int nByte, char *zOut);
static int sqlsaltSleep(sqlite3_vfs*, int microseconds);
static int sqlsaltCurrentTime(sqlite3_vfs*, double*);
static int sqlsaltGetLastError(sqlite3_vfs*, int, char *);
static int sqlsaltCurrentTimeInt64(sqlite3_vfs*, sqlite3_int64*);
static int sqlsaltSetSystemCall(sqlite3_vfs*, const char*,sqlite3_syscall_ptr);
static sqlite3_syscall_ptr sqlsaltGetSystemCall(sqlite3_vfs*, const char *z);
static const char *sqlsaltNextSystemCall(sqlite3_vfs*, const char *zName);

static sqlite3_vfs sqlsalt_vfs = {
  3,                            /* iVersion (set when registered) */
  0,                            /* szOsFile (set when registered) */
  1024,                         /* mxPathname */
  0,                            /* pNext */
  "sqlsaltvfs",                 /* zName */
  0,                            /* pAppData (set when registered) */ 
  sqlsaltOpen,                  /* xOpen */
  sqlsaltDelete,                /* xDelete */
  sqlsaltAccess,                /* xAccess */
  sqlsaltFullPathname,          /* xFullPathname */
  sqlsaltDlOpen,                /* xDlOpen */
  sqlsaltDlError,               /* xDlError */
  sqlsaltDlSym,                 /* xDlSym */
  sqlsaltDlClose,               /* xDlClose */
  sqlsaltRandomness,            /* xRandomness */
  sqlsaltSleep,                 /* xSleep */
  sqlsaltCurrentTime,           /* xCurrentTime */
  sqlsaltGetLastError,          /* xGetLastError */
  sqlsaltCurrentTimeInt64,      /* xCurrentTimeInt64 */
  sqlsaltSetSystemCall,         /* xSetSystemCall */
  sqlsaltGetSystemCall,         /* xGetSystemCall */
  sqlsaltNextSystemCall         /* xNextSystemCall */
};

static const sqlite3_io_methods sqlsalt_io_methods = {
  3,                            /* iVersion */
  sqlsaltClose,                 /* xClose */
  sqlsaltRead,                  /* xRead */
  sqlsaltWrite,                 /* xWrite */
  sqlsaltTruncate,              /* xTruncate */
  sqlsaltSync,                    /* xSync */
  sqlsaltFileSize,                /* xFileSize */
  sqlsaltLock,                    /* xLock */
  sqlsaltUnlock,                  /* xUnlock */
  sqlsaltCheckReservedLock,       /* xCheckReservedLock */
  sqlsaltFileControl,             /* xFileControl */
  sqlsaltSectorSize,              /* xSectorSize */
  sqlsaltDeviceCharacteristics,   /* xDeviceCharacteristics */
  sqlsaltShmMap,                  /* xShmMap */
  sqlsaltShmLock,                 /* xShmLock */
  sqlsaltShmBarrier,              /* xShmBarrier */
  sqlsaltShmUnmap,                /* xShmUnmap */
  sqlsaltFetch,                   /* xFetch */
  sqlsaltUnfetch                  /* xUnfetch */
};

#ifdef SQLITE_SQLSALT_INIT_FUNCNAME
/*
** SQL function:    initialize_sqlsaltvfs(SCHEMANAME)
**
** This SQL functions (whose name is actually determined at compile-time
** by the value of the SQLITE_SQLSALT_INIT_FUNCNAME macro) invokes:
**
**   sqlite3_file_control(db, SCHEMANAME, SQLITE_FCNTL_RESERVE_BYTE, &n);
**
** In order to set the reserve bytes value to N, so that sqlsaltvfs will
** operate.  This feature is provided (if and only if the
** SQLITE_SQLSALT_INIT_FUNCNAME compile-time option is set to a string
** which is the name of the SQL function) so as to provide the ability
** to invoke the file-control in programming languages that lack
** direct access to the sqlite3_file_control() interface (ex: Java).
**
** This interface is undocumented, apart from this comment.  Usage
** example:
**
**    1.  Compile with -DSQLITE_SQLSALT_INIT_FUNCNAME="sqlsaltvfs_init"
**    2.  Run:  "SELECT sqlsalt_init('main'); VACUUM;"
*/
static void sqlsaltInitFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  int nByte = N;
  const char *zSchemaName = (const char*)sqlite3_value_text(argv[0]);
  sqlite3 *db = sqlite3_context_db_handle(context);
  sqlite3_file_control(db, zSchemaName, SQLITE_FCNTL_RESERVE_BYTES, &nByte);
  /* Return NULL */
}
#endif /* SQLITE_SQLSALT_INIT_FUNCNAME */

typedef struct sqlsaltFile sqlsaltFile;
struct sqlsaltFile {
  sqlite3_file base;
  const char * zFName;
};

/*
** Close a sqlsalt-file.
*/
static int sqlsaltClose(sqlite3_file *pFile){
  sqlsaltFile *p = (sqlsaltFile *)pFile;
  (void) p;
  pFile = ORIGFILE(pFile);
  int rc = pFile->pMethods->xClose(pFile);
  DEBUG_PRINTF("close:%s rc:%d\n", p->zFName, rc);
  return rc;
}

/*
** Read data from a sqlsalt-file.
*/
static int sqlsaltRead(
  sqlite3_file *pFile, 
  void *zBuf, 
  int iAmt, 
  sqlite_int64 iOfst
){
  sqlsaltFile *p = (sqlsaltFile *)pFile;
  (void) p;
  pFile = ORIGFILE(pFile);
  int rc = pFile->pMethods->xRead(pFile, zBuf, iAmt, iOfst);
  DEBUG_PRINTF("read:%s iAmt:%d offset:%lld rc:%d\n", p->zFName, iAmt, iOfst, rc);
  return rc;
}

/*
** Write data to a sqlsalt-file.
*/
static int sqlsaltWrite(
  sqlite3_file *pFile,
  const void *zBuf,
  int iAmt,
  sqlite_int64 iOfst
){
  sqlsaltFile *p = (sqlsaltFile *)pFile;
  (void) p;
  pFile = ORIGFILE(pFile);
  int rc = pFile->pMethods->xWrite(pFile, zBuf, iAmt, iOfst);
  DEBUG_PRINTF("write:%s iAmt:%d offset:%lld, rc:%d\n", p->zFName, iAmt, iOfst, rc);
  return rc;
}

/*
** Truncate a sqlsalt-file.
*/
static int sqlsaltTruncate(sqlite3_file *pFile, sqlite_int64 size){
  sqlsaltFile *p = (sqlsaltFile *)pFile;
  (void) p;
  pFile = ORIGFILE(pFile);
  int rc = pFile->pMethods->xTruncate(pFile, size);
  DEBUG_PRINTF("truncate:%s size:%lld rc:%d\n", p->zFName, size, rc);
  return rc;
}

/*
** Sync a sqlsalt-file.
*/
static int sqlsaltSync(sqlite3_file *pFile, int flags){
  sqlsaltFile *p = (sqlsaltFile *)pFile;
  (void) p;
  pFile = ORIGFILE(pFile);
  int rc = pFile->pMethods->xSync(pFile, flags);
  DEBUG_PRINTF("sync:%s flags:%d\n", p->zFName, flags);
  return rc;
}

/*
** Return the current file-size of a sqlsalt-file.
*/
static int sqlsaltFileSize(sqlite3_file *pFile, sqlite_int64 *pSize){
  sqlsaltFile *p = (sqlsaltFile *)pFile;
  (void) p;
  pFile = ORIGFILE(p);
  int rc = pFile->pMethods->xFileSize(pFile, pSize);
  DEBUG_PRINTF("filesize:%s size:%lld rc:%d\n", p->zFName, *pSize, rc);
  return rc;
}


/*
** Lock a sqlsalt-file.
*/
static int sqlsaltLock(sqlite3_file *pFile, int eLock){
  sqlsaltFile *p = (sqlsaltFile *)pFile;
  (void) p;
  pFile = ORIGFILE(pFile);
  int rc = pFile->pMethods->xLock(pFile, eLock);
  DEBUG_PRINTF("lock:%s elock:%d rc:%d\n", p->zFName, eLock, rc);
  return rc;
}

/*
** Unlock a sqlsalt-file.
*/
static int sqlsaltUnlock(sqlite3_file *pFile, int eLock){
  sqlsaltFile *p = (sqlsaltFile *)pFile;
  (void) p;
  pFile = ORIGFILE(pFile);
  int rc = pFile->pMethods->xUnlock(pFile, eLock);
  DEBUG_PRINTF("unlock:%s elock:%d rc:%d\n", p->zFName, eLock, rc);
  return rc;
}

/*
** Check if another file-handle holds a RESERVED lock on a sqlsalt-file.
*/
static int sqlsaltCheckReservedLock(sqlite3_file *pFile, int *pResOut){
  sqlsaltFile *p = (sqlsaltFile *)pFile;
  (void) p;
  pFile = ORIGFILE(pFile);
  int rc = pFile->pMethods->xCheckReservedLock(pFile, pResOut);
  DEBUG_PRINTF("check_reserved_lock:%s res:%d rc:%d\n", p->zFName, *pResOut, rc);
  return rc;
}

/*
** File control method. For custom operations on a sqlsalt-file.
*/
static int sqlsaltFileControl(sqlite3_file *pFile, int op, void *pArg){
  sqlsaltFile *p = (sqlsaltFile*)pFile;
  (void) p;
  pFile = ORIGFILE(pFile);
  int rc = pFile->pMethods->xFileControl(pFile, op, pArg);
  DEBUG_PRINTF("filecontrol:%s op:%d rc:%d\n", p->zFName, op, rc);
  return rc;
}

/*
** Return the sector-size in bytes for a sqlsalt-file.
*/
static int sqlsaltSectorSize(sqlite3_file *pFile){
  sqlsaltFile *p = (sqlsaltFile *)pFile;
  (void) p;
  pFile = ORIGFILE(pFile);
  int rc = pFile->pMethods->xSectorSize(pFile);
  DEBUG_PRINTF("sectorsize:%s rc:%d\n", p->zFName, rc);
  return rc;
}

/*
** Return the device characteristic flags supported by a sqlsalt-file.
*/
static int sqlsaltDeviceCharacteristics(sqlite3_file *pFile){
  sqlsaltFile *p = (sqlsaltFile *)pFile;
  (void) p;
  pFile = ORIGFILE(pFile);
  int rc = pFile->pMethods->xDeviceCharacteristics(pFile);
  DEBUG_PRINTF("devicecharacteristics:%s rc:%d\n", p->zFName, rc);
  return rc;
}

/* Create a shared memory file mapping */
static int sqlsaltShmMap(
  sqlite3_file *pFile,
  int iPg,
  int pgsz,
  int bExtend,
  void volatile **pp
){
  sqlsaltFile *p = (sqlsaltFile *)pFile;
  (void) p;
  pFile = ORIGFILE(pFile);
  int rc = pFile->pMethods->xShmMap(pFile,iPg,pgsz,bExtend,pp);
  DEBUG_PRINTF("shmmap:%s pg:%d pgsz:%d extend:%d rc:%d\n", p->zFName, iPg, pgsz, bExtend, rc);
  return rc;
}

/* Perform locking on a shared-memory segment */
static int sqlsaltShmLock(sqlite3_file *pFile, int offset, int n, int flags){
  sqlsaltFile *p = (sqlsaltFile *)pFile;
  (void) p;
  pFile = ORIGFILE(pFile);
  int rc = pFile->pMethods->xShmLock(pFile,offset,n,flags);
  DEBUG_PRINTF("shmlock:%s offset:%d, n:%d flags:%d rc:%d\n", p->zFName, offset, n, flags, rc);
  return rc;
}

/* Memory barrier operation on shared memory */
static void sqlsaltShmBarrier(sqlite3_file *pFile){
  sqlsaltFile *p = (sqlsaltFile *)pFile;
  (void) p;
  pFile = ORIGFILE(pFile);
  DEBUG_PRINTF("shmbarrier:%s", p->zFName);
  pFile->pMethods->xShmBarrier(pFile);
}

/* Unmap a shared memory segment */
static int sqlsaltShmUnmap(sqlite3_file *pFile, int deleteFlag){
  sqlsaltFile *p = (sqlsaltFile *)pFile;
  (void) p;
  pFile = ORIGFILE(pFile);
  int rc = pFile->pMethods->xShmUnmap(pFile,deleteFlag);
  DEBUG_PRINTF("shmunmap:%s deleteflag:%d rc:%d\n", p->zFName, deleteFlag, rc);
  return rc;
}

/* Fetch a page of a memory-mapped file */
static int sqlsaltFetch(
  sqlite3_file *pFile,
  sqlite3_int64 iOfst,
  int iAmt,
  void **pp
){
  sqlsaltFile *p = (sqlsaltFile *)pFile;
  (void) p;
  pFile = ORIGFILE(pFile);
  int rc = pFile->pMethods->xFetch(pFile, iOfst, iAmt, pp);
  DEBUG_PRINTF("fetch:%s offset:%lld iamt:%d rc:%d\n", p->zFName, iOfst, iAmt, rc);
  return rc;
}

/* Release a memory-mapped page */
static int sqlsaltUnfetch(sqlite3_file *pFile, sqlite3_int64 iOfst, void *pPage){
  sqlsaltFile *p = (sqlsaltFile *)pFile;
  (void) p;
  pFile = ORIGFILE(pFile);
  int rc = pFile->pMethods->xUnfetch(pFile, iOfst, pPage);
  DEBUG_PRINTF("unfetch:%s offset:%lld rc:%d\n", p->zFName, iOfst, rc);
  return rc;
}

/*
** Open a sqlsalt file handle.
*/
static int sqlsaltOpen(
  sqlite3_vfs *pVfs,
  const char *zName,
  sqlite3_file *pFile,
  int flags,
  int *pOutFlags
){
  sqlsaltFile *p;
  sqlite3_file *pSubFile;
  sqlite3_vfs *pSubVfs;
  pSubVfs = ORIGVFS(pVfs);
  p = (sqlsaltFile*)pFile;
  (void) p;
  memset(p, 0, sizeof(*p));
  pSubFile = ORIGFILE(pFile);
  pFile->pMethods = &sqlsalt_io_methods;
  int rc = pSubVfs->xOpen(pSubVfs, zName, pSubFile, flags, pOutFlags);
  p->zFName = zName;
  DEBUG_PRINTF("open:%s flags:%d\n", zName, flags);
  return rc;
}

/*
** All other VFS methods are pass-thrus.
*/
static int sqlsaltDelete(sqlite3_vfs *pVfs, const char *zPath, int dirSync){
  int rc = ORIGVFS(pVfs)->xDelete(ORIGVFS(pVfs), zPath, dirSync);
  DEBUG_PRINTF("delete:%s dirsync:%d rc:%d\n", zPath, dirSync, rc);
  return rc;
}
static int sqlsaltAccess(
  sqlite3_vfs *pVfs, 
  const char *zPath, 
  int flags, 
  int *pResOut
){
  int rc = ORIGVFS(pVfs)->xAccess(ORIGVFS(pVfs), zPath, flags, pResOut);
  DEBUG_PRINTF("access:%s flags:%d resout:%d rc:%d\n", zPath, flags, *pResOut, rc);
  return rc;
}
static int sqlsaltFullPathname(
  sqlite3_vfs *pVfs, 
  const char *zPath, 
  int nOut, 
  char *zOut
){
  int rc = ORIGVFS(pVfs)->xFullPathname(ORIGVFS(pVfs),zPath,nOut,zOut);
  DEBUG_PRINTF("fullpathgeertje:%s nout:%d out:%s rc:%d\n", zPath, nOut, zOut, rc);
  return rc;
}
static void *sqlsaltDlOpen(sqlite3_vfs *pVfs, const char *zPath){
  DEBUG_PRINTF("dlopen:%s\n", zPath);
  return ORIGVFS(pVfs)->xDlOpen(ORIGVFS(pVfs), zPath);
}
static void sqlsaltDlError(sqlite3_vfs *pVfs, int nByte, char *zErrMsg){
  ORIGVFS(pVfs)->xDlError(ORIGVFS(pVfs), nByte, zErrMsg);
  DEBUG_PRINTF("dlerror:%s nbyte:%d\n", zErrMsg, nByte);
}
static void (*sqlsaltDlSym(sqlite3_vfs *pVfs, void *p, const char *zSym))(void){
  DEBUG_PRINTF("dlsym\n");
  return ORIGVFS(pVfs)->xDlSym(ORIGVFS(pVfs), p, zSym);
}
static void sqlsaltDlClose(sqlite3_vfs *pVfs, void *pHandle){
  ORIGVFS(pVfs)->xDlClose(ORIGVFS(pVfs), pHandle);
  DEBUG_PRINTF("dlclose handle:%p\n", pHandle);
}
static int sqlsaltRandomness(sqlite3_vfs *pVfs, int nByte, char *zBufOut){
  int rc = ORIGVFS(pVfs)->xRandomness(ORIGVFS(pVfs), nByte, zBufOut);
  DEBUG_PRINTF("randomness nbyte:%d rc:%d\n", nByte, rc);
  return rc;
}
static int sqlsaltSleep(sqlite3_vfs *pVfs, int nMicro){
  int rc = ORIGVFS(pVfs)->xSleep(ORIGVFS(pVfs), nMicro);
  DEBUG_PRINTF("sleep micro:%d rc:%d\n", nMicro, rc);
  return rc;
}
static int sqlsaltCurrentTime(sqlite3_vfs *pVfs, double *pTimeOut){
  int rc = ORIGVFS(pVfs)->xCurrentTime(ORIGVFS(pVfs), pTimeOut);
  DEBUG_PRINTF("currenttime:%lf rc:%d\n", *pTimeOut, rc);
  return rc;
}
static int sqlsaltGetLastError(sqlite3_vfs *pVfs, int a, char *b){
  int rc = ORIGVFS(pVfs)->xGetLastError(ORIGVFS(pVfs), a, b);
  DEBUG_PRINTF("getlasterror a:%d b:%s rc:%d\n", a, b, rc);
  return rc;
}
static int sqlsaltCurrentTimeInt64(sqlite3_vfs *pVfs, sqlite3_int64 *p){
  sqlite3_vfs *pOrig = ORIGVFS(pVfs);
  int rc;
  assert( pOrig->iVersion>=2 );
  if( pOrig->xCurrentTimeInt64 ){
    rc = pOrig->xCurrentTimeInt64(pOrig, p);
  }else{
    double r;
    rc = pOrig->xCurrentTime(pOrig, &r);
    *p = (sqlite3_int64)(r*86400000.0);
  }
  DEBUG_PRINTF("currenttimeint64:%lld rc:%d\n", *p, rc);
  return rc;
}
static int sqlsaltSetSystemCall(
  sqlite3_vfs *pVfs,
  const char *zName,
  sqlite3_syscall_ptr pCall
){
    return ORIGVFS(pVfs)->xSetSystemCall(ORIGVFS(pVfs),zName,pCall);
}
static sqlite3_syscall_ptr sqlsaltGetSystemCall(
  sqlite3_vfs *pVfs,
  const char *zName
){
    DEBUG_PRINTF("getsystemcall: %s\n", zName);
    return ORIGVFS(pVfs)->xGetSystemCall(ORIGVFS(pVfs),zName);
}
static const char *sqlsaltNextSystemCall(sqlite3_vfs *pVfs, const char *zName){
    DEBUG_PRINTF("nextsystemcall: %s\n", zName);
    return ORIGVFS(pVfs)->xNextSystemCall(ORIGVFS(pVfs), zName);
}

/*
** Register the sqlsalt VFS as the default VFS for the system.
*/
static int sqlsaltRegisterVfs(void){
  int rc = SQLITE_OK;
#if 1
  sqlite3_vfs *pOrig;
  if( sqlite3_vfs_find("sqlsaltvfs")!=0 ) return SQLITE_OK;
  pOrig = sqlite3_vfs_find(0);
  if( pOrig==0 ) return SQLITE_ERROR;
  sqlsalt_vfs.iVersion = pOrig->iVersion;
  sqlsalt_vfs.pAppData = pOrig;
  sqlsalt_vfs.szOsFile = pOrig->szOsFile + (int)sizeof(sqlsaltFile);
  rc = sqlite3_vfs_register(&sqlsalt_vfs, 1);
#endif
  return rc;
}

#if defined(SQLSALT_STATIC)
/* This variant of the initializer runs when the extension is
** statically linked.
*/
int sqlite3_register_sqlsaltvfs(const char *NotUsed){
  (void)NotUsed;
  return sqlsaltRegisterVfs();
}
int sqlite3_unregister_sqlsaltvfs(void){
  if( sqlite3_vfs_find("sqlsaltvfs") ){
    sqlite3_vfs_unregister(&sqlsalt_vfs);
  }
  return SQLITE_OK;
}
#endif /* defined(SQLSALT_STATIC) */

#if !defined(SQLSALT_STATIC)
/* This variant of the initializer function is used when the
** extension is shared library to be loaded at run-time.
*/
#ifdef _WIN32
__declspec(dllexport)
#endif

static int sqlsaltRegisterFunc(
  sqlite3 *db, 
  char **pzErrMsg, 
  const sqlite3_api_routines *pApi
){
  (void) db;
  (void) pzErrMsg;
  (void) pApi;
  return SQLITE_OK;
}

/* 
** This routine is called by sqlite3_load_extension() when the
** extension is first loaded.
***/
int sqlite3_sqlsalt_init(
  sqlite3 *db, 
  char **pzErrMsg, 
  const sqlite3_api_routines *pApi
){
  int rc;
  SQLITE_EXTENSION_INIT2(pApi);
  (void)pzErrMsg; /* not used */
  rc = sqlsaltRegisterFunc(db, 0, 0);
  if( rc==SQLITE_OK ){
    rc = sqlsaltRegisterVfs();
  }
  if( rc==SQLITE_OK ) rc = SQLITE_OK_LOAD_PERMANENTLY;
  return rc;
}
#endif /* !defined(SQLSALT_STATIC) */
