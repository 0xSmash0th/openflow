#!/usr/bin/perl -w

##############################################################################
#
# Wrapper for OpenFlow regression tests
# $Id: of_regress_test.pl 105 2008-06-06 04:07:05Z brandonh $
#
##############################################################################

use OF::Base;
use Test::RegressTest; 
use strict; 

# check vars are set.
check_OF_vars_set();

sub INT_Handler {
	my $signame = shift;
	print "\nNo interrupt handler implemented yet...\n";
	print "\nExited with SIG$signame\n";
	exit(1);
}

push @ARGV, "--root=$ENV{'OFT_ROOT'}", "--common-st-args=hp", "--controller=10.9.8.4:975", "--port_base=1";

run_regress_test( \&INT_Handler, @ARGV );