#!/usr/local/bin/perl

###
#
# pack.pl:  A script to pack up databases generated by gen_upgrade (in
#   upgrade.tcl) and package them into a tgz format suitable
#   for archiving and for the upgrade test.
#
# Before use, you must set the following parameters:

my $version = "3.0";
my $build_dir = "/work/db/db-3.0.47/build_unix";
my $db_dump_path = "$build_dir/db_dump";

#
# If, for instance, $version is 3.0, the script will take the databases in
# ./3.0le and ./3.0be and pack them up into a directory hierarchy in
# ./3.0.
#
# Note that this requires ./3.0le, ./3.0be, ./3.0, and 
# $build_dir to all be on the same filesystem;  it makes use of the link()
# system call to put together the collection of files it generates. 

use strict;

my $subdir;
my $file;
my $archive_name;

my $pwd = `pwd`;
chomp( $pwd );
my $packtmp = $pwd . "/$version/packtmp";

$| = 1;


opendir( DIR, $version . "le" ) || die;
while( $subdir = readdir( DIR ) )
{
	if( $subdir !~ m{^\.\.?$} )
	{
		opendir( SUBDIR, $version . "le/$subdir" ) || die;
		while( $file = readdir( SUBDIR ) )
		{
			if( $file !~ m{^\.\.?$} )
			{
				print "[" . localtime() . "] " . "$subdir $file", "\n";
				
				eval
				{
					system( "mkdir", "-p", "$version/$subdir" );
                                        system( "mkdir", "-p", $packtmp );
					$file =~ m{(.*)\.};
					$archive_name = "$1";
					$archive_name =~ s{Test}{test};
                                        link( $version . "le/$subdir/$file", 
                                            $packtmp . "/$archive_name-le.db") or die;
                                        link( $version . "be/$subdir/$file", 
                                            $packtmp . "/$archive_name-be.db") or die;
                                        link( db_dump( "$pwd/$version" . "le/$subdir/$file" ),
                                            $packtmp . "/$archive_name.dump") or die;
                                        link( tcl_dump( "$pwd/$version" . "le/$subdir/$file" ),
                                            $packtmp . "/$archive_name.tcldump") or die;
		
                        
                                        chdir $packtmp or die;       
                                        system( "tar", "-zcf", 
                                            "$pwd/$version/$subdir/$archive_name.tar.gz",
                                            "." );
                                        chdir $pwd or die;
                                        system( "rm", "-rf", $packtmp );
				};
				if( $@ )
				{
					print( "Could not process $file: $@\n" );
				}
			}
		}
	}
}

sub db_dump
{
	my ($file) = @_;

	#print $file, "\n";
	unlink( "temp.dump" );
	#print "$db_dump_path\n";
	system( "sh", "-c", "$db_dump_path -k $file >temp.dump" ) && die;
	if( -e "temp.dump" )
	{
		return "temp.dump";
	}
	else
	{
		die "db_dump failure: $file\n";
	}
}

sub tcl_dump
{
	my ($file) = @_;

	#print $file, "\n";
	unlink( "temp.dump" );
	open( TCL, "|tclsh" );
print TCL <<END;
cd $build_dir
source ../test/test.tcl
upgrade_dump $file $pwd/temp.dump
END
	close( TCL );
	if( -e "temp.dump" )
	{
		return "temp.dump";
	}
	else
	{
		die "TCL dump failure: $file\n";
	}
}
