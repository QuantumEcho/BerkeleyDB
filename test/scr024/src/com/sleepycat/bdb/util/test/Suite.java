/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2002-2003
 *	Sleepycat Software.  All rights reserved.
 *
 * $Id: Suite.java,v 1.7 2003/10/18 19:51:31 mhayes Exp $
 */

package com.sleepycat.bdb.util.test;

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
        suite.addTest(UtfTest.suite());
        suite.addTest(ExceptionWrapperTest.suite());
        return suite;
    }
}

