# See the file LICENSE for redistribution information.
#
# Copyright (c) 1996-2001
#	Sleepycat Software.  All rights reserved.
#
# $Id: test035.tcl,v 11.4 2001/01/25 18:23:10 bostic Exp $
#
# DB Test 35 {access method}
# DB_GET_BOTH functionality with off-page duplicates.
proc test035 { method {nentries 10000} args} {
	# Test with off-page duplicates
	eval {test033 $method $nentries 20 35 -pagesize 512} $args

	# Test with multiple pages of off-page duplicates
	eval {test033 $method [expr $nentries / 10] 100 35 -pagesize 512} $args
}
