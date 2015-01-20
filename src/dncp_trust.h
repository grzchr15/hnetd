/*
 * $Id: dncp_trust.h $
 *
 * Author: Markus Stenberg <markus stenberg@iki.fi>
 *
 * Copyright (c) 2014 cisco Systems, Inc.
 *
 * Created:       Thu Nov 20 11:46:44 2014 mstenber
 * Last modified: Tue Jan 20 15:25:17 2015 mstenber
 * Edit time:     21 min
 *
 */

#pragma once

#include "dncp.h"
#include "dncp_proto.h"

typedef struct dncp_trust_struct dncp_trust_s, *dncp_trust;

dncp_trust dncp_trust_create(dncp o, const char *filename);
void dncp_trust_destroy(dncp_trust t);

/*
 * Get effective trust verdict for the hash. This operation is
 * immediate and does NOT result in state changes in DNCP.
 *
 * The cname is also retrieved if available and the pointer is
 * non-NULL; the size of the pointed buffer must be >=
 * DNCP_T_TRUST_VERDICT_CNAME_LEN.
 */
int dncp_trust_get_verdict(dncp_trust t, const dncp_sha256 h, char *cname);

/*
 * Publish the request for a verdict into DNCP.
 */
void dncp_trust_request_verdict(dncp_trust t,
                                const dncp_sha256 h,
                                const char *cname);
/*
 * Add/Update local configured trust to have this particular entry
 * too.
 */
void dncp_trust_set(dncp_trust t, const dncp_sha256 h,
                    uint8_t verdict, const char *cname);

/*
 * Find next hash available. NULL's next is first hash, so this is a
 * looping iterator.
 */
dncp_sha256 dncp_trust_next_hash(dncp_trust t, const dncp_sha256 prev);

#define dncp_trust_for_each_hash(t, h) \
  for (h = dncp_trust_next_hash(t, NULL) ; h ; h = dncp_trust_next_hash(t, h))
