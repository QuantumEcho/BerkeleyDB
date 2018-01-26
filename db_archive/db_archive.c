/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 1996-2001
 *	Sleepycat Software.  All rights reserved.
 */

#include "db_config.h"

#ifndef lint
static const char copyright[] =
    "Copyright (c) 1996-2001\nSleepycat Software Inc.  All rights reserved.\n";
static const char revid[] =
    "$Id: db_archive.c,v 11.23 2001/05/10 17:13:55 bostic Exp $";
#endif

#ifndef NO_SYSTEM_INCLUDES
#include <sys/types.h>

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#endif

#include "db_int.h"
#include "common_ext.h"
#include "clib_ext.h"

int	 main __P((int, char *[]));
void	 usage __P((void));
void	 version_check __P((void));

DB_ENV	*dbenv;
const char
	*progname = "db_archive";			/* Program name. */

int
main(argc, argv)
	int argc;
	char *argv[];
{
	extern char *optarg;
	extern int optind;
	u_int32_t flags;
	int ch, e_close, exitval, ret, verbose;
	char **file, *home, **list;

	version_check();

	flags = 0;
	e_close = exitval = verbose = 0;
	home = NULL;
	while ((ch = getopt(argc, argv, "ah:lsVv")) != EOF)
		switch (ch) {
		case 'a':
			LF_SET(DB_ARCH_ABS);
			break;
		case 'h':
			home = optarg;
			break;
		case 'l':
			LF_SET(DB_ARCH_LOG);
			break;
		case 's':
			LF_SET(DB_ARCH_DATA);
			break;
		case 'V':
			printf("%s\n", db_version(NULL, NULL, NULL));
			return (EXIT_SUCCESS);
		case 'v':
			verbose = 1;
			break;
		case '?':
		default:
			usage();
		}
	argc -= optind;
	argv += optind;

	if (argc != 0)
		usage();

	/* Handle possible interruptions. */
	__db_util_siginit();

	/*
	 * Create an environment object and initialize it for error
	 * reporting.
	 */
	if ((ret = db_env_create(&dbenv, 0)) != 0) {
		fprintf(stderr,
		    "%s: db_env_create: %s\n", progname, db_strerror(ret));
		goto shutdown;
	}
	e_close = 1;

	dbenv->set_errfile(dbenv, stderr);
	dbenv->set_errpfx(dbenv, progname);

	if (verbose)
		(void)dbenv->set_verbose(dbenv, DB_VERB_CHKPOINT, 1);

	/*
	 * If attaching to a pre-existing environment fails, create a
	 * private one and try again.
	 */
	if ((ret = dbenv->open(dbenv,
	    home, DB_JOINENV | DB_USE_ENVIRON, 0)) != 0 &&
	    (ret = dbenv->open(dbenv, home, DB_CREATE |
	    DB_INIT_LOG | DB_INIT_TXN | DB_PRIVATE | DB_USE_ENVIRON, 0)) != 0) {
		dbenv->err(dbenv, ret, "open");
		goto shutdown;
	}

	/* Get the list of names. */
	if ((ret = log_archive(dbenv, &list, flags)) != 0) {
		dbenv->err(dbenv, ret, "log_archive");
		goto shutdown;
	}

	/* Print the list of names. */
	if (list != NULL) {
		for (file = list; *file != NULL; ++file)
			printf("%s\n", *file);
		__os_free(dbenv, list, 0);
	}

	if (0) {
shutdown:	exitval = 1;
	}
	if (e_close && (ret = dbenv->close(dbenv, 0)) != 0) {
		exitval = 1;
		fprintf(stderr,
		    "%s: dbenv->close: %s\n", progname, db_strerror(ret));
	}

	/* Resend any caught signal. */
	__db_util_sigresend();

	return (exitval == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

void
usage()
{
	(void)fprintf(stderr, "usage: db_archive [-alsVv] [-h home]\n");
	exit(EXIT_FAILURE);
}

void
version_check()
{
	int v_major, v_minor, v_patch;

	/* Make sure we're loaded with the right version of the DB library. */
	(void)db_version(&v_major, &v_minor, &v_patch);
	if (v_major != DB_VERSION_MAJOR ||
	    v_minor != DB_VERSION_MINOR || v_patch != DB_VERSION_PATCH) {
		fprintf(stderr,
	"%s: version %d.%d.%d doesn't match library version %d.%d.%d\n",
		    progname, DB_VERSION_MAJOR, DB_VERSION_MINOR,
		    DB_VERSION_PATCH, v_major, v_minor, v_patch);
		exit(EXIT_FAILURE);
	}
}
