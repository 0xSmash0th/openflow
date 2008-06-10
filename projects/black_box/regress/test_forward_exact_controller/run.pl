#!/usr/bin/perl -w
# test_forward_exact_controller

use strict;
use OF::Includes;

my $pkt_len   = 64;
my $pkt_total = 1;
my $max_idle  = 1;

sub send_expect_secchan {

	my ( $ofp, $sock, $in_port, $out_port, $max_idle, $pkt_len ) = @_;

	# in_port refers to the flow mod entry's input

	my $test_pkt_args = {
		DA     => "00:00:00:00:00:0" . ( $out_port + 1 ),
		SA     => "00:00:00:00:00:0" . ( $in_port + 1 ),
		src_ip => "0.0.0." .           ( $in_port + 1 ),
		dst_ip => "0.0.0." .           ( $out_port + 1 ),
		ttl    => 64,
		len    => $pkt_len,
		src_port => 1,
		dst_port => 0
	};
	my $test_pkt = new NF2::UDP_pkt(%$test_pkt_args);

	#print HexDump ( $test_pkt->packed );

	my $wildcards = 0x0;    # exact match

	my $flow_mod_pkt =
	  create_flow_mod_from_udp( $ofp, $test_pkt, $in_port, $out_port, $max_idle, $wildcards );

	#print HexDump($flow_mod_pkt);

	# Send 'flow_mod' message
	print $sock $flow_mod_pkt;
	print "sent flow_mod message\n";
	usleep(100000);

	# Send a packet - ensure packet comes out desired port
	nftest_send( nftest_get_iface( "eth" . ( $in_port + 1 ) ), $test_pkt->packed );

	my $recvd_mesg;
	sysread( $sock, $recvd_mesg, 1512 ) || die "Failed to receive message: $!";

	# Inspect  message
	my $msg_size = length($recvd_mesg);
	my $expected_size = $ofp->offsetof( 'ofp_packet_in', 'data' ) + length( $test_pkt->packed );
	compare( "msg size", $msg_size, '==', $expected_size );

	my $msg = $ofp->unpack( 'ofp_packet_in', $recvd_mesg );

	#print HexDump ($recvd_mesg);
	#print Dumper($msg);

	# Verify fields
	print "Verifying secchan message for packet sent in to eth" . ( $in_port + 1 ) . "\n";

	verify_header( $msg, 'OFPT_PACKET_IN', $msg_size );

	compare( "total len", $$msg{'total_len'}, '==', length( $test_pkt->packed ) );
	compare( "in_port",   $$msg{'in_port'},   '==', $in_port );
	compare( "reason",    $$msg{'reason'},    '==', $enums{'OFPR_ACTION'} );

	# verify packet was unchanged!
	my $recvd_pkt_data = substr( $recvd_mesg, $ofp->offsetof( 'ofp_packet_in', 'data' ) );
	if ( $recvd_pkt_data ne $test_pkt->packed ) {
		die "ERROR: sending from eth"
		  . $in_port + 1
		  . " received packet data didn't match packet sent\n";
	}
}

sub my_test {

	my ($sock) = @_;
	
	my $j      = $enums{'OFPP_CONTROLLER'}; #send to the secure channel

	enable_flow_expirations( $ofp, $sock );

	# send from every port, receive on every port except the send port
	for ( my $i = 0 ; $i < 4 ; $i++ ) {
		print "sending from $i to secure channel\n";
		send_expect_secchan( $ofp, $sock, $i, $j, $max_idle, $pkt_len );
		wait_for_flow_expired( $ofp, $sock, $pkt_len, $pkt_total );
	}
}

run_black_box_test( \&my_test );

