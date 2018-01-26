/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2002-2003
 *	Sleepycat Software.  All rights reserved.
 *
 * $Id: Sample.java,v 1.11 2003/10/18 19:50:17 mhayes Exp $
 */

package com.sleepycat.examples.bdb.shipment.basic;

import com.sleepycat.bdb.TransactionRunner;
import com.sleepycat.bdb.TransactionWorker;
import com.sleepycat.bdb.collection.StoredIterator;
import com.sleepycat.db.DbException;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.Collection;
import java.util.Iterator;
import java.util.Map;

/**
 * Sample is the main entry point for the sample program and may be run as
 * follows:
 *
 * <pre>
 * java com.sleepycat.examples.bdb.shipment.basic.Sample
 *      [-h <home-directory> ] [-multiprocess]
 * </pre>
 *
 * <p> The default for the home directory is ./tmp -- the tmp subdirectory of
 * the current directory where the sample is run. The home directory must exist
 * before running the sample.  To recreate the sample database from scratch,
 * delete all files in the home directory before running the sample.  </p>
 *
 * <p> The default processing model is single-process, in which Berkeley DB
 * recovery is always run when the environment is opened.  If a multi-process
 * model is desired, use the -multiprocess option and implement a monitor
 * process which handles recovery.  If -multiprocess is specified, Berkeley DB
 * recovery will not be run automatically when opening the environment. </p>
 *
 * @author Mark Hayes
 */
public class Sample {

    private SampleDatabase db;
    private SampleViews views;

    /**
     * Run the sample program.
     */
    public static void main(String[] args) {

        System.out.println("\nRunning sample: " + Sample.class);

        // Parse the command line arguments.
        //
        boolean runRecovery = true;
        String homeDir = "./tmp";
        for (int i = 0; i < args.length; i += 1) {
            String arg = args[i];
            if (args[i].equals("-h") && i < args.length - 1) {
                i += 1;
                homeDir = args[i];
            } else if (args[i].equals("-multiprocess")) {
                runRecovery = false;
            } else {
                System.err.println("Usage:\n java " + Sample.class.getName() +
                                  "\n  [-h <home-directory>] [-multiprocess]");
                System.exit(2);
            }
        }

        // Run the sample.
        //
        Sample sample = null;
        try {
            sample = new Sample(homeDir, runRecovery);
            sample.run();
        } catch (Exception e) {
            // If an exception reaches this point, the last transaction did not
            // complete.  If the exception is DbRunRecoveryException, follow
            // the Berkeley DB recovery procedures before running again.
            e.printStackTrace();
        } finally {
            if (sample != null) {
                try {
                    // Always attempt to close the database cleanly.
                    sample.close();
                } catch (Exception e) {
                    System.err.println("Exception during database close:");
                    e.printStackTrace();
                }
            }
        }
    }

    /**
     * Open the database and views.
     */
    private Sample(String homeDir, boolean runRecovery)
        throws DbException, FileNotFoundException {

        db = new SampleDatabase(homeDir, runRecovery);
        views = new SampleViews(db);
    }

    /**
     * Close the database cleanly.
     */
    private void close()
        throws DbException, IOException {

        db.close();
    }

    /**
     * Run two transactions to populate and print the database.  A
     * TransactionRunner is used to ensure consistent handling of transactions,
     * including deadlock retries.  But the best transaction handling mechanism
     * to use depends on the application.
     */
    private void run()
        throws Exception {

        TransactionRunner runner = new TransactionRunner(db.getEnvironment());
        runner.run(new PopulateDatabase());
        runner.run(new PrintDatabase());
    }

    /**
     * Populate the database in a single transaction.
     */
    private class PopulateDatabase implements TransactionWorker {

        public void doWork()
            throws Exception {
            addSuppliers();
            addParts();
            addShipments();
        }
    }

    /**
     * Print the database in a single transaction.  All entities are printed.
     */
    private class PrintDatabase implements TransactionWorker {

        public void doWork()
            throws Exception {
            printEntries("Parts",
                          views.getPartEntrySet().iterator());
            printEntries("Suppliers",
                          views.getSupplierEntrySet().iterator());
            printEntries("Shipments",
                          views.getShipmentEntrySet().iterator());
        }
    }

    /**
     * Populate the part entities in the database.  If the part map is not
     * empty, assume that this has already been done.
     */
    private void addParts() {

        Map parts = views.getPartMap();
        if (parts.isEmpty()) {
            System.out.println("Adding Parts");
            parts.put(new PartKey("P1"),
                      new PartValue("Nut", "Red",
                                    new Weight(12.0, Weight.GRAMS),
                                    "London"));
            parts.put(new PartKey("P2"),
                      new PartValue("Bolt", "Green",
                                    new Weight(17.0, Weight.GRAMS),
                                    "Paris"));
            parts.put(new PartKey("P3"),
                      new PartValue("Screw", "Blue",
                                    new Weight(17.0, Weight.GRAMS),
                                    "Rome"));
            parts.put(new PartKey("P4"),
                      new PartValue("Screw", "Red",
                                    new Weight(14.0, Weight.GRAMS),
                                    "London"));
            parts.put(new PartKey("P5"),
                      new PartValue("Cam", "Blue",
                                    new Weight(12.0, Weight.GRAMS),
                                    "Paris"));
            parts.put(new PartKey("P6"),
                      new PartValue("Cog", "Red",
                                    new Weight(19.0, Weight.GRAMS),
                                    "London"));
        }
    }

    /**
     * Populate the supplier entities in the database.  If the supplier map is
     * not empty, assume that this has already been done.
     */
    private void addSuppliers() {

        Map suppliers = views.getSupplierMap();
        if (suppliers.isEmpty()) {
            System.out.println("Adding Suppliers");
            suppliers.put(new SupplierKey("S1"),
                          new SupplierValue("Smith", 20, "London"));
            suppliers.put(new SupplierKey("S2"),
                          new SupplierValue("Jones", 10, "Paris"));
            suppliers.put(new SupplierKey("S3"),
                          new SupplierValue("Blake", 30, "Paris"));
            suppliers.put(new SupplierKey("S4"),
                          new SupplierValue("Clark", 20, "London"));
            suppliers.put(new SupplierKey("S5"),
                          new SupplierValue("Adams", 30, "Athens"));
        }
    }

    /**
     * Populate the shipment entities in the database.  If the shipment map
     * is not empty, assume that this has already been done.
     */
    private void addShipments() {

        Map shipments = views.getShipmentMap();
        if (shipments.isEmpty()) {
            System.out.println("Adding Shipments");
            shipments.put(new ShipmentKey("P1", "S1"),
                          new ShipmentValue(300));
            shipments.put(new ShipmentKey("P2", "S1"),
                          new ShipmentValue(200));
            shipments.put(new ShipmentKey("P3", "S1"),
                          new ShipmentValue(400));
            shipments.put(new ShipmentKey("P4", "S1"),
                          new ShipmentValue(200));
            shipments.put(new ShipmentKey("P5", "S1"),
                          new ShipmentValue(100));
            shipments.put(new ShipmentKey("P6", "S1"),
                          new ShipmentValue(100));
            shipments.put(new ShipmentKey("P1", "S2"),
                          new ShipmentValue(300));
            shipments.put(new ShipmentKey("P2", "S2"),
                          new ShipmentValue(400));
            shipments.put(new ShipmentKey("P2", "S3"),
                          new ShipmentValue(200));
            shipments.put(new ShipmentKey("P2", "S4"),
                          new ShipmentValue(200));
            shipments.put(new ShipmentKey("P4", "S4"),
                          new ShipmentValue(300));
            shipments.put(new ShipmentKey("P5", "S4"),
                          new ShipmentValue(400));
        }
    }

    /**
     * Print the key/value objects returned by an iterator of Map.Entry
     * objects.
     *
     * <p><b> IMPORTANT: All database iterators must be closed to avoid
     * serious database problems.  If the iterator is not closed, the
     * underlying Berkeley DB cursor is not closed either. </b></p>
     */
    private void printEntries(String label, Iterator iterator) {

        System.out.println("\n--- " + label + " ---");
        try {
            while (iterator.hasNext()) {
                Map.Entry entry = (Map.Entry) iterator.next();
                System.out.println(entry.getKey().toString());
                System.out.println(entry.getValue().toString());
            }
        } finally {
            // IMPORTANT: Use StoredIterator to close all database
            // iterators.  If java.util.Iterator is in hand, you can safely
            // close it by calling StoredIterator.close(Iterator).
            StoredIterator.close(iterator);
        }
    }
}
