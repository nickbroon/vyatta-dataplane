/*
 * Copyright (c) 2019-2021, AT&T Intellectual Property.  All rights reserved.
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

/**
 * @file cgn.c - CGNAT global variables and event handlers.
 */

#include <errno.h>
#include <netinet/in.h>
#include <linux/if.h>
#include <rte_log.h>

#include "compiler.h"
#include "if_var.h"
#include "util.h"
#include "dp_event.h"
#include "vplane_log.h"

#include "npf/cgnat/cgn.h"
#include "npf/cgnat/cgn_rc.h"
#include "npf/apm/apm.h"
#include "npf/cgnat/cgn_cmd_cfg.h"
#include "npf/cgnat/cgn_if.h"
#include "npf/cgnat/cgn_policy.h"
#include "npf/cgnat/cgn_session.h"
#include "npf/cgnat/cgn_source.h"
#include "npf/cgnat/cgn_log.h"
#include "npf/nat/nat_pool_event.h"
#include "npf/nat/nat_pool_public.h"


/**************************************************************************
 * CGNAT Global Variables
 **************************************************************************/

/* Hairpinning config enable/disable */
bool cgn_hairpinning_gbl = true;

/* snat-alg-bypass config enable/disable */
bool cgn_snat_alg_bypass_gbl;

/*
 * Simple global counts for the number of dest addr (sess2) hash tables
 * created and destroyed.  These URCU hash tables are fairly resource
 * intensive, so we want to get some idea of how often they are required.
 */
rte_atomic64_t cgn_sess2_ht_created;
rte_atomic64_t cgn_sess2_ht_destroyed;

/* max 3-tuple sessions, and sessions used */
int32_t cgn_sessions_max = CGN_SESSIONS_MAX;

/*
 * Count of all 3-tuple sessions.  Incremented and compared against
 * cgn_sessions_max before a 3-tuple session is created.  If it exceeds
 * cgn_sessions_max then cgn_session_table_full is set true.  It is
 * decremented by the GC routine a time after the session has expired.
 */
rte_atomic32_t cgn_sessions_used;

/* max 2-tuple sessions per 3-tuple session*/
int16_t cgn_dest_sessions_max = CGN_DEST_SESSIONS_INIT;

/* Size of 2-tuple hash table that may be added per 3-tuple session */
int16_t cgn_dest_ht_max = CGN_DEST_SESSIONS_INIT;

/* Global count of all 5-tuple sessions */
rte_atomic32_t cgn_sess2_used;

/* Set true when table is full.  Re-evaluated after GC. */
bool cgn_session_table_full;

/* Is CGNAT helper core enabled? */
uint8_t cgn_helper_thread_enabled;


/**************************************************************************
 * CGNAT Event Handlers
 **************************************************************************/

/*
 * NAT pool has been de-activated.  Clear all sessions and mappings that
 * derive from this nat pool.
 */
static void cgn_np_inactive(struct nat_pool *np)
{
	if (nat_pool_type_is_cgnat(np))
		cgn_session_expire_pool(true, np, true);
}

/* NAT pool event handlers */
static const struct np_event_ops cgn_np_event_ops = {
	.np_inactive = cgn_np_inactive,
};

/* Register with NAT pool event handler */
static void cgn_nat_pool_event_init(void)
{
	if (!nat_pool_event_register(&cgn_np_event_ops))
		RTE_LOG(ERR, CGNAT, "Failed to register with NAT pool\n");
}

/*
 * DP_EVT_INIT event handler
 */
static void cgn_init(void)
{
	cgn_rc_init();
	cgn_nat_pool_event_init();
	cgn_policy_init();
	cgn_session_init();
	cgn_source_init();
	apm_init();
}

/*
 * DP_EVT_UNINIT event handler
 */
static void cgn_uninit(void)
{
	cgn_session_uninit();
	apm_uninit();
	cgn_source_uninit();
	cgn_policy_uninit();
	cgn_log_disable_all_handlers();
	cgn_rc_uninit();
}

/*
 * Callback for dataplane DP_EVT_IF_INDEX_UNSET event.
 */
static void
cgn_event_if_index_unset(struct ifnet *ifp, uint32_t ifindex __unused)
{
	/*
	 * For each policy on interface:
	 *  1. Clear sessions,
	 *  2. Remove policy from cgn_if list
	 *  3. Remove policy from hash table
	 *  4. Release reference on policy
	 * Free cgn_if
	 */
	cgn_if_disable(ifp);
}

/*
 * CGNAT Event Handler
 */
static const struct dp_event_ops cgn_event_ops = {
	.init = cgn_init,
	.uninit = cgn_uninit,
	.if_index_unset = cgn_event_if_index_unset,
};

/* Register event handler */
DP_STARTUP_EVENT_REGISTER(cgn_event_ops);
