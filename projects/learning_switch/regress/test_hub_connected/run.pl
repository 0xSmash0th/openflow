#!/usr/bin/perl -w
# test_same_port?

use NF2::TestLib;
use NF2::PacketLib;
use OF::OFUtil;
use strict;

sub my_test {

	my %delta;

	# Host A and B are both on p0. they send unicast with unknown dest.

	my $pkt_args = {
		DA     => "00:00:00:00:00:09",
		SA     => "00:00:00:00:00:01",
		src_ip => "192.168.0.40",
		dst_ip => "192.168.7.40",
		ttl    => 64,
		len    => 64
	};
	my $pkt = new NF2::IP_pkt(%$pkt_args);

	# send one packet; controller should learn MAC, add a flow
	#  entry, and send this packet out the other interfaces
	print "Sending now: \n";
	send_and_count( nftest_get_iface('eth1'), $pkt->packed, \%delta );
	expect_and_count( nftest_get_iface('eth2'), $pkt->packed, \%delta );
	expect_and_count( nftest_get_iface('eth3'), $pkt->packed, \%delta );
	expect_and_count( nftest_get_iface('eth4'), $pkt->packed, \%delta );

	# sleep as long as needed for the test to finish
	sleep 0.5;

	my $pkt_args = {
		DA     => "00:00:00:00:00:08",
		SA     => "00:00:00:00:00:02",
		src_ip => "192.168.1.40",
		dst_ip => "192.168.6.40",
		ttl    => 64,
		len    => 64
	};
	my $pkt = new NF2::IP_pkt(%$pkt_args);
	send_and_count( nftest_get_iface('eth1'), $pkt->packed, \%delta );
	expect_and_count( nftest_get_iface('eth2'), $pkt->packed, \%delta );
	expect_and_count( nftest_get_iface('eth3'), $pkt->packed, \%delta );
	expect_and_count( nftest_get_iface('eth4'), $pkt->packed, \%delta );
	sleep 0.5;

  #  Now A and B try to talk to each other. see if switch drop the packet or not

	my $pkt_args = {
		DA     => "00:00:00:00:00:02",
		SA     => "00:00:00:00:00:01",
		src_ip => "192.168.0.40",
		dst_ip => "192.168.1.40",
		ttl    => 64,
		len    => 64
	};
	my $pkt = new NF2::IP_pkt(%$pkt_args);
	send_and_count( nftest_get_iface('eth1'), $pkt->packed, \%delta );
	return %delta;
}

# how do we pass the cmd-line arguments to the script?
run_learning_switch_test( \&my_test );
