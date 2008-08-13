/**
 * Filename: packet-openflow.c
 * Author:   David Underhill
 * Updated:  2008-Jul-15
 *
 * Defines a Wireshark 1.0.0+ dissector for the OpenFlow protocol version 0x83.
 */

/** the version of openflow this dissector was written for */
#define DISSECTOR_OPENFLOW_MIN_VERSION 0x83
#define DISSECTOR_OPENFLOW_MAX_VERSION 0x85
#define DISSECTOR_OPENFLOW_VERSION_DRAFT_THRESHOLD 0x84

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <epan/emem.h>
#include <epan/packet.h>
#include <epan/dissectors/packet-tcp.h>
#include <epan/prefs.h>
#include <string.h>
#include <arpa/inet.h>
#include <openflow.h>

/** if 0, padding bytes will not be shown in the dissector */
#define SHOW_PADDING 0

#define PROTO_TAG_OPENFLOW  "OFP"

/* Wireshark ID of the OPENFLOW protocol */
static int proto_openflow = -1;
static dissector_handle_t openflow_handle;
static void dissect_openflow(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree);

/* traffic will arrive with TCP port OPENFLOW_DST_TCP_PORT */
#define TCP_PORT_FILTER "tcp.port"
static int global_openflow_proto = OPENFLOW_DST_TCP_PORT;

/* try to find the ethernet dissector to dissect encapsulated Ethernet data */
static dissector_handle_t data_ethernet;

/* defines new types in 0x84 if not in the header file in our tree yet */
#ifndef OFPT_ECHO_REQUEST
#    define OFPT_ECHO_REQUEST (OFPT_STATS_REPLY+1)
#    define OFPT_ECHO_REPLY   (OFPT_ECHO_REQUEST+1)
#else
#    warning Do not need #defines for OFPT_ECHO_* types anymore.
#endif

/* AM=Async message, CSM=Control/Switch Message, SM=Symmetric Message */
/** names to bind to various values in the type field */
static const value_string names_ofp_type[] = {
    { OFPT_FEATURES_REQUEST,    "Features Request (CSM)" },
    { OFPT_FEATURES_REPLY,      "Features Reply (CSM)" },
    { OFPT_GET_CONFIG_REQUEST,  "Get Config Request (CSM)" },
    { OFPT_GET_CONFIG_REPLY,    "Get Config Reply (CSM)" },
    { OFPT_SET_CONFIG,          "Set Config (CSM)" },
    { OFPT_PACKET_IN,           "Packet In (AM)" },
    { OFPT_PACKET_OUT,          "Packet Out (CSM)" },
    { OFPT_FLOW_MOD,            "Flow Mod (CSM)" },
    { OFPT_FLOW_EXPIRED,        "Flow Expired (AM)" },
    { OFPT_TABLE,               "Table (CSM)" },
    { OFPT_PORT_MOD,            "Port Mod (CSM)" },
    { OFPT_PORT_STATUS,         "Port Status (AM)" },
    { OFPT_ERROR_MSG,           "Error Message (AM)" },
    { OFPT_STATS_REQUEST,       "Stats Request (CSM)" },
    { OFPT_STATS_REPLY,         "Stats Reply (CSM)" },
    { OFPT_ECHO_REQUEST,        "Echo Request (SM)" },
    { OFPT_ECHO_REPLY,          "Echo Reply (SM)" },
    { 0,                        NULL }
};
#define OFP_TYPE_MAX_VALUE OFPT_ERROR_MSG

/** names from ofp_action_type */
static const value_string names_ofp_action_type[] = {
    { OFPAT_OUTPUT,      "Output to switch port" },
    { OFPAT_SET_DL_VLAN, "VLAN" },
    { OFPAT_SET_DL_SRC,  "Ethernet source address" },
    { OFPAT_SET_DL_DST,  "Ethernet destination address" },
    { OFPAT_SET_NW_SRC,  "IP source address" },
    { OFPAT_SET_NW_DST,  "IP destination address" },
    { OFPAT_SET_TP_SRC,  "TCP/UDP source port" },
    { OFPAT_SET_TP_DST,  "TCP/UDP destination port"},
    { 0,                 NULL }
};
#define NUM_ACTIONS 8
#define NUM_PORT_FLAGS 1
#define NUM_PORT_FEATURES 7
#define NUM_WILDCARDS 10
#define NUM_CAPABILITIES 5

/** yes/no for bitfields field */
static const value_string names_choice[] = {
    { 0, "No"  },
    { 1, "Yes" },
    { 0, NULL  }
};

/** names from ofp_flow_mod_command */
static const value_string names_flow_mod_command[] = {
    { OFPFC_ADD,           "New flow" },
    { OFPFC_DELETE,        "Delete all matching flows" },
    { OFPFC_DELETE_STRICT, "Strictly match wildcards and priority" },
    { 0,                   NULL }
};

/** names of stats_types */
static const value_string names_stats_types[] = {
    { OFPST_FLOW,      "Individual flow statistics. The request body is struct ofp_flow_stats_request. The reply body is an array of struct ofp_flow_stats." },
    { OFPST_AGGREGATE, "Aggregate flow statistics. The request body is struct ofp_aggregate_stats_request. The reply body is struct ofp_aggregate_stats_reply." },
    { OFPST_TABLE,     "Flow table statistics. The request body is empty. The reply body is an array of struct ofp_table_stats." },
    { OFPST_PORT,      "Physical port statistics. The request body is empty. The reply body is an array of struct ofp_port_stats." },
    { 0, NULL }
};

/** names of ofp_reason */
static const value_string names_ofp_reason[] = {
    { OFPR_NO_MATCH, "No matching flow" },
    { OFPR_ACTION,   "Action explicitly output to controller" },
    { 0,             NULL }
};

/** names from ofp_flow_mod_command */
static const value_string names_ofp_port_reason[] = {
    { OFPPR_ADD,    "The port was added" },
    { OFPPR_DELETE, "The port was removed" },
    { OFPPR_MOD,    "Some attribute of the port has changed" },
    { 0,            NULL }
};

#define NUM_REPLIES 1

/* These variables are used to hold the IDs of our fields; they are
 * set when we call proto_register_field_array() in proto_register_openflow()
 */
static gint ofp                = -1;
static gint ofp_pad = -1;
static gint ofp_port = -1;

/* Open Flow Header */
static gint ofp_header         = -1;
static gint ofp_header_version = -1;
static gint ofp_header_type    = -1;
static gint ofp_header_length  = -1;
static gint ofp_header_xid     = -1;
static gint ofp_header_warn_ver = -1;
static gint ofp_header_warn_type = -1;

/* Common Structures */
static gint ofp_phy_port          = -1;
static gint ofp_phy_port_port_no  = -1;
static gint ofp_phy_port_hw_addr  = -1;
static gint ofp_phy_port_name     = -1;
static gint ofp_phy_port_flags_hdr= -1;
static gint ofp_phy_port_flags[NUM_PORT_FLAGS];
static gint ofp_phy_port_speed    = -1;
static gint ofp_phy_port_features_hdr = -1;
static gint ofp_phy_port_features[NUM_PORT_FEATURES];

static gint ofp_match           = -1;
static gint ofp_match_wildcards = -1;
static gint ofp_match_wildcard[NUM_WILDCARDS];
static gint ofp_match_in_port   = -1;
static gint ofp_match_dl_src    = -1;
static gint ofp_match_dl_dst    = -1;
static gint ofp_match_dl_vlan   = -1;
static gint ofp_match_dl_type   = -1;
static gint ofp_match_nw_src    = -1;
static gint ofp_match_nw_dst    = -1;
static gint ofp_match_nw_proto  = -1;
static gint ofp_match_tp_src    = -1;
static gint ofp_match_tp_dst    = -1;

static gint ofp_action         = -1;
static gint ofp_action_type    = -1;
static gint ofp_action_vlan_id = -1;
static gint ofp_action_dl_addr = -1;
static gint ofp_action_nw_addr = -1;
static gint ofp_action_tp      = -1;
static gint ofp_action_unknown = -1;
static gint ofp_action_warn    = -1;
static gint ofp_action_num     = -1;

/* type: ofp_action_output */
static gint ofp_action_output         = -1;
static gint ofp_action_output_max_len = -1;
static gint ofp_action_output_port    = -1;

/* Controller/Switch Messages */
static gint ofp_switch_features               = -1;
static gint ofp_switch_features_datapath_id   = -1;
static gint ofp_switch_features_table_info_hdr= -1;
static gint ofp_switch_features_n_exact       = -1;
static gint ofp_switch_features_n_compression = -1;
static gint ofp_switch_features_n_general     = -1;
static gint ofp_switch_features_buffer_limits_hdr = -1;
static gint ofp_switch_features_buffer_mb     = -1;
static gint ofp_switch_features_n_buffers     = -1;
static gint ofp_switch_features_capabilities_hdr = -1;
static gint ofp_switch_features_capabilities[NUM_CAPABILITIES];
static gint ofp_switch_features_actions_hdr = -1;
static gint ofp_switch_features_actions_warn = -1;
static gint ofp_switch_features_actions[NUM_ACTIONS];
static gint ofp_switch_features_ports_hdr = -1;
static gint ofp_switch_features_ports_num = -1;
static gint ofp_switch_features_ports_warn = -1;

static gint ofp_switch_config               = -1;
/* flags handled by ofp_switch_features_capabilities */
static gint ofp_switch_config_miss_send_len = -1;

static gint ofp_flow_mod           = -1;
static gint ofp_flow_mod_command   = -1;
static gint ofp_flow_mod_max_idle  = -1;
static gint ofp_flow_mod_buffer_id = -1;
static gint ofp_flow_mod_priority  = -1;
static gint ofp_flow_mod_reserved  = -1;

static gint ofp_port_mod      = -1;
/* field: ofp_phy_port */

static gint ofp_stats_request       = -1;
static gint ofp_stats_request_type  = -1;
static gint ofp_stats_request_flags = -1;
static gint ofp_stats_request_body  = -1;

static gint ofp_stats_reply       = -1;
static gint ofp_stats_reply_type  = -1;
static gint ofp_stats_reply_flags = -1;
static gint ofp_stats_reply_flag[NUM_REPLIES];
static gint ofp_stats_reply_body  = -1;

static gint ofp_flow_stats_request          = -1;
/* field: ofp_match */
static gint ofp_flow_stats_request_table_id = -1;

static gint ofp_flow_stats_reply              = -1;
/* length won't be put in the tree */
static gint ofp_flow_stats_reply_table_id     = -1;
/* field: ofp_match */
static gint ofp_flow_stats_reply_duration     = -1;
static gint ofp_flow_stats_reply_packet_count = -1;
static gint ofp_flow_stats_reply_byte_count   = -1;
static gint ofp_flow_stats_reply_priority     = -1;
static gint ofp_flow_stats_reply_max_idle     = -1;
/* field: ofp_actions */

static gint ofp_aggr_stats_request          = -1;
/* field: ofp_match */
static gint ofp_aggr_stats_request_table_id = -1;

static gint ofp_aggr_stats_reply              = -1;
static gint ofp_aggr_stats_reply_packet_count = -1;
static gint ofp_aggr_stats_reply_byte_count   = -1;
static gint ofp_aggr_stats_reply_flow_count   = -1;

static gint ofp_table_stats               = -1;
static gint ofp_table_stats_table_id      = -1;
static gint ofp_table_stats_name          = -1;
static gint ofp_table_stats_max_entries   = -1;
static gint ofp_table_stats_active_count  = -1;
static gint ofp_table_stats_matched_count = -1;

static gint ofp_port_stats            = -1;
static gint ofp_port_stats_port_no    = -1;
static gint ofp_port_stats_rx_count   = -1;
static gint ofp_port_stats_tx_count   = -1;
static gint ofp_port_stats_drop_count = -1;

static gint ofp_packet_out           = -1;
static gint ofp_packet_out_buffer_id = -1;
static gint ofp_packet_out_in_port   = -1;
static gint ofp_packet_out_out_port  = -1;
static gint ofp_packet_out_actions_hdr = -1;
static gint ofp_packet_out_data_hdr  = -1;

/* Asynchronous Messages */
static gint ofp_packet_in        = -1;
static gint ofp_packet_in_buffer_id = -1;
static gint ofp_packet_in_total_len = -1;
static gint ofp_packet_in_in_port   = -1;
static gint ofp_packet_in_reason    = -1;
static gint ofp_packet_in_data_hdr  = -1;

static gint ofp_flow_expired              = -1;
/* field: ofp_match */
static gint ofp_flow_expired_priority     = -1;
static gint ofp_flow_expired_duration     = -1;
static gint ofp_flow_expired_packet_count = -1;
static gint ofp_flow_expired_byte_count   = -1;

static gint ofp_port_status        = -1;
static gint ofp_port_status_reason = -1;
/* field: ofp_phy_port */

static gint ofp_error_msg      = -1;
static gint ofp_error_msg_type = -1;
static gint ofp_error_msg_code = -1;
static gint ofp_error_msg_data = -1;

/* These are the ids of the subtrees that we may be creating */
static gint ett_ofp = -1;

/* Open Flow Header */
static gint ett_ofp_header = -1;

/* Common Structures */
static gint ett_ofp_phy_port = -1;
static gint ett_ofp_phy_port_flags_hdr = -1;
static gint ett_ofp_phy_port_features_hdr = -1;
static gint ett_ofp_match = -1;
static gint ett_ofp_match_wildcards = -1;
static gint ett_ofp_action = -1;
static gint ett_ofp_action_output = -1;

/* Controller/Switch Messages */
static gint ett_ofp_switch_features = -1;
static gint ett_ofp_switch_features_table_info_hdr = -1;
static gint ett_ofp_switch_features_buffer_limits_hdr = -1;
static gint ett_ofp_switch_features_capabilities_hdr = -1;
static gint ett_ofp_switch_features_actions_hdr = -1;
static gint ett_ofp_switch_features_ports_hdr = -1;
static gint ett_ofp_switch_config = -1;
static gint ett_ofp_flow_mod = -1;
static gint ett_ofp_port_mod = -1;
static gint ett_ofp_stats_request = -1;
static gint ett_ofp_stats_reply = -1;
static gint ett_ofp_stats_reply_flags = -1;
static gint ett_ofp_flow_stats_request = -1;
static gint ett_ofp_flow_stats_reply = -1;
static gint ett_ofp_aggr_stats_request = -1;
static gint ett_ofp_aggr_stats_reply = -1;
static gint ett_ofp_table_stats = -1;
static gint ett_ofp_port_stats = -1;
static gint ett_ofp_packet_out = -1;
static gint ett_ofp_packet_out_actions_hdr = -1;
static gint ett_ofp_packet_out_data_hdr  = -1;

/* Asynchronous Messages */
static gint ett_ofp_packet_in = -1;
static gint ett_ofp_packet_in_data_hdr = -1;
static gint ett_ofp_flow_expired = -1;
static gint ett_ofp_port_status = -1;
static gint ett_ofp_error_msg = -1;

void proto_reg_handoff_openflow()
{
    openflow_handle = create_dissector_handle(dissect_openflow, proto_openflow);
    dissector_add(TCP_PORT_FILTER, global_openflow_proto, openflow_handle);
}

#define NO_STRINGS NULL
#define NO_MASK 0x0

/** Returns newly allocated string with two spaces in front of str. */
static inline char* indent( char* str ) {
    char* ret = malloc( strlen(str) + 3 );
    ret[0] = ' ';
    ret[1] = ' ';
    memcpy( &ret[2], str, strlen(str) + 1 );
    return ret;
}

void proto_register_openflow()
{
    data_ethernet = find_dissector("eth");

    /* initialize uninitialized header fields */
    int i;
    for( i=0; i<NUM_CAPABILITIES; i++ ) {
        ofp_switch_features_capabilities[i] = -1;
    }
    for( i=0; i<NUM_ACTIONS; i++ ) {
        ofp_switch_features_actions[i] = -1;
    }
    for( i=0; i<NUM_PORT_FLAGS; i++ ) {
        ofp_phy_port_flags[i] = -1;
    }
    for( i=0; i<NUM_PORT_FEATURES; i++ ) {
        ofp_phy_port_features[i] = -1;
    }
    for( i=0; i<NUM_WILDCARDS; i++ ) {
        ofp_match_wildcard[i] = -1;
    }
    for( i=0; i<NUM_REPLIES; i++ ) {
        ofp_stats_reply_flag[i] = -1;
    }

    /* A header field is something you can search/filter on.
    *
    * We create a structure to register our fields. It consists of an
    * array of register_info structures, each of which are of the format
    * {&(field id), {name, abbrev, type, display, strings, bitmask, blurb, HFILL}}.
    */
    static hf_register_info hf[] = {
        /* header fields */
        { &ofp,
          { "Data", "of.data", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "OpenFlow PDU", HFILL }},

        { &ofp_pad,
          { "Pad", "of.pad", FT_UINT8, BASE_DEC, NO_STRINGS, NO_MASK, "Pad", HFILL }},

        { &ofp_header,
          { "Header", "of.header", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "OpenFlow Header", HFILL }},

        { &ofp_header_version,
          { "Version", "of.ver", FT_UINT8, BASE_HEX, NO_STRINGS, NO_MASK, "Version", HFILL }},

        { &ofp_header_type,
          { "Type", "of.type", FT_UINT8, BASE_DEC, VALS(names_ofp_type), NO_MASK, "Type", HFILL }},

        { &ofp_header_length,
          { "Length", "of.len", FT_UINT8, BASE_DEC, NO_STRINGS, NO_MASK, "Length (bytes)", HFILL }},

        { &ofp_header_xid,
          { "Transaction ID", "of.id", FT_UINT32, BASE_DEC, NO_STRINGS, NO_MASK, "Transaction ID", HFILL }},

        { &ofp_header_warn_ver,
          { "Warning", "of.warn_ver", FT_STRING, BASE_NONE, NO_STRINGS, NO_MASK, "Version Warning", HFILL }},

        { &ofp_header_warn_type,
          { "Warning", "of.warn_type", FT_STRING, BASE_NONE, NO_STRINGS, NO_MASK, "Type Warning", HFILL }},

        /* CS: Common Structures */
        { &ofp_port,
          { "Port #", "of.port", FT_UINT16, BASE_DEC, NO_STRINGS, NO_MASK, "Port #", HFILL }}, /* for searching numerically */

        /* CS: Physical Port Information */
        { &ofp_phy_port,
          { "Physical Port", "of.port", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Physical Port", HFILL }},

        { &ofp_phy_port_port_no,
          { "Port #", "of.port_no", FT_STRING, BASE_NONE, NO_STRINGS, NO_MASK, "Port #", HFILL }},

        { &ofp_phy_port_hw_addr,
          { "MAC Address", "of.port_hw_addr", FT_ETHER, BASE_NONE, NO_STRINGS, NO_MASK, "MAC Address", HFILL }},

        { &ofp_phy_port_name,
          { "Port Name", "of.port_port_name", FT_STRING, BASE_NONE, NO_STRINGS, NO_MASK, "Port Name", HFILL }},

        { &ofp_phy_port_flags_hdr,
          { "Flags", "of.port_flags", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Flags", HFILL }},

        { &ofp_phy_port_flags[0],
          { "  Do not include this port when flooding", "of.port_flags_flood", FT_UINT32, BASE_DEC, VALS(names_choice), OFPPFL_NO_FLOOD, "Do not include this port when flooding", HFILL }},

        { &ofp_phy_port_speed,
          { "Speed (Mbps)", "of.port_speed", FT_UINT32, BASE_DEC, NO_STRINGS, NO_MASK, "Speed (Mbps)", HFILL }},

        { &ofp_phy_port_features_hdr,
          { "Features", "of.port_features", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Features", HFILL }},

        { &ofp_phy_port_features[0],
          { "   10 Mb half-duplex rate support", "of.port_features_10mb_hd" , FT_UINT32, BASE_DEC, VALS(names_choice), OFPPF_10MB_HD, "10 Mb half-duplex rate support", HFILL }},

        { &ofp_phy_port_features[1],
          { "   10 Mb full-duplex rate support", "of.port_features_10mb_fd",  FT_UINT32, BASE_DEC, VALS(names_choice), OFPPF_10MB_FD, "10 Mb full-duplex rate support", HFILL }},

        { &ofp_phy_port_features[2],
          { "  100 Mb half-duplex rate support", "of.port_features_100mb_hd", FT_UINT32, BASE_DEC, VALS(names_choice), OFPPF_100MB_HD, "100 Mb half-duplex rate support", HFILL }},

        { &ofp_phy_port_features[3],
          { "  100 Mb full-duplex rate support", "of.port_features_100mb_fd", FT_UINT32, BASE_DEC, VALS(names_choice), OFPPF_100MB_FD, "100 Mb full-duplex rate support", HFILL }},

        { &ofp_phy_port_features[4],
          { "    1 Gb half-duplex rate support", "of.port_features_1gb_hd",   FT_UINT32, BASE_DEC, VALS(names_choice), OFPPF_1GB_HD, "1 Gb half-duplex rate support", HFILL }},

        { &ofp_phy_port_features[5],
          { "    1 Gb full-duplex rate support", "of.port_features_1gb_fd",   FT_UINT32, BASE_DEC, VALS(names_choice), OFPPF_1GB_FD, "1 Gb full-duplex rate support", HFILL }},

        { &ofp_phy_port_features[6],
          { "   10 Gb full-duplex rate support", "of.port_features_10gb_hd",  FT_UINT32, BASE_DEC, VALS(names_choice), OFPPF_10GB_FD, "10 Gb full-duplex rate support", HFILL }},


        /* CS: match */
        { &ofp_match,
          { "Match", "of.match", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Match", HFILL }},

        { &ofp_match_wildcards,
          { "Match Types", "of.wildcards", FT_UINT16, BASE_HEX, NO_STRINGS, NO_MASK, "Match Types (Wildcards)", HFILL }},

        { &ofp_match_wildcard[0],
          { "  Input port", "of.wildcard_in_port" , FT_UINT16, BASE_DEC, VALS(names_choice), OFPFW_IN_PORT, "Input Port", HFILL }},

        { &ofp_match_wildcard[1],
          { "  VLAN", "of.wildcard_dl_vlan" , FT_UINT16, BASE_DEC, VALS(names_choice), OFPFW_DL_VLAN, "VLAN", HFILL }},

        { &ofp_match_wildcard[2],
          { "  Ethernet Src Addr", "of.wildcard_dl_src" , FT_UINT16, BASE_DEC, VALS(names_choice), OFPFW_DL_SRC, "Ethernet Source Address", HFILL }},

        { &ofp_match_wildcard[3],
          { "  Ethernet Dst Addr", "of.wildcard_dl_dst" , FT_UINT16, BASE_DEC, VALS(names_choice), OFPFW_DL_DST, "Ethernet Destination Address", HFILL }},

        { &ofp_match_wildcard[4],
          { "  Ethernet Type", "of.wildcard_dl_type" , FT_UINT16, BASE_DEC, VALS(names_choice), OFPFW_DL_TYPE, "Ethernet Type", HFILL }},

        { &ofp_match_wildcard[5],
          { "  IP Src Addr", "of.wildcard_nw_src" , FT_UINT16, BASE_DEC, VALS(names_choice), OFPFW_NW_SRC, "IP Source Address", HFILL }},

        { &ofp_match_wildcard[6],
          { "  IP Dst Addr", "of.wildcard_nw_dst" , FT_UINT16, BASE_DEC, VALS(names_choice), OFPFW_NW_DST, "IP Destination Address", HFILL }},

        { &ofp_match_wildcard[7],
          { "  IP Protocol", "of.wildcard_nw_proto" , FT_UINT16, BASE_DEC, VALS(names_choice), OFPFW_NW_PROTO, "IP Protocol", HFILL }},

        { &ofp_match_wildcard[8],
          { "  TCP/UDP Src Port", "of.wildcard_tp_src" , FT_UINT16, BASE_DEC, VALS(names_choice), OFPFW_TP_SRC, "TCP/UDP Source Port", HFILL }},

        { &ofp_match_wildcard[9],
          { "  TCP/UDP Dst Port", "of.wildcard_tp_dst" , FT_UINT16, BASE_DEC, VALS(names_choice), OFPFW_TP_DST, "TCP/UDP Destinatoin Port", HFILL }},

        { &ofp_match_in_port,
          { "Input Port", "of.match_in_port", FT_STRING, BASE_NONE, NO_STRINGS, NO_MASK, "Input Port", HFILL }},

        { &ofp_match_dl_src,
          { "Ethernet Src Addr", "of.match_dl_src", FT_ETHER, BASE_NONE, NO_STRINGS, NO_MASK, "Source MAC Address", HFILL }},

        { &ofp_match_dl_dst,
          { "Ethernet Dst Addr", "of.match_dl_dst", FT_ETHER, BASE_NONE, NO_STRINGS, NO_MASK, "Destination MAC Address", HFILL }},

        { &ofp_match_dl_vlan,
          { "Input VLAN", "of.match_dl_vlan", FT_UINT16, BASE_DEC, NO_STRINGS, NO_MASK, "Input VLAN", HFILL }},

        { &ofp_match_dl_type,
          { "Ethernet Type", "of.match_dl_type", FT_UINT16, BASE_HEX, NO_STRINGS, NO_MASK, "Ethernet Type", HFILL }},

        { &ofp_match_nw_src,
          { "IP Src Addr", "of.match_nw_src", FT_IPv4, BASE_DEC, NO_STRINGS, NO_MASK, "Source IP Address", HFILL }},

        { &ofp_match_nw_dst,
          { "IP Dst Addr", "of.match_nw_dst", FT_IPv4, BASE_DEC, NO_STRINGS, NO_MASK, "Destination IP Address", HFILL }},

        { &ofp_match_nw_proto,
          { "IP Protocol", "of.match_", FT_UINT8, BASE_HEX, NO_STRINGS, NO_MASK, "IP Protocol", HFILL }},

        { &ofp_match_tp_src,
          { "TCP/UDP Src Port", "of.match_tp_src", FT_UINT16, BASE_DEC, NO_STRINGS, NO_MASK, "TCP/UDP Source Port", HFILL }},

        { &ofp_match_tp_dst,
          { "TCP/UDP Dst Port", "of.match_tp_dst", FT_UINT16, BASE_DEC, NO_STRINGS, NO_MASK, "TCP/UDP Destination Port", HFILL }},


        /* CS: active type */
        { &ofp_action,
          { "Action", "of.action", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Action", HFILL }},

        { &ofp_action_type,
          { "Type", "of.action_type", FT_UINT16, BASE_DEC, VALS(names_ofp_action_type), NO_MASK, "Action Type", HFILL }},

        { &ofp_action_vlan_id,
          { "VLAN ID", "of.action_vland_id", FT_UINT16, BASE_DEC, NO_STRINGS, NO_MASK, "VLAN ID", HFILL }},

        { &ofp_action_dl_addr,
          { "MAC Addr", "of.action_dl_addr", FT_ETHER, BASE_NONE, NO_STRINGS, NO_MASK, "MAC Addr", HFILL }},

        { &ofp_action_nw_addr,
          { "IP Addr", "of.action_nw_addr", FT_IPv4, BASE_NONE, NO_STRINGS, NO_MASK, "IP Addr", HFILL }},

        { &ofp_action_tp,
          { "Port", "of.action_port", FT_UINT16, BASE_DEC, NO_STRINGS, NO_MASK, "TCP/UDP Port", HFILL }},

        { &ofp_action_unknown,
          { "Unknown Action Type", "of.action_unknown", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Unknown Action Type", HFILL }},

        { &ofp_action_warn,
          { "Warning", "of.action_warn", FT_STRING, BASE_NONE, NO_STRINGS, NO_MASK, "Warning", HFILL }},

        { &ofp_action_num,
          { "# of Actions", "of.action_num", FT_UINT32, BASE_DEC, NO_STRINGS, NO_MASK, "Number of Actions", HFILL }},

        /* CS: ofp_action_output */
        { &ofp_action_output,
          { "Output Action(s)", "of.action_output", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Output Action(s)", HFILL }},

        { &ofp_action_output_max_len,
          { "Max Bytes to Send", "of.action_output_max_len", FT_STRING, BASE_NONE, NO_STRINGS, NO_MASK, "Maximum Bytes to Send", HFILL }},

        { &ofp_action_output_port,
          { "Port", "of.action_output_port", FT_STRING, BASE_NONE, NO_STRINGS, NO_MASK, "Port", HFILL }},


        /* CSM: Features Request */
        /* nothing beyond the header */


        /* CSM: Features Reply */
        { &ofp_switch_features,
          { "Switch Features", "of.sf", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Switch Features", HFILL }},

        { &ofp_switch_features_datapath_id,
          { "Datapath ID", "of.sf_datapath_id", FT_UINT64, BASE_DEC, NO_STRINGS, NO_MASK, "Datapath ID", HFILL }},

        { &ofp_switch_features_table_info_hdr,
          { "Table Info", "of.sf_table_info", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Table Info", HFILL }},

        { &ofp_switch_features_n_exact,
          { "Max Exact-Match", "of.sf_n_exact", FT_UINT32, BASE_DEC, NO_STRINGS, NO_MASK, "Max Exact-Match", HFILL }},

        { &ofp_switch_features_n_compression,
          { "Max Entries Compressed", "of.sf_n_compression", FT_UINT32, BASE_DEC, NO_STRINGS, NO_MASK, "Max Entries Compressed", HFILL }},

        { &ofp_switch_features_n_general,
          { "Max Arbitrary Form Entries", "of.sf_n_general", FT_UINT32, BASE_DEC, NO_STRINGS, NO_MASK, "Max Arbitrary Form Entries", HFILL }},

        { &ofp_switch_features_buffer_limits_hdr,
          { "Buffer Limits", "of.sf_buffer_limits", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Buffer Limits", HFILL }},

        { &ofp_switch_features_buffer_mb,
          { "Buffer Space (MB)", "of.sf_buffer_mb", FT_UINT32, BASE_DEC, NO_STRINGS, NO_MASK, "", HFILL }},

        { &ofp_switch_features_n_buffers,
          { "Max Packets Buffered", "of.sf_", FT_UINT32, BASE_DEC, NO_STRINGS, NO_MASK, "", HFILL }},

        { &ofp_switch_features_capabilities_hdr,
          { "Capabilities", "of.sf_capabilities", FT_UINT32, BASE_HEX, NO_STRINGS, NO_MASK, "Capabilities", HFILL }},

        { &ofp_switch_features_capabilities[0],
          { "  Flow statistics", "of.sf_capabilities_flow_stats", FT_UINT32, BASE_DEC, VALS(names_choice), OFPC_FLOW_STATS, "Flow statistics", HFILL }},

        { &ofp_switch_features_capabilities[1],
          { "  Table statistics", "of.sf_capabilities_table_stats", FT_UINT32, BASE_DEC, VALS(names_choice), OFPC_TABLE_STATS, "Table statistics", HFILL }},

        { &ofp_switch_features_capabilities[2],
          { "  Port statistics", "of.sf_capabilities_port_stats", FT_UINT32, BASE_DEC, VALS(names_choice), OFPC_PORT_STATS, "Port statistics", HFILL }},

        { &ofp_switch_features_capabilities[3],
          { "  802.11d spanning tree", "of.sf_capabilities_stp", FT_UINT32, BASE_DEC, VALS(names_choice), OFPC_STP, "802.11d spanning tree", HFILL }},

        { &ofp_switch_features_capabilities[4],
          { "  Supports transmitting through multiple physical interface", "of.sf_capabilities_multi_phy_tx", FT_UINT32, BASE_DEC, VALS(names_choice), OFPC_MULTI_PHY_TX,  "Supports transmitting through multiple physical interface", HFILL }},

        { &ofp_switch_features_actions_hdr,
          { "Actions", "of.sf_actions", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Actions", HFILL }},

        { &ofp_switch_features_actions_warn,
          { "Warning: Actions are meaningless until version 0x90", "of.sf_actions_warn", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Warning", HFILL }},

        { &ofp_switch_features_actions[0],
          { "  Output to switch port", "of.sf_actions_output", FT_UINT32, BASE_DEC, VALS(names_choice), OFPAT_OUTPUT, "Output to switch port", HFILL }},

        { &ofp_switch_features_actions[1],
          { "  VLAN", "of.sf_actions_vlan", FT_UINT32, BASE_DEC, VALS(names_choice), OFPAT_SET_DL_VLAN, "VLAN", HFILL }},

        { &ofp_switch_features_actions[2],
          { "  Ethernet source address", "of.sf_actions_eth_src_addr", FT_UINT32, BASE_DEC, VALS(names_choice), OFPAT_SET_DL_SRC, "Ethernet source address", HFILL }},

        { &ofp_switch_features_actions[3],
          { "  Ethernet destination address", "of.sf_actions_eth_dst_addr", FT_UINT32, BASE_DEC, VALS(names_choice), OFPAT_SET_DL_DST, "Ethernet destination address", HFILL }},

        { &ofp_switch_features_actions[4],
          { "  IP source address", "of.sf_actions_ip_src_addr", FT_UINT32, BASE_DEC, VALS(names_choice), OFPAT_SET_NW_SRC, "IP source address", HFILL }},

        { &ofp_switch_features_actions[5],
          { "  IP destination address", "of.sf_actions_ip_dst_addr", FT_UINT32, BASE_DEC, VALS(names_choice), OFPAT_SET_NW_DST, "IP destination address", HFILL }},

        { &ofp_switch_features_actions[6],
          { "  TCP/UDP source", "of.sf_actions_src_port", FT_UINT32, BASE_DEC, VALS(names_choice), OFPAT_SET_TP_SRC, "TCP/UDP source port", HFILL }},

        { &ofp_switch_features_actions[7],
          { "  TCP/UDP destination", "of.sf_actions_dst_port", FT_UINT32, BASE_DEC, VALS(names_choice), OFPAT_SET_TP_DST, "TCP/UDP destination port", HFILL }},

        { &ofp_switch_features_ports_hdr,
          { "Port Definitions", "of.sf_ports", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Port Definitions", HFILL }},

        { &ofp_switch_features_ports_num,
          { "# of Ports", "of.sf_ports_num", FT_UINT32, BASE_DEC, NO_STRINGS, NO_MASK, "Number of Ports", HFILL }},

        { &ofp_switch_features_ports_warn,
          { "Warning", "of.sf_ports_warn", FT_STRING, BASE_NONE, NO_STRINGS, NO_MASK, "Warning", HFILL }},


        /* CSM: Get Config Request */
        /* nothing beyond the header */


        /* CSM: Get Config Reply */
        /* CSM: Set Config */
        { &ofp_switch_config,
          { "Switch Configuration", "of.sc", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Switch Configuration", HFILL } },

        { &ofp_switch_config_miss_send_len,
          { "Max Bytes of New Flow to Send to Controller", "of.sc_", FT_UINT16, BASE_DEC, NO_STRINGS, NO_MASK, "Max Bytes of New Flow to Send to Controller", HFILL } },


        /* AM:  Packet In */
        { &ofp_packet_in,
          { "Packet In", "of.pktin", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Packet In", HFILL }},

        { &ofp_packet_in_buffer_id,
          { "Buffer ID", "of.pktin_buffer_id", FT_UINT32, BASE_DEC, NO_STRINGS, NO_MASK, "Buffer ID", HFILL }},

        { &ofp_packet_in_total_len,
          { "Frame Total Length", "of.pktin_total_len", FT_UINT16, BASE_DEC, NO_STRINGS, NO_MASK, "Frame Total Length (B)", HFILL }},

        { &ofp_packet_in_in_port,
          { "Frame Recv Port", "of.pktin_in_port", FT_UINT16, BASE_DEC, NO_STRINGS, NO_MASK, "Port Frame was Received On", HFILL }},

        { &ofp_packet_in_reason,
          { "Reason Sent", "of.pktin_reason", FT_UINT8, BASE_DEC, VALS(names_ofp_reason), NO_MASK, "Reason Packet Sent", HFILL }},

        { &ofp_packet_in_data_hdr,
          { "Frame Data", "of.pktin_data", FT_BYTES, BASE_NONE, NO_STRINGS, NO_MASK, "Frame Data", HFILL }},


        /* CSM: Packet Out */
       { &ofp_packet_out,
          { "Packet Out", "of.pktout", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Packet Out", HFILL }},

        { &ofp_packet_out_buffer_id,
          { "Buffer ID", "of.pktout_buffer_id", FT_STRING, BASE_NONE, NO_STRINGS, NO_MASK, "Buffer ID", HFILL }},

        { &ofp_packet_out_in_port,
          { "Frame Recv Port", "of.pktout_in_port", FT_STRING, BASE_NONE, NO_STRINGS, NO_MASK, "Port Frame was Received On", HFILL }},

        { &ofp_packet_out_out_port,
          { "Frame Output Port", "of.pktout_out_port", FT_STRING, BASE_NONE, NO_STRINGS, NO_MASK, "Port Frame was Sent Out", HFILL }},

        { &ofp_packet_out_actions_hdr,
          { "Actions to Apply", "of.pktout_actions", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Actions to Apply to Packet", HFILL }},

        { &ofp_packet_out_data_hdr,
          { "Frame Data", "of.pktout_data", FT_BYTES, BASE_NONE, NO_STRINGS, NO_MASK, "Frame Data", HFILL }},


        /* CSM: Flow Mod */
        { &ofp_flow_mod,
          { "Flow Modification", "of.fm", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Flow Modification", HFILL } },

        { &ofp_flow_mod_command,
          { "Command", "of.fm_command", FT_UINT16, BASE_DEC, VALS(names_flow_mod_command), NO_MASK, "Command", HFILL } },

        { &ofp_flow_mod_max_idle,
          { "Idle Time (sec) Before Discarding", "of.fm_max_idle", FT_UINT16, BASE_DEC, NO_STRINGS, NO_MASK, "Idle Time (sec) Before Discarding", HFILL } },

        { &ofp_flow_mod_buffer_id,
          { "Buffer ID", "of.fm_buffer_id", FT_UINT32, BASE_DEC, NO_STRINGS, NO_MASK, "Buffer ID", HFILL } },

        { &ofp_flow_mod_priority,
          { "Priority", "of.fm_priority", FT_UINT16, BASE_DEC, NO_STRINGS, NO_MASK, "Priority", HFILL } },

        { &ofp_flow_mod_reserved,
          { "Reserved", "of.fm_reserved", FT_UINT32, BASE_DEC, NO_STRINGS, NO_MASK, "Reserved", HFILL } },


        /* AM:  Flow Expired */
        { &ofp_flow_expired,
          { "Flow Expired", "of.fe", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Flow Expired", HFILL } },

        { &ofp_flow_expired_priority,
          { "Priority", "of.fe_priority", FT_UINT16, BASE_DEC, NO_STRINGS, NO_MASK, "Priority", HFILL } },

        { &ofp_flow_expired_duration,
          { "Flow Duration (sec)", "of.fe_duration", FT_UINT32, BASE_DEC, NO_STRINGS, NO_MASK, "Time Flow was Alive (sec)", HFILL } },

        { &ofp_flow_expired_packet_count,
          { "Packet Count", "of.fe_packet_count", FT_UINT64, BASE_DEC, NO_STRINGS, NO_MASK, "Packet Cout", HFILL } },

        { &ofp_flow_expired_byte_count,
          { "Byte Count", "of.fe_byte_count", FT_UINT64, BASE_DEC, NO_STRINGS, NO_MASK, "Byte Count", HFILL } },


        /* CSM: Table */
        /* not yet defined by the spec */


        /* CSM: Port Mod */
        { &ofp_port_mod,
          { "Port Modification", "of.pm", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Port Modification", HFILL } },


        /* AM: Port Status */
        { &ofp_port_status,
          { "Port Status", "of.ps", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Port Status", HFILL } },

        { &ofp_port_status_reason,
          { "Reason", "of.ps_reason", FT_UINT8, BASE_DEC, VALS(names_ofp_port_reason), NO_MASK, "Reason", HFILL } },


        /* CSM: Stats Request */
        { &ofp_stats_request,
          { "Stats Request", "of.sreq", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Statistics Request", HFILL } },

        { &ofp_stats_request_type,
          { "Type", "of.sreq_type", FT_UINT16, BASE_HEX, VALS(names_stats_types), NO_MASK, "Type", HFILL } },

        { &ofp_stats_request_flags,
          { "Flags", "of.sreq_flags", FT_UINT16, BASE_HEX, NO_STRINGS, NO_MASK, "Flags", HFILL } },

        { &ofp_stats_request_body,
          { "Body", "of.sreq_body", FT_BYTES, BASE_NONE, NO_STRINGS, NO_MASK, "Body", HFILL } },



        /* CSM: Stats Reply */
        { &ofp_stats_reply,
          { "Stats Reply", "of.srep", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Statistics Reply", HFILL } },

        { &ofp_stats_reply_type,
          { "Type", "of.srep_type", FT_UINT16, BASE_HEX, VALS(names_stats_types), NO_MASK, "Type", HFILL } },

        { &ofp_stats_reply_flags,
          { "Flags", "of.srep_flags", FT_UINT16, BASE_DEC, NO_STRINGS, NO_MASK, "Flags", HFILL } },

        { &ofp_stats_reply_flag[0],
          { "  More replies to follow", "of.srep_more", FT_UINT16, BASE_DEC, VALS(names_choice), OFPSF_REPLY_MORE, "More replies to follow", HFILL }},

        { &ofp_stats_reply_body,
          { "Body", "of.srep_body", FT_BYTES, BASE_NONE, NO_STRINGS, NO_MASK, "Body", HFILL } },

        /* CSM: Stats: Flow: Request */
        { &ofp_flow_stats_request,
          { "Flow Stats Request", "of.stats_flow", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Flow Statistics Request", HFILL } },

        { &ofp_flow_stats_request_table_id,
          { "Table ID", "of.stats_flow_table_id", FT_STRING, BASE_NONE, NO_STRINGS, NO_MASK, "Table ID", HFILL } },

        /* CSM: Stats: Flow: Reply */
        { &ofp_flow_stats_reply,
          { "Flow Stats Reply", "of.stats_flow_", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Flow Statistics Reply", HFILL } },

        { &ofp_flow_stats_reply_table_id,
          { "Table ID", "of.stats_flow_table_id", FT_UINT8, BASE_DEC, NO_STRINGS, NO_MASK, "Table ID", HFILL } },

        { &ofp_flow_stats_reply_duration,
          { "Flow Duration (sec)", "of.stats_flow_duration", FT_UINT32, BASE_DEC, NO_STRINGS, NO_MASK, "Time Flow has Been Alive (sec)", HFILL } },

        { &ofp_flow_stats_reply_packet_count,
          { "Packet Count", "of.stats_flow_packet_count", FT_UINT64, BASE_DEC, NO_STRINGS, NO_MASK, "Packet Count", HFILL } },

        { &ofp_flow_stats_reply_byte_count,
          { "Byte Count", "of.stats_flow_byte_count", FT_UINT64, BASE_DEC, NO_STRINGS, NO_MASK, "Byte Count", HFILL } },

        { &ofp_flow_stats_reply_priority,
          { "Priority", "of.stats_flow_priority", FT_UINT16, BASE_DEC, NO_STRINGS, NO_MASK, "Priority", HFILL } },

        { &ofp_flow_stats_reply_max_idle,
          { "Idle Time (sec) Before Discarding", "of.stats_flow_max_idle", FT_UINT16, BASE_DEC, NO_STRINGS, NO_MASK, "Idle Time (sec) Before Discarding", HFILL } },

        /* CSM: Stats: Aggregate: Request */
        { &ofp_aggr_stats_request,
          { "Aggregate Stats Request", "of.stats_aggr", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Aggregate Statistics Request", HFILL } },

        { &ofp_aggr_stats_request_table_id,
          { "Table ID", "of.stats_aggr_table_id", FT_STRING, BASE_NONE, NO_STRINGS, NO_MASK, "Table ID", HFILL } },

        /* CSM: Stats: Aggregate: Reply */
        { &ofp_aggr_stats_reply,
          { "Aggregate Stats Reply", "of.stats_aggr", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Aggregate Statistics Reply", HFILL } },

        { &ofp_aggr_stats_reply_packet_count,
          { "Packet Count", "of.stats_aggr_packet_count", FT_UINT64, BASE_DEC, NO_STRINGS, NO_MASK, "Packet count", HFILL } },

        { &ofp_aggr_stats_reply_byte_count,
          { "Byte Count", "of.stats_aggr_byte_count", FT_UINT64, BASE_DEC, NO_STRINGS, NO_MASK, "Byte Count", HFILL } },

        { &ofp_aggr_stats_reply_flow_count,
          { "Flow Count", "of.stats_aggr_flow_count", FT_UINT32, BASE_DEC, NO_STRINGS, NO_MASK, "Flow Count", HFILL } },

        /* CSM: Stats: Port */
        { &ofp_port_stats,
          { "Port Stats", "of.stats_port", FT_STRING, BASE_NONE, NO_STRINGS, NO_MASK, "Port Stats", HFILL } },

        { &ofp_port_stats_port_no,
          { "Port #", "of.stats_port_port_no", FT_STRING, BASE_NONE, NO_STRINGS, NO_MASK, "", HFILL } },

        { &ofp_port_stats_rx_count,
          { "# Packets Recv  ", "of.stats_port_rx_count", FT_UINT64, BASE_DEC, NO_STRINGS, NO_MASK, "Number of Packets Received", HFILL } },

        { &ofp_port_stats_tx_count,
          { "# Packets Sent  ", "of.stats_port_tx_count", FT_UINT64, BASE_DEC, NO_STRINGS, NO_MASK, "Number of Packets Sent", HFILL } },

        { &ofp_port_stats_drop_count,
          { "# Packets Dropped", "of.stats_port_drop_count", FT_UINT64, BASE_DEC, NO_STRINGS, NO_MASK, "Number of Packets Dropped", HFILL } },

        /* CSM: Stats: Table */
        { &ofp_table_stats,
          { "Table Stats", "of.stats_table", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Table Stats", HFILL } },

        { &ofp_table_stats_table_id,
          { "Table ID", "of.stats_table_table_id", FT_UINT8, BASE_DEC, NO_STRINGS, NO_MASK, "Table ID", HFILL } },

        { &ofp_table_stats_name,
          { "Name", "of.stats_table_name", FT_STRING, BASE_NONE, NO_STRINGS, NO_MASK, "Name", HFILL } },

        { &ofp_table_stats_max_entries,
          { "Max Supported Entries", "of.stats_table_max_entries", FT_UINT32, BASE_DEC, NO_STRINGS, NO_MASK, "Max Supported Entries", HFILL } },

        { &ofp_table_stats_active_count,
          { "Active Entry Count", "of.stats_table_active_count", FT_UINT32, BASE_DEC, NO_STRINGS, NO_MASK, "Active Entry Count", HFILL } },

        { &ofp_table_stats_matched_count,
          { "Packet Match Count", "of.stats_table_match_count", FT_UINT64, BASE_DEC, NO_STRINGS, NO_MASK, "Packet Match Count", HFILL } },


        /* AM:  Error Message */
        { &ofp_error_msg,
          { "Error Message", "of.err", FT_NONE, BASE_NONE, NO_STRINGS, NO_MASK, "Error Message", HFILL } },

        { &ofp_error_msg_type,
          { "Type", "of.err_type", FT_UINT16, BASE_DEC, NO_STRINGS, NO_MASK, "Type", HFILL } },

        { &ofp_error_msg_code,
          { "Code", "of.err_code", FT_UINT16, BASE_DEC, NO_STRINGS, NO_MASK, "Code", HFILL } },

        { &ofp_error_msg_data,
          { "Data", "of.err_data", FT_BYTES, BASE_NONE, NO_STRINGS, NO_MASK, "Data", HFILL } },
    };

    static gint *ett[] = {
        &ett_ofp,
        &ett_ofp_header,
        &ett_ofp_phy_port,
        &ett_ofp_phy_port_flags_hdr,
        &ett_ofp_phy_port_features_hdr,
        &ett_ofp_match,
        &ett_ofp_match_wildcards,
        &ett_ofp_action,
        &ett_ofp_action_output,
        &ett_ofp_switch_features,
        &ett_ofp_switch_features_table_info_hdr,
        &ett_ofp_switch_features_buffer_limits_hdr,
        &ett_ofp_switch_features_capabilities_hdr,
        &ett_ofp_switch_features_actions_hdr,
        &ett_ofp_switch_features_ports_hdr,
        &ett_ofp_switch_config,
        &ett_ofp_flow_mod,
        &ett_ofp_port_mod,
        &ett_ofp_stats_request,
        &ett_ofp_stats_reply,
        &ett_ofp_stats_reply_flags,
        &ett_ofp_flow_stats_request,
        &ett_ofp_flow_stats_reply,
        &ett_ofp_aggr_stats_request,
        &ett_ofp_aggr_stats_reply,
        &ett_ofp_table_stats,
        &ett_ofp_port_stats,
        &ett_ofp_packet_out,
        &ett_ofp_packet_out_data_hdr,
        &ett_ofp_packet_out_actions_hdr,
        &ett_ofp_packet_in,
        &ett_ofp_packet_in_data_hdr,
        &ett_ofp_flow_expired,
        &ett_ofp_port_status,
        &ett_ofp_error_msg,
    };

    proto_openflow = proto_register_protocol( "OpenFlow Protocol",
                                              "OFP",
                                              "of" ); /* abbreviation for filters */

    proto_register_field_array(proto_openflow, hf, array_length(hf));
    proto_register_subtree_array(ett, array_length(ett));
    register_dissector("openflow", dissect_openflow, proto_openflow);
}

const char* ofp_type_to_string( gint8 type ) {
    static char str_unknown[17];

    if( type <= OFP_TYPE_MAX_VALUE )
        return names_ofp_type[type].strptr;
    else {
        snprintf( str_unknown, 17, "Unknown Type %u", type );
        return str_unknown;
    }
}

/**
 * Adds "hf" to "tree" starting at "offset" into "tvb" and using "length"
 * bytes.  offset is incremented by length.
 */
static void add_child( proto_item* tree, gint hf, tvbuff_t *tvb, guint32* offset, guint32 len ) {
    proto_tree_add_item( tree, hf, tvb, *offset, len, FALSE );
    *offset += len;
}

/**
 * Adds "hf" to "tree" starting at "offset" into "tvb" and using "length"
 * bytes.  offset is incremented by length.  The specified string is used as the
 * field's display value.
 */
static void add_child_str(proto_item* tree, gint hf, tvbuff_t *tvb, guint32* offset, guint32 len, const char* str) {
    proto_tree_add_string(tree, hf, tvb, *offset, len, str);
    *offset += len;
}

/**
 * Adds "hf" to "tree" starting at "offset" into "tvb" and using "length" bytes.
 */
static void add_child_const( proto_item* tree, gint hf, tvbuff_t *tvb, guint32 offset, guint32 len ) {
    proto_tree_add_item( tree, hf, tvb, offset, len, FALSE );
}

/** returns the length of a PDU which starts at the specified offset in tvb. */
static guint get_openflow_message_len(packet_info *pinfo, tvbuff_t *tvb, int offset) {
    return (guint)tvb_get_ntohs(tvb, offset+2); /* length is at offset 2 in the header */
}

static void dissect_pad(proto_tree* tree, guint32 *offset, guint pad_byte_count) {
#if SHOW_PADDING
    guint i;
    for( i=0; i<pad_byte_count; i++ )
        add_child(tree, ofp_pad, tvb, offset, 1);
#else
    *offset += pad_byte_count;
#endif
}

static void dissect_port(proto_tree* tree, gint hf, tvbuff_t *tvb, guint32 *offset) {
    /* get the port number */
    guint16 port = tvb_get_ntohs( tvb, *offset );

    /* save the numeric searchable field, but don't show it on the GUI */
    proto_tree_add_item_hidden( tree, ofp_port, tvb, *offset, 2, FALSE );

    /* check to see if the port is special (e.g. the name of a fake output ports defined by ofp_port) */
    const char* str_port = NULL;
    char str_num[6];
    switch( port ) {
    case OFPP_TABLE:
        str_port = "Table  (perform actions in flow table; only allowed for dst port packet out messages)";
        break;

    case OFPP_NORMAL:
        str_port = "Normal  (process with normal L2/L3 switching)";
        break;

    case OFPP_FLOOD:
        str_port = "Flood  (all physical ports except input port and those disabled by STP)";
        break;

    case OFPP_ALL:
        str_port = "All  (all physical ports except input port)";
        break;

    case OFPP_CONTROLLER:
        str_port = "Controller  (send to controller)";
        break;

    case OFPP_LOCAL:
        str_port = "Local  (local openflow \"port\")";
        break;

    case OFPP_NONE:
        str_port = "None  (not associated with a physical port)";
        break;

    default:
        /* no special name, so just use the number */
        str_port = str_num;
        snprintf(str_num, 6, "%u", port);
    }

    /* put the string-representation in the GUI tree */
    add_child_str( tree, hf, tvb, offset, 2, str_port );
}

static void dissect_phy_ports(proto_tree* tree, proto_item* item, tvbuff_t *tvb, packet_info *pinfo, guint32 *offset, guint num_ports)
{
    proto_item *port_item;
    proto_tree *port_tree;
    proto_item *flags_item;
    proto_tree *flags_tree;
    proto_item *features_item;
    proto_tree *features_tree;

    int i;
    while(num_ports-- > 0) {
        port_item = proto_tree_add_item(tree, ofp_phy_port, tvb, *offset, sizeof(struct ofp_phy_port), FALSE);
        port_tree = proto_item_add_subtree(port_item, ett_ofp_phy_port);

        dissect_port( port_tree, ofp_phy_port_port_no, tvb, offset );
        add_child( port_tree, ofp_phy_port_hw_addr, tvb, offset, 6 );
        add_child( port_tree, ofp_phy_port_name, tvb, offset, OFP_MAX_PORT_NAME_LEN );

        /* flags */
        flags_item = proto_tree_add_item(port_tree, ofp_phy_port_flags_hdr, tvb, *offset, 4, FALSE);
        flags_tree = proto_item_add_subtree(flags_item, ett_ofp_phy_port_flags_hdr);
        for(i=0; i<NUM_PORT_FLAGS; i++) {
            add_child_const(flags_tree, ofp_phy_port_flags[i], tvb, *offset, 4);
        }
        *offset += 4;

        add_child( port_tree, ofp_phy_port_speed, tvb, offset, 4 );

        /* features */
        features_item = proto_tree_add_item(port_tree, ofp_phy_port_features_hdr, tvb, *offset, 4, FALSE);
        features_tree = proto_item_add_subtree(features_item, ett_ofp_phy_port_features_hdr);
        for(i=0; i<NUM_PORT_FEATURES; i++) {
            add_child_const(features_tree, ofp_phy_port_features[i], tvb, *offset, 4);
        }
        *offset += 4;
    }
}

static void dissect_match(proto_tree* tree, proto_item* item, tvbuff_t *tvb, packet_info *pinfo, guint32 *offset)
{
    int i;
    proto_item *match_item = proto_tree_add_item(tree, ofp_match, tvb, *offset, sizeof(struct ofp_match), FALSE);
    proto_tree *match_tree = proto_item_add_subtree(match_item, ett_ofp_match);

    /* add wildcard subtree */
    guint16 wildcards = tvb_get_ntohs( tvb, *offset );
    proto_item *wild_item = proto_tree_add_item(match_tree, ofp_match_wildcards, tvb, *offset, 2, FALSE);
    proto_tree *wild_tree = proto_item_add_subtree(wild_item, ett_ofp_match_wildcards);
    for(i=0; i<NUM_WILDCARDS; i++)
        add_child_const(wild_tree, ofp_match_wildcard[i], tvb, *offset, 2 );
    *offset += 2;

    /* show only items whose corresponding wildcard bit is set */
    if( ~wildcards & OFPFW_IN_PORT )
        dissect_port(match_tree, ofp_match_in_port, tvb, offset);
    else
        *offset += 2;

    if( ~wildcards & OFPFW_DL_SRC )
        add_child(match_tree, ofp_match_dl_src, tvb, offset, 6);
    else
        *offset += 6;

    if( ~wildcards & OFPFW_DL_DST )
        add_child(match_tree, ofp_match_dl_dst, tvb, offset, 6);
    else
        *offset += 6;

    if( ~wildcards & OFPFW_DL_VLAN )
        add_child(match_tree, ofp_match_dl_vlan, tvb, offset, 2);
    else
        *offset += 2;

    if( ~wildcards & OFPFW_DL_TYPE )
        add_child(match_tree, ofp_match_dl_type, tvb, offset, 2);
    else
        *offset += 2;

    if( ~wildcards & OFPFW_NW_SRC )
        add_child(match_tree, ofp_match_nw_src, tvb, offset, 4);
    else
        *offset += 4;

    if( ~wildcards & OFPFW_NW_DST )
        add_child(match_tree, ofp_match_nw_dst, tvb, offset, 4);
    else
        *offset += 4;

    if( ~wildcards & OFPFW_NW_PROTO )
        add_child(match_tree, ofp_match_nw_proto, tvb, offset, 1);
    else
        *offset += 1;

    dissect_pad(match_tree, offset, 3);

    if( ~wildcards & OFPFW_TP_SRC )
        add_child(match_tree, ofp_match_tp_src, tvb, offset, 2);
    else
        *offset += 2;

    if( ~wildcards & OFPFW_TP_DST )
        add_child(match_tree, ofp_match_tp_dst, tvb, offset, 2);
    else
        *offset += 2;
}

static void dissect_action_output(proto_tree* tree, tvbuff_t *tvb, guint32 *offset)
{
    /* determine the maximum number of bytes to send (0 =>  no limit) */
    guint16 max_bytes = tvb_get_ntohs( tvb, *offset );
    if( max_bytes ) {
        char str[11];
        snprintf( str, 11, "%u", max_bytes );
        add_child_str( tree, ofp_action_output_max_len, tvb, offset, 2, str );
    }
    else
        add_child_str( tree, ofp_action_output_max_len, tvb, offset, 2, "entire packet (no limit)" );

    /* add the output port */
    dissect_port( tree, ofp_action_output_port, tvb, offset );
}

/** returns the number of bytes dissected (-1 if an unknown action type is
 *  encountered; and 12 for all other actions as of 0x83) */
static gint dissect_action(proto_tree* tree, proto_item* item, tvbuff_t *tvb, packet_info *pinfo, guint32 *offset)
{
    proto_item *action_item = proto_tree_add_item(tree, ofp_action, tvb, *offset, sizeof(struct ofp_action), FALSE);
    proto_tree *action_tree = proto_item_add_subtree(action_item, ett_ofp_action);

    guint32 offset_start = *offset;
    guint16 type = tvb_get_ntohs( tvb, *offset );
    add_child( action_tree, ofp_action_type, tvb, offset, 2 );

    /* two bytes of pad follows the type field (not shown in spec doc 0x83) */
    dissect_pad(action_tree, offset, 2);

    switch( type ) {
    case OFPAT_OUTPUT: {
        dissect_action_output(action_tree, tvb, offset);
        dissect_pad(action_tree, offset, 2);
        break;
    }

    case OFPAT_SET_DL_VLAN:
        add_child( action_tree, ofp_action_vlan_id, tvb, offset, 2 );
        dissect_pad(action_tree, offset, 4);
        break;

    case OFPAT_SET_DL_SRC:
    case OFPAT_SET_DL_DST:
        add_child( action_tree, ofp_action_dl_addr, tvb, offset, 6 );
        /* no padding; eth addr uses up all six bytes */
        break;

    case OFPAT_SET_NW_SRC:
    case OFPAT_SET_NW_DST:
        add_child( action_tree, ofp_action_nw_addr, tvb, offset, 4 );
        dissect_pad(action_tree, offset, 2);
        break;

    case OFPAT_SET_TP_SRC:
    case OFPAT_SET_TP_DST:
        add_child( action_tree, ofp_action_tp, tvb, offset, 2 );
        dissect_pad(action_tree, offset, 4);
        break;

    default:
        add_child( action_tree, ofp_action_unknown, tvb, offset, 0 );
        return -1;
    }

    /* two bytes pad at the end of each action (not shown in spec doc 0x83) */
    dissect_pad(action_tree, offset, 2);

    /* return the number of bytes which were consumed */
    return *offset - offset_start;
}

static void dissect_action_array(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, guint len, guint offset)
{
    guint total_len = len - offset;
    proto_item* action_item = proto_tree_add_item(tree, ofp_action_output, tvb, offset, -1, FALSE);
    proto_tree* action_tree = proto_item_add_subtree(action_item, ett_ofp_action_output);

    if( total_len == 0 )
        add_child_str(action_tree, ofp_action_warn, tvb, &offset, 0, "No actions were specified");
    else if( offset > len ) {
        /* not enough bytes => wireshark will already have reported the error */
    }
    else {
        guint offset_action_start = offset;
        guint num_actions = 0;
        while( total_len > 0 ) {
            num_actions += 1;
            int ret = dissect_action(action_tree, action_item, tvb, pinfo, &offset);
            if( ret < 0 )
                break; /* stop if we run into an action we couldn't dissect */
            else
                total_len -= ret;
        }
        proto_tree_add_uint(action_tree, ofp_action_num, tvb, offset_action_start, 0, num_actions);
    }
}

static void dissect_capability_array(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, guint offset, guint field_size) {
    proto_item *sf_cap_item = proto_tree_add_item(tree, ofp_switch_features_capabilities_hdr, tvb, offset, field_size, FALSE);
    proto_tree *sf_cap_tree = proto_item_add_subtree(sf_cap_item, ett_ofp_switch_features_capabilities_hdr);
    gint i;
    for(i=0; i<NUM_CAPABILITIES; i++)
        add_child_const(sf_cap_tree, ofp_switch_features_capabilities[i], tvb, offset, field_size);
}

static void dissect_ethernet(tvbuff_t *next_tvb, packet_info *pinfo, proto_tree *data_tree) {
    /* add seperators to existing column strings */
    if (check_col(pinfo->cinfo, COL_PROTOCOL))
        col_append_str( pinfo->cinfo, COL_PROTOCOL, "+" );

    if(check_col(pinfo->cinfo,COL_INFO))
        col_append_str( pinfo->cinfo, COL_INFO, " => " );

    /* set up fences so ethernet dissectors only appends to our column info */
    col_set_fence(pinfo->cinfo, COL_PROTOCOL);
    col_set_fence(pinfo->cinfo, COL_INFO);

    /* continue the dissection with the ethernet dissector */
    call_dissector(data_ethernet, next_tvb, pinfo, data_tree);
}

static void dissect_openflow_message(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree)
{
#   define STR_LEN 1024
    char str[STR_LEN];

    /* display our protocol text if the protocol column is visible */
    if (check_col(pinfo->cinfo, COL_PROTOCOL))
        col_set_str(pinfo->cinfo, COL_PROTOCOL, PROTO_TAG_OPENFLOW);

    /* Clear out stuff in the info column */
    if(check_col(pinfo->cinfo,COL_INFO))
        col_clear(pinfo->cinfo,COL_INFO);

    /* get some of the header fields' values for later use */
    guint8  ver  = tvb_get_guint8( tvb, 0 );
    guint8  type = tvb_get_guint8( tvb, 1 );
    guint16 len  = tvb_get_ntohs(  tvb, 2 );

    /* add a warning if the version is what the plugin was written to handle */
    guint8 ver_warning = 0;
    if( ver < DISSECTOR_OPENFLOW_MIN_VERSION || ver > DISSECTOR_OPENFLOW_MAX_VERSION || ver >= DISSECTOR_OPENFLOW_VERSION_DRAFT_THRESHOLD ) {
        if( ver>=DISSECTOR_OPENFLOW_VERSION_DRAFT_THRESHOLD && ver<=DISSECTOR_OPENFLOW_MAX_VERSION )
            snprintf( str, STR_LEN, "DRAFT Dissector written for this OpenFlow version v0x%0X", ver );
        else {
            ver_warning = 1;
            if( DISSECTOR_OPENFLOW_MIN_VERSION == DISSECTOR_OPENFLOW_MAX_VERSION )
                snprintf( str, STR_LEN,
                          "Dissector written for OpenFlow v0x%0X (differs from this packet's version v0x%0X)",
                          DISSECTOR_OPENFLOW_MIN_VERSION, ver );
            else
                snprintf( str, STR_LEN,
                          "Dissector written for OpenFlow v0x%0X-v0x%0X (differs from this packet's version v0x%0X)",
                          DISSECTOR_OPENFLOW_MIN_VERSION, DISSECTOR_OPENFLOW_MAX_VERSION, ver );
        }
    }

    /* clarify protocol name display with version, length, and type information */
    if (check_col(pinfo->cinfo, COL_INFO)) {
        /* special handling so we can put buffer IDs in the description */
        char str_extra[32];
        str_extra[0] = '\0';
        if( type==OFPT_PACKET_IN || type==OFPT_PACKET_OUT ) {
            guint32 bid = tvb_get_ntohl(tvb, sizeof(struct ofp_header));
            if( bid != 0xFFFFFFFF )
                snprintf(str_extra, 32, "(BufID=%u) ", bid);
        }

        if( ver_warning )
            col_add_fstr( pinfo->cinfo, COL_INFO, "%s %s(%uB) Ver Warning!", ofp_type_to_string(type), str_extra, len );
        else
            col_add_fstr( pinfo->cinfo, COL_INFO, "%s %s(%uB)", ofp_type_to_string(type), str_extra, len );
    }

    if (tree) { /* we are being asked for details */
        proto_item *item        = NULL;
        proto_item *sub_item    = NULL;
        proto_tree *ofp_tree    = NULL;
        proto_tree *header_tree = NULL;
        guint32 offset = 0;
        proto_item *type_item  = NULL;
        proto_tree *type_tree  = NULL;

        /* consume the entire tvb for the openflow packet, and add it to the tree */
        item = proto_tree_add_item(tree, proto_openflow, tvb, 0, -1, FALSE);
        ofp_tree = proto_item_add_subtree(item, ett_ofp);

        /* put the header in its own node as a child of the openflow node */
        sub_item = proto_tree_add_item( ofp_tree, ofp_header, tvb, offset, -1, FALSE );
        header_tree = proto_item_add_subtree(sub_item, ett_ofp_header);

        if( ver_warning )
            add_child_str( header_tree, ofp_header_warn_ver, tvb, &offset, 0, str );

        /* add the headers field as children of the header node */
        add_child( header_tree, ofp_header_version, tvb, &offset, 1 );
        add_child( header_tree, ofp_header_type,    tvb, &offset, 1 );
        add_child( header_tree, ofp_header_length,  tvb, &offset, 2 );
        add_child( header_tree, ofp_header_xid,     tvb, &offset, 4 );

        switch( type ) {
        case OFPT_FEATURES_REQUEST:
            /* nothing else in this packet type */
            break;

        case OFPT_FEATURES_REPLY: {
            proto_item *sf_ti_item = NULL;
            proto_tree *sf_ti_tree = NULL;
            proto_item *sf_bl_item = NULL;
            proto_tree *sf_bl_tree = NULL;
            proto_item *sf_act_item = NULL;
            proto_tree *sf_act_tree = NULL;
            proto_item *sf_port_item = NULL;
            proto_tree *sf_port_tree = NULL;
            guint i, num_ports;
            gint sz;

            type_item = proto_tree_add_item(ofp_tree, ofp_switch_features, tvb, offset, -1, FALSE);
            type_tree = proto_item_add_subtree(type_item, ett_ofp_switch_features);

            /* fields we'll put directly in the subtree */
            add_child(type_tree, ofp_switch_features_datapath_id, tvb, &offset, 8);

            /* Table info */
            sf_ti_item = proto_tree_add_item(type_tree, ofp_switch_features_table_info_hdr, tvb, offset, 12, FALSE);
            sf_ti_tree = proto_item_add_subtree(sf_ti_item, ett_ofp_switch_features_table_info_hdr);
            add_child(sf_ti_tree, ofp_switch_features_n_exact, tvb, &offset, 4);
            add_child(sf_ti_tree, ofp_switch_features_n_compression, tvb, &offset, 4);
            add_child(sf_ti_tree, ofp_switch_features_n_general, tvb, &offset, 4);

            /* Buffer limits */
            sf_bl_item = proto_tree_add_item(type_tree, ofp_switch_features_buffer_limits_hdr, tvb, offset, 8, FALSE);
            sf_bl_tree = proto_item_add_subtree(sf_bl_item, ett_ofp_switch_features_buffer_limits_hdr);
            add_child(sf_bl_tree, ofp_switch_features_buffer_mb, tvb, &offset, 4);
            add_child(sf_bl_tree, ofp_switch_features_n_buffers, tvb, &offset, 4);

            /* capabilities */
            dissect_capability_array(tvb, pinfo, type_tree, offset, 4);
            offset += 4;

            /* actions */
            sf_act_item = proto_tree_add_item(type_tree, ofp_switch_features_actions_hdr, tvb, offset, 4, FALSE);
            sf_act_tree = proto_item_add_subtree(sf_act_item, ett_ofp_switch_features_actions_hdr);
            if( ver < 0x90 ) {
                /* add warning: meaningless until v0x90 */
                add_child_const(sf_act_tree, ofp_switch_features_actions_warn, tvb, offset, 4);
            }
            for(i=0; i<NUM_ACTIONS; i++) {
                add_child_const(sf_act_tree, ofp_switch_features_actions[i], tvb, offset, 4);
            }
            offset += 4;

            /* pad */
            if (OFP_VERSION >= 0x85) {
                dissect_pad(type_tree, &offset, 4);
            }

            /* handle ports */
            sf_port_item = proto_tree_add_item(type_tree, ofp_switch_features_ports_hdr, tvb, offset, -1, FALSE);
            sf_port_tree = proto_item_add_subtree(sf_port_item, ett_ofp_switch_features_ports_hdr);
            sz = len - sizeof(struct ofp_switch_features);
            if( sz > 0 ) {
                num_ports = sz / sizeof(struct ofp_phy_port); /* number of ports */
                proto_tree_add_uint(sf_port_tree, ofp_switch_features_ports_num, tvb, offset, num_ports*sizeof(struct ofp_phy_port), num_ports);

                dissect_phy_ports(sf_port_tree, sf_port_item, tvb, pinfo, &offset, num_ports);
                if( num_ports * sizeof(struct ofp_phy_port) < sz ) {
                    snprintf(str, STR_LEN, "%uB were leftover at end of packet", sz - num_ports*sizeof(struct ofp_phy_port));
                    add_child_str(sf_port_tree, ofp_switch_features_ports_warn, tvb, &offset, 0, str);
                }
            }
            else if( sz < 0 ) {
                /* not enough bytes => wireshark will already have reported the error */
            }
            else {
                snprintf(str, STR_LEN, "No ports were specified");
                add_child_str(sf_port_tree, ofp_switch_features_ports_warn, tvb, &offset, 0, str);
            }
            break;
        }

        case OFPT_GET_CONFIG_REQUEST:
            /* nothing else in this packet type */
            break;

        case OFPT_GET_CONFIG_REPLY:
        case OFPT_SET_CONFIG: {
            type_item = proto_tree_add_item(ofp_tree, ofp_switch_config, tvb, offset, -1, FALSE);
            type_tree = proto_item_add_subtree(type_item, ett_ofp_switch_config);
            dissect_capability_array(tvb, pinfo, type_tree, offset, 2);
            offset += 2;
            add_child(type_tree, ofp_switch_config_miss_send_len, tvb, &offset, 2);
            break;
        }

        case OFPT_PACKET_IN: {
            type_item = proto_tree_add_item(ofp_tree, ofp_packet_in, tvb, offset, -1, FALSE);
            type_tree = proto_item_add_subtree(type_item, ett_ofp_packet_in);

            add_child(type_tree, ofp_packet_in_buffer_id, tvb, &offset, 4);

            /* explicitly pull out the length so we can use it to determine data's size */
            guint16 total_len = tvb_get_ntohs( tvb, offset );
            proto_tree_add_uint(type_tree, ofp_packet_in_total_len, tvb, offset, 2, total_len);
            offset += 2;

            add_child(type_tree, ofp_packet_in_in_port, tvb, &offset, 2);
            add_child(type_tree, ofp_packet_in_reason, tvb, &offset, 1);
            dissect_pad(type_tree, &offset, 1);

            /* continue the dissection with the Ethernet dissector */
            if( data_ethernet ) {
                proto_item *data_item = proto_tree_add_item(type_tree, ofp_packet_in_data_hdr, tvb, offset, -1, FALSE);
                proto_tree *data_tree = proto_item_add_subtree(data_item, ett_ofp_packet_in_data_hdr);
                tvbuff_t *next_tvb = tvb_new_subset(tvb, offset, -1, total_len);
                dissect_ethernet(next_tvb, pinfo, data_tree);
            }
            else {
                /* if we couldn't load the ethernet dissector, just display the bytes */
                add_child(type_tree, ofp_packet_in_data_hdr, tvb, &offset, total_len);
            }

            break;
        }

        case OFPT_PACKET_OUT: {
            type_item = proto_tree_add_item(ofp_tree, ofp_packet_out, tvb, offset, -1, FALSE);
            type_tree = proto_item_add_subtree(type_item, ett_ofp_packet_out);

            /* explicitly pull out the buffer id so we can use it to determine
               what the last field is */
            guint32 buffer_id = tvb_get_ntohl( tvb, offset );
            if( buffer_id == 0xFFFFFFFF )
                add_child_str(type_tree, ofp_packet_out_buffer_id, tvb, &offset, 4, "None");
            else {
                snprintf(str, STR_LEN, "%u", buffer_id);
                add_child_str(type_tree, ofp_packet_out_buffer_id, tvb, &offset, 4, str);
            }

            /* check whether in_port exists */
            dissect_port(type_tree, ofp_packet_out_in_port,  tvb, &offset);
            dissect_port(type_tree, ofp_packet_out_out_port, tvb, &offset);

            if( buffer_id == -1 ) {
                /* continue the dissection with the Ethernet dissector */
                guint total_len = len - offset;
                if( data_ethernet ) {
                    proto_item *data_item = proto_tree_add_item(type_tree, ofp_packet_out_data_hdr, tvb, offset, -1, FALSE);
                    proto_tree *data_tree = proto_item_add_subtree(data_item, ett_ofp_packet_out_data_hdr);
                    tvbuff_t *next_tvb = tvb_new_subset(tvb, offset, -1, total_len);
                    dissect_ethernet(next_tvb, pinfo, data_tree);
                }
                else {
                    /* if we couldn't load the ethernet dissector, just display the bytes */
                    add_child(type_tree, ofp_packet_out_data_hdr, tvb, &offset, total_len);
                }
            }
            else {
                /* handle actions */
                dissect_action_array(tvb, pinfo, type_tree, len, offset);
            }

            break;
        }

        case OFPT_FLOW_MOD: {
            type_item = proto_tree_add_item(ofp_tree, ofp_flow_mod, tvb, offset, -1, FALSE);
            type_tree = proto_item_add_subtree(type_item, ett_ofp_flow_mod);

            dissect_match(type_tree, type_item, tvb, pinfo, &offset);
            add_child(type_tree, ofp_flow_mod_command, tvb, &offset, 2);
            add_child(type_tree, ofp_flow_mod_max_idle, tvb, &offset, 2);
            add_child(type_tree, ofp_flow_mod_buffer_id, tvb, &offset, 4);
            add_child(type_tree, ofp_flow_mod_priority, tvb, &offset, 2);
            dissect_pad(type_tree, &offset, 2);
            add_child(type_tree, ofp_flow_mod_reserved, tvb, &offset, 4);
            dissect_action_array(tvb, pinfo, type_tree, len, offset);
            break;
        }

        case OFPT_FLOW_EXPIRED: {
            type_item = proto_tree_add_item(ofp_tree, ofp_flow_expired, tvb, offset, -1, FALSE);
            type_tree = proto_item_add_subtree(type_item, ett_ofp_flow_expired);

            dissect_match(type_tree, type_item, tvb, pinfo, &offset);
            add_child(type_tree, ofp_flow_expired_priority, tvb, &offset, 2);
            dissect_pad(type_tree, &offset, 2);
            add_child(type_tree, ofp_flow_expired_duration, tvb, &offset, 4);
            if (OFP_VERSION >= 0x85) {
                dissect_pad(type_tree, &offset, 2);
            }
            add_child(type_tree, ofp_flow_expired_packet_count, tvb, &offset, 8);
            add_child(type_tree, ofp_flow_expired_byte_count, tvb, &offset, 8);
            break;
        }

        case OFPT_TABLE: {
            /* add a warning: this type is not yet specified */
            snprintf(str, STR_LEN, "Dissector does not dissect type %u (OFPT_TABLE not specified yet)", type);
            add_child_str(tree, ofp_header_warn_type, tvb, &offset, len - offset, str);
            break;
        }

        case OFPT_PORT_MOD: {
            type_item = proto_tree_add_item(ofp_tree, ofp_port_mod, tvb, offset, -1, FALSE);
            type_tree = proto_item_add_subtree(type_item, ett_ofp_port_mod);

            dissect_phy_ports(type_tree, type_item, tvb, pinfo, &offset, 1);
            break;
        }

        case OFPT_PORT_STATUS: {
            type_item = proto_tree_add_item(ofp_tree, ofp_port_status, tvb, offset, -1, FALSE);
            type_tree = proto_item_add_subtree(type_item, ett_ofp_port_status);

            add_child(type_tree, ofp_port_status_reason, tvb, &offset, 1);
            dissect_pad(type_tree, &offset, 3);
            dissect_phy_ports(type_tree, type_item, tvb, pinfo, &offset, 1);
            break;
        }

        case OFPT_STATS_REQUEST: {
            type_item = proto_tree_add_item(ofp_tree, ofp_stats_request, tvb, offset, -1, FALSE);
            type_tree = proto_item_add_subtree(type_item, ett_ofp_stats_request);

            guint16 type = tvb_get_ntohs( tvb, offset );
            add_child(type_tree, ofp_stats_request_type, tvb, &offset, 2);
            add_child(type_tree, ofp_stats_request_flags, tvb, &offset, 2);

            switch( type ) {
            case OFPST_FLOW: {
                proto_item *flow_item = proto_tree_add_item(type_tree, ofp_flow_stats_request, tvb, offset, -1, FALSE);
                proto_tree *flow_tree = proto_item_add_subtree(flow_item, ett_ofp_flow_stats_request);

                dissect_match(flow_tree, flow_item, tvb, pinfo, &offset);

                guint8 id = tvb_get_guint8( tvb, offset );
                if( id == 0xFF )
                    add_child_str(flow_tree, ofp_flow_stats_request_table_id, tvb, &offset, 1, "All Tables");
                else {
                    snprintf(str, STR_LEN, "%u", id);
                    add_child_str(flow_tree, ofp_flow_stats_request_table_id, tvb, &offset, 1, str);
                }

                dissect_pad(flow_tree, &offset, 3);
                break;
            }

            case OFPST_AGGREGATE: {
                proto_item *aggr_item = proto_tree_add_item(type_tree, ofp_aggr_stats_request, tvb, offset, -1, FALSE);
                proto_tree *aggr_tree = proto_item_add_subtree(aggr_item, ett_ofp_aggr_stats_request);

                dissect_match(aggr_tree, aggr_item, tvb, pinfo, &offset);

                guint8 id = tvb_get_guint8( tvb, offset );
                if( id == 0xFF )
                    add_child_str(aggr_tree, ofp_aggr_stats_request_table_id, tvb, &offset, 1, "All Tables");
                else {
                    snprintf(str, STR_LEN, "%u", id);
                    add_child_str(aggr_tree, ofp_aggr_stats_request_table_id, tvb, &offset, 1, str);
                }

                dissect_pad(aggr_tree, &offset, 3);
                break;
            }

            case OFPST_TABLE:
            case OFPST_PORT:
                /* no body for these types of requests */
                break;

            default:
                /* add as bytes if type isn't one we know how to dissect */
                add_child(type_tree, ofp_stats_request_body, tvb, &offset, len - offset);
            }

            break;
        }

        case OFPT_STATS_REPLY: {
            type_item = proto_tree_add_item(ofp_tree, ofp_stats_reply, tvb, offset, -1, FALSE);
            type_tree = proto_item_add_subtree(type_item, ett_ofp_stats_reply);

            guint16 type = tvb_get_ntohs( tvb, offset );
            add_child(type_tree, ofp_stats_reply_type, tvb, &offset, 2);
            add_child(type_tree, ofp_stats_reply_flags, tvb, &offset, 2);

            switch( type ) {
            case OFPST_FLOW: {
                /* process each flow stats struct in the packet */
                while( offset < len ) {
                    proto_item* flow_item = proto_tree_add_item(type_tree, ofp_flow_stats_reply, tvb, offset, -1, FALSE);
                    proto_tree* flow_tree = proto_item_add_subtree(flow_item, ett_ofp_flow_stats_reply);

                    /* just get the length of this part of the packet; no need
                       to put it in the tree */
                    guint16 total_len = tvb_get_ntohs( tvb, offset );
                    guint offset_start = offset;
                    offset += 2;

                    add_child(flow_tree, ofp_flow_stats_reply_table_id, tvb, &offset, 2);
                    dissect_pad(flow_tree, &offset, 1);
                    dissect_match(flow_tree, flow_item, tvb, pinfo, &offset);
                    add_child(flow_tree, ofp_flow_stats_reply_duration, tvb, &offset, 4);
                    if (OFP_VERSION <= 0x84) {
                        add_child(flow_tree, ofp_flow_stats_reply_packet_count, tvb, &offset, 8);
                        add_child(flow_tree, ofp_flow_stats_reply_byte_count, tvb, &offset, 8);
                    }
                    add_child(flow_tree, ofp_flow_stats_reply_priority, tvb, &offset, 2);
                    add_child(flow_tree, ofp_flow_stats_reply_max_idle, tvb, &offset, 2);

                    if (OFP_VERSION >= 0x85) {
                        add_child(flow_tree, ofp_flow_stats_reply_packet_count, tvb, &offset, 8);
                        add_child(flow_tree, ofp_flow_stats_reply_byte_count, tvb, &offset, 8);
                    }

                    /* parse the actions for this flow */
                    dissect_action_array(tvb, pinfo, flow_tree, total_len + offset_start, offset);
                }
                break;
            }

            case OFPST_AGGREGATE: {
                proto_item* aggr_item = proto_tree_add_item(type_tree, ofp_aggr_stats_reply, tvb, offset, -1, FALSE);
                proto_tree* aggr_tree = proto_item_add_subtree(aggr_item, ett_ofp_aggr_stats_reply);

                add_child(aggr_tree, ofp_aggr_stats_reply_packet_count, tvb, &offset, 8);
                add_child(aggr_tree, ofp_aggr_stats_reply_byte_count, tvb, &offset, 8);
                add_child(aggr_tree, ofp_aggr_stats_reply_flow_count, tvb, &offset, 4);

                if (OFP_VERSION >= 0x85) {
                    dissect_pad(aggr_tree, &offset, 4);
                }
                break;
            }

            case OFPST_TABLE: {
                /* process each table stats struct in the packet */
                while( offset < len ) {
                    proto_item *table_item = proto_tree_add_item(type_tree, ofp_table_stats, tvb, offset, -1, FALSE);
                    proto_tree *table_tree = proto_item_add_subtree(table_item, ett_ofp_table_stats);

                    add_child(table_tree, ofp_table_stats_table_id, tvb, &offset, 1);
                    dissect_pad(table_tree, &offset, 3);
                    add_child(table_tree, ofp_table_stats_name, tvb, &offset, OFP_MAX_TABLE_NAME_LEN);
                    add_child(table_tree, ofp_table_stats_max_entries, tvb, &offset, 4);
                    add_child(table_tree, ofp_table_stats_active_count, tvb, &offset, 4);
                    if (OFP_VERSION >= 0x85) {
                        dissect_pad(table_tree, &offset, 2);
                    }
                    add_child(table_tree, ofp_table_stats_matched_count, tvb, &offset, 8);
                }
                break;
            }

            case OFPST_PORT: {
                /* process each port stats struct in the packet */
                while( offset < len ) {
                    proto_item *port_item = proto_tree_add_item(type_tree, ofp_port_stats, tvb, offset, -1, FALSE);
                    proto_tree *port_tree = proto_item_add_subtree(port_item, ett_ofp_port_stats);

                    dissect_port(port_tree, ofp_port_stats_port_no, tvb, &offset);
                    if (OFP_VERSION <= 0x84) {
                        dissect_pad(port_tree, &offset, 2);
                    }
                    else if (OFP_VERSION >= 0x85) {
                        dissect_pad(port_tree, &offset, 6);
                    }
                    add_child(port_tree, ofp_port_stats_rx_count, tvb, &offset, 8);
                    add_child(port_tree, ofp_port_stats_tx_count, tvb, &offset, 8);
                    add_child(port_tree, ofp_port_stats_drop_count, tvb, &offset, 8);
                }
                break;
            }

            default:
                /* add as bytes if type isn't one we know how to dissect */
                add_child(type_tree, ofp_stats_reply_body, tvb, &offset, len - offset);
            }

            break;
        }

        case OFPT_ERROR_MSG: {
            type_item = proto_tree_add_item(ofp_tree, ofp_error_msg, tvb, offset, -1, FALSE);
            type_tree = proto_item_add_subtree(type_item, ett_ofp_error_msg);

            add_child(type_tree, ofp_error_msg_type, tvb, &offset, 2);
            add_child(type_tree, ofp_error_msg_code, tvb, &offset, 2);
            add_child(type_tree, ofp_error_msg_data, tvb, &offset, len - offset);
            break;
        }

        default:
            /* add a warning if we encounter an unrecognized packet type */
            snprintf(str, STR_LEN, "Dissector does not recognize type %u", type);
            add_child_str(tree, ofp_header_warn_type, tvb, &offset, len - offset, str);
        }
    }
}

static void dissect_openflow(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree)
{
    /* have wireshark reassemble our PDUs; call dissect_openflow_when full PDU assembled */
    tcp_dissect_pdus(tvb, pinfo, tree, TRUE, 4, get_openflow_message_len, dissect_openflow_message);
}
