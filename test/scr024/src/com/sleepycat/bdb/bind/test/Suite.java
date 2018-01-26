/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2002-2003
 *	Sleepycat Software.  All rights reserved.
 *
 * $Id: Suite.java,v 1.6 2003/10/18 19:51:21 mhayes Exp $
 */

package com.sleepycat.bdb.bind.test;

import junit.framework.Test;
import junit.framework.TestSuite;

/**
 * @author Mark Hayes
 */
public class Suite {

    public static void main(String[] args)
        throws Exception {

        junit.textui.TestRunner.run(suite());
    }

    public static Test suite()
        throws Exception {

        TestSuite suite = new TestSuite();
        suite.addTest(com.sleepycat.bdb.bind.tuple.test.Suite.suite());
        suite.addTest(com.sleepycat.bdb.bind.serial.test.Suite.suite());
        suite.addTest(BindingSpeedTest.suite());
        return suite;
    }
}

