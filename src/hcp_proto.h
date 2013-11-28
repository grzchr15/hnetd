/*
 * $Id: hcp_proto.h $
 *
 * Author: Markus Stenberg <mstenber@cisco.com>
 *
 * Copyright (c) 2013 cisco Systems, Inc.
 *
 * Created:       Wed Nov 27 18:17:46 2013 mstenber
 * Last modified: Wed Nov 27 20:22:12 2013 mstenber
 * Edit time:     4 min
 *
 */

#ifndef HCP_PROTO_H
#define HCP_PROTO_H

/******************************** Not standardized, but hopefully one day..  */

/* Let's assume we use MD5 for the time being.. */
#define HCP_HASH_LEN 16

/* 64 bit version of the hash */
#define HCP_HASH64_LEN 8

/* When do we start to worry about the other side.. */
#define HCP_INTERVAL_WORRIED (60 * HNETD_TIME_PER_SECOND)

/* Exponentially backed off retries to 'ping' other side somehow
 * ( either within protocol or without protocol ) to hear from them.
 */
#define HCP_INTERVAL_RETRIES 3


/******************************************************************* TLV T's */

enum {
  /* Request TLVs (not to be really stored anywhere) */
  HCP_T_REQ_NET_HASH = 1, /* empty */
  HCP_T_REQ_NODE_DATA = 5, /* = just normal hash */

  /* This should be included in every message to facilitate neighbor
   * discovery of peers. */
  HCP_T_LINK_ID = 3,

  HCP_T_NETWORK_HASH = 2, /* = just normal hash */
 /* not implemented */
  HCP_T_NODE_STATE = 4,

  HCP_T_NODE_DATA = 6,
  HCP_T_NODE_DATA_KEY = 7, /* public key payload, not implemented*/
  HCP_T_NODE_DATA_NEIGHBOR = 8,

  HCP_T_CUSTOM = 9, /* not implemented */

  HCP_T_SIGNATURE = 0xFFFF /* not implemented */
};

#define TLV_SIZE sizeof(struct tlv_attr)

/* HCP_T_LINK_ID */
typedef struct __packed {
  unsigned char node_identifier_hash[HCP_HASH_LEN];
  uint32_t link_id;
} hcp_t_link_id_s, *hcp_t_link_id;

/* HCP_T_NODE_STATE */
typedef struct __packed {
  unsigned char node_identifier_hash[HCP_HASH_LEN];
  uint32_t update_number;
  uint32_t seconds_since_origination;
  unsigned char node_data_hash[HCP_HASH_LEN];
} hcp_t_node_state_s, *hcp_t_node_state;

/* HCP_T_NODE_DATA */
typedef struct __packed {
  unsigned char node_identifier_hash[HCP_HASH_LEN];
  uint32_t update_number;
} hcp_t_node_data_header_s, *hcp_t_node_data_header;

/* HCP_T_NODE_DATA_NEIGHBOR */
typedef struct __packed {
  unsigned char neighbor_node_identifier_hash[HCP_HASH_LEN];
  uint32_t neighbor_link_id;
  uint32_t link_id;
} hcp_t_node_data_neighbor_s, *hcp_t_node_data_neighbor;

/**************************************************************** Addressing */

#define HCP_PORT 8808
#define HCP_MCAST_GROUP "ff02::8808"

/************** Various tunables, that we in practise hardcode (not options) */

/* How often we retry multicast joins? Once per second seems sane
 * enough. */
#define HCP_REJOIN_INTERVAL (1 * HNETD_TIME_PER_SECOND)

/* Minimum interval trickle starts at. The first potential time it may
 * send something is actually this divided by two. */
#define HCP_TRICKLE_IMIN (HNETD_TIME_PER_SECOND / 4)

/* Note: This is concrete value, NOT exponent # as noted in RFC. I
 * don't know why RFC does that.. We don't want to ever need do
 * exponentiation in any case in code. 64 seconds for the time being.. */
#define HCP_TRICKLE_IMAX (64 * HNETD_TIME_PER_SECOND)

/* Redundancy constant. */
#define HCP_TRICKLE_K 1



#endif /* HCP_PROTO_H */