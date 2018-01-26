/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2010 Oracle and/or its affiliates.  All rights reserved.
 */

/*
** This file implements the sqlite btree.h interface for Berkeley DB.
**
** Build-time options:
**
**  BDBSQL_OMIT_LEAKCHECK -- omit combined sqlite and BDB memory allocation
**  BDBSQL_OMIT_SHARING -- keep all environment on the heap (necessary on
**                         platforms without mmap).
**  BDBSQL_PRELOAD_HANDLES -- open all tables when first connecting
**  BDBSQL_SEMITXN_TRUNCATE -- perform truncates with minimal logging
**  BDBSQL_SINGLE_THREAD -- omit support for multithreading
*/
#include <errno.h>
#include <assert.h>

#include "sqliteInt.h"
#include "btreeInt.h"
#include <db.h>

#ifdef BDBSQL_OMIT_LEAKCHECK
#define	sqlite3_malloc malloc
#define	sqlite3_free free
#define	sqlite3_strdup strdup
#else
#define	sqlite3_strdup btreeStrdup
#endif

/*
 * We use the following internal DB functions.
 */
extern int __os_dirlist(ENV *env,
    const char *dir, int returndir, char ***namesp, int *cntp);
extern void __os_dirfree(ENV *env, char **namesp, int cnt);
extern int __os_exists (ENV *, const char *, int *);
extern int __os_mkdir (ENV *, const char *, int);
extern int __os_unlink (ENV *, const char *, int);

#define	GIGABYTE 1073741824

#define	DB_MIN_CACHESIZE 20		/* pages */

#define	pDbEnv		(pBt->dbenv)
#define	pMetaDb		(pBt->metadb)
#define	pTablesDb	(pBt->tablesdb)
#define	pFamilyTxn	(p->family_txn)
#define	pReadTxn	(p->read_txn)
#define	pSavepointTxn	(p->savepoint_txn)

/* Forward declarations for internal functions. */
static int btreeCleanupEnv(const char *home);
static int btreeCloseCursor(BtCursor *pCur, int removeList);
static int btreeCreateDataTable(Btree *, int, CACHED_DB **);
static int btreeCreateSharedBtree(
    Btree *, const char *, sqlite3 *, int, storage_mode_t);
static int btreeCreateTable(Btree *p, int *piTable, int flags);
static void btreeFreeSharedBtree(BtShared *p);
static int btreeGetSharedBtree(
    BtShared **, const char *, sqlite3 *, storage_mode_t);
static int btreeMoveto(BtCursor *pCur,
    const void *pKey, i64 nKey, int bias, int *pRes);
static int btreeOpenEnvironment(Btree *p, int needLock);
static int btreePrepareEnvironment(Btree *p);
static int btreeOpenMetaTables(Btree *p, int *pCreating);
static int btreeRestoreCursorPosition(BtCursor *pCur, int skipMoveto);
static int btreeLoadBufferIntoTable(BtCursor *pCur);
static int btreeLockSchema(Btree *p, lock_mode_t lockMode);
static int btreeTripAll(Btree *p, int iTable, int incrblobUpdate);
static int btreeTripWatchers(BtCursor *pBt, int incrblobUpdate);

/*
 * Globals are protected by the static "open" mutex (SQLITE_MUTEX_STATIC_OPEN).
 */

/* The head of the linked list of shared Btree objects */
struct BtShared *g_shared_btrees = NULL;

/* The environment handle used for temporary environments (NULL or open). */
DB_ENV *g_tmp_env;

/* The unique id for the next shared Btree object created. */
u_int32_t g_uid_next = 0;

/* Number of times we're prepared to try multiple gets. */
#define	MAX_SMALLS 100

/* Number of times to retry operations that return a "busy" error. */
#define	BUSY_RETRY_COUNT	100

/* This should match SQLite VFS.mxPathname */
#define	BT_MAX_PATH 512
/* TODO: This should probably be '\' on Windows. */
#define	PATH_SEPARATOR	"/"

#define	pBDb	(pCur->db)
#define	pDbc	(pCur->dbc)
#define	pIntKey	((pCur->flags & BTREE_INTKEY) != 0)
#define	pIsBuffer	(pCur->pBtree->pBt->resultsBuffer)

#define	GET_TABLENAME(b, sz, i, suffix)	do {			\
	if (pBt->dbStorage == DB_STORE_NAMED)			\
		sqlite3_snprintf((sz), (b), "table%05d%s",	\
		(i), (suffix));					\
	else if (pBt->dbStorage == DB_STORE_INMEM)		\
		sqlite3_snprintf((sz), (b), "temp%05d_%05d%s",	\
		    pBt->uid, (i), (suffix));			\
	else							\
		b = NULL;					\
} while (0)

#define	GET_AUTO_COMMIT(pBt, txn) (((pBt)->transactional &&	\
	(!(txn) || (txn) == pFamilyTxn)) ? DB_AUTO_COMMIT : 0)

#define	GET_DURABLE(pBt)					\
	((pBt)->dbStorage == DB_STORE_NAMED &&			\
	((pBt)->flags & BTREE_OMIT_JOURNAL) == 0)

/*
 * There is some subtlety about which mutex to use: for shared handles, we
 * update some structures that are protected by the open mutex.  In-memory
 * databases all share the same g_tmp_env handle, so we need to make sure they
 * get it single-threaded (so the initial open is done once).
 *
 * However, we can't use the open mutex to protect transient database opens and
 * closes: we might already be holding locks in a shared environment when we
 * try to open the temporary env, which would lead to a lock/mutex deadlock.
 * We take a different static mutex from SQLite, previously used in the pager.
 */
#define	OPEN_MUTEX(store)	((store == DB_STORE_NAMED) ?	\
	SQLITE_MUTEX_STATIC_OPEN : SQLITE_MUTEX_STATIC_LRU)

#define	MAP_ERR(rc, ret)					\
	((rc != SQLITE_OK) ? rc : (ret == 0) ? SQLITE_OK : dberr2sqlite(ret))

#define	MAP_ERR_LOCKED(rc, ret)					\
	((rc != SQLITE_OK) ? rc : (ret == 0) ? SQLITE_OK :	\
	    dberr2sqlitelocked(ret))

#ifndef BDBSQL_SINGLE_THREAD
#define	RMW(pCur)						\
	(pCur->wrFlag && pCur->pBtree->pBt->dbStorage == DB_STORE_NAMED ? \
	DB_RMW : 0)
#else
#define	RMW(pCur) 0
#endif

static int dberr2sqlite(int err)
{
	switch (err) {
	case 0:
		return SQLITE_OK;
	case DB_LOCK_DEADLOCK:
	case DB_LOCK_NOTGRANTED:
		return SQLITE_BUSY;
	case DB_NOTFOUND:
		return SQLITE_NOTFOUND;
	case DB_RUNRECOVERY:
		return SQLITE_CORRUPT;
	case EACCES:
		return SQLITE_READONLY;
	case EIO:
		return SQLITE_IOERR;
	case EPERM:
		return SQLITE_PERM;
	case ENOMEM:
		return SQLITE_NOMEM;
	case ENOENT:
		return SQLITE_CANTOPEN;
	case ENOSPC:
		return SQLITE_FULL;
	default:
		return SQLITE_ERROR;
	}
}

/*
 * Used in cases where SQLITE_LOCKED should be returned instead of
 * SQLITE_BUSY.
 */
static int dberr2sqlitelocked(int err)
{
	int rc = dberr2sqlite(err);
	if (rc == SQLITE_BUSY)
		rc = SQLITE_LOCKED;
	return rc;
}

#ifndef BDBSQL_OMIT_LEAKCHECK
/*
 * Wrap the sqlite malloc and realloc APIs before using them in Berkeley DB
 * since they use different parameter types to the standard malloc and
 * realloc.
 * The signature of free matches, so we don't need to wrap it.
 */
static void *btreeMalloc(size_t size)
{
	if (size != (size_t)(int)size)
		return NULL;

	return sqlite3_malloc((int)size);
}

static void *btreeRealloc(void * buff, size_t size)
{
	if (size != (size_t)(int)size)
		return NULL;

	return sqlite3_realloc(buff, (int)size);
}

static char *btreeStrdup(const char *sq)
{
	return sqlite3_mprintf("%s", sq);
}
#endif

/*
 * We have borrowed the code built into Berkeley DB for encoding unsigned
 * integers that is optimized for small values.  We want an encoding for signed
 * values that does not require an (expensive) comparison callback.
 *
 * {encode,decode}I64 map signed values to the unsigned encoding in a
 * way that preserves the natural integer ordering, while staying optimized
 * for small positive values.
 */
#define	__INT64_MAX ((((i64)0x7fffffff) << 32) | 0xffffffff)

static int encodeI64(u_int8_t *buf, i64 num)
{
	int reserve;

	reserve = 0;

	if (num >= 0 && num < __INT64_MAX)
		num += 1; /* Need to leave '\0' so negatives sort lower. */
	else if (num == __INT64_MAX) {
		reserve = 1;
		/*
		 * Make sure it will sort bigger than __INT64_MAX - 1.
		 *
		 * Note: it would be possible to optimize this case, because
		 * our encoding has some free bits at the top of the first
		 * byte.  It doesn't seem worth it for one value, though.
		 */
		buf[9] = 1;
	} else {
		/* Negative numbers */
		*buf++ = 0; /* Smaller than any non-negative value. */
		reserve = 1;
	}

	return __dbsql_compress_int(buf, (u_int64_t)num) + reserve;
}

static i64 decodeI64(u_int8_t *data, int size)
{
	u_int64_t num;
	int negative, sz;

	/* Handle negative numbers. */
	if (data[0] == 0) {
		++data;
		--size;
		negative = 1;
	} else
		negative = 0;

	sz = __dbsql_decompress_int(data, &num);
	assert(sz == size || ((sz + 1) == size && (i64)num == __INT64_MAX));

	return (i64)((!negative && sz == size) ? num - 1 : num);
}

/*
 * An internal function that opens the metadata database that is present for
 * every SQLite Btree, and the special "tables" database maintained by Berkeley
 * DB that lists all of the subdatabases in a file.
 *
 * This is split out into a separate function so that it will be easy to change
 * the Btree layer to create Berkeley DB database handles per Btree object,
 * rather than per BtShared object.
 */
static int btreeOpenMetaTables(Btree *p, int *pCreating)
{
	BtShared *pBt;
#ifdef BDBSQL_SPLIT_META_TABLE
	DB *tmp_db_handle;
#endif
	DBC *dbc;
	DBT key, data;
	int i, idx, rc, ret, t_ret;

	pBt = p->pBt;
	rc = SQLITE_OK;
	ret = t_ret = 0;

	if (pMetaDb != NULL) {
		*pCreating = 0;
		goto addmeta;
	}

	if ((ret = db_create(&pMetaDb, pDbEnv, 0)) != 0 ||
	    (ret = db_create(&pTablesDb, pDbEnv, 0)) != 0)
		goto err;

	if (!GET_DURABLE(pBt)) {
		/* Ensure that log records are not written to disk. */
		if ((ret =
		    pMetaDb->set_flags(pMetaDb, DB_TXN_NOT_DURABLE)) != 0)
			goto err;
	}

	/*
	 * The metadata DB is the first one opened in the file, so it is
	 * sufficient to set the page size on it -- other databases in the
	 * same file will inherit the same pagesize.  We must open it before
	 * the table DB because this open call may be creating the file.
	 */
	if (pBt->pageSize != 0 &&
	    (ret = pMetaDb->set_pagesize(pMetaDb, pBt->pageSize)) != 0)
		goto err;

	/*
	 * We open the metadata and tables databases in auto-commit
	 * transactions.  These may deadlock or conflict, and should be safe to
	 * retry, but for safety we limit how many times we'll do that before
	 * returning the error.
	 */
	i = 0;
	do {
		ret = pMetaDb->open(pMetaDb, NULL, pBt->meta_name,
		    pBt->dbStorage == DB_STORE_NAMED ? "metadb" : NULL,
		    DB_BTREE,
		    pBt->db_oflags | GET_AUTO_COMMIT(pBt, NULL), 0);
	} while ((ret == DB_LOCK_DEADLOCK || ret == DB_LOCK_NOTGRANTED) &&
	    ++i < BUSY_RETRY_COUNT);

	if (ret != 0) {
		if (ret == EACCES && pBt->db_oflags & DB_RDONLY)
			rc = SQLITE_READONLY;
		goto err;
	}
#ifdef BDBSQL_SPLIT_META_TABLE
	/*
	 * If creating the BtShared, and the metadata is split out create an
	 * empty "contents" database, so we can open a handle to iterate over
	 * sub-databases. The sub-db opened here will never have any content,
	 * but that is OK.
	 */
	if ((ret = db_create(&tmp_db_handle, pDbEnv, 0)) != 0)
		goto err;
	if ((ret = tmp_db_handle->open(tmp_db_handle, NULL, pBt->short_name,
	    NULL, DB_BTREE, (pBt->db_oflags & ~DB_CREATE), 0)) != 0) {
		/* The file does not exist, create an empty table. */
		tmp_db_handle->close(tmp_db_handle, DB_NOSYNC);
		tmp_db_handle = NULL;
		if ((ret = db_create(&tmp_db_handle, pDbEnv, 0)) != 0)
			goto err;
		if ((ret = tmp_db_handle->open(tmp_db_handle, NULL,
		    pBt->short_name, "table00001", DB_BTREE,
		    pBt->db_oflags | GET_AUTO_COMMIT(pBt, NULL), 0)) != 0)
			goto err;
	}
	tmp_db_handle->close(tmp_db_handle, DB_NOSYNC);
#endif
	i = 0;
	do {
		ret = pTablesDb->open(pTablesDb, NULL, pBt->short_name, NULL,
		    DB_BTREE, (pBt->db_oflags & ~DB_CREATE) | DB_RDONLY |
		    GET_AUTO_COMMIT(pBt, NULL), 0);
	} while ((ret == DB_LOCK_DEADLOCK || ret == DB_LOCK_NOTGRANTED) &&
	    ++i < BUSY_RETRY_COUNT);

	if (ret != 0)
		goto err;

	/* Set the default max_page_count */
	sqlite3BtreeMaxPageCount(p, pBt->pageCount);

	/* Check whether we're creating the database */
	if ((ret = pTablesDb->cursor(pTablesDb, NULL, &dbc, 0)) != 0)
		goto err;

	memset(&key, 0, sizeof key);
	memset(&data, 0, sizeof data);
	data.flags = DB_DBT_PARTIAL | DB_DBT_USERMEM;
	ret = dbc->get(dbc, &key, &data, DB_LAST);
	if (ret == 0)
		*pCreating =
		    (strncmp((const char *)key.data, "metadb", key.size) == 0);
	if ((t_ret = dbc->close(dbc)) != 0 && ret == 0)
		ret = t_ret;
	if (ret != 0)
		goto err;

addmeta:/*
	 * Pepulate the MetaDb with any values that were set prior to
	 * the sqlite3BtreeOpen that triggers this.
	 */
	for (idx = 0; idx < NUMMETA; idx++) {
		if (!pBt->meta[idx].cached)
			continue;
		if ((rc = sqlite3BtreeUpdateMeta(p,
		    idx, pBt->meta[idx].value)) != SQLITE_OK)
			goto err;
	}

err:	if (rc != SQLITE_OK || ret != 0) {
		if (pTablesDb != NULL)
			(void)pTablesDb->close(pTablesDb, DB_NOSYNC);
		if (pMetaDb != NULL)
			(void)pMetaDb->close(pMetaDb, DB_NOSYNC);
		pTablesDb = pMetaDb = NULL;
	}

	return MAP_ERR(rc, ret);
}

/*
 * Berkeley DB doesn't NUL-terminate database names, do the conversion
 * manually to avoid making a copy just in order to call strtol.
 */
static int btreeTableNameToId(const char *subdb, int len, int *pid)
{
	const char *p;
	int id;

	assert(len > 5);
	assert(strncmp(subdb, "table", 5) == 0);

	id = 0;
	for (p = subdb + 5; p < subdb + len; p++) {
		if (*p < '0' || *p > '9')
			return (EINVAL);
		id = (id * 10) + (*p - '0');
	}
	*pid = id;
	return (0);
}

#ifdef BDBSQL_PRELOAD_HANDLES
static int btreePreloadHandles(Btree *p)
{
	BtShared *pBt;
	CACHED_DB *cached_db;
	DBC *dbc;
	DBT key, data;
	int iTable, ret;

	pBt = p->pBt;
	dbc = NULL;

	if ((ret = pTablesDb->cursor(pTablesDb, NULL, &dbc, 0)) != 0)
		goto err;

	memset(&key, 0, sizeof key);
	memset(&data, 0, sizeof data);
	data.flags = DB_DBT_PARTIAL | DB_DBT_USERMEM;

	sqlite3_mutex_enter(pBt->mutex);
	while ((ret = dbc->get(dbc, &key, &data, DB_NEXT)) == 0) {
		if (strncmp((const char *)key.data, "table", 5) != 0)
			continue;
		if ((ret = btreeTableNameToId(
		    (const char *)key.data, key.size, &iTable)) != 0)
			break;
		cached_db = NULL;
		(void)btreeCreateDataTable(p, iTable, &cached_db);
	}
	sqlite3_mutex_leave(pBt->mutex);

err:	if (ret == DB_NOTFOUND)
		ret = 0;
	if (dbc != NULL)
		(void)dbc->close(dbc);
	return (ret);
}
#endif /* BDBSQL_PRELOAD_HANDLES */

/*
** Free an allocated BtShared and any dependent allocated objects.
*/
static void btreeFreeSharedBtree(BtShared *p)
{
	if (p == NULL)
		return;

	if (p->mutex != NULL)
		sqlite3_mutex_free(p->mutex);
	if (p->dir_name != NULL)
		sqlite3_free(p->dir_name);
	if (p->meta_name != NULL && p->meta_name != p->short_name)
		sqlite3_free(p->meta_name);
	if (p->full_name != NULL)
		sqlite3_free(p->full_name);
	if (p->orig_name != NULL)
		sqlite3_free(p->orig_name);

	sqlite3_free(p);
}

/*
 * This function finds, opens or creates the Berkeley DB environment associated
 * with a database opened using sqlite3BtreeOpen. There are a few different
 * cases:
 *  * Temporary and transient databases share a single environment. If the
 *    shared handle exists, return it, otherwise create a shared handle.
 *  * For named databases, attempt to open an existing environment, if one
 *    exists, otherwise create a new environment.
 */
static int btreePrepareEnvironment(Btree *p)
{
	BtShared *pBt;
	DB_ENV *tmp_env;
	char *envDirName, envDirNameBuf[BT_MAX_PATH];
#ifdef BDBSQL_SPLIT_META_TABLE
	char metaNameBuf[BT_MAX_PATH];
	int meta_exists;
#endif
	int attrs, env_exists, f_exists, rc, reuse, ret;
	sqlite3_file *fp;

	pBt = p->pBt;
	envDirName = NULL;
	reuse = ret = 0;
	rc = SQLITE_OK;
	env_exists = f_exists = 0;

	pBt->env_oflags = DB_INIT_MPOOL |
	    ((pBt->dbStorage == DB_STORE_NAMED) ? 0 : DB_PRIVATE)
#ifndef BDBSQL_SINGLE_THREAD
	    | DB_THREAD
#endif
		;

	if (pBt->dbStorage == DB_STORE_NAMED) {
		memset(envDirNameBuf, 0, BT_MAX_PATH);
		sqlite3_snprintf(sizeof envDirNameBuf, envDirNameBuf,
		    "%s-journal", pBt->full_name);

		envDirName = envDirNameBuf;

		f_exists = !__os_exists(NULL, pBt->full_name, NULL);
		env_exists = !__os_exists(NULL, envDirName, NULL);
#ifdef BDBSQL_SPLIT_META_TABLE
		memset(metaNameBuf, 0, BT_MAX_PATH);
		sqlite3_snprintf(sizeof metaNameBuf, metaNameBuf,
		    "%s.meta", pBt->full_name);
		meta_exists = !__os_exists(NULL, &metaNameBuf[0], NULL);
#endif

		if ((pBt->dir_name = sqlite3_strdup(envDirName)) == NULL) {
			rc = SQLITE_NOMEM;
			goto err;
		}

		if ((p->vfsFlags & SQLITE_OPEN_READONLY) && !f_exists) {
			rc = SQLITE_READONLY;
			goto err;
		}

		if (!f_exists) {
			if ((p->vfsFlags & SQLITE_OPEN_READONLY) != 0) {
				rc = SQLITE_READONLY;
				goto err;
			} else if (!(p->vfsFlags & SQLITE_OPEN_CREATE)) {
				rc = SQLITE_CANTOPEN;
				goto err;
			}
		} else {
			/*
			 * If we don't have write permission for a file,
			 * automatically open any databases read-only.
			 */
			fp = (sqlite3_file *)sqlite3_malloc(
			    pBt->db->pVfs->szOsFile);
			if (fp == NULL) {
				rc = SQLITE_NOMEM;
				goto err;
			}
			memset(fp, 0, pBt->db->pVfs->szOsFile);
			rc = sqlite3OsOpen(pBt->db->pVfs, pBt->full_name, fp,
			    SQLITE_OPEN_MAIN_DB | SQLITE_OPEN_READWRITE,
			    &attrs);
			if (attrs & SQLITE_OPEN_READONLY)
				pBt->db_oflags |= DB_RDONLY;
			if (rc == SQLITE_OK)
				(void)sqlite3OsClose(fp);
			sqlite3_free(fp);

			/*
			 * Always open existing tables, even if the matching
			 * env does not exist (yet).
			 */
			pBt->env_oflags |= DB_CREATE;
		}

		if (env_exists && !f_exists) {
			if ((ret = btreeCleanupEnv(envDirName)) != 0)
				goto err;
		} else if (!env_exists && !(pBt->db_oflags & DB_RDONLY) &&
#ifndef BDBSQL_SPLIT_META_TABLE
		    f_exists) {
#else
		    (f_exists || meta_exists)) {
#endif
			/*
			 * Reset the LSNs in the database, so that we can open
			 * the database in a new environment.
			 *
			 * Ignore any errors that come from here, it is a
			 * cleanup phase.
			 */
			ret = db_env_create(&tmp_env, 0);
			if (ret == 0) {
				ret = tmp_env->open(tmp_env, NULL,
				    DB_CREATE | DB_PRIVATE | DB_INIT_MPOOL, 0);
				if (ret == 0 && f_exists)
					(void)tmp_env->lsn_reset(
					    tmp_env, pBt->short_name, 0);
#ifdef BDBSQL_SPLIT_META_TABLE
				if (ret == 0 && meta_exists &&
				    pBt->meta_name != pBt->short_name)
					(void)tmp_env->lsn_reset(
					    tmp_env, pBt->meta_name, 0);
#endif
				(void)tmp_env->close(tmp_env, 0);
			}
			ret = 0;
		}

		if (!env_exists)
			(void)__os_mkdir(NULL, envDirName, 0777);

		if ((ret = db_env_create(&pDbEnv, 0)) != 0)
			goto err;
		pDbEnv->set_errpfx(pDbEnv, pBt->full_name);
#ifndef BDBSQL_SINGLE_THREAD
		pDbEnv->set_lk_detect(pDbEnv, DB_LOCK_DEFAULT);
		pDbEnv->set_lk_max_lockers(pDbEnv, BDBSQL_MAX_LOCKERS);
		pDbEnv->set_lk_max_locks(pDbEnv, BDBSQL_MAX_LOCKS);
		pDbEnv->set_lk_max_objects(pDbEnv, BDBSQL_MAX_LOCK_OBJECTS);
#endif
		pDbEnv->set_lg_regionmax(pDbEnv, BDBSQL_LOG_REGIONMAX);
#ifndef BDBSQL_OMIT_LEAKCHECK
		pDbEnv->set_alloc(pDbEnv, btreeMalloc, btreeRealloc,
		    sqlite3_free);
#endif
#ifdef BDBSQL_MUTEX_MAX
		if ((ret =
		    pDbEnv->mutex_set_max(pDbEnv, BDBSQL_MUTEX_MAX)) != 0)
			goto err;
#endif
		if ((ret = pDbEnv->set_lg_max(pDbEnv, pBt->logFileSize)) != 0)
			goto err;
#ifndef BDBSQL_OMIT_LOG_REMOVE
		if ((ret = pDbEnv->log_set_config(pDbEnv,
		    DB_LOG_AUTO_REMOVE, 1)) != 0)
			goto err;
#endif
		/*
		 * Set the directory where the database file will be created
		 * to the parent of the environment directory.
		 */
		pDbEnv->set_data_dir(pDbEnv, "..");

		/*
		 * If we are opening a database read-only, and there is not
		 * already an environment, create a non-transactional
		 * private environment to use. Otherwise we run into issues
		 * with mismatching LSNs.
		 */
		if (!env_exists && (pBt->db_oflags & DB_RDONLY)) {
			pBt->env_oflags |= DB_PRIVATE;
			pBt->transactional = 0;
		} else {
			pBt->env_oflags |= DB_INIT_LOG | DB_INIT_TXN
#ifndef BDBSQL_SINGLE_THREAD
			    | DB_INIT_LOCK
#endif
#ifdef BDBSQL_OMIT_SHARING
			    | DB_PRIVATE | DB_CREATE
#else
			    | DB_REGISTER
#endif
			    ;
		}

		/*
		 * If we're prepared to create the environment, do that now.
		 * Otherwise, if the table is being created, SQLite will call
		 * sqlite3BtreeCursor and expect a "SQLITE_EMPTY" return, then
		 * call sqlite3BtreeCreateTable.  The result of this open is
		 * recorded in the Btree object passed in.
		 */
		if ((pBt->env_oflags & DB_CREATE) != 0) {
			if ((pBt->env_oflags & DB_INIT_TXN) != 0)
				pBt->env_oflags |= DB_RECOVER;
			rc = btreeOpenEnvironment(p, 0);
		} else {
			pBt->env_oflags |= DB_CREATE;
			if ((pBt->env_oflags & DB_INIT_TXN) != 0)
				pBt->env_oflags |= DB_RECOVER;
		}
	} else if (g_tmp_env == NULL) {
		/*
		 * Creating environment shared by temp and transient tables.
		 * We're just creating a handle here, so it doesn't matter if
		 * we race with some other thread at this point, as long as
		 * only one of the environment handles is opened.
		 */
		if ((ret = db_env_create(&pDbEnv, 0)) != 0)
			goto err;
		pDbEnv->set_errpfx(pDbEnv, "<temp>");
		pBt->env_oflags |= DB_CREATE | DB_INIT_TXN | DB_PRIVATE;

		/*
		 * Never create log files.  We mark all databases non-durable,
		 * but BDB still occasionally writes log records (e.g., for
		 * checkpoints).  This guarantees that those log records aren't
		 * written to files.  A small buffer should be fine.
		 */
		pDbEnv->set_lg_bsize(pDbEnv, 64 * 1024);
		pDbEnv->set_lg_max(pDbEnv, 32 * 1024);
#ifndef BDBSQL_OMIT_LEAKCHECK
		pDbEnv->set_alloc(pDbEnv, btreeMalloc, btreeRealloc,
		    sqlite3_free);
#endif
		pDbEnv->log_set_config(pDbEnv, DB_LOG_IN_MEMORY, 1);
	} else
		rc = btreeOpenEnvironment(p, 0);

err:	return MAP_ERR(rc, ret);
}

/*
 * Called from sqlite3BtreeCreateTable, if it the Berkeley DB environment
 * did not already exist when sqlite3BtreeOpen was called.
 */
static int btreeOpenEnvironment(Btree *p, int needLock)
{
	BtShared *pBt;
	CACHED_DB *cached_db;
	int creating, iTable, newEnv, rc, ret, reuse_env, writeLock;
	sqlite3_mutex *mutexOpen;
	txn_mode_t txn_mode;
	i64 cache_sz;

	newEnv = ret = reuse_env = 0;
	rc = SQLITE_OK;
	cached_db = NULL;
	mutexOpen = NULL;

	pBt = p->pBt;

	/*
	 * The open (and setting pBt->env_opened) is protected by the open
	 * mutex, to prevent concurrent threads trying to call DB_ENV->open
	 * simultaneously.
	 */
	if (needLock) {
		mutexOpen = sqlite3MutexAlloc(OPEN_MUTEX(pBt->dbStorage));
		sqlite3_mutex_enter(mutexOpen);
#ifdef SQLITE_DEBUG
	} else if (pBt->dbStorage == DB_STORE_NAMED) {
		mutexOpen = sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_OPEN);
		assert(sqlite3_mutex_held(mutexOpen));
		mutexOpen = NULL;
#endif
	}

	/*
	 * If we already created a handle and someone has opened the global
	 * handle in the meantime, close our handle to free the memory.
	 */
	if (pBt->dbStorage != DB_STORE_NAMED && g_tmp_env != NULL) {
		assert(!pBt->env_opened);
		assert(pDbEnv != g_tmp_env);
		if (pDbEnv != NULL)
			(void)pDbEnv->close(pDbEnv, 0);

		pDbEnv = g_tmp_env;
		pBt->env_opened = newEnv = reuse_env = 1;
	}
	if (!pBt->env_opened) {
		cache_sz = (i64)pBt->cacheSize;
		if (cache_sz < DB_MIN_CACHESIZE)
			cache_sz = DB_MIN_CACHESIZE;
		cache_sz *= (pBt->pageSize > 0) ?
		    pBt->pageSize : DEFAULT_PAGESIZE;
		pDbEnv->set_cachesize(pDbEnv,
		    (u_int32_t)(cache_sz / GIGABYTE),
		    (u_int32_t)(cache_sz % GIGABYTE), 0);
		pDbEnv->set_mp_mmapsize(pDbEnv, 0);
		pDbEnv->set_errfile(pDbEnv, stderr);

		if (pBt->dir_name != NULL)
			(void)__os_mkdir(NULL, pBt->dir_name, 0777);

		if ((ret = pDbEnv->open(
		    pDbEnv, pBt->dir_name, pBt->env_oflags, 0)) != 0) {
			if (ret == ENOENT && (pBt->env_oflags & DB_CREATE) == 0)
				return SQLITE_OK;
			goto err;
		}
		pBt->env_opened = newEnv = 1;

		if (pBt->dbStorage != DB_STORE_NAMED) {
			g_tmp_env = pDbEnv;
			reuse_env = 1;
		}
	}

	assert(!p->connected);
	p->connected = 1;

	/*
	 * If the environment was already open, drop the open mutex before
	 * proceeding.  Some other thread may be holding a schema lock and
	 * be waiting for the open mutex, which would lead to a latch deadlock.
	 *
	 * On the other hand, if we are creating the environment, this thread
	 * is expecting to find the schema table empty, so we need to hold
	 * onto the open mutex and get an exclusive schema lock, to prevent
	 * some other thread getting in ahead of us.
	 */
	if (!newEnv && needLock) {
		assert(sqlite3_mutex_held(mutexOpen));
		sqlite3_mutex_leave(mutexOpen);
		needLock = 0;
	}

	pBt->db_oflags |= p->vfsFlags & SQLITE_OPEN_READONLY ? DB_RDONLY : 0;
	if (!(pBt->db_oflags & DB_RDONLY) && p->vfsFlags & SQLITE_OPEN_CREATE)
		pBt->db_oflags |= DB_CREATE;

	creating = 1;
	if (pBt->dbStorage == DB_STORE_NAMED &&
	    (rc = btreeOpenMetaTables(p, &creating)) != SQLITE_OK)
		goto err;
	if (creating) {
		if ((rc = btreeCreateTable(p, &iTable,
		    BTREE_INTKEY | BTREE_LEAFDATA)) != SQLITE_OK)
			goto err;

		assert(iTable == MASTER_ROOT);
	}

#ifdef BDBSQL_PRELOAD_HANDLES
	if (newEnv && !creating && pBt->dbStorage == DB_STORE_NAMED)
		(void)btreePreloadHandles(p);
#endif

	/*
	 * If transactions were started before the environment was opened,
	 * start them now.  Also, if creating a new environment, take a write
	 * lock to prevent races setting up the metadata tables.  Always start
	 * the ultimate parent by starting a read transaction.
	 */
	writeLock = (p->schemaLockMode == LOCKMODE_WRITE) ||
	    (newEnv && !(pBt->db_oflags & DB_RDONLY));

	if (pBt->transactional) {
		txn_mode = p->inTrans;
		p->inTrans = TRANS_NONE;

		if ((ret = pDbEnv->txn_begin(pDbEnv,
		    NULL, &pFamilyTxn, DB_TXN_FAMILY)) != 0)
			return dberr2sqlite(ret);

		if ((writeLock || txn_mode != TRANS_NONE) &&
		    (rc = sqlite3BtreeBeginTrans(p,
		    (writeLock || txn_mode == TRANS_WRITE))) != SQLITE_OK)
			goto err;
	}

	if (p->schemaLockMode != LOCKMODE_NONE) {
		p->schemaLockMode = LOCKMODE_NONE;
		rc = sqlite3BtreeLockTable(p, MASTER_ROOT, writeLock);
		if (rc != SQLITE_OK)
			goto err;
	}

	/*
	 * It is now okay for other threads to use this BtShared handle.
	 */
err:	if (rc != SQLITE_OK || ret != 0) {
		p->connected = 0;
	}
	if (needLock) {
		assert(sqlite3_mutex_held(mutexOpen));
		sqlite3_mutex_leave(mutexOpen);
	}
	return MAP_ERR(rc, ret);
}

static int btreeGetSharedBtree(
    BtShared **ppBt,
    const char *zFilename,
    sqlite3 *db,
    storage_mode_t store)
{
	Btree *pExisting;
	BtShared *next_bt;
	int iDb;

#ifdef SQLITE_DEBUG
	sqlite3_mutex *mutexOpen = sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_OPEN);
	assert(sqlite3_mutex_held(mutexOpen));
#endif

	/*
	 * SQLite uses this check, but Berkeley DB always operates with a
	 * shared cache.
	if (sqlite3GlobalConfig.sharedCacheEnabled != 1)
		return 1;
	*/

	*ppBt = NULL;
	for (next_bt = g_shared_btrees; next_bt != NULL;
	    next_bt = next_bt->pNextDb) {
		assert(next_bt->nRef > 0);
		if ((store != DB_STORE_NAMED && next_bt->full_name == NULL) ||
		    (store == DB_STORE_NAMED &&
		    strcmp(zFilename, next_bt->orig_name) == 0)) {
			/*
			 * Check to make sure that the btree handle being added
			 * does not already exist in the list of handles.
			 */
			for (iDb = db->nDb - 1; iDb >= 0; iDb--) {
				pExisting = db->aDb[iDb].pBt;
				if (pExisting && pExisting->pBt == next_bt)
					/* Leave mutex. */
					return SQLITE_CONSTRAINT;
			}
			*ppBt = next_bt;
			sqlite3_mutex_enter(next_bt->mutex);
			next_bt->nRef++;
			sqlite3_mutex_leave(next_bt->mutex);
			break;
		}
	}

	return SQLITE_OK;
}

static int btreeCreateSharedBtree(
    Btree *p,
    const char *zFilename,
    sqlite3 *db,
    int flags,
    storage_mode_t store)
{
	BtShared *new_bt;
	char *dirPathName, dirPathBuf[BT_MAX_PATH];
#ifdef BDBSQL_SPLIT_META_TABLE
	int meta_len;
#endif

#ifdef SQLITE_DEBUG
	if (store == DB_STORE_NAMED) {
		sqlite3_mutex *mutexOpen =
		    sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_OPEN);
		assert(sqlite3_mutex_held(mutexOpen));
	}
#endif

	new_bt = NULL;
	if ((new_bt = (struct BtShared *)sqlite3_malloc(
	    sizeof(struct BtShared))) == NULL)
		return SQLITE_NOMEM;
	memset(new_bt, 0, sizeof(struct BtShared));
	new_bt->dbStorage = store;
	if (store == DB_STORE_TMP) {
		new_bt->transactional = 0;
		new_bt->resultsBuffer = 1;
	} else {
		new_bt->transactional = 1;
		new_bt->resultsBuffer = 0;
	}
	new_bt->env_opened = 0;
	new_bt->flags = flags;
	new_bt->mutex = sqlite3MutexAlloc(SQLITE_MUTEX_FAST);
	if (new_bt->mutex == NULL && sqlite3GlobalConfig.bCoreMutex)
		goto err_nomem;

	/*
	 * Always open database with read-uncommitted enabled
	 * since SQLite allows DB_READ_UNCOMMITTED cursors to
	 * be created on any table.
	 */
#ifndef BDBSQL_SINGLE_THREAD
	new_bt->db_oflags = DB_THREAD |
	    (new_bt->transactional ? DB_READ_UNCOMMITTED : 0);
#endif
	sqlite3HashInit(&new_bt->db_cache);
	if (store == DB_STORE_NAMED) {
		/* Store full path of zfilename */
		dirPathName = dirPathBuf;
		sqlite3OsFullPathname(
		    db->pVfs, zFilename, sizeof dirPathBuf, dirPathName);
		if ((new_bt->full_name = sqlite3_strdup(dirPathName)) == NULL)
			goto err_nomem;
		if ((new_bt->orig_name = sqlite3_strdup(zFilename)) == NULL)
			goto err_nomem;

		/* Extract just the file name component. */
		new_bt->short_name = strrchr(new_bt->orig_name, '/');
		if (new_bt->short_name == NULL ||
		    new_bt->short_name < strrchr(new_bt->orig_name, '\\'))
			new_bt->short_name =
			    strrchr(new_bt->orig_name, '\\');
		if (new_bt->short_name == NULL)
			new_bt->short_name = new_bt->orig_name;
		else
			/* Move past actual path seperator. */
			++new_bt->short_name;
#ifdef BDBSQL_SPLIT_META_TABLE
		meta_len = strlen(new_bt->short_name) + 6;
		if ((new_bt->meta_name = sqlite3_malloc(meta_len)) == NULL)
			goto err_nomem;
		sqlite3_snprintf(meta_len, new_bt->meta_name, "%s.meta",
		    new_bt->short_name);
#else
		new_bt->meta_name = new_bt->short_name;
#endif
	}

	new_bt->cacheSize = SQLITE_DEFAULT_CACHE_SIZE;
	new_bt->pageCount = SQLITE_MAX_PAGE_COUNT;
	new_bt->db = db;
	new_bt->nRef = 1;
	new_bt->uid = g_uid_next++;
	new_bt->logFileSize = SQLITE_DEFAULT_JOURNAL_SIZE_LIMIT;
#ifdef SQLITE_SECURE_DELETE
	new_bt->secureDelete = 1;
#endif

	p->pBt = new_bt;

	return SQLITE_OK;

err_nomem:
	btreeFreeSharedBtree(new_bt);
	return SQLITE_NOMEM;
}

/*
** Open a new database.
**
** zFilename is the name of the database file.  If zFilename is NULL a new
** database with a random name is created.  This randomly named database file
** will be deleted when sqlite3BtreeClose() is called.
*/
int sqlite3BtreeOpen(
	const char *zFilename,		/* Name of the file containing the
					   BTree database */
	sqlite3 *db,			/* Associated database connection */
	Btree **ppBtree,		/* Pointer to new Btree object
					   written here */
	int flags,			/* Options */
	int vfsFlags			/* Flags passed through to VFS open */
) {
	Btree *p;
	BtShared *pBt, *next_bt;
	char *zDBFilename;
	int rc;
	sqlite3_mutex *mutexOpen;
	storage_mode_t store;

	log_msg(LOG_VERBOSE, "sqlite3BtreeOpen(%s, %p, %p, %u, %u)", zFilename,
	    db, ppBtree, flags, vfsFlags);

	pBt = NULL;
	rc = SQLITE_OK;
	zDBFilename = (char*)zFilename;
	mutexOpen = NULL;

	if ((p = (Btree *)sqlite3_malloc(sizeof(Btree))) == NULL)
		return SQLITE_NOMEM;
	memset(p, 0, sizeof(Btree));
	p->db = db;
	p->vfsFlags = vfsFlags;
	p->pBt = NULL;
	zDBFilename = (char*)zFilename;

	if ((vfsFlags & SQLITE_OPEN_TRANSIENT_DB) != 0) {
		log_msg(LOG_DEBUG, "sqlite3BtreeOpen creating temporary DB.");
		store = DB_STORE_TMP;
	} else if (zFilename == NULL ||
	    (zFilename[0] == '\0' || strcmp(zFilename, ":memory:") == 0) ||
	    (flags & BTREE_MEMORY) != 0) {
		/*
		 * Berkeley DB treats in-memory and temporary databases the
		 * same way: if there is not enough space in cache, pages
		 * overflow to temporary files.
		 */
		log_msg(LOG_DEBUG, "sqlite3BtreeOpen creating in-memory DB.");
		store = DB_STORE_INMEM;
	} else {
		log_msg(LOG_DEBUG, "sqlite3BtreeOpen creating named DB.");
		store = DB_STORE_NAMED;
		/*
		 * We always use the shared cache of handles, but SQLite
		 * performs additional checks for conflicting table locks
		 * when it is in shared cache mode, and aborts early.
		 * We use the sharable flag to control that behavior.
		 */
		if (vfsFlags & SQLITE_OPEN_SHAREDCACHE)
			p->sharable = 1;
		/* Strip leading "./" strings of zFilename. */
		if (zDBFilename[0] == '.' &&
		    (zDBFilename[1] == '\\' || zDBFilename[1] == '/'))
			zDBFilename += 2;
	}

	mutexOpen = sqlite3MutexAlloc(OPEN_MUTEX(store));
	sqlite3_mutex_enter(mutexOpen);

	/* Non-named databases never share any content in BtShared. */
	if (store == DB_STORE_NAMED) {
		if ((rc = btreeGetSharedBtree(&pBt,
		    zDBFilename, db, store)) != SQLITE_OK)
			goto err;
	}

	if (pBt != NULL) {
		p->pBt = pBt;
		if ((rc = btreeOpenEnvironment(p, 0)) != SQLITE_OK)
			goto err;
	} else {
		if ((rc = btreeCreateSharedBtree(p,
		    zDBFilename, db, flags, store)) != 0)
			goto err;
		pBt = p->pBt;
		if (!pBt->resultsBuffer &&
		    (rc = btreePrepareEnvironment(p)) != 0) {
			btreeFreeSharedBtree(pBt);
			goto err;
		}
		/* Only named databases are in the shared btree cache. */
		if (store == DB_STORE_NAMED) {
			if (g_shared_btrees == NULL) {
				pBt->pPrevDb = NULL;
				g_shared_btrees = pBt;
			} else {
				for (next_bt = g_shared_btrees;
				    next_bt->pNextDb != NULL;
				    next_bt = next_bt->pNextDb) {}
				next_bt->pNextDb = pBt;
				pBt->pPrevDb = next_bt;
			}
		}
	}

	*ppBtree = p;

err:	if (rc != SQLITE_OK)
		sqlite3_free(p);
	if (mutexOpen != NULL) {
		assert(sqlite3_mutex_held(mutexOpen));
		sqlite3_mutex_leave(mutexOpen);
	}
	return rc;
}

static int btreeCleanupEnv(const char *home)
{
	DB_ENV *tmp_env;
	int count, i, ret;
	char **names, buf[BT_MAX_PATH];

	log_msg(LOG_DEBUG, "btreeCleanupEnv removing existing env.");
	/*
	 * If there is a directory (environment), but no
	 * database file. Clear the environment to avoid
	 * carrying over information from earlier sessions.
	 */
	if ((ret = db_env_create(&tmp_env, 0)) != 0)
		return ret;

	/* Remove log files */
	if ((ret = __os_dirlist(tmp_env->env, home, 0, &names, &count)) != 0) {
		(void)tmp_env->close(tmp_env, 0);
		return ret;
	}

	for (i = 0; i < count; i++) {
		if (strncmp(names[i], "log.", 4) != 0)
			continue;
		sqlite3_snprintf(sizeof buf, buf, "%s%s%s",
		    home, PATH_SEPARATOR, names[i]);
		/*
		 * Use Berkeley DB __os_unlink (not sqlite3OsDelete) since
		 * this file has always been managed by Berkeley DB.
		 */
		(void)__os_unlink(NULL, buf, 0);
	}

	__os_dirfree(tmp_env->env, names, count);

	/*
	 * TODO: Do we want force here? Ideally all handles
	 * would always be closed on exit, so DB_FORE would
	 * not be necessary. The world is not currently ideal.
	 */
	return tmp_env->remove(tmp_env, home, DB_FORCE);
}

/* Close all cursors for the given transaction. */
static int btreeCloseAllCursors(Btree *p, DB_TXN *txn)
{
	BtCursor *c, *nextc, *prevc, *free_cursors;
	BtShared *pBt;
	DB_TXN *db_txn, *dbc_txn;
	int rc, t_rc;

	log_msg(LOG_VERBOSE, "btreeCloseAllCursors(%p, %p)", p, txn);

	free_cursors = NULL;
	pBt = p->pBt;
	rc = SQLITE_OK;

	sqlite3_mutex_enter(pBt->mutex);
	for (c = pBt->first_cursor, prevc = NULL;
	    c != NULL;
	    prevc = c, c = nextc) {
		nextc = c->next;
		if (p != c->pBtree)
			continue;
		if (txn != NULL) {
			if (c->dbc == NULL)
				continue;
			dbc_txn = c->dbc->txn;
			db_txn = c->dbc->dbp->cur_txn;
			while (dbc_txn != NULL && dbc_txn != txn)
				dbc_txn = dbc_txn->parent;
			while (db_txn != NULL && db_txn != txn)
				db_txn = db_txn->parent;
			if (dbc_txn != txn && db_txn != txn)
				continue;
		}

		/*
		 * Detach the cursor from the main list and add it to the free
		 * list.
		 */
		if (prevc == NULL)
			pBt->first_cursor = nextc;
		else
			prevc->next = nextc;

		c->next = free_cursors;
		free_cursors = c;
		c = prevc;
	}
	sqlite3_mutex_leave(pBt->mutex);

	for (c = free_cursors; c != NULL; c = c->next) {
		t_rc = btreeCloseCursor(c, 0);
		if (t_rc != SQLITE_OK && rc == SQLITE_OK)
			rc = t_rc;
	}

	if (p->schemaLock != NULL && txn != NULL) {
		dbc_txn = p->schemaLock->txn;
		while (dbc_txn != NULL && dbc_txn != txn)
		    dbc_txn = dbc_txn->parent;
		if (dbc_txn == txn &&
		    (t_rc = btreeLockSchema(p, LOCKMODE_NONE)) != SQLITE_OK &&
		    rc == SQLITE_OK)
			rc = t_rc;
	}

	return rc;
}

static int btreeCleanupCachedHandles(Btree *p, cleanup_mode_t cleanup)
{
	DB *dbp;
	CACHED_DB *cached_db;
	BtShared *pBt;
	HashElem *e;
	int ret, rc;

	log_msg(LOG_VERBOSE, "btreeCleanupCachedHandles(%p, %d)",
	    p, (int)cleanup);

	pBt = p->pBt;
	e = NULL;
	rc = SQLITE_OK;

	for (e = sqliteHashFirst(&pBt->db_cache); e != NULL;
	    e = sqliteHashNext(e)) {
		cached_db = sqliteHashData(e);

		if (cached_db == NULL)
			continue;

		if ((dbp = cached_db->dbp) != NULL) {
			/*
			 * We have to clear the cache of any stale DB handles.
			 * If a transaction has been aborted, the handle will
			 * no longer be open.  We peek inside the handle at
			 * the flags to find out: otherwise, we would need to
			 * track all parent / child relationships when
			 * rolling back transactions.
			 */
			if (cleanup != CLEANUP_CLOSE &&
			    (dbp->flags & DB_AM_OPEN_CALLED) != 0)
				continue;

#ifndef BDBSQL_SINGLE_THREAD
			if (dbp->app_private != NULL)
				sqlite3_free(dbp->app_private);
#endif
			if ((ret = dbp->close(dbp, DB_NOSYNC)) == 0 &&
			    rc == SQLITE_OK)
				rc = dberr2sqlite(ret);
		}
		if (cleanup == CLEANUP_CLOSE)
			sqlite3_free(cached_db);
		else
			cached_db->dbp = NULL;
	}

	return rc;
}

/*
** Close an open database and invalidate all cursors.
*/
int sqlite3BtreeClose(Btree *p)
{
	BtShared *pBt;
	int ret, rc, t_rc, t_ret;
	sqlite3_mutex *mutexOpen;

	log_msg(LOG_VERBOSE, "sqlite3BtreeClose(%p)", p);

	ret = 0;
	pBt = p->pBt;
	rc = SQLITE_OK;

	if (pBt == NULL)
		goto done;

	rc = btreeCloseAllCursors(p, NULL);

	if (pReadTxn != NULL &&
	    (t_rc = sqlite3BtreeRollback(p)) != SQLITE_OK && rc == SQLITE_OK)
		rc = t_rc;
	assert(pReadTxn == NULL);

	if (pFamilyTxn != NULL) {
		ret = pFamilyTxn->commit(pFamilyTxn, 0);
		pFamilyTxn = NULL;
		p->inTrans = TRANS_NONE;
		if (ret != 0 && rc == SQLITE_OK)
			rc = dberr2sqlite(ret);
	}

	if (p->schema != NULL) {
		if (p->free_schema != NULL)
			p->free_schema(p->schema);
		/* This needs to be a real call to sqlite3_free. */
#ifdef BDBSQL_OMIT_LEAKCHECK
#undef	sqlite3_free
#endif
		sqlite3_free(p->schema);
#ifdef BDBSQL_OMIT_LEAKCHECK
#define	sqlite3_free free
#endif
	}

	/*
	 * #18538 -- another thread may be attempting to open this BtShared at
	 * the same time that we are closing it.
	 *
	 * To avoid a race, we need to hold the open mutex until the
	 * environment is closed.  Otherwise, the opening thread might open its
	 * handle before this one is completely closed, and DB_REGISTER doesn't
	 * support that.
	 */
	mutexOpen = sqlite3MutexAlloc(OPEN_MUTEX(pBt->dbStorage));
	sqlite3_mutex_enter(mutexOpen);

	if (--pBt->nRef == 0) {
		if (pBt->dbStorage == DB_STORE_NAMED) {
			/* Remove it from the linked list of shared envs. */
			assert(pBt == g_shared_btrees || pBt->pPrevDb != NULL);
			if (pBt == g_shared_btrees)
				g_shared_btrees = pBt->pNextDb;
			else
				pBt->pPrevDb->pNextDb = pBt->pNextDb;
			if (pBt->pNextDb != NULL)
				pBt->pNextDb->pPrevDb = pBt->pPrevDb;
		}

		/*
		 * At this point, the BtShared has been removed from the shared
		 * list, so it cannot be reused and it is safe to close any
		 * handles.
		 */
		t_rc = btreeCleanupCachedHandles(p, CLEANUP_CLOSE);
		if (t_rc != SQLITE_OK && rc == SQLITE_OK)
			rc = t_rc;
		sqlite3HashClear(&pBt->db_cache);

		if (pTablesDb != NULL && (t_ret =
		    pTablesDb->close(pTablesDb, DB_NOSYNC)) != 0 && ret == 0)
			ret = t_ret;
		if (pMetaDb != NULL && (t_ret =
		    pMetaDb->close(pMetaDb, DB_NOSYNC)) != 0 && ret == 0)
			ret = t_ret;
		pTablesDb = pMetaDb = NULL;

		/* We never close down the shared tmp environment. */
		if (pBt->dbStorage == DB_STORE_NAMED && pDbEnv) {
			/*
			 * Checkpoint when closing if we have switched log
			 * files. This allows log file auto-removal, which
			 * keeps the size of the environment directory small.
			 * It also bounds the time we would have to spend in
			 * recovery in the event of a crash.
			 */
			if (pBt->transactional && pBt->env_opened && (t_ret =
			    pDbEnv->txn_checkpoint(pDbEnv, 0, 0, 0)) != 0 &&
			    ret == 0)
				ret = t_ret;
			if ((t_ret = pDbEnv->close(pDbEnv, 0)) != 0 && ret == 0)
				ret = t_ret;
		}

		btreeFreeSharedBtree(pBt);
	}

	sqlite3_mutex_leave(mutexOpen);

done:	sqlite3_free(p);
	return MAP_ERR(rc, ret);
}

/*
** Change the limit on the number of pages allowed in the cache.
**
** The maximum number of cache pages is set to the absolute value of mxPage.
** If mxPage is negative in SQLite, the pager will operate asynchronously - it
** will not stop to do fsync()s to insure data is written to the disk surface
** before continuing.
**
** The Berkeley DB cache always operates in asynchronously (except when writing
** a checkpoint), but log writes are triggered to maintain write-ahead logging
** semantics.
*/
int sqlite3BtreeSetCacheSize(Btree *p, int mxPage)
{
	BtShared *pBt;
	log_msg(LOG_VERBOSE, "sqlite3BtreeSetCacheSize(%p, %u)", p, mxPage);

	pBt = p->pBt;
	if (mxPage < 0)
		mxPage = -mxPage;

	if (!p->connected)
		pBt->cacheSize = mxPage;
	return SQLITE_OK;
}

/*
** Change the way data is synced to disk in order to increase or decrease how
** well the database resists damage due to OS crashes and power failures.
** Level 1 is the same as asynchronous (no syncs() occur and there is a high
** probability of damage)  Level 2 is the default.  There is a very low but
** non-zero probability of damage.  Level 3 reduces the probability of damage
** to near zero but with a write performance reduction.
**
** Berkeley DB always does the equivalent of "fullSync".
*/
int sqlite3BtreeSetSafetyLevel(Btree *p, int level, int fullSync)
{
	BtShared *pBt;
	log_msg(LOG_VERBOSE,
	    "sqlite3BtreeSetSafetyLevel(%p, %u, %u)", p, level, fullSync);

	pBt = p->pBt;
	if (GET_DURABLE(p->pBt)) {
		pDbEnv->set_flags(pDbEnv, DB_TXN_NOSYNC, (level == 1));
		pDbEnv->set_flags(pDbEnv, DB_TXN_WRITE_NOSYNC, (level == 2));
	}
	return SQLITE_OK;
}

/*
** Attempt to start a new transaction. A write-transaction is started if the
** second argument is true, otherwise a read-transaction. No-op if a
** transaction is already in progress.
**
** A write-transaction must be started before attempting any changes to the
** database.  None of the following routines will work unless a transaction
** is started first:
**
**      sqlite3BtreeCreateTable()
**      sqlite3BtreeCreateIndex()
**      sqlite3BtreeClearTable()
**      sqlite3BtreeDropTable()
**      sqlite3BtreeInsert()
**      sqlite3BtreeDelete()
**      sqlite3BtreeUpdateMeta()
*/
int sqlite3BtreeBeginTrans(Btree *p, int wrflag)
{
	BtShared *pBt;
	int rc;

	log_msg(LOG_VERBOSE,
	    "sqlite3BtreeBeginTrans(%p, %u) -- writer %s",
	    p, wrflag, pReadTxn ? "active" : "inactive");

	if (wrflag && (p->pBt->db_oflags & DB_RDONLY))
		return SQLITE_READONLY;

	pBt = p->pBt;
	rc = SQLITE_OK;

	if (!p->connected) {
		p->inTrans = (wrflag || p->inTrans == TRANS_WRITE) ?
		    TRANS_WRITE : TRANS_READ;
		return SQLITE_OK;
	}
	if (pBt->transactional) {
		if (wrflag && p->inTrans != TRANS_WRITE)
			p->inTrans = TRANS_WRITE;
		else if (p->inTrans == TRANS_NONE)
			p->inTrans = TRANS_READ;

		if (pReadTxn == NULL || p->nSavepoint <= p->db->nSavepoint)
			rc = sqlite3BtreeBeginStmt(p, p->db->nSavepoint);
	}
	return rc;
}

/***************************************************************************
** This routine does the first phase of a two-phase commit.  This routine
** causes a rollback journal to be created (if it does not already exist)
** and populated with enough information so that if a power loss occurs the
** database can be restored to its original state by playing back the journal.
** Then the contents of the journal are flushed out to the disk. After the
** journal is safely on oxide, the changes to the database are written into
** the database file and flushed to oxide. At the end of this call, the
** rollback journal still exists on the disk and we are still holding all
** locks, so the transaction has not committed. See sqlite3BtreeCommit() for
** the second phase of the commit process.
**
** This call is a no-op if no write-transaction is currently active on pBt.
**
** Otherwise, sync the database file for the engine pBt. zMaster points to
** the name of a master journal file that should be written into the
** individual journal file, or is NULL, indicating no master journal file
** (single database transaction).
**
** When this is called, the master journal should already have been created,
** populated with this journal pointer and synced to disk.
**
** Once this is routine has returned, the only thing required to commit the
** write-transaction for this database file is to delete the journal.
*/
int sqlite3BtreeCommitPhaseOne(Btree *p, const char *zMaster)
{
	log_msg(LOG_VERBOSE,
	    "sqlite3BtreeCommitPhaseOne(%p, %s)", p, zMaster);
	return SQLITE_OK;
}

/***************************************************************************
** Commit the transaction currently in progress.
**
** This routine implements the second phase of a 2-phase commit.  The
** sqlite3BtreeSync() routine does the first phase and should be invoked prior
** to calling this routine.  The sqlite3BtreeSync() routine did all the work
** of writing information out to disk and flushing the contents so that they
** are written onto the disk platter.  All this routine has to do is delete
** or truncate the rollback journal (which causes the transaction to commit)
** and drop locks.
**
** This will release the write lock on the database file.  If there are no
** active cursors, it also releases the read lock.
*/
int sqlite3BtreeCommitPhaseTwo(Btree *p)
{
	BtShared *pBt;
#ifdef BDBSQL_SEMITXN_TRUNCATE
	DELETED_TABLE *dtable, *next;
	char *tableName, tableNameBuf[DBNAME_SIZE];
#endif
	int rc, ret, t_rc;

	log_msg(LOG_VERBOSE,
	    "sqlite3BtreeCommitPhaseTwo(%p) -- writer %s",
	    p, pReadTxn ? "active" : "inactive");

	pBt = p->pBt;
	rc = SQLITE_OK;

	if (pReadTxn && p->db->activeVdbeCnt <= 1) {
		t_rc = btreeCloseAllCursors(p, pReadTxn);
		if (t_rc != SQLITE_OK && rc == SQLITE_OK)
			rc = t_rc;

		/*
		 * Even if we get an error, we can't use the
		 * transaction handle again, so we should keep going
		 * and clear out the Btree fields.
		 */
		if ((ret = pReadTxn->commit(pReadTxn, 0)) != 0 &&
		    rc == SQLITE_OK)
			rc = dberr2sqlite(ret);

		pSavepointTxn = pReadTxn = NULL;
		p->nSavepoint = 0;

	       /*
		* Checkpoint if there has been more than a log files worth of
		* log records since the last checkpoint.
		*/
		if (rc == SQLITE_OK) {
			rc = dberr2sqlite(pDbEnv->txn_checkpoint(pDbEnv,
				pBt->logFileSize/1024, 0, 0));
		}

#ifdef BDBSQL_SEMITXN_TRUNCATE
		for (dtable = p->deleted_tables;
		    dtable != NULL;
		    dtable = next) {
			tableName = tableNameBuf;
			GET_TABLENAME(tableName, sizeof tableNameBuf,
			    dtable->iTable, "-old");

			ret = pDbEnv->dbremove(pDbEnv, NULL,
			    pBt->short_name, tableName, DB_NOSYNC);

			next = dtable->next;
			sqlite3_free(dtable);
		}
		p->deleted_tables = NULL;
#endif
	} else if (p->inTrans == TRANS_WRITE)
		rc = sqlite3BtreeSavepoint(p, SAVEPOINT_RELEASE, 0);

	if (p->db->activeVdbeCnt > 1)
		p->inTrans = TRANS_READ;
	else {
		p->inTrans = TRANS_NONE;
		if (p->schemaLockMode > LOCKMODE_NONE)
			rc = btreeLockSchema(p, LOCKMODE_NONE);
	}

	return rc;
}

/*
** Do both phases of the commit.
*/
int sqlite3BtreeCommit(Btree *p)
{
	BtShared *pBt;
	int rc;

	log_msg(LOG_VERBOSE, "sqlite3BtreeCommit(%p)", p);

	pBt = p->pBt;
	rc = sqlite3BtreeCommitPhaseOne(p, NULL);
	if (rc == SQLITE_OK)
		rc = sqlite3BtreeCommitPhaseTwo(p);

	return (rc);
}

/*
** Rollback the transaction in progress.  All cursors will be invalidated
** by this operation.  Any attempt to use a cursor that was open at the
** beginning of this operation will result in an error.
**
** This will release the write lock on the database file.  If there are no
** active cursors, it also releases the read lock.
*/
int sqlite3BtreeRollback(Btree *p)
{
	BtShared *pBt;
	int rc, t_rc;

	log_msg(LOG_VERBOSE, "sqlite3BtreeRollback(%p)", p);

	rc = SQLITE_OK;
	pBt = p->pBt;
	if (pReadTxn != NULL)
		rc = sqlite3BtreeSavepoint(p, SAVEPOINT_ROLLBACK, -1);
	if (p->schemaLockMode > LOCKMODE_NONE &&
	    (t_rc = btreeLockSchema(p, LOCKMODE_NONE)) != SQLITE_OK &&
	    rc == SQLITE_OK)
		rc = t_rc;

	return rc;
}

/*
** Start a statement subtransaction.  The subtransaction can be rolled back
** independently of the main transaction. You must start a transaction
** before starting a subtransaction. The subtransaction is ended automatically
** if the main transaction commits or rolls back.
**
** Only one subtransaction may be active at a time.  It is an error to try
** to start a new subtransaction if another subtransaction is already active.
**
** Statement subtransactions are used around individual SQL statements that
** are contained within a BEGIN...COMMIT block.  If a constraint error
** occurs within the statement, the effect of that one statement can be
** rolled back without having to rollback the entire transaction.
*/
int sqlite3BtreeBeginStmt(Btree *p, int iStatement)
{
	BtShared *pBt;
	int ret;

	log_msg(LOG_VERBOSE, "sqlite3BtreeBeginStmt(%p, %d)", p, iStatement);

	pBt = p->pBt;
	ret = 0;

	if (pBt->transactional) {
		assert(p->inTrans != TRANS_NONE && pFamilyTxn != NULL);

		if (!pReadTxn) {
			if ((ret = pDbEnv->txn_begin(pDbEnv, pFamilyTxn,
			    &pReadTxn, 0)) != 0)
				return dberr2sqlite(ret);
			pSavepointTxn = pReadTxn;
		}

		while (p->nSavepoint <= iStatement) {
			if ((ret = pDbEnv->txn_begin(pDbEnv, pSavepointTxn,
			    &pSavepointTxn, 0)) != 0)
				return dberr2sqlite(ret);
			p->nSavepoint++;
		}
	}
	return SQLITE_OK;
}

static int btreeCompare(DB *dbp, const DBT *dbt1, const DBT *dbt2)
{
	int res;

	log_msg(LOG_VERBOSE, "btreeCompare(%p, %p, %p)", dbp, dbt1, dbt2);

	if (dbt1->app_data != NULL)
		/* Use the unpacked key from dbt1 */
		res = -sqlite3VdbeRecordCompare(dbt2->size, dbt2->data,
		    dbt1->app_data);
	else if (dbt2->app_data != NULL)
		/* Use the unpacked key from dbt2 */
		res = sqlite3VdbeRecordCompare(dbt1->size, dbt1->data,
		    dbt2->app_data);
	else
	{
		/*
		 * We don't have an unpacked key cached, generate one.
		 *
		 * This code should only execute if we are inside
		 * DB->sort_multiple, or some uncommon paths inside Berkeley
		 * DB, such as deferred delete of an item in a Btree.
		 */

		struct KeyInfo *keyInfo;
		UnpackedRecord *p;
		char aSpace[150];

#ifdef BDBSQL_SINGLE_THREAD
		/* Use the keyInfo pointer that was stashed in app_private. */
		keyInfo = dbp->app_private;
		assert(keyInfo);
#else
		/* Find a cursor for this table, and use its keyInfo. */
		TableInfo *tableInfo = dbp->app_private;
		BtShared *pBt = tableInfo->pBt;
		BtCursor *pCur = NULL;
		int iTable;

		iTable = tableInfo->iTable;

		/*
		 * We can end up in here while closing a cursor, but we take
		 * care not to be holding the BtShared mutex.  Keep the mutex
		 * until we are done so that some other thread can't free the
		 * keyInfo from under us.
		 */
		if (!pBt->resultsBuffer)
			sqlite3_mutex_enter(pBt->mutex);

		for (pCur = pBt->first_cursor;
		    pCur != NULL;
		    pCur = pCur->next)
			if (pCur->tableIndex == iTable)
				break;

		assert(pCur);
		keyInfo = pCur->keyInfo;
#endif

		p = sqlite3VdbeRecordUnpack(keyInfo, dbt2->size, dbt2->data,
		    aSpace, sizeof aSpace);

		/*
		 * XXX If we are out of memory, the call to unpack the record
		 * may have returned NULL.  We want to return that error to
		 * Berkeley DB, but there is no way to do that.  For now,
		 * choose an arbitrary result.
		 */
		res = (p == NULL) ? -1 :
		    sqlite3VdbeRecordCompare(dbt1->size, dbt1->data, p);
		if (p != NULL)
			sqlite3VdbeDeleteUnpackedRecord(p);

#ifndef BDBSQL_SINGLE_THREAD
		if (!pBt->resultsBuffer)
			sqlite3_mutex_leave(pBt->mutex);
#endif
	}
	return res;
}

/*
 * A utility function to create the table containing the actual data.
 */
static int btreeCreateDataTable(
	Btree *p,			/* The btree */
	int iTable,			/* Root page of table to create */
	CACHED_DB **ppCachedDb)
{
	BtShared *pBt;
	CACHED_DB *cached_db, *stale_db;
	DB *dbp;
	DB_MPOOLFILE *pMpf;
#ifndef BDBSQL_SINGLE_THREAD
	TableInfo *tableInfo;
#endif
	char *fileName, *tableName, tableNameBuf[DBNAME_SIZE];
	int ret;
	u_int32_t meta_flags;

	log_msg(LOG_VERBOSE, "sqlite3BtreeCreateDataTable(%p, %u, %p)",
	    p, iTable, ppCachedDb);

	pBt = p->pBt;

	dbp = NULL;
	assert(ppCachedDb != NULL);
	cached_db = *ppCachedDb;

	/* Odd-numbered tables have integer keys. */
	meta_flags = (iTable & 1) ? BTREE_INTKEY : 0;

	if ((ret = db_create(&dbp, pDbEnv, 0)) != 0)
		goto err;
#ifndef BDBSQL_SINGLE_THREAD
	if ((tableInfo = sqlite3_malloc(sizeof(TableInfo))) == NULL) {
		ret = ENOMEM;
		goto err;
	}
	tableInfo->pBt = pBt;
	tableInfo->iTable = iTable;
	dbp->app_private = tableInfo;
#endif
	if ((meta_flags & BTREE_INTKEY) == 0)
		dbp->set_bt_compare(dbp, btreeCompare);
	if (pBt->pageSize != 0 &&
	    (ret = dbp->set_pagesize(dbp, pBt->pageSize)) != 0)
		goto err;
	if (pBt->dbStorage == DB_STORE_INMEM) {
		/* Make sure the cache does not overflow to disk. */
		pMpf = dbp->get_mpf(dbp);
		pMpf->set_flags(pMpf, DB_MPOOL_NOFILE, 1);
	}
	if (!pBt->resultsBuffer && !GET_DURABLE(pBt) &&
	    (ret = dbp->set_flags(dbp, DB_TXN_NOT_DURABLE)) != 0)
		goto err;
	tableName = tableNameBuf;
	GET_TABLENAME(tableName, sizeof tableNameBuf, iTable, "");
	log_msg(LOG_VERBOSE,
	  "sqlite3BtreeCursor creating the actual DB: file name:"
	  "%s, table name: %s type: %u.",
	  pBt->full_name, tableName, pBt->dbStorage);

	if (!pBt->resultsBuffer) {
		/*
		 * The first table is "special", and belongs in the metadata
		 * file, if metadata splitting is enabled. Ensure the correct
		 * file name is used.
		 */
		if (iTable == 1 && pBt->dbStorage == DB_STORE_NAMED)
			fileName = pBt->meta_name;
		else
			fileName = pBt->short_name;
		/*
		 * First try without DB_CREATE, in auto-commit mode, so the
		 * handle can be safely shared in the cache.  If we are really
		 * creating the table, we should be holding the schema lock,
		 * which will protect the handle in cache until we are done.
		 */
		ret = ENOENT;
		if (pBt->dbStorage == DB_STORE_NAMED &&
			(pBt->db_oflags & DB_CREATE) != 0)
			ret = dbp->open(dbp, pFamilyTxn, fileName, tableName,
				DB_BTREE, (pBt->db_oflags & ~DB_CREATE) |
				GET_AUTO_COMMIT(pBt, pFamilyTxn), 0);
		if (ret == ENOENT)
			ret = dbp->open(dbp, pSavepointTxn, fileName, tableName,
				DB_BTREE, pBt->db_oflags |
				GET_AUTO_COMMIT(pBt, pSavepointTxn), 0);
		if (ret != 0)
			goto err;
	}

	if (cached_db == NULL) {
		if ((cached_db = (CACHED_DB *)sqlite3_malloc(
		    sizeof(CACHED_DB))) == NULL)
		{
			ret = ENOMEM;
			goto err;
		}
		memset(cached_db, 0, sizeof(CACHED_DB));
		sqlite3_snprintf(sizeof cached_db->key,
		    cached_db->key, "%x", iTable);

		assert(sqlite3_mutex_held(pBt->mutex));
		stale_db = sqlite3HashInsert(&pBt->db_cache, cached_db->key,
		    (int)strlen(cached_db->key), cached_db);
		if (stale_db) {
			sqlite3_free(stale_db);
			/*
			 * Hash table out of memory when returned pointer is
			 * same as the original value pointer.
			 */
			if (stale_db == cached_db) {
				ret = ENOMEM;
				goto err;
			}
		}
	}

	assert(cached_db->dbp == NULL);
	cached_db->dbp = dbp;
	++p->cached_dbs;
	cached_db->flags = meta_flags;
	cached_db->created = 1;
	*ppCachedDb = cached_db;
	return SQLITE_OK;

err:	if (dbp != NULL) {
#ifndef BDBSQL_SINGLE_THREAD
		if (dbp->app_private != NULL)
			sqlite3_free(dbp->app_private);
#endif
		(void)dbp->close(dbp, DB_NOSYNC);
		dbp = NULL;
	}
	return (ret == 0) ? SQLITE_OK : dberr2sqlite(ret);
}

/*
** Create a new cursor for the BTree whose root is on the page iTable. The act
** of acquiring a cursor gets a read lock on the database file.
**
** If wrFlag==0, then the cursor can only be used for reading.
** If wrFlag==1, then the cursor can be used for reading or for writing if
** other conditions for writing are also met.  These are the conditions that
** must be met in order for writing to be allowed:
**
** 1:  The cursor must have been opened with wrFlag==1
**
** 2:  No other cursors may be open with wrFlag==0 on the same table
**
** 3:  The database must be writable (not on read-only media)
**
** 4:  There must be an active transaction.
**
** Condition 2 warrants further discussion.  If any cursor is opened on a table
** with wrFlag==0, that prevents all other cursors from writing to that table.
** This is a kind of "read-lock".  When a cursor is opened with wrFlag==0
** it is guaranteed that the table will not change as long as the cursor
** is open.  This allows the cursor to do a sequential scan of the table
** without having to worry about entries being inserted or deleted during the
** scan.  Cursors should be opened with wrFlag==0 only if this read-lock
** property is needed. That is to say, cursors should be opened with
** wrFlag==0 only if they intend to use sqlite3BtreeNext() system call.
** All other cursors should be opened with wrFlag==1 even if they never really
** intend to write.
**
** No checking is done to make sure that page iTable really is the root page
** of a b-tree.  If it is not, then the cursor acquired will not work
** correctly.
**
** The comparison function must be logically the same for every cursor on a
** particular table.  Changing the comparison function will result in
** incorrect operations.  If the comparison function is NULL, a default
** comparison function is used.  The comparison function is always ignored
** for INTKEY tables.
*/
int sqlite3BtreeCursor(
	Btree *p,			/* The btree */
	int iTable,			/* Root page of table to open */
	int wrFlag,			/* 1 to write. 0 read-only */
	struct KeyInfo *keyInfo,	/* First argument to compare function */
	BtCursor *pCur)			/* Write new cursor here */
{
	BtShared *pBt;
	char cached_db_key[CACHE_KEY_SIZE];
	CACHED_DB *cached_db;
	int rc, ret;

	log_msg(LOG_VERBOSE, "sqlite3BtreeCursor(%p, %u, %u, %p, %p)",
	    p, iTable, wrFlag, keyInfo, pCur);

	pBt = p->pBt;
	rc = SQLITE_OK;
	ret = 0;

	if (!p->connected) {
		/*
		 * If the table is temporary, vdbe expects the table to be
		 * created automatically when the first cursor is opened.
		 * Otherwise, if the database does not exist yet, the caller
		 * expects a SQLITE_EMPTY return, vdbe will then call
		 * sqlite3BtreeCreateTable directly.
		 * If the code created the temporary environment the first time
		 * sqlite3BtreeOpen is called, it would not be possible to
		 * honor cache size setting pragmas.
		 */
		if (pBt->dbStorage != DB_STORE_TMP &&
		    !wrFlag && !pBt->env_opened)
			return SQLITE_EMPTY;
		else if (!pBt->resultsBuffer &&
		    (rc = btreeOpenEnvironment(p, 1)) != SQLITE_OK)
			goto err;
	}

	if (wrFlag && (pBt->db_oflags & DB_RDONLY))
		return SQLITE_READONLY;

	assert(p->connected || pBt->resultsBuffer);
	assert(!pBt->transactional || p->inTrans != TRANS_NONE);

	pCur->pBtree = p;
	pCur->tableIndex = iTable;

	/* SQLite should guarantee that an appropriate transaction is active. */
	assert(!pBt->transactional || pReadTxn != NULL);
	assert(!pBt->transactional || !wrFlag || pSavepointTxn != NULL);

	/* Retrieve the matching handle from the cache. */
	sqlite3_snprintf(sizeof cached_db_key, cached_db_key, "%x", iTable);

	sqlite3_mutex_enter(pBt->mutex);
	cached_db = sqlite3HashFind(&pBt->db_cache,
	    cached_db_key, (int)strlen(cached_db_key));
	if (cached_db == NULL || cached_db->dbp == NULL)
		rc = btreeCreateDataTable(p, iTable, &cached_db);
	sqlite3_mutex_leave(pBt->mutex);
	if (rc != SQLITE_OK)
		goto err;
	assert(cached_db != NULL && cached_db->dbp != NULL);

	pBDb = cached_db->dbp;
	pCur->flags = cached_db->flags;
	pCur->keyInfo = keyInfo;

	/*
	 * Always use the savepoint transaction for write cursors, or the
	 * top-level cursor for read-only cursors (to avoid tripping and
	 * re-opening the read cursor for updates within a select).
	 */
	pCur->txn = wrFlag ? pSavepointTxn : pReadTxn;

	if (!pBt->resultsBuffer) {
		ret = pBDb->cursor(pBDb, pCur->txn, &pDbc,
		    (pBt->transactional &&
		    (p->db->flags & SQLITE_ReadUncommitted)) ?
		    DB_READ_UNCOMMITTED : 0);
		if (ret != 0) {
			rc = dberr2sqlite(ret);
			goto err;
		}
	}
#ifdef BDBSQL_SINGLE_THREAD
	pBDb->app_private = keyInfo;
#endif

	pCur->skipMulti = 1;
	pCur->multiData.data = NULL;

	pCur->wrFlag = wrFlag;
	pCur->eState = CURSOR_INVALID;
	pCur->lastRes = 0;
	if (pBt->resultsBuffer && !wrFlag) {
		/*
		 * The sqlite btree API doesn't care about the position of
		 * cursors on error.  Setting this flag avoids cursor
		 * duplication inside Berkeley DB.  We can only do it for
		 * read-only cursors, however: deletes don't complete until the
		 * cursor is closed.
		 */
		pDbc->flags |= DBC_TRANSIENT;
	}

	sqlite3_mutex_enter(pBt->mutex);
	assert(pCur != pBt->first_cursor);
	pCur->next = pBt->first_cursor;
	pBt->first_cursor = pCur;
	sqlite3_mutex_leave(pBt->mutex);
	return SQLITE_OK;

err:	if (pDbc != NULL) {
		(void)pDbc->close(pDbc);
		pDbc = NULL;
	}
	pCur->eState = CURSOR_FAULT;
	pCur->error = rc;
	return SQLITE_OK;
}

/*
** Return the size of a BtCursor object in bytes.
**
** This interfaces is needed so that users of cursors can preallocate
** sufficient storage to hold a cursor.  The BtCursor object is opaque
** to users so they cannot do the sizeof() themselves - they must call
** this routine.
*/
int sqlite3BtreeCursorSize(void)
{
	return (sizeof(BtCursor));
}

/*
** Initialize memory that will be converted into a BtCursor object.
**
** The simple approach here would be to memset() the entire object
** to zero.  But if there are large parts that can be skipped, do
** that here to save time.
*/
void sqlite3BtreeCursorZero(BtCursor *pCur)
{
	memset(pCur, 0, sizeof(BtCursor));
}

static int btreeCloseCursor(BtCursor *pCur, int listRemove)
{
	BtCursor *c, *prev;
	Btree *p;
	BtShared *pBt;
	int ret;

	assert(pCur->pBtree != NULL);
	p = pCur->pBtree;
	pBt = p->pBt;
	ret = 0;

	/*
	 * Change the cursor's state to invalid before closing it, and do
	 * so holding the BtShared mutex, so that no other thread will attempt
	 * to access this cursor while it is being closed.
	 */
	sqlite3_mutex_enter(pBt->mutex);
	pCur->eState = CURSOR_INVALID;
	sqlite3_mutex_leave(pBt->mutex);

	/*
	 * Warning: it is important that we call DBC->close while the cursor
	 * is still on the list.  It is possible that closing a cursor will
	 * result in the comparison callback being called, which in turn
	 * may go looking on the list for a matching cursor, in order to find
	 * a KeyInfo pointer it can use.
	 */
	if (pDbc) {
		ret = pDbc->close(pDbc);
		pDbc = NULL;
	}

	if (listRemove) {
		sqlite3_mutex_enter(pBt->mutex);
		for (prev = NULL, c = pBt->first_cursor; c != NULL;
		    prev = c, c = c->next)
			if (c == pCur) {
				if (prev == NULL)
					pBt->first_cursor = c->next;
				else
					prev->next = c->next;
				break;
			}
		sqlite3_mutex_leave(pBt->mutex);
	}

	if ((pCur->key.flags & DB_DBT_APPMALLOC) != 0) {
		sqlite3_free(pCur->key.data);
		pCur->key.data = NULL;
		pCur->key.flags &= ~DB_DBT_APPMALLOC;
	}
	if (pCur->multiData.data != NULL) {
		sqlite3_free(pCur->multiData.data);
		pCur->multiData.data = NULL;
	}

	pCur->pBtree = NULL;

	return (ret == 0) ? SQLITE_OK : dberr2sqlite(ret);
}

/*
** Close a cursor.
*/
int sqlite3BtreeCloseCursor(BtCursor *pCur)
{
	Btree *p;
	BtShared *pBt;
	int rc;

	log_msg(LOG_VERBOSE, "sqlite3BtreeCloseCursor(%p)", pCur);

	if (!pCur || !pCur->pBtree)
		return SQLITE_OK;

	p = pCur->pBtree;
	pBt = p->pBt;

	rc = btreeCloseCursor(pCur, 1);

	return rc;
}

/* Move the cursor so that it points to an entry near pUnKey/nKey.
** Return a success code.
**
** For INTKEY tables, only the nKey parameter is used.  pUnKey is ignored. For
** other tables, nKey is the number of bytes of data in nKey. The comparison
** function specified when the cursor was created is used to compare keys.
**
** If an exact match is not found, then the cursor is always left pointing at
** a leaf page which would hold the entry if it were present. The cursor
** might point to an entry that comes before or after the key.
**
** The result of comparing the key with the entry to which the cursor is
** written to *pRes if pRes!=NULL.  The meaning of this value is as follows:
**
**     *pRes<0      The cursor is left pointing at an entry that is smaller
**                  than pUnKey or if the table is empty and the cursor is
**                  therefore left point to nothing.
**
**     *pRes==0     The cursor is left pointing at an entry that exactly
**                  matches pUnKey.
**
**     *pRes>0      The cursor is left pointing at an entry that is larger
**                  than pUnKey.
*/
int sqlite3BtreeMovetoUnpacked(
	BtCursor *pCur,
	UnpackedRecord *pUnKey,
	i64 nKey,
	int bias,
	int *pRes)
{
	int rc, res, ret;

	log_msg(LOG_VERBOSE, "sqlite3BtreeMovetoUnpacked(%p, %p, %u, %u, %p)",
	    pCur, pUnKey, (int)nKey, bias, pRes);

	res = -1;

	/* Invalidate current cursor state. */
	if (pDbc == NULL &&
	    (rc = btreeRestoreCursorPosition(pCur, 1)) != SQLITE_OK)
		return rc;

	if (pCur->eState == CURSOR_VALID &&
	    pIntKey && pCur->savedIntKey == nKey) {
		*pRes = 0;
		return SQLITE_OK;
	}

	pCur->multiGetPtr = pCur->multiPutPtr = NULL;
	memset(&pCur->key, 0, sizeof pCur->key);
	memset(&pCur->data, 0, sizeof pCur->data);

	if (pIntKey) {
		pCur->key.size = encodeI64(pCur->nKeyBuf, nKey);
		pCur->key.data = pCur->nKeyBuf;
	} else {
		assert(pUnKey != NULL);
		pCur->key.app_data = pUnKey;
	}

	ret = pDbc->get(pDbc, &pCur->key, &pCur->data,
	   DB_SET_RANGE | RMW(pCur));

	if (ret == DB_NOTFOUND)
		ret = pDbc->get(pDbc,
		    &pCur->key, &pCur->data, DB_LAST | RMW(pCur));

	if (ret == 0) {
		/* Check whether we got an exact match. */
		if (pIntKey) {
			pCur->savedIntKey =
			    decodeI64(pCur->key.data, pCur->key.size);
			res = (pCur->savedIntKey == nKey) ?
			    0 : (pCur->savedIntKey < nKey) ? -1 : 1;
		} else {
			DBT target;
			memset(&target, 0, sizeof target);
			target.app_data = pUnKey;
			/* paranoia */
			pCur->key.app_data = NULL;
			res = btreeCompare(pBDb, &pCur->key, &target);
		}
		pCur->eState = CURSOR_VALID;
	} else if (ret == DB_NOTFOUND) {
		/* The table is empty. */
		log_msg(LOG_VERBOSE, "sqlite3BtreeMoveto the table is empty.");
		ret = 0;
		pCur->eState = CURSOR_INVALID;
	} else {
		pCur->eState = CURSOR_FAULT;
		pCur->error = ret;
	}

	if (pRes != NULL)
		*pRes = res;
	pCur->skipMulti = 1;
	return (ret == 0) ? SQLITE_OK : dberr2sqlitelocked(ret);
}

int btreeMoveto(
	BtCursor *pCur,
	const void *pKey,
	i64 nKey,
	int bias,
	int *pRes)
{
	UnpackedRecord *p;
	char aSpace[150];
	int res;

	/*
	 * Cache an unpacked key in the DBT so we don't have to unpack
	 * it on every comparison.
	 */
	p = sqlite3VdbeRecordUnpack(pCur->keyInfo, (int)nKey, pKey, aSpace,
	    sizeof aSpace);

	res = sqlite3BtreeMovetoUnpacked(pCur, p, nKey, bias, pRes);

	sqlite3VdbeDeleteUnpackedRecord(p);
	pCur->key.app_data = NULL;

	return res;
}

static int btreeTripCursor(BtCursor *pCur, int incrBlobUpdate)
{
	DBC *dbc;
	int rc;
	void *keyCopy;

	/*
	 * This is protected by the BtShared mutex so that other threads won't
	 * attempt to access the cursor in btreeTripWatchers while we are
	 * closing it.
	 */
	assert(sqlite3_mutex_held(pCur->pBtree->pBt->mutex));

	dbc = pDbc;
	pDbc = NULL;

	/*
	 * Need to close here to so that the update happens unambiguously in
	 * the primary cursor.  That means the memory holding our copy of the
	 * key will be freed, so take a copy here.
	 */
	if (!pIntKey) {
		if ((keyCopy = sqlite3_malloc(pCur->key.size)) == NULL)
			return SQLITE_NOMEM;
		memcpy(keyCopy, pCur->key.data, pCur->key.size);
		pCur->key.data = keyCopy;
		pCur->key.flags |= DB_DBT_APPMALLOC;
	}

	pCur->eState = (pCur->isIncrblobHandle && !incrBlobUpdate) ?
	    CURSOR_INVALID : CURSOR_REQUIRESEEK;

	rc = dberr2sqlite(dbc->close(dbc));
	pCur->multiGetPtr = NULL;
	return rc;
}

static int btreeTripWatchers(BtCursor *pCur, int incrBlobUpdate)
{
	BtShared *pBt;
	BtCursor *pC;
	int cmp, rc;

	pBt = pCur->pBtree->pBt;
	rc = SQLITE_OK;

	sqlite3_mutex_enter(pBt->mutex);
	for (pC = pBt->first_cursor;
	    pC != NULL && rc == SQLITE_OK;
	    pC = pC->next) {
		if (pC == pCur || pCur->pBtree != pC->pBtree ||
		    pC->tableIndex != pCur->tableIndex ||
		    pC->eState != CURSOR_VALID)
			continue;
		if (pC->multiGetPtr == NULL &&
		    (pDbc->cmp(pDbc, pC->dbc, &cmp, 0) != 0 || cmp != 0))
			continue;

		rc = btreeTripCursor(pC, incrBlobUpdate);
	}
	sqlite3_mutex_leave(pBt->mutex);

	return rc;
}

static int btreeTripAll(Btree *p, int iTable, int incrBlobUpdate)
{
	BtShared *pBt;
	BtCursor *pC;
	int rc;

	pBt = p->pBt;
	rc = SQLITE_OK;

	assert(sqlite3_mutex_held(pBt->mutex));
	for (pC = pBt->first_cursor;
	    pC != NULL && rc == SQLITE_OK;
	    pC = pC->next) {
		if (pC->tableIndex != iTable || pC->eState != CURSOR_VALID)
			continue;
		if (pC->pBtree != p)
			return SQLITE_LOCKED_SHAREDCACHE;
		rc = btreeTripCursor(pC, incrBlobUpdate);
	}

	return rc;
}

static int btreeRestoreCursorPosition(BtCursor *pCur, int skipMoveto)
{
	Btree *p;
	BtShared *pBt;
	void *keyCopy;
	int rc, ret;

	if (pCur->eState == CURSOR_FAULT)
		return pCur->error;
	else if (pCur->pBtree == NULL ||
	    (pCur->eState == CURSOR_INVALID && !skipMoveto))
		return SQLITE_ABORT;

	p = pCur->pBtree;
	pBt = p->pBt;

	assert(pDbc == NULL);

	if (pIsBuffer) {
		rc = btreeLoadBufferIntoTable(pCur);
		if (rc != SQLITE_OK)
			return rc;
	} else {
		/*
		 * SQLite should guarantee that an appropriate transaction is
		 * active.
		 */
		assert(!pBt->transactional || pReadTxn != NULL);
		assert(!pBt->transactional || !pCur->wrFlag ||
		    pSavepointTxn != NULL);

		pCur->txn = pCur->wrFlag ? pSavepointTxn : pReadTxn;

		if ((ret = pBDb->cursor(pBDb, pCur->txn, &pDbc,
		    (pBt->transactional &&
		    (p->db->flags & SQLITE_ReadUncommitted)) ?
		    DB_READ_UNCOMMITTED : 0)) != 0)
			return dberr2sqlite(ret);
	}

	if (skipMoveto) {
		if ((pCur->key.flags & DB_DBT_APPMALLOC) != 0) {
			sqlite3_free(pCur->key.data);
			pCur->key.data = NULL;
			pCur->key.flags &= ~DB_DBT_APPMALLOC;
		}
		pCur->eState = CURSOR_INVALID;
		return SQLITE_OK;
	}

	if (pIntKey)
		return sqlite3BtreeMovetoUnpacked(pCur, NULL,
		    pCur->savedIntKey, 0, &pCur->lastRes);

	/*
	 * The pointer in pCur->key.data will be overwritten when we
	 * reposition, so we need to take a copy.
	 */
	assert((pCur->key.flags & DB_DBT_APPMALLOC) != 0);
	pCur->key.flags &= ~DB_DBT_APPMALLOC;
	keyCopy = pCur->key.data;
	rc = btreeMoveto(pCur, keyCopy, pCur->key.size,
	    0, &pCur->lastRes);
	sqlite3_free(keyCopy);
	return rc;
}

/*
 * Create a temporary table and load the contents of the multi buffer into it.
 */
static int btreeLoadBufferIntoTable(BtCursor *pCur)
{
	Btree *p;
	BtShared *pBt;
	int rc, ret;
	void *temp;
	sqlite3_mutex *mutexOpen;

	p = pCur->pBtree;
	pBt = p->pBt;
	ret = 0;

	temp = pCur->multiData.data;
	pCur->multiData.data = NULL;
	assert(pIsBuffer == 1);
	pIsBuffer = 0;
#ifndef BDBSQL_SINGLE_THREAD
	if (pCur->db != NULL) {
		sqlite3_free(pCur->db->app_private);
		pCur->db->app_private = NULL;
	}
#endif
	if ((rc = btreeCloseCursor(pCur, 1)) != SQLITE_OK)
		goto err;

	if (pBt->dbenv == NULL) {
		mutexOpen = sqlite3MutexAlloc(OPEN_MUTEX(pBt->dbStorage));
		sqlite3_mutex_enter(mutexOpen);
		rc = btreePrepareEnvironment(p);
		sqlite3_mutex_leave(mutexOpen);
		if (rc != SQLITE_OK)
			goto err;
	}
	rc = sqlite3BtreeCursor(p, MASTER_ROOT, 1, 0, pCur);
	if (pCur->eState == CURSOR_FAULT)
		rc = pCur->error;
	if (rc != SQLITE_OK)
		goto err;
	pCur->multiData.data = temp;
	temp = NULL;
	if (pCur->multiData.data != NULL) {
		if ((ret = pBDb->sort_multiple(pBDb, &pCur->multiData, NULL,
		    DB_MULTIPLE_KEY)) != 0)
			goto err;
		if ((ret = pBDb->put(pBDb, pCur->txn, &pCur->multiData, NULL,
		    DB_MULTIPLE_KEY)) != 0)
			goto err;
	}

err:	/*
	 * If we get to here and we haven't set up the newly-opened cursor
	 * properly, free the buffer it was holding now.  SQLite may not close
	 * the cursor explicitly, and it is no longer in the list of open
	 * cursors for the environment, so it will not be cleaned up on close.
	 */
	if (temp != NULL) {
		assert(rc != SQLITE_OK || ret != 0);
		sqlite3_free(temp);
	}
	return MAP_ERR(rc, ret);
}

/*
** Set *pSize to the size of the buffer needed to hold the value of the key
** for the current entry.  If the cursor is not pointing to a valid entry,
** *pSize is set to 0.
**
** For a table with the INTKEY flag set, this routine returns the key itself,
** not the number of bytes in the key.
*/
int sqlite3BtreeKeySize(BtCursor *pCur, i64 *pSize)
{
	int rc;

	log_msg(LOG_VERBOSE, "sqlite3BtreeKeySize(%p, %p)", pCur, pSize);

	if (pCur->eState != CURSOR_VALID &&
	    (rc = btreeRestoreCursorPosition(pCur, 0)) != SQLITE_OK)
		return rc;

	if (pIntKey)
		*pSize = pCur->savedIntKey;
	else
		*pSize = (pCur->eState == CURSOR_VALID) ? pCur->key.size : 0;

	return SQLITE_OK;
}

/*
** Set *pSize to the number of bytes of data in the entry the cursor currently
** points to.  Always return SQLITE_OK. Failure is not possible. If the cursor
** is not currently pointing to an entry (which can happen, for example, if
** the database is empty) then *pSize is set to 0.
*/
int sqlite3BtreeDataSize(BtCursor *pCur, u32 *pSize)
{
	int rc;

	log_msg(LOG_VERBOSE, "sqlite3BtreeDataSize(%p, %p)", pCur, pSize);

	if (pCur->eState != CURSOR_VALID &&
	    (rc = btreeRestoreCursorPosition(pCur, 0)) != SQLITE_OK)
		return rc;

	*pSize = (pCur->eState == CURSOR_VALID) ? pCur->data.size : 0;
	return SQLITE_OK;
}

/*
** Read part of the key associated with cursor pCur.  Exactly "amt" bytes will
** be transfered into pBuf[].  The transfer begins at "offset".
**
** Return SQLITE_OK on success or an error code if anything goes wrong. An
** error is returned if "offset+amt" is larger than the available payload.
*/
int sqlite3BtreeKey(BtCursor *pCur, u32 offset, u32 amt, void *pBuf)
{
	int rc;

	log_msg(LOG_VERBOSE, "sqlite3BtreeKey(%p, %u, %u, %p)",
	    pCur, offset, amt, pBuf);

	if (pCur->eState != CURSOR_VALID &&
	    (rc = btreeRestoreCursorPosition(pCur, 0)) != SQLITE_OK)
		return rc;

	assert(pCur->eState == CURSOR_VALID);
	memcpy(pBuf, (u_int8_t *)pCur->key.data + offset, amt);
	return SQLITE_OK;
}

/*
** Read part of the data associated with cursor pCur.  Exactly "amt" bytes
** will be transfered into pBuf[].  The transfer begins at "offset".
**
** Return SQLITE_OK on success or an error code if anything goes wrong. An
** error is returned if "offset+amt" is larger than the available payload.
*/
int sqlite3BtreeData(BtCursor *pCur, u32 offset, u32 amt, void *pBuf)
{
	int rc;

	log_msg(LOG_VERBOSE, "sqlite3BtreeData(%p, %u, %u, %p)",
	    pCur, offset, amt, pBuf);

	if (pCur->eState != CURSOR_VALID &&
	    (rc = btreeRestoreCursorPosition(pCur, 0)) != SQLITE_OK)
		return rc;

	assert(pCur->eState == CURSOR_VALID);
	memcpy(pBuf, (u_int8_t *)pCur->data.data + offset, amt);
	return SQLITE_OK;
}

/*
** For the entry that cursor pCur is point to, return as many bytes of the
** key or data as are available on the local b-tree page. Write the number
** of available bytes into *pAmt.
**
** The pointer returned is ephemeral.  The key/data may move or be destroyed
** on the next call to any Btree routine.
**
** These routines is used to get quick access to key and data in the common
** case where no overflow pages are used.
*/
const void *sqlite3BtreeKeyFetch(BtCursor *pCur, int *pAmt)
{
	log_msg(LOG_VERBOSE, "sqlite3BtreeKeyFetch(%p, %p)", pCur, pAmt);

	assert(pCur->eState == CURSOR_VALID);
	*pAmt = pCur->key.size;
	return pCur->key.data;
}

const void *sqlite3BtreeDataFetch(BtCursor *pCur, int *pAmt)
{
	log_msg(LOG_VERBOSE, "sqlite3BtreeDataFetch(%p, %p)", pCur, pAmt);

	assert(pCur->eState == CURSOR_VALID);
	*pAmt = pCur->data.size;
	return pCur->data.data;
}

/*
** Clear the current cursor position.
*/
void sqlite3BtreeClearCursor(BtCursor *pCur)
{
	log_msg(LOG_VERBOSE, "sqlite3BtreeClearCursor(%p)", pCur);

	pCur->eState = CURSOR_INVALID;
}

static int cursorGet(BtCursor *pCur, int op, int *pRes)
{
	static int numMultiGets, numBufferGets, numBufferSmalls;
	int ret;

	log_msg(LOG_VERBOSE, "cursorGet(%p, %u, %p)", pCur, op, pRes);

	if (op == DB_NEXT && pCur->multiGetPtr != NULL) {
		DB_MULTIPLE_KEY_NEXT(pCur->multiGetPtr, &pCur->multiData,
		    pCur->key.data, pCur->key.size, pCur->data.data,
		    pCur->data.size);
		if (pCur->multiGetPtr != NULL) {
			++numBufferGets;
			*pRes = 0;
			if (pIntKey)
				pCur->savedIntKey =
				    decodeI64(pCur->key.data, pCur->key.size);
			return SQLITE_OK;
		} else if (pIsBuffer) {
			*pRes = 1;
			return SQLITE_OK;
		}
	}

	if (pIsBuffer && op == DB_LAST) {
		DBT key, data;
		if (pCur->multiGetPtr == NULL) {
			*pRes = 1;
			return SQLITE_OK;
		}
		memset(&key, 0, sizeof key);
		memset(&data, 0, sizeof data);
		do {
			DB_MULTIPLE_KEY_NEXT(pCur->multiGetPtr, &pCur->multiData,
				key.data, key.size, data.data, data.size);
			if (pCur->multiGetPtr != NULL) {
				pCur->key.data = key.data;
				pCur->key.size = key.size;
				pCur->data.data = data.data;
				pCur->data.size = data.size;
				++numBufferGets;
			}
		} while (pCur->multiGetPtr != 0);
		*pRes = 0;
		if (pIntKey)
			pCur->savedIntKey =
				decodeI64(pCur->key.data, pCur->key.size);
		return SQLITE_OK;
	}

	if (op == DB_FIRST || (op == DB_NEXT && !pCur->skipMulti)) {
		++numMultiGets;

		if (pCur->multiData.data == NULL) {
			pCur->multiData.data = sqlite3_malloc(MULTI_BUFSIZE);
			if (pCur->multiData.data == NULL)
				return SQLITE_NOMEM;
			pCur->multiData.flags = DB_DBT_USERMEM;
			pCur->multiData.ulen = MULTI_BUFSIZE;
		}

		/*
		 * We can't keep DBC_TRANSIENT set on a bulk get
		 * cursor: if the buffer turns out to be too small, we
		 * have no way to restore the position.
		 */
		pDbc->flags &= ~DBC_TRANSIENT;
		ret = pDbc->get(pDbc, &pCur->key, &pCur->multiData,
		    op | DB_MULTIPLE_KEY);
		if (!pCur->wrFlag)
			pDbc->flags |= DBC_TRANSIENT;

		if (ret == 0) {
			DB_MULTIPLE_INIT(pCur->multiGetPtr, &pCur->multiData);
			DB_MULTIPLE_KEY_NEXT(pCur->multiGetPtr,
			    &pCur->multiData, pCur->key.data, pCur->key.size,
			    pCur->data.data, pCur->data.size);
			pCur->eState = CURSOR_VALID;
			*pRes = 0;
			if (pIntKey)
				pCur->savedIntKey =
				    decodeI64(pCur->key.data, pCur->key.size);
			return SQLITE_OK;
		} else if (ret == DB_BUFFER_SMALL) {
			++numBufferSmalls;
#if 0
			if (pCur->numBufferSmalls == MAX_SMALLS)
				fprintf(stderr,
				    "Skipping multi-gets, size == %d!\n",
				    pCur->multiData.size);
#endif
		} else
			goto err;
	} else if (op == DB_NEXT)
		pCur->skipMulti = 0;

	pCur->lastRes = 0;

	ret = pDbc->get(pDbc, &pCur->key, &pCur->data, op | RMW(pCur));
	if (ret == 0) {
		pCur->eState = CURSOR_VALID;
		*pRes = 0;
		if (pIntKey)
			pCur->savedIntKey =
			    decodeI64(pCur->key.data, pCur->key.size);
	} else {
err:		if (ret == DB_NOTFOUND)
			ret = 0;
		else
			log_msg(LOG_NORMAL, "cursorGet get returned error: %s",
			    db_strerror(ret));
		pCur->key.size = pCur->data.size = 0;
		pCur->eState = CURSOR_INVALID;
		*pRes = 1;
	}
	return (ret == 0) ? SQLITE_OK : dberr2sqlitelocked(ret);
}

/* Move the cursor to the first entry in the table.  Return SQLITE_OK on
** success.  Set *pRes to 0 if the cursor actually points to something or set
** *pRes to 1 if the table is empty.
*/
int sqlite3BtreeFirst(BtCursor *pCur, int *pRes)
{
	u_int32_t get_flag, rc;

	log_msg(LOG_VERBOSE, "sqlite3BtreeFirst(%p, %p)", pCur, pRes);

	get_flag = DB_FIRST;

	/*
	 * We might be lucky, and be holding all of a transient table in
	 * the bulk buffer.  If so, sort and retrieve...
	 */
	if (pCur->multiPutPtr != NULL) {
		if (pCur->eState == CURSOR_FAULT)
			return pCur->error;
		pBDb->sort_multiple(pBDb,
		    &pCur->multiData, NULL, DB_MULTIPLE_KEY);
		DB_MULTIPLE_INIT(pCur->multiGetPtr, &pCur->multiData);
		pCur->multiPutPtr = NULL;
		pCur->eState = CURSOR_VALID;
		get_flag = DB_NEXT;
	} else {
		pCur->multiGetPtr = NULL;

		if (pDbc == NULL &&
		    (rc = btreeRestoreCursorPosition(pCur, 1)) != SQLITE_OK)
			return rc;
	}

	return cursorGet(pCur, get_flag, pRes);
}

/*
** Move the cursor to the last entry in the table.  Return SQLITE_OK on
** success.  Set *pRes to 0 if the cursor actually points to something or set
** *pRes to 1 if the table is empty.
*/
int sqlite3BtreeLast(BtCursor *pCur, int *pRes)
{
	int rc;

	log_msg(LOG_VERBOSE, "sqlite3BtreeLast(%p, %p)", pCur, pRes);

	if (pCur->eState == CURSOR_FAULT)
		return pCur->error;

	if (pCur->pBtree != NULL && pIsBuffer) {
		if (pCur->multiPutPtr == NULL) {
			*pRes = 1;
			return SQLITE_OK;
		}

		pBDb->sort_multiple(pBDb,
			&pCur->multiData, NULL, DB_MULTIPLE_KEY);
		DB_MULTIPLE_INIT(pCur->multiGetPtr, &pCur->multiData);
		pCur->multiPutPtr = NULL;
		pCur->eState = CURSOR_VALID;
	} else {
		if (pDbc == NULL &&
		    (rc = btreeRestoreCursorPosition(pCur, 1)) != SQLITE_OK)
			return rc;

		pCur->multiGetPtr = NULL;
	}

	return cursorGet(pCur, DB_LAST, pRes);
}

/*
** Return TRUE if the cursor is not pointing at an entry of the table.
**
** TRUE will be returned after a call to sqlite3BtreeNext() moves past the last
** entry in the table or sqlite3BtreePrev() moves past the first entry. TRUE
** is also returned if the table is empty.
*/
int sqlite3BtreeEof(BtCursor *pCur)
{
	log_msg(LOG_VERBOSE, "sqlite3BtreeEof(%p)", pCur);

	return pCur->eState == CURSOR_INVALID;
}

/*
** Advance the cursor to the next entry in the database.  If successful then
** set *pRes=0.  If the cursor was already pointing to the last entry in the
** database before this routine was called, then set *pRes=1.
*/
int sqlite3BtreeNext(BtCursor *pCur, int *pRes)
{
	int rc;
	log_msg(LOG_VERBOSE, "sqlite3BtreeNext(%p, %p)", pCur, pRes);

	if (pDbc != NULL && pCur->eState == CURSOR_INVALID) {
		*pRes = 1;
		return SQLITE_OK;
	}

	if (pDbc == NULL &&
	    (rc = btreeRestoreCursorPosition(pCur, 0)) != SQLITE_OK)
		return rc;

	if (pCur->lastRes > 0) {
		pCur->lastRes = 0;
		*pRes = 0;
		return SQLITE_OK;
	}

	return cursorGet(pCur, DB_NEXT, pRes);
}

/*
** Step the cursor to the back to the previous entry in the database.  If
** successful then set *pRes=0.  If the cursor was already pointing to the
** first entry in the database before this routine was called, then set *pRes=1.
*/
int sqlite3BtreePrevious(BtCursor *pCur, int *pRes)
{
	int rc;
	log_msg(LOG_VERBOSE, "sqlite3BtreePrevious(%p, %p)", pCur, pRes);

	if (pDbc != NULL && pCur->eState == CURSOR_INVALID) {
		*pRes = 1;
		return SQLITE_OK;
	}

	if (pDbc == NULL &&
	    (rc = btreeRestoreCursorPosition(pCur, 0)) != SQLITE_OK)
		return rc;

	if (pCur->lastRes < 0) {
		pCur->lastRes = 0;
		*pRes = 0;
		return SQLITE_OK;
	}

	return cursorGet(pCur, DB_PREV, pRes);
}

static int insertData(BtCursor *pCur, int nZero, int nData)
{
	int ret;

	ret = pDbc->put(pDbc, &pCur->key, &pCur->data, DB_KEYLAST);

	if (ret == 0 && nZero > 0) {
		DBT zeroData;
		u8 zero;

		zero = 0;
		memset(&zeroData, 0, sizeof zeroData);
		zeroData.data = &zero;
		zeroData.size = zeroData.dlen = zeroData.ulen = 1;
		zeroData.doff = nData + nZero - 1;
		zeroData.flags = DB_DBT_PARTIAL | DB_DBT_USERMEM;

		ret = pDbc->put(pDbc, &pCur->key, &zeroData, DB_CURRENT);
	}
	return ret;
}

/*
** Insert a new record into the BTree.  The key is given by (pKey,nKey) and
** the data is given by (pData,nData).  The cursor is used only to define
** what table the record should be inserted into.  The cursor is left
** pointing at a random location.
**
** For an INTKEY table, only the nKey value of the key is used.  pKey is
** ignored.  For a ZERODATA table, the pData and nData are both ignored.
*/
int sqlite3BtreeInsert(
	BtCursor *pCur,		/* Insert data into the table of this cursor */
	const void *pKey, i64 nKey,	/* The key of the new record */
	const void *pData, int nData,	/* The data of the new record */
	int nZero,			/* Number of extra 0 bytes */
	int appendBias,			/* True if this likely an append */
	int seekResult)		/* Result of prior sqlite3BtreeMoveto() call */
{
	int rc, ret;
	u_int8_t encKey[INTKEY_BUFSIZE];
	UnpackedRecord *p;
	char aSpace[150];

	log_msg(LOG_VERBOSE,
	    "sqlite3BtreeInsert(%p, %p, %u, %p, %u, %u, %u, %u)",
	    pCur, pKey, (int)nKey, pData, nData, nZero, appendBias, seekResult);

	if (!pCur->wrFlag)
		return SQLITE_READONLY;

	p = NULL;
	rc = SQLITE_OK;

	/* Invalidate current cursor state. */
	pCur->multiGetPtr = NULL;
	memset(&pCur->key, 0, sizeof pCur->key);
	memset(&pCur->data, 0, sizeof pCur->data);

	if (pIntKey) {
		pCur->key.size = encodeI64(encKey, nKey);
		pCur->key.data = encKey;
	} else {
		pCur->key.data = (void *)pKey;
		pCur->key.size = (u_int32_t)nKey;
	}
	pCur->data.data = (void *)pData;
	pCur->data.size = nData;

	if (pIsBuffer) {
		ret = 0;
		if (nZero == 0) {
			if (pCur->multiData.data == NULL) {
				if ((pCur->multiData.data =
				    sqlite3_malloc(MULTI_BUFSIZE)) == NULL) {
					ret = ENOMEM;
					goto err;
				}
				pCur->multiData.flags = DB_DBT_USERMEM;
				pCur->multiData.ulen = MULTI_BUFSIZE;
				DB_MULTIPLE_WRITE_INIT(pCur->multiPutPtr,
					&pCur->multiData);
			}
			/*
			 * It is possible for temporary results to be written,
			 * read, then written again.  In that case just load
			 * the results into a table.
			 */
			if (pCur->multiPutPtr != NULL) {
				DB_MULTIPLE_KEY_WRITE_NEXT(pCur->multiPutPtr,
				    &pCur->multiData,
				    pCur->key.data, pCur->key.size,
				    pCur->data.data, pCur->data.size);
			}
		} else
			pCur->multiPutPtr = NULL;
		if (pCur->multiPutPtr == NULL) {
			rc = btreeLoadBufferIntoTable(pCur);
			if (rc != SQLITE_OK)
				return rc;
			ret = insertData(pCur, nZero, nData);
		}
		goto err;
	}
	if (!pIntKey && pKey != NULL) {
		/*
		 * Cache an unpacked key in the DBT so we don't have to unpack
		 * it on every comparison.
		 */
		pCur->key.app_data = p = sqlite3VdbeRecordUnpack(pCur->keyInfo,
			(int)nKey, pKey, aSpace, sizeof aSpace);
	}

	ret = insertData(pCur, nZero, nData);

	if (ret == 0) {
		/*
		 * We may have updated a record or inserted into a range that
		 * is cached by another cursor.
		 */
		if ((rc = btreeTripWatchers(pCur, 0)) != SQLITE_OK)
			goto err;
		pCur->skipMulti = 0;
	} else
		pCur->eState = CURSOR_INVALID;
err:	if (p != NULL)
		sqlite3VdbeDeleteUnpackedRecord(p);
	pCur->key.app_data = NULL;
	return MAP_ERR_LOCKED(rc, ret);
}

/*
** Delete the entry that the cursor is pointing to.  The cursor is left
** pointing at a random location.
*/
int sqlite3BtreeDelete(BtCursor *pCur)
{
	int rc, ret;

	log_msg(LOG_VERBOSE, "sqlite3BtreeDelete(%p)", pCur);

	ret = 0;
	if (!pCur->wrFlag)
		return SQLITE_READONLY;

	if (pIsBuffer) {
		int res;
		rc = btreeMoveto(pCur, pCur->key.data, pCur->key.size,
			0, &res);
		if (rc != SQLITE_OK)
			return rc;
	}

	if (pCur->multiGetPtr != NULL || pIsBuffer) {
		DBT dummy;
		pCur->multiGetPtr = NULL;
		memset(&dummy, 0, sizeof dummy);
		dummy.flags = DB_DBT_USERMEM | DB_DBT_PARTIAL;
		if ((ret = pDbc->get(pDbc,
		    &pCur->key, &dummy, DB_SET | RMW(pCur))) != 0)
			return dberr2sqlitelocked(ret);
		pCur->eState = CURSOR_VALID;
	}

	if ((rc = btreeTripWatchers(pCur, 0)) != SQLITE_OK)
		return rc;
	ret = pDbc->del(pDbc, 0);
	pCur->eState = CURSOR_INVALID;

	return (ret == 0) ? SQLITE_OK : dberr2sqlitelocked(ret);
}

/*
** Create a new BTree table.  Write into *piTable the page number for the root
** page of the new table.
**
** The type of type is determined by the flags parameter.  Only the following
** values of flags are currently in use.  Other values for flags might not
** work:
**
**     BTREE_INTKEY|BTREE_LEAFDATA     Used for SQL tables with rowid keys
**     BTREE_ZERODATA                  Used for SQL indices
*/
static int btreeCreateTable(Btree *p, int *piTable, int flags)
{
	BtShared *pBt;
	CACHED_DB *cached_db;
	DBC *dbc;
	DBT key, data;
	int iTable, lastTable, rc, ret, t_ret;

	cached_db = NULL;
	pBt = p->pBt;
	rc = SQLITE_OK;
	lastTable = 0;
	ret = 0;

	dbc = NULL;
	if (pBt->dbStorage == DB_STORE_NAMED) {
		ret = pTablesDb->cursor(pTablesDb, pFamilyTxn, &dbc, 0);
		if (ret != 0)
			goto err;

		memset(&key, 0, sizeof key);
		memset(&data, 0, sizeof data);
		data.flags = DB_DBT_PARTIAL | DB_DBT_USERMEM;

		if ((ret = dbc->get(dbc, &key, &data, DB_LAST)) != 0)
			goto err;

		if (strncmp((const char *)key.data, "table", 5) == 0 &&
		    (ret = btreeTableNameToId(
		    (const char *)key.data, key.size, &lastTable)) != 0)
			goto err;

		ret = dbc->close(dbc);
		dbc = NULL;
		if (ret != 0)
			goto err;
	}

	sqlite3_mutex_enter(pBt->mutex);

	if (pBt->dbStorage != DB_STORE_NAMED)
		lastTable = pBt->last_table;

	iTable = lastTable + 1;

	/* Make sure (iTable & 1) iff BTREE_INTKEY is set */
	if ((flags & BTREE_INTKEY) != 0) {
		if ((iTable & 1) == 0)
			iTable += 1;
	} else if ((iTable & 1) == 1)
		iTable += 1;

	rc = btreeCreateDataTable(p, iTable, &cached_db);

	if (rc == SQLITE_OK) {
		pBt->last_table = iTable;
		*piTable = iTable;
	}

	sqlite3_mutex_leave(pBt->mutex);

err:	if (dbc != NULL)
		if ((t_ret = dbc->close(dbc)) != 0 && ret == 0)
			ret = t_ret;

	return MAP_ERR(rc, ret);
}

int sqlite3BtreeCreateTable(Btree *p, int *piTable, int flags)
{
	BtShared *pBt;
	int rc;
	sqlite3_mutex *mutexOpen;

	log_msg(LOG_VERBOSE, "sqlite3BtreeCreateTable(%p, %p, %u)",
	    p, piTable, flags);

	pBt = p->pBt;

	/*
	 * Temporary indexes must be in tables, since there is no
	 * current way to remove duplicates from the buffer.
	 */
	if ((flags & BTREE_ZERODATA) != 0) {
		pBt->resultsBuffer = 0;
		if (pBt->dbenv == NULL) {
			mutexOpen =
			    sqlite3MutexAlloc(OPEN_MUTEX(pBt->dbStorage));
			sqlite3_mutex_enter(mutexOpen);
			rc = btreePrepareEnvironment(p);
			sqlite3_mutex_leave(mutexOpen);
			if (rc != SQLITE_OK)
				return rc;
		}
	}

	/*
	 * With ephemeral tables, there are at most two tables created: the
	 * initial master table, which is used for INTKEY tables, or, for
	 * indices, a second table is opened and the master table is unused.
	 */
	if (pBt->resultsBuffer) {
		*piTable = 2;
		return SQLITE_OK;
	}

	if (!p->connected &&
	    (rc = btreeOpenEnvironment(p, 1)) != SQLITE_OK)
		return rc;

	return btreeCreateTable(p, piTable, flags);
}

/*
** Delete all information from a single table in the database.  iTable is the
** page number of the root of the table.  After this routine returns, the root
** page is empty, but still exists.
**
** This routine will fail with SQLITE_LOCKED if there are any open read
** cursors on the table.  Open write cursors are moved to the root of the
** table.
**
** If pnChange is not NULL, then table iTable must be an intkey table. The
** integer value pointed to by pnChange is incremented by the number of
** entries in the table.
*/
int sqlite3BtreeClearTable(Btree *p, int iTable, int *pnChange)
{
	BtShared *pBt;
	char cached_db_key[CACHE_KEY_SIZE];
	CACHED_DB *cached_db;
	DB *dbp;
#ifdef BDBSQL_SEMITXN_TRUNCATE
	DELETED_TABLE *dtable;
	char *tableName, tableNameBuf[DBNAME_SIZE];
	char *oldTableName, oldTableNameBuf[DBNAME_SIZE];
#endif
	int rc, ret;
	u_int32_t count;

	log_msg(LOG_VERBOSE, "sqlite3BtreeClearTable(%p, %u, %p)",
	    p, iTable, pnChange);

	pBt = p->pBt;
	count = 0;
	ret = 0;
	if (pBt->db_oflags & DB_RDONLY)
		return SQLITE_READONLY;

	/* Close any open cursors. */
	sqlite3_mutex_enter(pBt->mutex);

	/*
	 * SQLite expects all cursors apart from read-uncommitted cursors to be
	 * closed.  However, Berkeley DB cannot truncate unless *all* cursors
	 * are closed.  This call to btreeTripAll will fail if there are any
	 * cursors open on other connections with * SQLITE_LOCKED_SHAREDCACHE,
	 * which makes tests shared2-1.[23] fail with "table locked" errors.
	 */
	if ((rc = btreeTripAll(p, iTable, 0)) != SQLITE_OK) {
		sqlite3_mutex_leave(pBt->mutex);
		return rc;
	}

	sqlite3_snprintf(sizeof cached_db_key, cached_db_key, "%x", iTable);
	cached_db = sqlite3HashFind(&pBt->db_cache,
	    cached_db_key, (int)strlen(cached_db_key));

	if ((cached_db == NULL || cached_db->dbp == NULL) &&
	    (rc = btreeCreateDataTable(p, iTable, &cached_db)) != SQLITE_OK) {
		sqlite3_mutex_leave(pBt->mutex);
		return rc;
	}
	sqlite3_mutex_leave(pBt->mutex);

	assert(cached_db != NULL && cached_db->dbp != NULL);
	dbp = cached_db->dbp;

#ifdef BDBSQL_SEMITXN_TRUNCATE
	/*
	 * The motivation here is that logging all of the contents of pages
	 * we want to clear is slow.  Instead, we can transactionally create
	 * a new, empty table, and rename the old one.  If this transaction
	 * goes on to commit, we can non-transactionally free the old pages
	 * at that point.
	 *
	 * Steps are:
	 *   1. do a transactional rename of the old table
	 *   2. do a transactional create of a new table with the same name
	 *   3. if/when this transaction commits, do a non-transactional
	 *      remove of the old table.
	 */
	if (pBt->transactional) {
		/* TODO: do we need to count the records? */

#ifndef BDBSQL_SINGLE_THREAD
		if (dbp->app_private != NULL)
			sqlite3_free(dbp->app_private);
#endif
		if ((ret = dbp->close(dbp, DB_NOSYNC)) != 0)
			return dberr2sqlitelocked(ret);
		cached_db->dbp = dbp = NULL;

		tableName = tableNameBuf;
		GET_TABLENAME(tableName, sizeof tableNameBuf, iTable, "");
		oldTableName = oldTableNameBuf;
		GET_TABLENAME(oldTableName, sizeof oldTableNameBuf,
			iTable, "-old");

		ret = pDbEnv->dbrename(pDbEnv, pSavepointTxn,
		    pBt->short_name, tableName, oldTableName, DB_NOSYNC);
		if (ret != 0)
			return dberr2sqlitelocked(ret);

		/*
		 * It's important that the new handle be opened in the update
		 * transaction: it should not be used outside this txn until
		 * commit.
		 */
		ret = btreeCreateDataTable(p, iTable, &cached_db);
		if (ret != 0)
			return dberr2sqlitelocked(ret);
		dbp = cached_db->dbp;

		dtable = (DELETED_TABLE *)sqlite3_malloc(sizeof(DELETED_TABLE));
		if (dtable == NULL)
			return SQLITE_NOMEM;
		dtable->iTable = iTable;
		dtable->txn = pSavepointTxn;
		dtable->next = p->deleted_tables;
		p->deleted_tables = dtable;
	} else
#else
	ret = dbp->truncate(dbp, pSavepointTxn, &count, 0);
#endif

	if (ret == 0 && pnChange != NULL)
		*pnChange += count;
	return (ret == 0) ? SQLITE_OK : dberr2sqlitelocked(ret);
}

/*
** Erase all information in a table and add the root of the table to the
** freelist.  Except, the root of the principle table (the one on page 1) is
** never added to the freelist.
**
** This routine will fail with SQLITE_LOCKED if there are any open cursors on
** the table.
*/
int sqlite3BtreeDropTable(Btree *p, int iTable, int *piMoved)
{
	char cached_db_key[CACHE_KEY_SIZE];
	BtShared *pBt;
	CACHED_DB *cached_db;
	DB *dbp;
	int ret, t_ret;
	char *tableName, tableNameBuf[DBNAME_SIZE];

	log_msg(LOG_VERBOSE, "sqlite3BtreeDropTable(%p, %u, %p)",
	    p, iTable, piMoved);

	pBt = p->pBt;
	*piMoved = 0;
	ret = 0;

	/* Close any cached handle */
	sqlite3_snprintf(sizeof cached_db_key, cached_db_key, "%x", iTable);
	sqlite3_mutex_enter(pBt->mutex);
	cached_db = sqlite3HashFind(&pBt->db_cache,
	    cached_db_key, (int)strlen(cached_db_key));
	if (cached_db != NULL && (dbp = cached_db->dbp) != NULL) {
#ifndef BDBSQL_SINGLE_THREAD
		if (dbp->app_private != NULL)
			sqlite3_free(dbp->app_private);
#endif
		ret = dbp->close(dbp, DB_NOSYNC);
		cached_db->dbp = NULL;
	}
	sqlite3HashInsert(
	    &pBt->db_cache, cached_db_key, (int)strlen(cached_db_key), NULL);
	sqlite3_mutex_leave(pBt->mutex);
	sqlite3_free(cached_db);

	if (pBt->dbStorage == DB_STORE_NAMED) {
		tableName = tableNameBuf;
		GET_TABLENAME(tableName, sizeof tableNameBuf, iTable, "");
		t_ret = pDbEnv->dbremove(pDbEnv, pSavepointTxn,
		    pBt->short_name, tableName, DB_NOSYNC);
		if (t_ret == ENOENT || t_ret == DB_NOTFOUND)
			t_ret = 0;
		if (ret == 0 && t_ret != 0)
			ret = t_ret;
	}

	return dberr2sqlitelocked(ret);
}

/*
** Read the meta-information out of a database file.  Meta[0] is the number
** of free pages currently in the database.  Meta[1] through meta[15] are
** available for use by higher layers.  Meta[0] is read-only, the others are
** read/write.
**
** The schema layer numbers meta values differently.  At the schema layer (and
** the SetCookie and ReadCookie opcodes) the number of free pages is not
** visible.  So Cookie[0] is the same as Meta[1].
*/
void sqlite3BtreeGetMeta(Btree *p, int idx, u32 *pMeta)
{
	BtShared *pBt;
	int ret;
	DBT key, data;
	u_int8_t metaKey[INTKEY_BUFSIZE], metaData[INTKEY_BUFSIZE];

	log_msg(LOG_VERBOSE, "sqlite3BtreeGetMeta(%p, %u, %p)",
	    p, idx, pMeta);

	pBt = p->pBt;
	assert(idx >= 0 && idx < NUMMETA);

	/* Once connected to a shared environment, don't trust the cache. */
	if (idx > 0 && idx < NUMMETA && pBt->meta[idx].cached
	    && (!p->connected || pBt->dbStorage != DB_STORE_NAMED)) {
		*pMeta = pBt->meta[idx].value;
		return;
	} else if (idx == 0 || !p->connected ||
	    pBt->dbStorage != DB_STORE_NAMED) {
		*pMeta = 0;
		return;
	}

	assert(p->pBt->dbStorage == DB_STORE_NAMED);

	memset(&key, 0, sizeof key);
	key.data = metaKey;
	key.size = key.ulen = encodeI64(metaKey, idx);
	key.flags = DB_DBT_USERMEM;
	memset(&data, 0, sizeof data);
	data.data = metaData;
	data.size = data.ulen = sizeof metaData;
	data.flags = DB_DBT_USERMEM;

	if ((ret = pMetaDb->get(pMetaDb, pFamilyTxn, &key, &data, 0)) == 0) {
		*pMeta = (u32)decodeI64(data.data, data.size);
		if (idx < NUMMETA) {
			pBt->meta[idx].value = *pMeta;
			pBt->meta[idx].cached = 1;
		}
	} else if (ret == DB_NOTFOUND || ret == DB_KEYEMPTY) {
		*pMeta = 0;
		ret = 0;
	}

	assert(ret == 0);
}

/*
** Write meta-information back into the database.  Meta[0] is read-only and
** may not be written.
*/
int sqlite3BtreeUpdateMeta(Btree *p, int idx, u32 iMeta)
{
	BtShared *pBt;
	int ret;
	DBT key, data;
	u_int8_t metaKey[INTKEY_BUFSIZE], metaData[INTKEY_BUFSIZE];

	log_msg(LOG_VERBOSE, "sqlite3BtreeUpdateMeta(%p, %u, %u)",
	    p, idx, iMeta);

	pBt = p->pBt;
	if (pBt->db_oflags & DB_RDONLY)
		return SQLITE_READONLY;

	assert(idx > 0 && idx < NUMMETA);

	if (idx < NUMMETA) {
		pBt->meta[idx].value = iMeta;
		pBt->meta[idx].cached = 1;
	}

	/* Skip the database update for private environments. */
	if (!p->connected || pBt->dbStorage != DB_STORE_NAMED)
		return SQLITE_OK;

	memset(&key, 0, sizeof key);
	key.data = metaKey;
	key.size = key.ulen = encodeI64(metaKey, idx);
	key.flags = DB_DBT_USERMEM;
	memset(&data, 0, sizeof data);
	data.data = metaData;
	data.size = data.ulen = encodeI64(metaData, iMeta);
	data.flags = DB_DBT_USERMEM;

	ret = pMetaDb->put(pMetaDb, pSavepointTxn, &key, &data, 0);

	return (ret == 0) ? SQLITE_OK : dberr2sqlite(ret);
}

#ifndef SQLITE_OMIT_BTREECOUNT
/*
** The first argument, pCur, is a cursor opened on some b-tree. Count the
** number of entries in the b-tree and write the result to *pnEntry.
**
** SQLITE_OK is returned if the operation is successfully executed.
** Otherwise, if an error is encountered (i.e. an IO error or database
** corruption) an SQLite error code is returned.
*/
int sqlite3BtreeCount(BtCursor *pCur, i64 *pnEntry)
{
	Btree *p;
	DB_BTREE_STAT *stat;
	int ret;

	p = pCur->pBtree;

	if ((ret =
	    pBDb->stat(pBDb, pFamilyTxn, &stat, DB_READ_COMMITTED)) == 0) {
		*pnEntry = stat->bt_ndata;
		sqlite3_free(stat);
	}

	return (ret == 0) ? SQLITE_OK : dberr2sqlite(ret);
}
#endif

/*
** This routine does a complete check of the given BTree file.  aRoot[] is
** an array of pages numbers were each page number is the root page of a table.
** nRoot is the number of entries in aRoot.
**
** If everything checks out, this routine returns NULL.  If something is amiss,
** an error message is written into memory obtained from malloc() and a
** pointer to that error message is returned.  The calling function is
** responsible for freeing the error message when it is done.
*/
char *sqlite3BtreeIntegrityCheck(
	Btree *pBt,	/* The btree to be checked */
	int *aRoot,	/* An array of root page numbers for individual trees */
	int nRoot,	/* Number of entries in aRoot[] */
	int mxErr,	/* Stop reporting errors after this many */
	int *pnErr)	/* Write number of errors seen to this variable */
{
	int ret;

	log_msg(LOG_VERBOSE, "sqlite3BtreeIntegrityCheck(%p, %p, %u, %u, %p)",
	    pBt, aRoot, nRoot, mxErr, pnErr);

	ret = 0;
	*pnErr = 0;
#if 0
	DB *db;
	int i;
	char *tableName, tableNameBuf[DBNAME_SIZE];
	/*
	 * XXX: Have to do this outside the environment, verify doesn't play
	 * nice with locking.
	 */
	for (i = 0; i < nRoot && ret == 0; i++) {
		tableName = tableNameBuf;
		GET_TABLENAME(tableName, sizeof tableNameBuf, aRoot[i], "");
		if ((ret = db_create(&db, pDbEnv, 0)) == 0)
			ret = db->verify(db, tableName,
			    NULL, NULL, DB_NOORDERCHK);
	}

#endif
	return (ret == 0) ? NULL : sqlite3_strdup(db_strerror(ret));
}

/*
** Return the full pathname of the underlying database file.
*/
const char *sqlite3BtreeGetFilename(Btree *p)
{
	log_msg(LOG_VERBOSE, "sqlite3BtreeGetFilename(%p) (%s)",
	    p, p->pBt->full_name);

	return (p->pBt->full_name != NULL) ? p->pBt->full_name : "";
}

/*
** Return non-zero if a transaction is active.
*/
int sqlite3BtreeIsInTrans(Btree *p)
{
	return (p && p->inTrans == TRANS_WRITE);
}

/*
 * Determine whether or not a cursor has moved from the position it was last
 * placed at.
 */
int sqlite3BtreeCursorHasMoved(BtCursor *pCur, int *pHasMoved)
{
	int rc;

	/* Set this here in case of error. */
	*pHasMoved = 1;

	/*
	 * We only want to return an error if the cursor is faulted, not just
	 * if it is not pointing at anything.
	 */
	if (pCur->eState != CURSOR_VALID && pCur->eState != CURSOR_INVALID &&
	    (rc = btreeRestoreCursorPosition(pCur, 0)) != SQLITE_OK)
		return rc;

	if (pCur->eState == CURSOR_VALID && pCur->lastRes == 0)
		*pHasMoved = 0;
	return SQLITE_OK;
}

#ifndef NDEBUG
/*
** Return true if the given BtCursor is valid.  A valid cursor is one that is
** currently pointing to a row in a (non-empty) table.
**
** This is a verification routine, it is used only within assert() statements.
*/
int sqlite3BtreeCursorIsValid(BtCursor *pCur)
{
	return (pCur != NULL && pCur->eState == CURSOR_VALID);
}
#endif /* NDEBUG */

/*****************************************************************
** Argument pCsr must be a cursor opened for writing on an INTKEY table
** currently pointing at a valid table entry. This function modifies the
** data stored as part of that entry. Only the data content may be modified,
** it is not possible to change the length of the data stored.
*/
int sqlite3BtreePutData(BtCursor *pCur, u32 offset, u32 amt, void *z)
{
	DBT pdata;
	int rc, ret;
	log_msg(LOG_VERBOSE, "sqlite3BtreePutData(%p, %u, %u, %p)",
	    pCur, offset, amt, z);

	/*
	 * Check that the cursor is open for writing and the cursor points at a
	 * valid row of an intKey table.
	 */
	if (!pCur->wrFlag)
		return SQLITE_READONLY;

	if (pDbc == NULL &&
	    (rc = btreeRestoreCursorPosition(pCur, 0)) != SQLITE_OK)
		return rc;

	if (pCur->eState != CURSOR_VALID)
		return SQLITE_ABORT;

	assert(!pCur->multiGetPtr);

#ifndef SQLITE_OMIT_INCRBLOB
	assert(pCur);
	assert(pDbc);

	rc = SQLITE_OK;
	memcpy((u_int8_t *)pCur->data.data + offset, z, amt);

	memset(&pdata, 0, sizeof(DBT));
	pdata.data = (void *)z;
	pdata.size = pdata.dlen = amt;
	pdata.doff = offset;
	pdata.flags |= DB_DBT_PARTIAL;

	if ((rc = btreeTripWatchers(pCur, 1)) != SQLITE_OK)
		return rc;

	ret = pDbc->put(pDbc, &pCur->key, &pdata, DB_CURRENT);
	if (ret != 0)
		rc = dberr2sqlitelocked(ret);
#endif
	return rc;
}

/*****************************************************************
** Set a flag on this cursor to indicate that it is an incremental blob
** cursor.  Incrblob cursors are invalidated differently to ordinary cursors:
** if the value under an incrblob cursor is modified, attempts to access
** the cursor again will result in an error.
*/
void sqlite3BtreeCacheOverflow(BtCursor *pCur)
{
	log_msg(LOG_VERBOSE, "sqlite3BtreeCacheOverflow(%p)", pCur);
	pCur->isIncrblobHandle = 1;
}

/*****************************************************************
** Return non-zero if a read (or write) transaction is active.
*/
int sqlite3BtreeIsInReadTrans(Btree *p)
{
	log_msg(LOG_VERBOSE, "sqlite3BtreeIsInReadTrans(%p)", p);
	return (p && p->inTrans != TRANS_NONE);
}

/***************************************************************************
** This routine sets the state to CURSOR_FAULT and the error code to errCode
** for every cursor on BtShared that pengine references.
**
** Every cursor is tripped, including cursors that belong to other databases
** connections that happen to be sharing the cache with pengine.
**
** This routine gets called when a rollback occurs. All cursors using the same
** cache must be tripped to prevent them from trying to use the engine after
** the rollback.  The rollback may have deleted tables or moved root pages, so
** it is not sufficient to save the state of the cursor. The cursor must be
** invalidated.
*/
void sqlite3BtreeTripAllCursors(Btree*	p, int errCode)
{
	BtShared *pBt;
	BtCursor *pCur;

	log_msg(LOG_VERBOSE, "sqlite3BtreeTripAllCursors(%p, %u)", p, errCode);

	pBt = p->pBt;

	sqlite3_mutex_enter(pBt->mutex);
	for (pCur = pBt->first_cursor; pCur != NULL; pCur = pCur->next) {
		pCur->eState = CURSOR_FAULT;
		pCur->error = errCode;
	}
	sqlite3_mutex_leave(pBt->mutex);
}

static int btreeLockSchema(Btree *p, lock_mode_t lockMode)
{
	BtCursor *pCur, tmpCursor;
	BtShared *pBt;
	DBC *oldCur;
	int opened, rc, res, ret;

	pBt = p->pBt;
	pCur = &tmpCursor;
	oldCur = NULL;
	opened = 0;
	rc = SQLITE_OK;

	if (!p->connected) {
		if (lockMode == LOCKMODE_NONE || lockMode > p->schemaLockMode)
			p->schemaLockMode = lockMode;
		return SQLITE_OK;
	}

	if (lockMode == LOCKMODE_NONE)
		goto done;

	sqlite3BtreeCursorZero(pCur);
	rc = sqlite3BtreeCursor(p, MASTER_ROOT,
	    lockMode == LOCKMODE_WRITE, NULL, pCur);
	opened = (rc == SQLITE_OK);
	if (pCur->eState == CURSOR_FAULT)
		rc = pCur->error;

	/*
	 * Any repeatable operation would do: we get the last item just because
	 * it doesn't try to do a bulk get.
	 */
	if (rc == SQLITE_OK)
		rc = sqlite3BtreeLast(pCur, &res);

done:	if (p->schemaLock != NULL) {
		if ((ret = p->schemaLock->close(p->schemaLock)) != 0 &&
		    rc == SQLITE_OK)
			rc = dberr2sqlite(ret);
		p->schemaLock = NULL;
	}

	if (opened && rc == SQLITE_OK) {
		p->schemaLockMode = lockMode;
		p->schemaLock = pDbc;
		pDbc = NULL;
	} else
		p->schemaLockMode = LOCKMODE_NONE;
	if (opened)
		(void)sqlite3BtreeCloseCursor(pCur);

	return rc;
}

/*****************************************************************
** Obtain a lock on the table whose root page is iTab.  The lock is a write
** lock if isWritelock is true or a read lock if it is false.
*/
int sqlite3BtreeLockTable(Btree *p, int iTable, u8 isWriteLock)
{
	lock_mode_t lockMode;
	int rc;

	log_msg(LOG_VERBOSE, "sqlite3BtreeLockTable(%p, %u, %u)",
	    p, iTable, isWriteLock);

	lockMode = isWriteLock ? LOCKMODE_WRITE : LOCKMODE_READ;

	if (iTable != MASTER_ROOT || !p->pBt->transactional ||
	    p->schemaLockMode >= lockMode)
		return SQLITE_OK;

	rc = btreeLockSchema(p, lockMode);

	if (!p->connected && rc != SQLITE_NOMEM) {
		p->schemaLockMode = lockMode;
		return SQLITE_OK;
	}

	if (rc == SQLITE_BUSY)
		rc = SQLITE_LOCKED;

	return rc;
}

/*****************************************************************
** Return true if another user of the same shared engine as the argument
** handle holds an exclusive lock on the sqlite_master table.
*/
int sqlite3BtreeSchemaLocked(Btree *p)
{
	BtCursor *pCur;
	BtShared *pBt;

	log_msg(LOG_VERBOSE, "sqlite3BtreeSchemaLocked(%p)", p);

	pBt = p->pBt;

	if (p->sharable) {
		sqlite3_mutex_enter(pBt->mutex);
		for (pCur = pBt->first_cursor;
		    pCur != NULL;
		    pCur = pCur->next) {
			if (pCur->pBtree != p && pCur->pBtree->connected &&
			    pCur->pBtree->schemaLockMode == LOCKMODE_WRITE) {
				sqlite3_mutex_leave(pBt->mutex);
				return SQLITE_LOCKED_SHAREDCACHE;
			}
		}
		sqlite3_mutex_leave(pBt->mutex);
	}

	return SQLITE_OK;
}

/*****************************************************************
** No op.
*/
int sqlite3BtreeSyncDisabled(Btree *p)
{
	log_msg(LOG_VERBOSE, "sqlite3BtreeSyncDisabled(%p)", p);
	return (0);
}

#if !defined(SQLITE_OMIT_PAGER_PRAGMAS) || !defined(SQLITE_OMIT_VACUUM)
/*
** Change the default pages size and the number of reserved bytes per page.
** Or, if the page size has already been fixed, return SQLITE_READONLY
** without changing anything.
**
** The page size must be a power of 2 between 512 and 65536.  If the page
** size supplied does not meet this constraint then the page size is not
** changed.
**
** Page sizes are constrained to be a power of two so that the region of the
** database file used for locking (beginning at PENDING_BYTE, the first byte
** past the 1GB boundary, 0x40000000) needs to occur at the beginning of a page.
**
** If parameter nReserve is less than zero, then the number of reserved bytes
** per page is left unchanged.
**
** If the iFix!=0 then the pageSizeFixed flag is set so that the page size
** and autovacuum mode can no longer be changed.
*/
int sqlite3BtreeSetPageSize(Btree *p, int pageSize, int nReserve, int iFix)
{
	BtShared *pBt;

	log_msg(LOG_VERBOSE, "sqlite3BtreeSetPageSize(%p, %u, %u)",
	    p, pageSize, nReserve);

	if (pageSize < 512 || pageSize > 65536 ||
	    ((pageSize - 1) & pageSize) != 0)
		return SQLITE_OK;

	pBt = p->pBt;
	if (pBt->pageSizeFixed != 0)
		return SQLITE_READONLY;

	/* Can't set the page size once a table has been created. */
	if (pMetaDb != NULL)
		return SQLITE_OK;

	pBt->pageSize = pageSize;
	if (iFix)
		pBt->pageSizeFixed = 1;

	return SQLITE_OK;
}

/***************************************************************************
** Return the currently defined page size.
*/
int sqlite3BtreeGetPageSize(Btree *p)
{
	BtShared *pBt;
	u_int32_t pagesize;

	log_msg(LOG_VERBOSE, "sqlite3BtreeGetPageSize(%p)", p);

	pBt = p->pBt;

	/* This is not true - we use the Berkeley DB default page size. */
	if (pMetaDb != NULL &&
	    pMetaDb->get_pagesize(pMetaDb, &pagesize) == 0)
		return (int)pagesize;
	if (pBt->pageSize == 0)
		return SQLITE_DEFAULT_PAGE_SIZE;
	return p->pBt->pageSize;
}

/***************************************************************************
** No op.
*/
int sqlite3BtreeGetReserve(Btree *p)
{
	log_msg(LOG_VERBOSE, "sqlite3BtreeGetReserve(%p)", p);
	/* FIXME: Need to check how this is used by SQLite. */
	return (0);
}

/***************************************************************************
**
** Set the maximum page count for a database if mxPage is positive.
** No changes are made if mxPage is 0 or negative.
** Regardless of the value of mxPage, return the current maximum page count.
**
** If mxPage <= minimum page count, set it to the minimum possible value.
*/
int sqlite3BtreeMaxPageCount(Btree *p, int mxPage)
{
	int defPgCnt, newPgCnt;
	BtShared *pBt;
	CACHED_DB *cached_db;
	DB_MPOOLFILE *pMpf;
	u_int32_t gBytes, bytes;
	u_int32_t pgSize;
	db_pgno_t minPgNo;
	HashElem *e;

	log_msg(LOG_VERBOSE, "sqlite3BtreeMaxPageCount(%p, %u)", p, mxPage);

	pBt = p->pBt;
	if (!pMetaDb) {
		if (mxPage > 0)
			pBt->pageCount = mxPage;
		return pBt->pageCount;
	}

	pMpf = pMetaDb->get_mpf(pMetaDb);
	assert(pMpf);
	gBytes = bytes = pgSize = 0;

	/* Get the current maximum page number. */
	pMetaDb->get_pagesize(pMetaDb, &pgSize);
	pMpf->get_maxsize(pMpf, &gBytes, &bytes);
	defPgCnt = (int)(gBytes * (GIGABYTE / pgSize) + bytes / pgSize);

	if (mxPage <= 0 || (pBt->db_oflags & DB_RDONLY))
		return defPgCnt;

	/*
	 * Retrieve the current last page number, so we can avoid setting a
	 * value smaller than that.
	 */
	minPgNo = 0;
	if (pMpf->get_last_pgno(pMpf, &minPgNo) != 0)
		return defPgCnt;

	/*
	 * If sqlite3BtreeCreateTable has been called, but the table has not
	 * yet been created, reserve an additional two pages for the table.
	 * This is a bit of a hack, otherwise sqlite3BtreeCursor can return
	 * SQLITE_FULL, which the VDBE code does not expect.
	 */
	for (e = sqliteHashFirst(&pBt->db_cache); e != NULL;
	    e = sqliteHashNext(e)) {
		cached_db = sqliteHashData(e);
		if (cached_db == NULL)
			continue;
		if (cached_db->created == 0)
			minPgNo += 2;
	}
	/*
	 * If mxPage is less than the current last page, set the maximum
	 * page number to the current last page number.
	 */
	newPgCnt = (mxPage < (int)minPgNo) ? (int)minPgNo : mxPage;

	gBytes = (u_int32_t) (newPgCnt / (GIGABYTE / pgSize));
	bytes = (u_int32_t) ((newPgCnt % (GIGABYTE / pgSize)) * pgSize);
	if (pMpf->set_maxsize(pMpf, gBytes, bytes) != 0)
		return defPgCnt;

	return newPgCnt;
}

/*
** Set the secureDelete flag if newFlag is 0 or 1.  If newFlag is -1,
** then make no changes.  Always return the value of the secureDelete
** setting after the change.
*/
int sqlite3BtreeSecureDelete(Btree *p, int newFlag)
{
	int oldFlag;

	oldFlag = 0;
	if (p != NULL) {
		sqlite3_mutex_enter(p->pBt->mutex);
		if (newFlag >= 0)
			p->pBt->secureDelete = (newFlag != 0);
		oldFlag = p->pBt->secureDelete;
		sqlite3_mutex_leave(p->pBt->mutex);
	}

	return oldFlag;
}
#endif /* !defined(SQLITE_OMIT_PAGER_PRAGMAS) */

/*****************************************************************
** Return the pathname of the journal file for this database. The return
** value of this routine is the same regardless of whether the journal file
** has been created or not.
**
** The pager journal filename is invariant as long as the pager is open so
** it is safe to access without the BtShared mutex.
*/
const char *sqlite3BtreeGetJournalname(Btree *p)
{
	BtShared *pBt;

	log_msg(LOG_VERBOSE, "sqlite3BtreeGetJournalname(%p)", p);
	pBt = p->pBt;
	return (pBt->dir_name != 0 ? pBt->dir_name : "");
}

/*****************************************************************
** This function returns a pointer to a blob of memory associated with a
** single shared-engine. The memory is used by client code for its own
** purposes (for example, to store a high-level schema associated with the
** shared-engine). The engine layer manages reference counting issues.
**
** The first time this is called on a shared-engine, nBytes bytes of memory
** are allocated, zeroed, and returned to the caller. For each subsequent call
** the nBytes parameter is ignored and a pointer to the same blob of memory
** returned.
**
** Just before the shared-engine is closed, the function passed as the xFree
** argument when the memory allocation was made is invoked on the blob of
** allocated memory. This function should not call sqlite3_free() on the
** memory, the engine layer does that.
*/
void *sqlite3BtreeSchema(Btree *p, int nBytes, void (*xFree)(void *))
{
	log_msg(LOG_VERBOSE, "sqlite3BtreeSchema(%p, %u, fn_ptr)", p, nBytes);
	/* This was happening when an environment open failed in bigfile.
	if (p == NULL || p->pBt == NULL)
		return NULL;*/

	if (p->schema == NULL && nBytes > 0) {
		p->schema = sqlite3MallocZero(nBytes);
		p->free_schema = xFree;
	}
	return (p->schema);
}

#ifndef NDEBUG
/* Utility functions. */

void log_msg(loglevel_t level, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (level >= CURRENT_LOG_LEVEL) {
		vfprintf(stdout, fmt, ap);
		fputc('\n', stdout);
	}
	fflush(stdout);
	va_end(ap);
}
#endif

/* Stubs. */
int sqlite3BtreeIncrVacuum(Btree *p)
{
	p = NULL;
	return SQLITE_DONE;
}
int sqlite3BtreeIsInBackup(Btree *p)
{
	p = NULL;
	return 0;
}
int sqlite3BtreeGetAutoVacuum(Btree *p)
{
	p = NULL;
	return 0;
}

int sqlite3BtreeSetAutoVacuum(Btree *p, int onoff)
{
	p = NULL;
	onoff = 0;
	return 0;
}

sqlite3_int64 sqlite3BtreeGetCachedRowid(BtCursor *pCur)
{
	return pCur->cachedRowid;
}

void sqlite3BtreeSetCachedRowid(BtCursor *pCur, sqlite3_int64 iRowid)
{
	BtShared *pBt;
	BtCursor *pC;

	pBt = pCur->pBtree->pBt;

	sqlite3_mutex_enter(pBt->mutex);
	for (pC = pBt->first_cursor; pC != NULL; pC = pC->next)
		if (pC->db == pCur->db)
			pC->cachedRowid = iRowid;
	sqlite3_mutex_leave(pBt->mutex);
}

int sqlite3BtreeSavepoint(Btree *p, int op, int iSavepoint)
{
	BtShared *pBt;
	DB_TXN *txn;
#ifdef BDBSQL_SEMITXN_TRUNCATE
	DB_TXN *ttxn;
	DELETED_TABLE *dtable, *prev, *next;
#endif
	int rc, ret;

	log_msg(LOG_VERBOSE, "sqlite3BtreeSavepoint(%p,%d,%d)",
	    p, op, iSavepoint);

	/*
	 * If iSavepoint + 2 > p->nSavepoint, then the savepoint has been
	 * created, but sqlite3BtreeBeginStmt has not been called to create the
	 * actual child transaction.
	 */
	if (!p || pSavepointTxn == NULL || iSavepoint + 2 > p->nSavepoint)
		return SQLITE_OK;

	pBt = p->pBt;

	/*
	 * Note that iSavepoint can be negative, meaning that all savepoints
	 * should be released or rolled back.
	 */
	if (iSavepoint < 0)
		txn = pReadTxn;
	else {
		txn = pSavepointTxn;
		while (--p->nSavepoint > iSavepoint + 1 && txn->parent != NULL)
			txn = txn->parent;
	}

#ifdef BDBSQL_SEMITXN_TRUNCATE
	if (op == SAVEPOINT_ROLLBACK && p->deleted_tables != NULL) {
		for (ttxn = pSavepointTxn;; ttxn = ttxn->parent) {
			prev = NULL;
			for (dtable = p->deleted_tables;
			    dtable != NULL;
			    dtable = next) {
				next = dtable->next;
				if (dtable->txn != ttxn) {
					prev = dtable;
					continue;
				}
				sqlite3_free(dtable);
				if (prev)
					prev->next = next;
				else
					p->deleted_tables = next;
			}
			if (ttxn == txn)
				break;
		}
	}
#endif

	if (txn->parent == NULL) {
		assert(iSavepoint < 0);
		pReadTxn = pSavepointTxn = NULL;
		p->nSavepoint = 0;
		p->inTrans = TRANS_NONE;
	} else
		pSavepointTxn = txn->parent;

	rc = btreeCloseAllCursors(p, txn);
	if (rc != SQLITE_OK)
		return rc;

	ret = (op == SAVEPOINT_RELEASE) ?
	    txn->commit(txn, DB_TXN_NOSYNC) : txn->abort(txn);
	if (ret != 0)
		goto err;

	if (op == SAVEPOINT_ROLLBACK && p->cached_dbs != 0 &&
	    (rc = btreeCleanupCachedHandles(p, CLEANUP_ABORT)) != SQLITE_OK)
		return rc;

err:	return dberr2sqlite(ret);
}

/* Stub out enough to make sqlite3_file_control fail gracefully. */
Pager *sqlite3BtreePager(Btree *p)
{
	return (Pager *)p;
}

#ifndef SQLITE_OMIT_SHARED_CACHE
/*
** Enable or disable the shared pager and schema features.
**
** This routine has no effect on existing database connections.
** The shared cache setting effects only future calls to
** sqlite3_open(), sqlite3_open16(), or sqlite3_open_v2().
*/
int sqlite3_enable_shared_cache(int enable)
{
	sqlite3GlobalConfig.sharedCacheEnabled = enable;
	return SQLITE_OK;
}
#endif

#ifdef BDBSQL_OMIT_LEAKCHECK
#undef sqlite3_malloc
#undef sqlite3_free
#undef sqlite3_strdup
#endif
