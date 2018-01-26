/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2002-2003
 *	Sleepycat Software.  All rights reserved.
 *
 * $Id: SampleDatabase.java,v 1.15 2003/10/18 19:50:21 mhayes Exp $
 */

package com.sleepycat.examples.bdb.shipment.entity;

import com.sleepycat.bdb.bind.KeyExtractor;
import com.sleepycat.bdb.bind.serial.SerialFormat;
import com.sleepycat.bdb.bind.serial.SerialSerialKeyExtractor;
import com.sleepycat.bdb.ForeignKeyIndex;
import com.sleepycat.bdb.DataIndex;
import com.sleepycat.bdb.DataStore;
import com.sleepycat.bdb.StoredClassCatalog;
import com.sleepycat.db.Db;
import com.sleepycat.db.DbEnv;
import com.sleepycat.db.DbException;
import java.io.FileNotFoundException;
import java.io.IOException;

/**
 * SampleDatabase defines the storage containers, indices and foreign keys
 * for the sample database.
 *
 * @author Mark Hayes
 */
public class SampleDatabase {

    private static final String CLASS_CATALOG = "java_class_catalog";
    private static final String SUPPLIER_STORE = "supplier_store";
    private static final String PART_STORE = "part_store";
    private static final String SHIPMENT_STORE = "shipment_store";
    private static final String SHIPMENT_PART_INDEX = "shipment_part_index";
    private static final String SHIPMENT_SUPPLIER_INDEX =
                                    "shipment_supplier_index";
    private static final String SUPPLIER_CITY_INDEX = "supplier_city_index";

    private DbEnv env;
    private DataStore supplierStore;
    private DataStore partStore;
    private DataStore shipmentStore;
    private DataIndex supplierByCityIndex;
    private ForeignKeyIndex shipmentByPartIndex;
    private ForeignKeyIndex shipmentBySupplierIndex;
    private StoredClassCatalog javaCatalog;
    private SerialFormat partKeyFormat;
    private SerialFormat partValueFormat;
    private SerialFormat supplierKeyFormat;
    private SerialFormat supplierValueFormat;
    private SerialFormat shipmentKeyFormat;
    private SerialFormat shipmentValueFormat;
    private SerialFormat cityKeyFormat;

    /**
     * Open all storage containers, indices, and catalogs.
     */
    public SampleDatabase(String homeDirectory, boolean runRecovery)
        throws DbException, FileNotFoundException {

        // Open the Berkeley DB environment in transactional mode.
        //
        int envFlags = Db.DB_INIT_TXN | Db.DB_INIT_LOCK | Db.DB_INIT_MPOOL |
                       Db.DB_CREATE;
        if (runRecovery) envFlags |= Db.DB_RECOVER;
        env = new DbEnv(0);
        System.out.println("Opening environment in: " + homeDirectory);
        env.open(homeDirectory, envFlags, 0);

        // Set the flags for opening all stores and indices.
        //
        int flags = Db.DB_CREATE | Db.DB_AUTO_COMMIT;

        // Create the Serial class catalog.  This holds the serialized class
        // format for all database records of format SerialFormat.
        //
        javaCatalog = new StoredClassCatalog(env, CLASS_CATALOG, null, flags);

        // Create the data formats.  In this example, all formats are
        // SerialFormat.
        //
        partKeyFormat = new SerialFormat(javaCatalog,
                                             PartKey.class);
        partValueFormat = new SerialFormat(javaCatalog,
                                               PartValue.class);
        supplierKeyFormat = new SerialFormat(javaCatalog,
                                                 SupplierKey.class);
        supplierValueFormat = new SerialFormat(javaCatalog,
                                                   SupplierValue.class);
        shipmentKeyFormat = new SerialFormat(javaCatalog,
                                                 ShipmentKey.class);
        shipmentValueFormat = new SerialFormat(javaCatalog,
                                                   ShipmentValue.class);
        cityKeyFormat = new SerialFormat(javaCatalog,
                                             String.class);

        // Open the Berkeley DB database, along with the associated
        // DataStore, for the part, supplier and shipment stores.
        // In this sample, the stores are opened with the DB_BTREE access
        // method and no duplicate keys allowed.  Although the DB_BTREE method
        // provides ordered keys, the ordering of serialized key objects
        // is not very useful. Duplicate keys are not allowed for any entity
        // with indexes or foreign key relationships.
        //
        Db partDb = new Db(env, 0);
        partDb.open(null, PART_STORE, null, Db.DB_BTREE, flags, 0);
        partStore = new DataStore(partDb, partKeyFormat,
                                  partValueFormat, null);

        Db supplierDb = new Db(env, 0);
        supplierDb.open(null, SUPPLIER_STORE, null, Db.DB_BTREE, flags, 0);
        supplierStore = new DataStore(supplierDb, supplierKeyFormat,
                                      supplierValueFormat, null);

        Db shipmentDb = new Db(env, 0);
        shipmentDb.open(null, SHIPMENT_STORE, null, Db.DB_BTREE, flags, 0);
        shipmentStore = new DataStore(shipmentDb, shipmentKeyFormat,
                                      shipmentValueFormat, null);

        // Create the KeyExtractor objects for the part and supplier
        // indices of the shipment store.  Each key extractor object defines
        // its associated index, since it is responsible for mapping between
        // the indexed value and the index key.
        //
        KeyExtractor cityExtractor = new SupplierByCityExtractor(
                                                    supplierKeyFormat,
                                                    supplierValueFormat,
                                                    cityKeyFormat);
        KeyExtractor partExtractor = new ShipmentByPartExtractor(
                                                    shipmentKeyFormat,
                                                    shipmentValueFormat,
                                                    partKeyFormat);
        KeyExtractor supplierExtractor = new ShipmentBySupplierExtractor(
                                                    shipmentKeyFormat,
                                                    shipmentValueFormat,
                                                    supplierKeyFormat);

        // Open the Berkeley DB database, along with the associated
        // ForeignKeyIndex, for the part and supplier indices of the shipment
        // store.
        // In this sample, the indices are opened with the DB_BTREE access
        // method and sorted duplicate keys.  Although the DB_BTREE method
        // provides ordered keys, the ordering of serialized key objects
        // is not very useful. Duplicate keys are allowed since more than one
        // shipment may exist for the same supplier or part. For indices, if
        // duplicates are allowed they should always be sorted to allow for
        // efficient joins.
        //
        Db cityIndexDb = new Db(env, 0);
        cityIndexDb.setFlags(Db.DB_DUPSORT);
        cityIndexDb.open(null, SUPPLIER_CITY_INDEX, null, Db.DB_BTREE,
                         flags, 0);
        supplierByCityIndex = new DataIndex(supplierStore, cityIndexDb,
                                            cityKeyFormat, cityExtractor);

        Db partIndexDb = new Db(env, 0);
        partIndexDb.setFlags(Db.DB_DUPSORT);
        partIndexDb.open(null, SHIPMENT_PART_INDEX, null, Db.DB_BTREE,
                         flags, 0);
        shipmentByPartIndex = new ForeignKeyIndex(shipmentStore, partIndexDb,
                                            partExtractor, partStore,
                                            ForeignKeyIndex.ON_DELETE_CASCADE);

        Db supplierIndexDb = new Db(env, 0);
        supplierIndexDb.setFlags(Db.DB_DUPSORT);
        supplierIndexDb.open(null, SHIPMENT_SUPPLIER_INDEX, null, Db.DB_BTREE,
                             flags, 0);
        shipmentBySupplierIndex = new ForeignKeyIndex(shipmentStore,
                                        supplierIndexDb,
                                        supplierExtractor, supplierStore,
                                        ForeignKeyIndex.ON_DELETE_CASCADE);
    }

    /**
     * Return the storage environment for the database.
     */
    public final DbEnv getEnvironment() {

        return env;
    }

    /**
     * Return the DataFormat of the part key.
     */
    public final SerialFormat getPartKeyFormat() {

        return partKeyFormat;
    }

    /**
     * Return the DataFormat of the part value.
     */
    public final SerialFormat getPartValueFormat() {

        return partValueFormat;
    }

    /**
     * Return the DataFormat of the supplier key.
     */
    public final SerialFormat getSupplierKeyFormat() {

        return supplierKeyFormat;
    }

    /**
     * Return the DataFormat of the supplier value.
     */
    public final SerialFormat getSupplierValueFormat() {

        return supplierValueFormat;
    }

    /**
     * Return the DataFormat of the shipment key.
     */
    public final SerialFormat getShipmentKeyFormat() {

        return shipmentKeyFormat;
    }

    /**
     * Return the DataFormat of the shipment value.
     */
    public final SerialFormat getShipmentValueFormat() {

        return shipmentValueFormat;
    }

    /**
     * Return the DataFormat of the city key.
     */
    public final SerialFormat getCityKeyFormat() {

        return cityKeyFormat;
    }

    /**
     * Return the part storage container.
     */
    public final DataStore getPartStore() {

        return partStore;
    }

    /**
     * Return the supplier storage container.
     */
    public final DataStore getSupplierStore() {

        return supplierStore;
    }

    /**
     * Return the shipment storage container.
     */
    public final DataStore getShipmentStore() {

        return shipmentStore;
    }

    /**
     * Return the shipment-by-part index.
     */
    public final ForeignKeyIndex getShipmentByPartIndex() {

        return shipmentByPartIndex;
    }

    /**
     * Return the shipment-by-supplier index.
     */
    public final ForeignKeyIndex getShipmentBySupplierIndex() {

        return shipmentBySupplierIndex;
    }

    /**
     * Return the supplier-by-city index.
     */
    public final DataIndex getSupplierByCityIndex() {

        return supplierByCityIndex;
    }

    /**
     * Close all stores (closing a store automatically closes its indices).
     */
    public void close()
        throws DbException, IOException {

        partStore.close();
        supplierStore.close();
        shipmentStore.close();
        // And don't forget to close the catalog and the environment.
        javaCatalog.close();
        env.close(0);
    }

    /**
     * The KeyExtractor for the SupplierByCity index.  This is an
     * extension of the abstract class SerialSerialKeyExtractor, which
     * implements KeyExtractor for the case where the data keys and value
     * are all of the format SerialFormat.
     */
    private static class SupplierByCityExtractor
        extends SerialSerialKeyExtractor {

        /**
         * Construct the part key extractor.
         * @param primaryKeyFormat is the shipment key format.
         * @param valueFormat is the shipment value format.
         * @param indexKeyFormat is the part key format.
         */
        private SupplierByCityExtractor(SerialFormat primaryKeyFormat,
                                        SerialFormat valueFormat,
                                        SerialFormat indexKeyFormat) {

            super(primaryKeyFormat, valueFormat, indexKeyFormat);
        }

        /**
         * Extract the city key from a supplier key/value pair.  The city key
         * is stored in the supplier value, so the supplier key is not used.
         */
        public Object extractIndexKey(Object primaryKeyInput,
                                      Object valueInput)
            throws IOException {

            SupplierValue supplierValue = (SupplierValue) valueInput;
            return supplierValue.getCity();
        }

        /**
         * This method will never be called since ON_DELETE_CLEAR was not
         * specified when creating the index.
         */
        public Object clearIndexKey(Object valueInputOutput)
            throws IOException {

            throw new UnsupportedOperationException();
        }
    }

    /**
     * The KeyExtractor for the ShipmentByPart index.  This is an
     * extension of the abstract class SerialSerialKeyExtractor, which
     * implements KeyExtractor for the case where the data keys and value
     * are all of the format SerialFormat.
     */
    private static class ShipmentByPartExtractor
        extends SerialSerialKeyExtractor {

        /**
         * Construct the part key extractor.
         * @param primaryKeyFormat is the shipment key format.
         * @param valueFormat is the shipment value format.
         * @param indexKeyFormat is the part key format.
         */
        private ShipmentByPartExtractor(SerialFormat primaryKeyFormat,
                                        SerialFormat valueFormat,
                                        SerialFormat indexKeyFormat) {

            super(primaryKeyFormat, valueFormat, indexKeyFormat);
        }

        /**
         * Extract the part key from a shipment key/value pair.  The part key
         * is stored in the shipment key, so the shipment value is not used.
         */
        public Object extractIndexKey(Object primaryKeyInput,
                                      Object valueInput)
            throws IOException {

            ShipmentKey shipmentKey = (ShipmentKey) primaryKeyInput;
            return new PartKey(shipmentKey.getPartNumber());
        }

        /**
         * This method will never be called since ON_DELETE_CLEAR was not
         * specified when creating the index.
         */
        public Object clearIndexKey(Object valueInputOutput)
            throws IOException {

            throw new UnsupportedOperationException();
        }
    }

    /**
     * The KeyExtractor for the ShipmentBySupplier index.  This is an
     * extension of the abstract class SerialSerialKeyExtractor, which
     * implements KeyExtractor for the case where the data keys and value
     * are all of the format SerialFormat.
     */
    private static class ShipmentBySupplierExtractor
        extends SerialSerialKeyExtractor {

        /**
         * Construct the supplier key extractor.
         * @param primaryKeyFormat is the shipment key format.
         * @param valueFormat is the shipment value format.
         * @param indexKeyFormat is the supplier key format.
         */
        private ShipmentBySupplierExtractor(SerialFormat primaryKeyFormat,
                                            SerialFormat valueFormat,
                                            SerialFormat indexKeyFormat) {

            super(primaryKeyFormat, valueFormat, indexKeyFormat);
        }

        /**
         * Extract the supplier key from a shipment key/value pair.  The
         * supplier key is stored in the shipment key, so the shipment value is
         * not used.
         */
        public Object extractIndexKey(Object primaryKeyInput,
                                      Object valueInput)
            throws IOException {

            ShipmentKey shipmentKey = (ShipmentKey) primaryKeyInput;
            return new SupplierKey(shipmentKey.getSupplierNumber());
        }

        /**
         * This method will never be called since ON_DELETE_CLEAR was not
         * specified when creating the index.
         */
        public Object clearIndexKey(Object valueInputOutput)
            throws IOException {

            throw new UnsupportedOperationException();
        }
    }
}
