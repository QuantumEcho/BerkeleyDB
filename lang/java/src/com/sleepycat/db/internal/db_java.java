/* ----------------------------------------------------------------------------
 * This file was automatically generated by SWIG (http://www.swig.org).
 * Version 2.0.2
 *
 * Do not make changes to this file unless you know what you are doing--modify
 * the SWIG interface file instead.
 * ----------------------------------------------------------------------------- */

package com.sleepycat.db.internal;

/* package */ class db_java {
  public static void DbEnv_lock_vec(DbEnv dbenv, int locker, int flags, com.sleepycat.db.LockRequest[] list, int offset, int nlist) throws com.sleepycat.db.DatabaseException {
    db_javaJNI.DbEnv_lock_vec(DbEnv.getCPtr(dbenv), dbenv, locker, flags, list, offset, nlist);
  }

  public static void DbTxn_commit(DbTxn txn, int flags) throws com.sleepycat.db.DatabaseException {
    db_javaJNI.DbTxn_commit(DbTxn.getCPtr(txn), txn, flags);
  }

  /* package */ static long initDbEnvRef0(DbEnv self, Object handle) {
    return db_javaJNI.initDbEnvRef0(DbEnv.getCPtr(self), self, handle);
  }

  /* package */ static long initDbRef0(Db self, Object handle) {
    return db_javaJNI.initDbRef0(Db.getCPtr(self), self, handle);
  }

  /* package */ static void deleteRef0(long ref) {
    db_javaJNI.deleteRef0(ref);
  }

  /* package */ static DbEnv getDbEnv0(Db self) {
    long cPtr = db_javaJNI.getDbEnv0(Db.getCPtr(self), self);
    return (cPtr == 0) ? null : new DbEnv(cPtr, false);
  }

}
