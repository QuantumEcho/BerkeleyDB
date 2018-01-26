/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2002-2003
 *	Sleepycat Software.  All rights reserved.
 *
 * $Id: SupplierValue.java,v 1.8 2003/10/18 19:50:19 mhayes Exp $
 */

package com.sleepycat.examples.bdb.shipment.basic;

import java.io.Serializable;

/**
 * A SupplierValue serves as the value in the key/value pair for a supplier
 * entity.
 *
 * <p> In this sample, SupplierValue is used both as the storage data for the
 * value as well as the object binding to the value.  Because it is used
 * directly as storage data using SerialFormat, it must be Serializable. </p>
 *
 * @author Mark Hayes
 */
public class SupplierValue implements Serializable {

    private String name;
    private int status;
    private String city;

    public SupplierValue(String name, int status, String city) {

        this.name = name;
        this.status = status;
        this.city = city;
    }

    public final String getName() {

        return name;
    }

    public final int getStatus() {

        return status;
    }

    public final String getCity() {

        return city;
    }

    public String toString() {

        return "[SupplierValue: name=" + name +
               " status=" + status +
               " city=" + city + ']';
    }
}
