/* Loglevel redefinition */
#define PA_L_LEVEL 7

#ifdef PA_L_LEVEL
#ifdef L_LEVEL
#undef L_LEVEL
#endif
#define L_LEVEL PA_L_LEVEL
#endif

#ifdef L_PREFIX
#undef L_PREFIX
#endif
#define L_PREFIX "pa - "

#include "pa.h"

#include "hnetd.h"

#include <stdlib.h>
#include <string.h>
#include <libubox/list.h>
#include <libubox/avl.h>

/* Different algorithm flavours */
#define PA_ALGO_ARKKO	0
#define PA_ALGO_PFISTER	1

#define PA_ALGO PA_ALGO_PFISTER

#ifndef PA_ALGO
#error "No prefix assignment algorithm defined"
#elif (PA_ALGO > 1 && PA_ALGO < 0)
#error "Invalid prefix assignment algorithm"
#endif



/* #of ms waiting when we want immediate pa run */
#define PA_SCHEDULE_RUNNEXT_MS  10

#define PA_PREFIX_SEARCH_MAX_ROUNDS 128

#define PA_RIDCMP(r1, r2) memcmp((r1)->id, (r2)->id, PA_RIDLEN)

#define PA_CONF_DFLT_FLOODING_DELAY    15  * HNETD_TIME_PER_SECOND

#define PA_ASSIGN_DELAY(flooding_delay) (2 * (flooding_delay))
#define PA_LOCAL_DELAY(flooding_delay)  (2 * (flooding_delay))

#define PA_CONF_DFLT_LOCAL_VALID       600 * HNETD_TIME_PER_SECOND
#define PA_CONF_DFLT_LOCAL_PREFERRED   300 * HNETD_TIME_PER_SECOND
#define PA_CONF_DFLT_LOCAL_UPDATE      330 * HNETD_TIME_PER_SECOND

#define PA_CONF_DFLT_USE_ULA             1
#define PA_CONF_DFLT_NO_ULA_IF_V6        1
#define PA_CONF_DFLT_USE_V4              1
#define PA_CONF_DFLT_NO_V4_IF_V6         0
#define PA_CONF_DFLT_USE_RDM_ULA         1
#define PA_CONF_DFLT_ULA_RDM_PLEN        48

/* 10/8 */
static struct prefix PA_CONF_DFLT_V4 = {
	.prefix = { .s6_addr = {
			0x00,0x00, 0x00,0x00,  0x00,0x00, 0x00,0x00,
			0x00,0x00, 0xff,0xff,  0x0a }},
	.plen = 104 };

/* PA's interface structure.
 * We don't only care about internal because hcp could
 * possibly provide eaps on external interfaces. */
struct pa_iface {
	struct list_head le;   /* Linked in pa's ifaces list */
	char ifname[IFNAMSIZ]; /* Interface name */

	bool internal;         /* Whether the iface is internal */
	bool do_dhcp;          /* Whether we should do dhcp on this interface */
	bool designated;       /* Whether we are designated router. */

	struct list_head laps; /* laps on that iface */
	struct list_head eaps; /* eaps on that iface */
	struct list_head dps;  /* dps on that iface */
};

/* Delayed actions */
struct pa_lap_delayed {
	/* When to trigger action or 0
	 * if not delayed action */
	hnetd_time_t delete_time;
	hnetd_time_t flooding_time;
	hnetd_time_t assign_time;

	/* The value to set */
	bool flooding_value;
	bool assign_value;

	struct uloop_timeout timeout;
};

/* Locally assigned prefix */
struct pa_lap {
	struct avl_node avl_node; /* Put in pa's lap tree */
	struct prefix prefix;	  /* The assigned prefix */

	struct pa_iface *iface;   /* Iface for that lap */
	struct list_head if_le;   /* Linked in iface list */

	struct pa_dp *dp;		  /* The used delegated prefix*/
	struct list_head dp_le;   /* Linked in dp list */

	struct pa *pa;            /* Need that because of timeout callback */

	bool flooded;             /* Whether it was given to hcp */
	bool assigned;            /* Whether it was assigned */

	/* Used by pa algo. marked true before running.
	 * Checked against at the end of pa algo. */
	bool invalid;

	/* Whether we are the one that is currently owner
	 * of that assignment (only one per link) */
	bool own;

	struct pa_lap_delayed delayed;
};

/* Externally assigned prefix */
struct pa_eap {
	struct avl_node avl_node; /* Put in pa's eap tree */
	struct prefix prefix;     /* The assigned prefix */
	struct pa_rid rid;        /* Sender's router id */

	struct pa_iface *iface;   /* Iface for that eap (or NULL) */
	struct list_head if_le;   /* Linked in iface list (or not) */
};

/* Management of ULA addresses and IPv4 */
struct pa_local_elem {
	hnetd_time_t create_start;
	hnetd_time_t timeout;
	struct pa_dp *dp;

#define PA_LOCAL_CAN_CREATE 0x01
#define PA_LOCAL_CAN_KEEP	0x02
	uint8_t (*get_status)(struct pa *, struct pa_local_elem *);
	void (*create)(struct pa *, struct pa_local_elem *);
	hnetd_time_t (*update)(struct pa *pa, struct pa_local_elem *elem, hnetd_time_t now);
};

/* A delegated prefix (for both ldp and edp) */
struct pa_dp {
	struct list_head le;          /* Linked in pa's dp tree */
	struct prefix prefix;         /* The delegated prefix */
	hnetd_time_t valid_until;     /* Valid until (zero means not valid) */
	hnetd_time_t preferred_until;

	struct list_head laps;        /* laps attached to that prefix */
	bool local;                   /* Whether we own that assignment */
	struct pa_rid rid;            /* If not local, source rid */

	bool excluded_valid;          /* Whether there are an excluded prefix */
	struct prefix excluded;

	size_t dhcpv6_len;            /* dhcpv6 data length (or 0) */
	void *dhcpv6_data;      /* dhcpv6 data (or NULL) */

	struct pa_iface *iface;       /* When local, delegating side iface */
	struct list_head if_le;       /* When iface is not null, this is linked in iface dps */

	struct pa_local_elem *l;           /* non-null when generated by the local element */
};

struct pa_local {
	struct pa_local_elem ula;
	struct pa_local_elem ipv4;

	hnetd_time_t current_timeout;
	struct uloop_timeout timeout;
};

struct pa_ipv4 {
	bool available;
	void *dhcp_data;
	size_t dhcp_len;
};

struct pa {
	struct pa_conf conf;

	struct avl_tree laps; /* Locally assigned prefixes */
	struct avl_tree eaps; /* Externally assigned prefixes */

	struct list_head dps; /* Delegated prefixes list */

	struct list_head ifaces; /* List of interfaces known by pa */

	struct pa_rid rid; /* Our router id */

	struct pa_flood_callbacks fcb;  /* hcp callbacks */
	struct pa_iface_callbacks ifcb; /* iface callbacks */

	struct iface_user ifu; /* Subscriber to ifaces callbacks */

	bool started;   /* Whether the pa is started */
	bool scheduled; /* Schedule time */
	struct uloop_timeout pa_short_timeout; /* PA short delay schedule */

	hnetd_time_t pa_dp_when; /* When the dp event is scheduled */
	struct uloop_timeout pa_dp_timeout; /* For dp events */
/* Ways of optimizing the pa algorithm */
#define PA_TODO_ALL    0xffff
	uint32_t todo_flags;

	struct pa_local local;
	struct pa_ipv4 ipv4;
};

#define PA_EAP_L				 "eap '%s'@"PA_RID_L
#define PA_EAP_LA(eap) 	PREFIX_REPR(&(eap)->prefix), PA_RID_LA(&(eap)->rid)

#define PA_IF_L 	"pa_iface '%s'"
#define PA_IF_LA(iface)	(iface)?(iface)->ifname:"NULL"

#define PA_LAP_L				"lap %s%%%s"
#define PA_LAP_LA(lap)	PREFIX_REPR(&(lap)->prefix), (lap)->iface->ifname

#define PA_DP_L				"dp %s(local=%d)"
#define PA_DP_LA(dp)	PREFIX_REPR(&(dp)->prefix), (dp)->local

/**************************************************************/
/*********************** Prototypes ***************************/
/**************************************************************/
static void pa_do(struct pa *pa);
static void pa_eap_iface_assign(struct pa *pa, struct pa_eap *eap, struct pa_iface *iface);
static void pa_lap_destroy(struct pa *pa, struct pa_lap *lap);
static int pa_dp_iface_assign(struct pa *pa, struct pa_dp *dp, struct pa_iface *iface);
static void pa_storage_pushprefix(struct pa *pa, struct pa_iface *iface,
		const struct prefix *prefix);
static void pa_local_elem_destroy(struct pa *pa, struct pa_local_elem *elem);

/**************************************************************/
/*********************** Utilities ****************************/
/**************************************************************/

/* avl_tree key comparator */
static int pa_avl_prefix_cmp (const void *k1, const void *k2,
		__attribute__((unused))void *ptr)
{
	int i = prefix_cmp((struct prefix *)k1, (struct prefix *)k2);
	if(!i)
		return 0;
	return (i>0)?1:-1;
}
/*
static int pa_ridcmp_debug(struct pa_rid *r1, struct pa_rid *r2) {
	int i = memcmp((r1)->id, (r2)->id, PA_RIDLEN);
	L_DEBUG("Comparing two rids "PA_RID_L" ? "PA_RID_L" => %d", PA_RID_LA(r1), PA_RID_LA(r2), i);

	return i;
}
*/

/**************************************************************/
/************************ pa general **************************/
/**************************************************************/

static void pa_schedule(struct pa *pa, uint32_t todo_flags)
{
	L_DEBUG("Scheduling prefix assignment algorithm");
	pa->todo_flags |= todo_flags;
	if(pa->started && pa->todo_flags && !pa->scheduled) {
		uloop_timeout_set(&pa->pa_short_timeout, PA_SCHEDULE_RUNNEXT_MS);
		pa->scheduled = true;
	}
}

void pa_conf_default(struct pa_conf *conf)
{
	conf->flooding_delay = PA_CONF_DFLT_FLOODING_DELAY;

	conf->use_ula = PA_CONF_DFLT_USE_ULA;
	conf->no_ula_if_glb_ipv6 = PA_CONF_DFLT_NO_ULA_IF_V6;
	conf->use_random_ula = PA_CONF_DFLT_USE_RDM_ULA;
	conf->random_ula_plen = PA_CONF_DFLT_ULA_RDM_PLEN;

	conf->use_ipv4 = PA_CONF_DFLT_USE_V4;
	conf->no_ipv4_if_glb_ipv6 = PA_CONF_DFLT_NO_V4_IF_V6;
	conf->v4_prefix = PA_CONF_DFLT_V4;

	conf->storage = NULL;

	conf->local_valid_lifetime = PA_CONF_DFLT_LOCAL_VALID;
	conf->local_preferred_lifetime = PA_CONF_DFLT_LOCAL_PREFERRED;
	conf->local_update_delay = PA_CONF_DFLT_LOCAL_UPDATE;
}

void pa_flood_subscribe(pa_t pa, const struct pa_flood_callbacks *cb)
{
	L_DEBUG("Flooding protocol just subscribed (%d,%d)",
			!!cb->updated_lap, !!cb->updated_ldp);
	pa->fcb = *cb;
}

void pa_iface_subscribe(pa_t pa, const struct pa_iface_callbacks *cb)
{
	L_DEBUG("Iface just subscribed (%d,%d)",
				!!cb->update_link_owner, !!cb->update_prefix);
	pa->ifcb = *cb;
}

/**************************************************************/
/********************** Utilities *****************************/
/**************************************************************/

/* Safe timeout set.
 * Use when = 0 to cancel. */
static void pa_uloop_set(struct uloop_timeout *timeout,
		hnetd_time_t now, hnetd_time_t when)
{
	hnetd_time_t delay;
	if(!when) {
		if(timeout->pending)
			uloop_timeout_cancel(timeout);
		return;
	}

	if(when < now)
		delay = 0;

	delay = when - now;

	if(delay > INTMAX_MAX)
		delay = INTMAX_MAX;

	uloop_timeout_set(timeout, (int) delay);
}

static bool pa_has_global_highest_rid(struct pa *pa)
{
	struct pa_eap *eap;
	struct pa_dp *dp;

	avl_for_each_element(&pa->eaps, eap, avl_node) {
		if(PA_RIDCMP(&eap->rid, &pa->rid))
			return false;
	}

	list_for_each_entry(dp, &pa->dps, le) {
		if(!dp->local && PA_RIDCMP(&dp->rid, &pa->rid) > 0)
					return false;
	}

	return true;
}

/**************************************************************/
/******************* iface managment **************************/
/**************************************************************/

static struct pa_iface *pa_iface_goc(struct pa *pa, const char *ifname)
{
	struct pa_iface *iface;

	list_for_each_entry(iface, &pa->ifaces, le) {
		if(!strcmp(ifname, iface->ifname))
			return iface;
	}

	if(strlen(ifname) >= IFNAMSIZ)
		return NULL; //Name too long

	if(!(iface = (struct pa_iface *)malloc(sizeof(struct pa_iface))))
		return NULL;

	strcpy(iface->ifname, ifname);
	list_init_head(&iface->eaps);
	list_init_head(&iface->laps);
	list_init_head(&iface->dps);
	iface->internal = 0;
	iface->do_dhcp = 0;
	iface->designated = 0;
	list_add(&iface->le, &pa->ifaces);

	L_INFO("Creating new "PA_IF_L, PA_IF_LA(iface));

	return iface;
}

/* Removes all laps from that interface */
static void pa_iface_rmlaps(struct pa *pa, struct pa_iface *iface)
{
	L_DEBUG("Removing all laps from "PA_IF_L, PA_IF_LA(iface));
	struct pa_lap *lap;
	struct pa_lap *slap;
	list_for_each_entry_safe(lap, slap, &iface->laps, if_le)
		pa_lap_destroy(pa, lap);
}

/* Delete an iface */
static void pa_iface_destroy(struct pa *pa, struct pa_iface *iface)
{
	struct pa_eap *eap;
	struct pa_dp *dp;

	L_INFO("Destroying "PA_IF_L, PA_IF_LA(iface));

	/* Destroys all laps */
	pa_iface_rmlaps(pa, iface);

	if(!(list_empty(&iface->dps) && list_empty(&iface->eaps)))
		L_WARN("Should not destroy "PA_IF_L" while it has eaps or dps", PA_IF_LA(iface));

	/* Remove iface from all dps */
	list_for_each_entry(dp, &iface->dps, if_le)
		pa_dp_iface_assign(pa, dp, NULL);

	/* Remove iface from all eaps */
	list_for_each_entry(eap, &iface->eaps, if_le)
		pa_eap_iface_assign(pa, eap, NULL);

	list_del(&iface->le);
	free(iface);
}

/* Check whether we need to delete that interface or
 * its laps */
static void pa_iface_cleanmaybe(struct pa *pa,
		struct pa_iface *iface)
{
	if(iface->internal)
		return;

	if(list_empty(&iface->eaps) && list_empty(&iface->dps)) {
		/* We don't need an external iface with no eaps or dps */
		pa_iface_destroy(pa, iface);
	} else {
		/* External ifaces can't have laps */
		pa_iface_rmlaps(pa, iface);
	}
}

static void pa_iface_set_do_dhcp(struct pa *pa,
		struct pa_iface *iface, bool do_dhcp)
{
	if(iface->do_dhcp == do_dhcp)
		return;

	L_INFO("Changing "PA_IF_L" do_dhcp flag to (%d)", PA_IF_LA(iface), do_dhcp);
	iface->do_dhcp = do_dhcp;

	/* When iface ownership changes,
	 * it can be important to run PA again. */
	pa_schedule(pa, PA_TODO_ALL);

	/* Tell iface about link ownership */
	if(pa->ifcb.update_link_owner)
		pa->ifcb.update_link_owner(iface->ifname, do_dhcp,
				pa->ifcb.priv);
}

static void pa_iface_set_internal(struct pa *pa,
		struct pa_iface *iface, bool internal)
{
	if(iface->internal != internal)
		pa_schedule(pa, PA_TODO_ALL);

	L_INFO("Changing "PA_IF_L" internal flag to (%d)", PA_IF_LA(iface), internal);
	iface->internal = internal;

	if(!iface->internal) {
		iface->designated = false;
		pa_iface_set_do_dhcp(pa, iface, false);
	}

	pa_schedule(pa, PA_TODO_ALL);

	pa_iface_cleanmaybe(pa, iface);
}

/**************************************************************/
/********************* eap managment **************************/
/**************************************************************/

/* iface are optional for eaps */
static void pa_eap_iface_assign(struct pa *pa,
		struct pa_eap *eap, struct pa_iface *iface)
{
	if(eap->iface == iface)
		return;

	if(eap->iface)
		list_remove(&eap->if_le);

	eap->iface = iface;

	if(eap->iface)
		list_add(&eap->if_le, &iface->eaps);

	L_DEBUG("Assigning "PA_EAP_L" to "PA_IF_L, PA_EAP_LA(eap), PA_IF_LA(iface));
	pa_schedule(pa, PA_TODO_ALL);
}

/* Find an eap with specified prefix and rid */
static struct pa_eap *pa_eap_get(struct pa *pa,
		const struct prefix *prefix, const struct pa_rid *rid)
{
	struct pa_eap *iter;
	struct pa_eap *first;
	struct pa_eap *last;

	first = avl_find_ge_element(&pa->eaps, prefix, iter, avl_node);
	last = avl_find_le_element(&pa->eaps, prefix, iter, avl_node);

	if(!(first && last))
		return NULL;

	avl_for_element_range(first, last, iter, avl_node) {
		if(!PA_RIDCMP(rid, &iter->rid))
			return iter;
	}

	return NULL;
}

static int pa_eap_iface_assignbyname(struct pa *pa,
		struct pa_eap *eap, const char *ifname)
{
	struct pa_iface *iface = NULL;
	if(ifname && strlen(ifname)) {
		iface = pa_iface_goc(pa, ifname);

		if(iface == NULL)
			return -1;
	}

	pa_eap_iface_assign(pa, eap, iface);
	return 0;
}

static struct pa_eap *pa_eap_create(struct pa *pa, const struct prefix *prefix,
		const struct pa_rid *rid)
{
	struct pa_eap *eap;

	if(!(eap = malloc(sizeof(struct pa_eap))))
		goto malloc;

	eap->iface = NULL; /* Important for assign */

	eap->rid = *rid;
	eap->prefix = *prefix;

	eap->avl_node.key = &eap->prefix;
	if(avl_insert(&pa->eaps, &eap->avl_node))
		goto insert;

	L_INFO("Creating "PA_EAP_L, PA_EAP_LA(eap));

	/* New eap means we rerun algo */
	pa_schedule(pa, PA_TODO_ALL);

	return eap;
insert:
	free(eap);
malloc:
	return NULL;
}

/* Only hcp controls eaps.
 * Destroying it is straightworward. */
static void pa_eap_destroy(struct pa *pa, struct pa_eap *eap)
{
	pa_eap_iface_assign(pa, eap, NULL);
	avl_delete(&pa->eaps, &eap->avl_node);
	L_INFO("Destroying "PA_EAP_L, PA_EAP_LA(eap));
	free(eap);

	/* Destoyed eap, we rerun algo */
	pa_schedule(pa, PA_TODO_ALL);
}

static struct pa_eap *pa_eap_goc(struct pa *pa, const struct prefix *prefix,
		const char *ifname, const struct pa_rid *rid)
{
	struct pa_eap *eap;

	eap = pa_eap_get(pa, prefix, rid);

	if(!eap)
		eap = pa_eap_create(pa, prefix, rid);

	if(!eap)
		return NULL;

	if(pa_eap_iface_assignbyname(pa, eap, ifname)) {
		pa_eap_destroy(pa, eap);
		return NULL;
	}

	return eap;
}

static void pa_eap_update(struct pa *pa, struct pa_eap *eap,
		bool to_delete)
{
	if(to_delete) {
		pa_eap_destroy(pa, eap);
		pa_schedule(pa, PA_TODO_ALL);
	}
}

/**************************************************************/
/******************* lap delayed mangmt ***********************/
/**************************************************************/

static void pa_lap_delayed_cb(struct uloop_timeout *t);

static void pa_lap_delayed_init(struct pa_lap_delayed *d) {
	d->timeout = (struct uloop_timeout) {.cb = pa_lap_delayed_cb};
	d->assign_time = 0;
	d->flooding_time = 0;
	d->delete_time = 0;
}

static void pa_lap_delayed_term(struct pa_lap_delayed *d) {
	if(d->timeout.pending)
		uloop_timeout_cancel(&d->timeout);
}

static void pa_lap_delayed_update(struct pa_lap_delayed *d,
		hnetd_time_t now)
{
	hnetd_time_t timeout = 0;
	if(d->assign_time &&
			(!timeout || d->assign_time < timeout))
		timeout = d->assign_time;

	if(d->delete_time &&
				(!timeout || d->delete_time < timeout))
			timeout = d->delete_time;

	if(d->flooding_time &&
					(!timeout || d->flooding_time < timeout))
				timeout = d->flooding_time;

	pa_uloop_set(&d->timeout, now, timeout);
}

/**************************************************************/
/********************* lap management *************************/
/**************************************************************/



static struct pa_lap *pa_lap_create(struct pa *pa, const struct prefix *prefix,
		struct pa_iface *iface, struct pa_dp* dp)
{
	struct pa_lap *lap;

	if(!(lap = calloc(1, sizeof(struct pa_lap))))
		return NULL;

	lap->assigned = false;
	lap->flooded = false;
	lap->invalid = false; /* For pa algo */
	lap->own = false;
	lap->prefix = *prefix;
	lap->avl_node.key = &lap->prefix;
	lap->pa = pa;
	if(avl_insert(&pa->laps, &lap->avl_node)) {
		free(lap);
		return NULL;
	}

	/* Attaching dp */
	lap->dp = dp;
	list_add(&lap->dp_le, &dp->laps);

	/* Attaching iface */
	lap->iface = iface;
	list_add(&lap->if_le, &iface->laps);

	/* Setting delayed operations */
	pa_lap_delayed_init(&lap->delayed);

	L_INFO("Creating "PA_LAP_L, PA_LAP_LA(lap));

	pa_schedule(pa, PA_TODO_ALL);

	return lap;
}

static void pa_lap_tellhcp(struct pa *pa, struct pa_lap *lap)
{
	// Tell hcp about that
		if(pa->fcb.updated_lap)
			pa->fcb.updated_lap(&lap->prefix, lap->iface->ifname,
					!lap->flooded, pa->fcb.priv);
}

static void pa_lap_telliface(struct pa *pa, struct pa_lap *lap)
{
	// Tell ifaces about that
	if(pa->ifcb.update_prefix)
		pa->ifcb.update_prefix(&lap->prefix, lap->iface->ifname,
				(lap->assigned)?lap->dp->valid_until:0,
						(lap->assigned)?lap->dp->preferred_until:0,
						lap->dp->dhcpv6_data,
						lap->dp->dhcpv6_len,
						pa->ifcb.priv);
}

static void pa_lap_setflood(struct pa *pa, struct pa_lap *lap,
		bool enable)
{
	if(lap->delayed.flooding_time) {
		lap->delayed.flooding_time = 0;
		pa_lap_delayed_update(&lap->delayed, hnetd_time());
	}

	if(enable == lap->flooded)
		return;

	L_INFO("Setting "PA_LAP_L" flood flag to %d", PA_LAP_LA(lap), enable);
	lap->flooded = enable;

	pa_lap_tellhcp(pa, lap);
}

static void pa_lap_setassign(struct pa *pa, struct pa_lap *lap,
		bool enable)
{
	if(lap->delayed.assign_time) {
		/* Cancel the existing delayed set. */
		lap->delayed.assign_time = 0;
		pa_lap_delayed_update(&lap->delayed, hnetd_time());
	}

	if(enable == lap->assigned)
		return;

	L_INFO("Setting "PA_LAP_L" assign flag to %d", PA_LAP_LA(lap), enable);
	lap->assigned = enable;

	pa_lap_telliface(pa, lap);

	if(lap->assigned) /* Saving in stable storage */
		pa_storage_pushprefix(pa, lap->iface, &lap->prefix);
}

static void pa_lap_setdp(struct pa *pa, struct pa_lap *lap,
		struct pa_dp *dp)
{
	if(lap->dp == dp)
		return;

	list_remove(&lap->dp_le);
	lap->dp = dp;
	list_add(&lap->dp_le, &dp->laps);

	L_DEBUG("Setting "PA_LAP_L" prefix to "PA_DP_L, PA_LAP_LA(lap), PA_DP_LA(dp));

	if(lap->assigned)
		pa_lap_telliface(pa, lap);
}

static void pa_lap_destroy(struct pa *pa, struct pa_lap *lap)
{
	/* Unassign if assigned */
	pa_lap_setassign(pa, lap, false);

	/* Unflood if flooded */
	pa_lap_setflood(pa, lap, false);

	/* Terminate delayed operations */
	pa_lap_delayed_term(&lap->delayed);

	list_remove(&lap->dp_le);
	list_remove(&lap->if_le);

	avl_delete(&pa->laps, &lap->avl_node);
	L_INFO("Destroying "PA_LAP_L, PA_LAP_LA(lap));
	free(lap);

	pa_schedule(pa, PA_TODO_ALL);
}

/* This set of functions allows delayed actions.
 * Without flags, previous delayed action are always overridden.
 * It is also overriden when a direct assignment is made.
 * Flags allow to not do something in some particular cases. */
#define PA_DF_NOT_IF_LATER_AND_EQUAL 0x01 /* Do not update if same value and when is later */

/*
static void pa_lap_setdelete_delayed(struct pa *pa, struct pa_lap *lap,
		hnetd_time_t when, hnetd_time_t now, int flags)
{
	if((flags & PA_DF_NOT_IF_LATER_AND_EQUAL) &&
			lap->delayed.delete_time &&
			when > lap->delayed.delete_time)
		return;

	L_DEBUG("Delayed delete of "PA_LAP_L" in %ld ms",
			PA_LAP_LA(lap), when - now);

	lap->delayed.delete_time = when;
	pa_lap_delayed_update(&lap->delayed, now);
}
*/

static void pa_lap_setassign_delayed(struct pa_lap *lap,
		hnetd_time_t when, hnetd_time_t now, bool assign, int flags)
{
	/* No change needed
	 * delayed value is always different than current value */
	if(assign == lap->assigned && !lap->delayed.assign_time)
		return;

	if((flags & PA_DF_NOT_IF_LATER_AND_EQUAL) &&
			lap->delayed.assign_time &&
			(assign == lap->delayed.assign_value) &&
				when > lap->delayed.assign_time)
			return;

	L_DEBUG("Delayed assignment of "PA_LAP_L" in %lld ms to (%d)",
			PA_LAP_LA(lap), (long long)(when - now), assign);

	lap->delayed.assign_time = when;
	lap->delayed.assign_value = assign;
	pa_lap_delayed_update(&lap->delayed, now);
}

/*
static void pa_lap_setflooding_delayed(struct pa *pa, struct pa_lap *lap,
		hnetd_time_t when, hnetd_time_t now, bool flood, int flags)
{
	if(flood == lap->flooded && !lap->delayed.flooding_time)
		return;

	if((flags & PA_DF_NOT_IF_LATER_AND_EQUAL) &&
				lap->delayed.flooding_time &&
				(flood == lap->delayed.flooding_value) &&
					when > lap->delayed.flooding_time)
				return;

	L_DEBUG("Delayed flooding of "PA_LAP_L" in %ld ms to (%d)",
				PA_LAP_LA(lap), when - now, flood);

	lap->delayed.flooding_time = when;
	lap->delayed.flooding_value = flood;
	pa_lap_delayed_update(&lap->delayed, now);
}
*/

static void pa_lap_delayed_cb(struct uloop_timeout *t)
{
	struct pa_lap_delayed *d = container_of(t, struct pa_lap_delayed, timeout);
	struct pa_lap *lap = container_of(d, struct pa_lap, delayed);
	struct pa *pa =  lap->pa;

	hnetd_time_t now = hnetd_time();

	if(lap->delayed.assign_time && lap->delayed.assign_time <= now)
		pa_lap_setassign(pa, lap, lap->delayed.assign_value);

	if(lap->delayed.flooding_time && lap->delayed.flooding_time <= now)
			pa_lap_setflood(pa, lap, lap->delayed.flooding_value);

	if(lap->delayed.delete_time && lap->delayed.delete_time <= now)
			pa_lap_destroy(pa, lap);

	pa_lap_delayed_update(&lap->delayed, now);
}

/**************************************************************/
/********************* dp managment **************************/
/**************************************************************/

struct pa_dp *pa_dp_get(struct pa *pa, const struct prefix *p,
		const struct pa_rid *rid)
{
	struct pa_dp *dp;
	L_DEBUG("Looking for dp with prefix %s", PREFIX_REPR(p));
	list_for_each_entry(dp, &pa->dps, le) {
		if((!prefix_cmp(p, &dp->prefix)) &&
				((dp->local && !rid)||(!dp->local && rid && !PA_RIDCMP(&dp->rid, rid))))
			return dp;
	}
	return NULL;
}

/* Returns whether there was a change */
static int pa_dp_iface_assign(struct pa *pa,
		struct pa_dp *dp, struct pa_iface *iface)
{
	if(dp->iface == iface)
		return 0;

	if(dp->iface)
		list_remove(&dp->if_le);

	dp->iface = iface;

	if(dp->iface)
		list_add(&dp->if_le, &iface->dps);

	L_DEBUG("Assigning "PA_DP_L" to "PA_IF_L, PA_DP_LA(dp), PA_IF_LA(iface));

	pa_schedule(pa, PA_TODO_ALL);

	return 1;
}

/* Returns whether there was a change */
static int pa_dp_iface_assignbyname(struct pa *pa,
		struct pa_dp *dp, const char *ifname)
{
	struct pa_iface *iface = NULL;
	if(ifname && strlen(ifname)) {
		iface = pa_iface_goc(pa, ifname);

		if(iface == NULL)
			return 0;
	}

	pa_dp_iface_assign(pa, dp, iface);
	return 1;
}

/* Returns whether there was a change */
static int pa_dp_excluded_set(struct pa *pa,
		struct pa_dp *dp, const struct prefix *excluded)
{
	struct pa_lap *lap;

	/* Already no excluded */
	if(!excluded && !dp->excluded_valid)
		return 0;

	/* Excluded are identical */
	if(excluded && dp->excluded_valid && !prefix_cmp(excluded, &dp->prefix))
		return 0;

	L_DEBUG("Set "PA_DP_L" excluded prefix to %s", PA_DP_LA(dp),
			(excluded)?PREFIX_REPR(excluded):"NULL");

	dp->excluded_valid = !!excluded;
	if(excluded)
		dp->excluded = *excluded;

	/* The excluded prefix just changed. Which means we need to destroy lap
	 * that has become invalid. */
	bool destroy = false;
	list_for_each_entry(lap, &dp->laps, dp_le) {
		if(prefix_contains(excluded, &lap->prefix)) {
			pa_lap_destroy(pa, lap);
			destroy = true;
		}
	}

	if(destroy)
		pa_schedule(pa, PA_TODO_ALL);

	return 1;
}

/* Updates dhcpv6 data
   Returns whether there was a change */
static int pa_dp_dhcpv6_set(struct pa_dp *dp,
		const void *dhcpv6_data, size_t dhcpv6_len)
{
	if(!dhcpv6_data)
		dhcpv6_len = 0;

	/* No change */
	if(!dhcpv6_len && !dp->dhcpv6_data)
		return 0;

	/* No change */
	if(dhcpv6_len && dhcpv6_len == dp->dhcpv6_len &&
			!memcmp(dp->dhcpv6_data, dhcpv6_data, dhcpv6_len))
		return 0;

	L_DEBUG("Set "PA_DP_L" dhcpv6 data (length %d)",
			PA_DP_LA(dp), (int) dhcpv6_len);

	if(dp->dhcpv6_data)
		free(dp->dhcpv6_data);

	if(dhcpv6_len) {
		if(!(dp->dhcpv6_data = malloc(dhcpv6_len))) {
			L_WARN("Malloc failed for "PA_DP_L" dhcpv6 data assign", PA_DP_LA(dp));
		} else {
			memcpy(dp->dhcpv6_data, dhcpv6_data, dhcpv6_len);
			dp->dhcpv6_len = dhcpv6_len;
		}
	} else {
		dp->dhcpv6_data = NULL;
		dp->dhcpv6_len = 0;
	}

	return 1;
}


static int pa_dp_times_set(struct pa *pa, struct pa_dp *dp,
		hnetd_time_t valid_until,hnetd_time_t preferred_until)
{
	if(valid_until == dp->valid_until &&
				preferred_until == dp->preferred_until)
			return 0;

	dp->valid_until = valid_until;
	dp->preferred_until = preferred_until;

	L_DEBUG("Updating dp "PA_DP_L" with times (%lld, %lld)",
			PA_DP_LA(dp), (long long)valid_until, (long long)preferred_until);

	pa_schedule(pa, PA_TODO_ALL);

	return 1;
}

static void pa_dp_tell_hcp(struct pa *pa,
		struct pa_dp *dp)
{
	//Notify hcp iff local
	if(dp->local && pa->fcb.updated_ldp)
			pa->fcb.updated_ldp(&dp->prefix, /* prefix */
								(dp->excluded_valid)?&dp->excluded:NULL,
								(dp->iface)?dp->iface->ifname:NULL,
								dp->valid_until, dp->preferred_until,
								dp->dhcpv6_data, dp->dhcpv6_len,
								pa->fcb.priv);
}

/* Creates an empty unused dp with the given prefix */
static struct pa_dp *pa_dp_create(struct pa *pa,
		const struct prefix *prefix,
		const struct pa_rid *rid)
{
	struct pa_dp *dp;
	if(!(dp = calloc(1, sizeof(struct pa_dp))))
		return NULL;

	dp->prefix = *prefix;
	list_init_head(&dp->laps);
	dp->valid_until = 0;
	dp->preferred_until = 0;
	dp->dhcpv6_data = NULL;
	dp->dhcpv6_len = 0;
	dp->iface = NULL;
	dp->excluded_valid = false;
	if(!rid) {
		dp->local = 1;
	} else {
		dp->local = 0;
		dp->rid = *rid;
	}
	dp->l = NULL;

	list_add(&dp->le, &pa->dps); /* Adding dp */

	L_DEBUG("Creating "PA_DP_L, PA_DP_LA(dp));

	/* Rerun algo. when new dp */
	pa_schedule(pa, PA_TODO_ALL);
	return dp;
}

static struct pa_dp *pa_dp_goc(struct pa *pa, const struct prefix *prefix,
		const struct pa_rid *rid)
{
	struct pa_dp *dp = pa_dp_get(pa, prefix, rid);

	if(dp)
		return dp;

	return pa_dp_create(pa, prefix, rid);
}

static void pa_dp_destroy(struct pa *pa, struct pa_dp *dp)
{
	struct pa_lap *lap;
	struct pa_lap *slap;
	struct pa_dp *s_dp;
	bool found;

	L_DEBUG("Destroying "PA_DP_L, PA_DP_LA(dp));

	/* If generated by local algorithm */
	if(dp->l)
		dp->l = NULL;

	/* Destoy all laps attached to that dp.
	 * If we can't reattach the lap to another dp (temporarly) */
	list_for_each_entry_safe(lap, slap, &dp->laps, dp_le) {
		/* Find another dp that could temporarily accept that lap */
		found = false;
		list_for_each_entry(s_dp, &pa->dps, le) {
			if(s_dp != dp && prefix_contains(&s_dp->prefix, &lap->prefix)){
				found = true;
				break;
			}
		}
		L_DEBUG("Considering "PA_LAP_L" adoption by another dp (%d)", PA_LAP_LA(lap), found);
		if(found) {
			pa_lap_setdp(pa, lap, s_dp);
		} else {
			pa_lap_destroy(pa, lap);
		}
	}

	pa_dp_iface_assign(pa, dp, NULL);
	pa_dp_dhcpv6_set(dp, NULL, 0);
	pa_dp_times_set(pa, dp, 0, 0);

	//Notify hcp iff local
	pa_dp_tell_hcp(pa, dp);

	//Remove that dp from database
	list_remove(&dp->le);
	free(dp);
}

static void pa_dp_update(struct pa *pa, struct pa_dp *dp,
		const char *ifname,
		const struct prefix *excluded,
		hnetd_time_t valid_until, hnetd_time_t preferred_until,
		const void *dhcpv6_data, size_t dhcpv6_len)
{
	int b1 = 0, b2 = 0;
	struct pa_lap *lap;

	if(!valid_until) {
		pa_dp_destroy(pa, dp); /* That already tells hcp */
	} else if((b1 |= pa_dp_times_set(pa, dp, valid_until, preferred_until)) |
			(b2 |= pa_dp_dhcpv6_set(dp, dhcpv6_data, dhcpv6_len)) |
			pa_dp_excluded_set(pa, dp, excluded) |
			pa_dp_iface_assignbyname(pa, dp, ifname)) {
		if(dp->local)
			pa_dp_tell_hcp(pa, dp);

		/* Iface needs dp info for each lap.
		 * When a lap is modified we tell iface (iff assigned) */
		if(b1 || b2) {
			list_for_each_entry(lap, &dp->laps, dp_le) {
				if(lap->assigned)
					pa_lap_telliface(pa, lap);
			}
		}
	}
}

static void pa_dp_cleanmaybe(struct pa *pa, struct pa_dp *dp,
		hnetd_time_t now)
{
	if(now >= dp->valid_until)
		pa_dp_destroy(pa, dp);
}

/**************************************************************/
/***************** Local prefixes mngmt ***********************/
/**************************************************************/

static struct pa_iface *pa_iface_get_internal(struct pa *pa)
{
	struct pa_iface *iface;
	list_for_each_entry(iface, &pa->ifaces, le) {
		if(iface->internal)
			return iface;
	}
	return NULL;
}

static struct pa_dp *pa_dp_get_globalv6(struct pa *pa)
{
	struct pa_dp *dp;
	list_for_each_entry(dp, &pa->dps, le) {
		if(!prefix_is_ipv4(&dp->prefix) && !prefix_is_ipv6_ula(&dp->prefix))
			return dp;
	}
	return NULL;
}

static void pa_local_elem_destroy(struct pa *pa, struct pa_local_elem *elem)
{
	if(elem->dp) {
		pa_dp_update(pa, elem->dp, NULL, NULL, 0, 0, NULL, 0);
		elem->dp = NULL;
	}
}

static uint8_t pa_local_ula_get_status(struct pa *pa,
		__attribute__((unused))struct pa_local_elem *elem)
{
	if(!pa->conf.use_ula || !pa_iface_get_internal(pa) ||
			(pa->conf.no_ula_if_glb_ipv6 && pa_dp_get_globalv6(pa)))
		return 0;

	struct pa_dp *dp, *best_dp = NULL;
	list_for_each_entry(dp, &pa->dps, le) {
		if(prefix_is_ipv6_ula(&dp->prefix) &&
				(!best_dp || PA_RIDCMP(&dp->rid, &best_dp->rid) > 0))
			best_dp = dp;
	}

	if(best_dp && PA_RIDCMP(&best_dp->rid, &pa->rid) > 0)
		return 0;

	bool highest = pa_has_global_highest_rid(pa);

	uint8_t status = PA_LOCAL_CAN_KEEP;
	if(!best_dp && highest)
		status |= PA_LOCAL_CAN_CREATE;

	return status;
}

static void pa_local_ula_create(struct pa *pa, struct pa_local_elem *elem)
{
	const struct prefix *p = NULL;
	struct prefix pr, p0;

	if(pa->conf.use_random_ula) {
		if(pa->conf.storage) {
			p = pa_store_ula_get(pa->conf.storage);
		}
		if(!p) {
			/* Generate a new one */
			p0.plen = 0;
			prefix_random(&p0, &pr, pa->conf.random_ula_plen);
			p = &pr;
			if(pa->conf.storage) /* Saving the chosen ula */
				pa_store_ula_set(pa->conf.storage, p);
		}
	} else {
		p = &pa->conf.ula_prefix;
	}

	elem->dp = pa_dp_create(pa, p, NULL);
	elem->dp->l = elem;
}

static hnetd_time_t pa_local_ula_update(struct pa *pa, struct pa_local_elem *elem, hnetd_time_t now)
{
	pa_dp_update(pa, elem->dp, NULL, NULL,
			now + pa->conf.local_valid_lifetime,
			now + pa->conf.local_preferred_lifetime,
			NULL, 0);
	return elem->dp->valid_until - pa->conf.local_update_delay;
}

static uint8_t pa_local_ipv4_get_status(struct pa *pa,
		__attribute__((unused))struct pa_local_elem *elem)
{
	if(!pa->conf.use_ipv4 || !pa_iface_get_internal(pa) ||
			!pa->ipv4.available ||
			(pa->conf.no_ipv4_if_glb_ipv6 && pa_dp_get_globalv6(pa)))
		return 0;

	struct pa_dp *dp, *best_dp = NULL;
	list_for_each_entry(dp, &pa->dps, le) {
		if(prefix_is_ipv4(&dp->prefix) && !best_dp->local &&
				(!best_dp || PA_RIDCMP(&dp->rid, &best_dp->rid) > 0))
			best_dp = dp;
	}

	if(best_dp && PA_RIDCMP(&best_dp->rid, &pa->rid) > 0)
		return 0;

	if(best_dp)
		return PA_LOCAL_CAN_KEEP;

	return PA_LOCAL_CAN_KEEP & PA_LOCAL_CAN_CREATE;
}

static void pa_local_ipv4_create(struct pa *pa, struct pa_local_elem *elem)
{
	elem->dp = pa_dp_create(pa, &pa->conf.v4_prefix, NULL);
	elem->dp->l = elem;
}

static hnetd_time_t pa_local_ipv4_update(struct pa *pa, struct pa_local_elem *elem, hnetd_time_t now)
{
	pa_dp_update(pa, elem->dp, NULL, NULL,
			now + pa->conf.local_valid_lifetime,
			now + pa->conf.local_preferred_lifetime,
			pa->ipv4.dhcp_data, pa->ipv4.dhcp_len);
	return elem->dp->valid_until - pa->conf.local_update_delay;
}

/* Generic function for IPv4 and ULA generation */
static void pa_local_algo(struct pa *pa, hnetd_time_t now, struct pa_local_elem *elem)
{
	uint8_t status = (elem->get_status)?elem->get_status(pa, elem):0;

	if(!status)
		goto destroy;

	if(elem->dp) {
		if(!(status & PA_LOCAL_CAN_KEEP)) {
			goto destroy;
		} else if (elem->timeout <= now) {
			elem->timeout = elem->update(pa, elem, now);
		}
	} else if (status & PA_LOCAL_CAN_CREATE) {
		if(!elem->create_start) {
			elem->create_start = now;
			elem->timeout = now + PA_LOCAL_DELAY(pa->conf.flooding_delay);
		} else if (now >= elem->create_start + PA_LOCAL_DELAY(pa->conf.flooding_delay)) {
			elem->create(pa, elem);
			elem->create_start = 0;
			elem->timeout = elem->update(pa, elem, now);
		}
	} else {
		elem->timeout = 0;
	}
	return;

destroy:
	pa_local_elem_destroy(pa, elem);
	elem->create_start = 0;
	elem->timeout = 0;
	return;
}

static void pa_local_update_timeout(struct pa *pa, hnetd_time_t now) {
	hnetd_time_t next = 0;

	if(pa->local.ula.timeout)
		next = pa->local.ula.timeout;

	if(pa->local.ipv4.timeout && (!next || next > pa->local.ipv4.timeout))
		next = pa->local.ipv4.timeout;

	if(next != pa->local.current_timeout) {
		L_DEBUG("Scheduling local timeout");
		pa->local.current_timeout = next;
		pa_uloop_set(&pa->local.timeout, now, next);
	}
}

static void pa_local_do(struct pa *pa, hnetd_time_t now)
{
	pa_local_algo(pa, now, &pa->local.ula);
	pa_local_algo(pa, now, &pa->local.ipv4);
	pa_local_update_timeout(pa, now);
}

static void pa_local_timeout_cb(struct uloop_timeout *to)
{
	struct pa *pa = container_of(to, struct pa, local.timeout);
	pa->todo_flags |= PA_TODO_ALL;
	pa->local.current_timeout = 0;
	pa_do(pa);
}

static void pa_local_init(struct pa_local *local)
{
	local->timeout = (struct uloop_timeout) { .cb = pa_local_timeout_cb };
	local->current_timeout = 0;

	local->ula.dp = NULL;
	local->ula.timeout = 0;
	local->ula.create_start = 0;
	local->ula.get_status = pa_local_ula_get_status;
	local->ula.create = pa_local_ula_create;
	local->ula.update = pa_local_ula_update;

	local->ipv4.dp = NULL;
	local->ipv4.timeout = 0;
	local->ipv4.create_start = 0;
	local->ipv4.get_status = pa_local_ipv4_get_status;
	local->ipv4.create = pa_local_ipv4_create;
	local->ipv4.update = pa_local_ipv4_update;
}

static void pa_local_term(struct pa *pa)
{
	pa_local_elem_destroy(pa, &pa->local.ula);
	pa_local_elem_destroy(pa, &pa->local.ipv4);

	if(pa->local.timeout.pending)
		uloop_timeout_cancel(&pa->local.timeout);

	pa->local.current_timeout = 0;
}

/**************************************************************/
/********************* PA algorithm ***************************/
/**************************************************************/

/* Check whether a foreign assignment exists on a link different than iface
 * with a higher or equal router id. */
static bool pa_prefix_checkcollision(struct pa *pa, const struct prefix *prefix,
		struct pa_iface *exclude_iface, struct pa_rid *rid,
		bool check_foreign, bool check_local)
{
	struct pa_eap *eap;
	struct pa_eap *lap;

	if(check_foreign) {
		avl_for_each_element(&pa->eaps, eap, avl_node) {
			if((!exclude_iface || eap->iface != exclude_iface) &&
					prefix_contains(&eap->prefix, prefix) &&
					(!rid ||  PA_RIDCMP(&eap->rid, rid) > 0))
				return true;
		}
	}

	if(check_local) {
		avl_for_each_element(&pa->laps, lap, avl_node) {
			if((!exclude_iface || lap->iface != exclude_iface) &&
					prefix_contains(&lap->prefix, prefix) &&
					(!rid || PA_RIDCMP(&pa->rid, rid) > 0))
				return true;
		}
	}

	return false;
}

static int pa_get_newprefix_random(struct pa *pa,
		__attribute__((unused))struct pa_iface *iface,
		struct pa_dp *dp, struct prefix *new_prefix) {

	int res;
	bool looped;
	uint32_t rounds, i;
	uint8_t plen;

	if(dp->prefix.plen <= 64) {
		plen = 64;
	} else if (dp->prefix.plen == 104) { //IPv4
		plen = 120;
	} else {
		L_WARN("Delegated prefix length (%d) not supported", dp->prefix.plen);
		return -1;
	}

	/* Excluded contains the whole prefix */
	if(dp->excluded_valid && prefix_contains(&dp->excluded, &dp->prefix)) {
		L_WARN("Excluded prefix %s contains the "PA_DP_L, PREFIX_REPR(&dp->excluded), PA_DP_LA(dp));
		return -1;
	}

	/* The router first choose a random prefix. Then it iterates over all
	 * the next prefixes, with a limit of PA_PREFIX_SEARCH_MAX_ROUNDS iterations.
	 * When the chosen prefix is in excluded, it jumps to the end of the exclusion domain. */
	if(plen - dp->prefix.plen >= 32 || (1 << (plen - dp->prefix.plen)) >= PA_PREFIX_SEARCH_MAX_ROUNDS) {
		rounds = PA_PREFIX_SEARCH_MAX_ROUNDS;
	} else {
		rounds = (1 << (plen - dp->prefix.plen));
	}

	looped = false;
	prefix_random(&dp->prefix, new_prefix, plen);
	for(i=0; i<rounds; i++) {
		if(dp->excluded_valid && prefix_contains(&dp->excluded, new_prefix)) {
			L_DEBUG("Skipping excluded prefixes");
			prefix_last(new_prefix, new_prefix, dp->excluded.plen);
		} else if (!pa_prefix_checkcollision(pa, new_prefix, NULL, NULL, true, true)) {
			return 0;
		}

		L_DEBUG("Prefix %s can't be used", PREFIX_REPR(new_prefix));

		if((res = prefix_increment(new_prefix, new_prefix, dp->prefix.plen)) == -1)
			return -1;

		if(res) {
			if(looped)
				return -1;
			looped = true;
		}
	}

	return -1;
}

static void pa_storage_pushprefix(struct pa *pa, struct pa_iface *iface,
		const struct prefix *prefix)
{
	if(pa->conf.storage)
		pa_store_prefix_add(pa->conf.storage, iface->ifname, prefix);
}

struct pa_storage_match_priv {
	struct pa *pa;
	struct pa_dp *dp;
};

static int pa_store_match(const struct prefix *p,
		__attribute__((unused))const char *ifname,  void *priv)
{
	struct pa_storage_match_priv *pr = (struct pa_storage_match_priv *)priv;

	if(prefix_contains(&pr->dp->prefix, p) &&
			!pa_prefix_checkcollision(pr->pa, p, NULL, NULL, true, true))
		return 1;

	return 0;
}

static int pa_storage_getprefix(struct pa *pa, struct pa_iface *iface,
		struct pa_dp *dp, struct prefix *new_prefix) {
	const struct prefix *p;
	struct pa_storage_match_priv priv;

	if(!pa->conf.storage)
		return -1;

	priv.pa = pa;
	priv.dp = dp;

	if((p = pa_store_prefix_find(pa->conf.storage, iface->ifname, pa_store_match, &priv))) {
		*new_prefix = *p;
		return 0;
	}

	return -1;
}

/* Executes pa algorithm */
static void pa_do(struct pa *pa)
{
	hnetd_time_t now, timeout;
	struct pa_iface *iface, *s_iface;
	struct pa_dp *dp, *s_dp;
	struct pa_lap *lap, *s_lap;
	struct pa_eap *eap, *s_eap;
	struct prefix *prefix;
	struct prefix new_prefix;
	bool found, own, link_highest_rid, wait_for_neigh;

	now = hnetd_time();

	L_DEBUG("Running prefix assignment algorithm");

	if(!pa->todo_flags) {
		L_DEBUG("Nothing to do");
		return;
	}

	/* This is at the beginning because any modification
	 * to laps should make the algorithm run again */
	pa->scheduled = false;
	pa->todo_flags = 0;

	/* Clean interfaces that should be destroyed
	 * (external with no eaps)*/
	list_for_each_entry_safe(iface, s_iface, &pa->ifaces, le) {
		pa_iface_cleanmaybe(pa, iface);
	}

	/* Clean dps that are outdated */
	list_for_each_entry_safe(dp, s_dp, &pa->dps, le) {
		pa_dp_cleanmaybe(pa, dp, now);
	}

	/* Get next dp timeout */
	timeout = 0;
	list_for_each_entry(dp, &pa->dps, le) {
		if(!timeout || timeout < dp->valid_until) {
			timeout = dp->valid_until;
		}
	}
	if(timeout != pa->pa_dp_when) {
		pa->pa_dp_when = timeout;
		pa_uloop_set(&pa->pa_dp_timeout, now, timeout);
	}

	/* IPv6 ULA and IPv4 local prefixes */
	pa_local_do(pa, now);

	/* Mark all laps as invalid */
	avl_for_each_element(&pa->laps, lap, avl_node) {
		lap->invalid = true;
	}

	/* Go through all internal ifaces */
	list_for_each_entry(iface, &pa->ifaces, le) {
		/* SHOULD NOT DELETE IFACE HERE */
		if(!iface->internal)
			continue;

		/* Go through all dps */
		list_for_each_entry(dp, &pa->dps, le) {
			L_DEBUG("Considering "PA_DP_L" on "PA_IF_L,
					PA_DP_LA(dp), PA_IF_LA(iface));

			/* Check if the dp doesn't contain another smaller dp */
			found = false;
			list_for_each_entry(s_dp, &pa->dps, le) {
				if(dp->prefix.plen > s_dp->prefix.plen &&
						prefix_contains(&dp->prefix, &s_dp->prefix)) {
					found = true;
					break;
				}
			}

			/* TODO: Maybe we need to prevent ULA/IPv4 prefixes that do not
			 have priority (Depends if we trust other hosts).*/

			/* Only use smaller dps
			 * Laps that are not in smaller dps will
			 * be removed due to invalid-labelling */
			if(found)
				continue;

			/* See whether we have a lap for this
			 * iface/dp pair */
			lap = NULL;
			list_for_each_entry(s_lap, &iface->laps, if_le) {
				/* Prefix ownership is not important here. */
				if(prefix_contains(&dp->prefix, &s_lap->prefix)) {
					lap = s_lap;
					/* lap is attached to a dp.
					 * This dp will be updated when
					 * lap->invalid is set to false (if it does) */
					break;
				}
			}

			if(lap) { L_DEBUG(PA_LAP_L" found on "PA_IF_L, PA_LAP_LA(lap), PA_IF_LA(iface)); }

			/* See whether someone else made an assignment
			 * on that same link. Keep the highest rid. */
			eap = NULL;
			list_for_each_entry(s_eap, &iface->eaps, if_le) {
				if(prefix_contains(&dp->prefix, &s_eap->prefix) &&
						(!eap || PA_RIDCMP(&s_eap->rid, &eap->rid) > 0 )) {
					eap = s_eap;
				}
			}

			if(eap) { L_DEBUG(PA_EAP_L" found on "PA_IF_L, PA_EAP_LA(eap), PA_IF_LA(iface)); }

			/* See whether we have highest router id on that link */
			link_highest_rid = true;
			list_for_each_entry(s_eap, &iface->eaps, if_le) {
				if(PA_RIDCMP(&s_eap->rid, &pa->rid) > 0) {
					link_highest_rid = false;
					break;
				}
			}


			/* See if someone overrides our assignment */
			if(lap && eap && PA_RIDCMP(&eap->rid, &pa->rid) > 0) {
				if(prefix_cmp(&lap->prefix, &eap->prefix)) {
					/* Guy with higher id floods a different prefix */
					pa_lap_destroy(pa, lap);
					lap = NULL;
				} else if(lap->own) {
					/* We agree on the prefix, but the other guy has higher
					 * prefix. So stop owning it.
					 * Note: Important the pa_lap_set_flooding is called later on */
					lap->own = false;
				}
			}

			if(lap && lap->own &&
					pa_prefix_checkcollision(pa, &lap->prefix, iface, &pa->rid, true, false)) {
				/* This is case i. of algorithm
				 * We have an assignment but we need to check for collisions
				 * on other links. */
				pa_lap_destroy(pa, lap);
				lap = NULL;
			}

			if(!lap) {
				/* This is step 6 of the algorithm
				 * Assignment generation. */

				prefix = NULL;

				wait_for_neigh = false;
				if(eap) {
					/* Let's try to use that guy's eap.
					 * But only if its valid against all other links
					 * assignments. */
					if(!pa_prefix_checkcollision(pa, &eap->prefix, iface, &eap->rid,
							true, true)) {
						L_DEBUG("Choosing "PA_EAP_L" from neighbor ", PA_EAP_LA(eap));
						prefix = &eap->prefix;
#if PA_ALGO == PA_ALGO_ARKKO
						own = false; /* The other guy owns it */
#elif PA_ALGO == PA_ALGO_PFISTER
						/* If we do dhcp and we have highest link id
						 * we claim ownership of all prefixes so that
						 * it converges to a single owner per link. */
						own = (link_highest_rid && iface->designated);
#endif
					} else {
						/* We detected a collision, but just silently ignore it */
#if PA_ALGO == PA_ALGO_ARKKO
						wait_for_neigh = true;
#elif PA_ALGO == PA_ALGO_PFISTER
						wait_for_neigh = !iface->designated;
#endif
					}
				}

				if(!prefix && link_highest_rid && !wait_for_neigh) {
					/* Let's choose a prefix for our own assignment */
					if(!pa_storage_getprefix(pa, iface, dp, &new_prefix)) {
						/* Got one from stable storage */
						L_DEBUG("Got prefix from storage %s", PREFIX_REPR(&new_prefix));
						prefix = &new_prefix;
						own = true;
					} else if(!pa_get_newprefix_random(pa, iface, dp, &new_prefix)) {
						/* Got one from random choice */
						L_DEBUG("Created random prefix %s", PREFIX_REPR(&new_prefix));
						prefix = &new_prefix;
						own = true;
					}
				}

				if(prefix) {
					/* We can make an assignment. */
					lap = pa_lap_create(pa, prefix, iface, dp);
					lap->own = own; /* Important to know whether we are owner. */
				} else if (link_highest_rid && !wait_for_neigh) {
					L_WARN("Could not generate a prefix for interface %s", iface->ifname);
				}
			}

			/* Check iface assignment and flooding */
			if(lap) {

				/* If nobody else is advertising the prefix
				 * anymore, we need to become owner of it. */
				if(!lap->own) {
					eap = NULL;
					list_for_each_entry(s_eap, &iface->eaps, if_le) {
						if(!prefix_cmp(&lap->prefix, &s_eap->prefix)) {
							eap = s_eap;
						}
					}
					if(!eap)
						lap->own = true;
				}

				lap->invalid = false;
				pa_lap_setdp(pa, lap, dp);
				pa_lap_setflood(pa, lap, lap->own); /* No delayed flooding for now */

				timeout = now + PA_ASSIGN_DELAY(pa->conf.flooding_delay);
				pa_lap_setassign_delayed(lap, timeout, now, true,
							PA_DF_NOT_IF_LATER_AND_EQUAL);

			}

		}

	}

	/* Clean invalid laps */
	avl_for_each_element_safe(&pa->laps, lap, avl_node, s_lap) {
		if(lap->invalid)
			pa_lap_destroy(pa, lap);
	}

	/* Do interface ownership check */
	list_for_each_entry(iface, &pa->ifaces, le) {
		if(!iface->internal)
				continue;

		/* An interface is designated iff there are no eap,
		 * or if it is advertising at least noe lap and all eaps
		 * have lower rids. */
		if(list_is_empty(&iface->eaps)) {
			iface->designated = true;
		} else {
			iface->designated = false;
			/* TODO: Do dhcp only if a owned prefix is actualy assigned ? (or iface can take care of that) */
			list_for_each_entry(lap, &iface->laps, if_le) {
				if(lap->own) {
					iface->designated = true;
					break;
				}
			}

			if(iface->designated) {
				list_for_each_entry(eap, &iface->eaps, if_le) {
					if(PA_RIDCMP(&eap->rid, &pa->rid) > 0 ) {
						iface->designated = false;
						break;
					}
				}
			}
		}
		pa_iface_set_do_dhcp(pa, iface, iface->designated && !list_is_empty(&iface->laps));
	}
}

static void pa_dp_do_uloop(struct uloop_timeout *t)
{
	struct pa *pa = container_of(t, struct pa, pa_dp_timeout);
	pa->todo_flags |= PA_TODO_ALL; //TODO: DP only
	pa_do(pa);
}

static void pa_do_uloop(struct uloop_timeout *t)
{
	struct pa *pa = container_of(t, struct pa, pa_short_timeout);
	pa_do(pa);
}


/**************************************************************/
/********************* hcp interface **************************/
/**************************************************************/

void pa_set_rid(pa_t pa, const struct pa_rid *rid)
{
	if(!PA_RIDCMP(&pa->rid, rid))
		return;

	L_NOTICE("Setting router id to "PA_RID_L, PA_RID_LA(rid));
	pa->rid = *rid;
	pa_schedule(pa, PA_TODO_ALL);
}

/* Called by hcp when it wants to update an eap */
int pa_update_eap(pa_t pa, const struct prefix *prefix,
		const struct pa_rid *rid,
		const char *ifname, bool to_delete)
{
	struct pa_eap *eap;

	if(!(eap = pa_eap_goc(pa, prefix, ifname, rid)))
		return -1;

	pa_eap_update(pa, eap, to_delete);
	return 0;
}

int pa_update_edp(pa_t pa, const struct prefix *prefix,
		const struct pa_rid *rid,
		const struct prefix *excluded,
		hnetd_time_t valid_until, hnetd_time_t preferred_until,
		const void *dhcpv6_data, size_t dhcpv6_len)
{
	struct pa_dp *dp;

	if(!rid) /* Do not accept local dps */
		return -1;

	if(!(dp = pa_dp_goc(pa, prefix, rid)))
		return -1;

	if(valid_until < 0)
		valid_until = 0;

	pa_dp_update(pa, dp, NULL, excluded,
			valid_until, preferred_until,
			dhcpv6_data, dhcpv6_len);
	return 0;
}


/**************************************************************/
/********************* iface callbacks ************************/
/**************************************************************/

static void pa_ifu_ipv4(struct iface_user *u, bool available,
		const void *dhcp_data, size_t dhcp_len)
{
	/* Tells when IPv4 connectivity is available. */
	struct pa *pa = container_of(u, struct pa, ifu);
	void *new_dhcp = NULL;
	hnetd_time_t now;

	bool changed = false;
	if(pa->ipv4.available != available) {
		pa->ipv4.available = available;
		changed = true;
	}

	if(dhcp_data && pa->ipv4.dhcp_data && dhcp_len == pa->ipv4.dhcp_len &&
			!memcmp(dhcp_data, pa->ipv4.dhcp_data, dhcp_len))
		goto update;

	if(!dhcp_data && !pa->ipv4.dhcp_data)
		goto update;

	changed = true;
	if(dhcp_data && dhcp_len) {
		new_dhcp = malloc(dhcp_len);
		if(!new_dhcp) {
			/* In case of malloc error, do not do IPv4 */
			pa->ipv4.available = false;
			goto update;
		}
		memcpy(new_dhcp, dhcp_data, dhcp_len);
	}

	if(pa->ipv4.dhcp_data) {
		free(pa->ipv4.dhcp_data);
	}

	pa->ipv4.dhcp_data = new_dhcp;
	pa->ipv4.dhcp_len = (new_dhcp)?dhcp_len:0;

update:
	if(changed) {
		pa->local.ipv4.timeout = 0; /* This is to force update */
		now = hnetd_time();
		pa_local_algo(pa, now, &pa->local.ipv4);
		pa_local_update_timeout(pa, now);
	}
}

static void pa_ifu_intiface(struct iface_user *u,
		const char *ifname, bool enabled)
{
	struct pa *pa = container_of(u, struct pa, ifu);
	struct pa_iface *iface;

	iface = pa_iface_goc(pa, ifname);

	if(!iface)
		return;

	L_DEBUG("pa_ifu_intiface %s=%s", ifname, enabled ? "enabled" : "disabled");

	pa_iface_set_internal(pa, iface, enabled);
}

static void pa_ifu_pd(struct iface_user *u, const char *ifname,
		const struct prefix *prefix, const struct prefix *excluded,
		hnetd_time_t valid_until, hnetd_time_t preferred_until,
		const void *dhcpv6_data, size_t dhcpv6_len)
{
	struct pa *pa = container_of(u, struct pa, ifu);

	/* Null because local */
	struct pa_dp *dp = pa_dp_goc(pa, prefix, NULL);

	if(!dp)
		return;

	L_DEBUG("pa_ifu_pd @%s %s %lld/%lld", ifname,
			PREFIX_REPR(prefix),
			(long long)valid_until, (long long)preferred_until);

	if(valid_until < 0)
		valid_until = 0;

	pa_dp_update(pa, dp, ifname, excluded,
		valid_until, preferred_until,
		dhcpv6_data, dhcpv6_len);
}

/**************************************************************/
/********************* main management ************************/
/**************************************************************/

int pa_set_conf(pa_t pa, const struct pa_conf *conf)
{
	if(conf->use_ula && !conf->use_random_ula &&
			!prefix_is_ipv6_ula(&conf->ula_prefix))
		return -1;

	pa->conf = *conf;
	return 0;
}

pa_t pa_create(const struct pa_conf *conf)
{
	struct pa *pa;

	if(!(pa = malloc(sizeof(struct pa))))
		return NULL;

	avl_init(&pa->eaps, pa_avl_prefix_cmp, true, NULL);
	avl_init(&pa->laps, pa_avl_prefix_cmp, false, NULL);
	list_init_head(&pa->dps);
	list_init_head(&pa->ifaces);

	pa->started = false;
	pa->todo_flags = 0;
	pa->scheduled = false;

	pa->ifu.cb_intiface = pa_ifu_intiface;
	pa->ifu.cb_prefix = pa_ifu_pd;
	pa->ifu.cb_extdata = NULL; //TODO ?
	pa->ifu.ipv4_update = pa_ifu_ipv4;

	pa->pa_short_timeout = (struct uloop_timeout) { .cb = pa_do_uloop };
	pa->pa_dp_when = 0;
	pa->pa_dp_timeout = (struct uloop_timeout) { .cb = pa_dp_do_uloop };
	pa->rid = (struct pa_rid) {.id = {} };

	pa->ipv4.available = false;

	if(pa_set_conf(pa, conf)) {
		free(pa);
		return NULL;
	}

	pa_local_init(&pa->local);

	L_NOTICE("New pa structure created");
	/* Don't schedule PA here because no iface or dp yet... */

	return pa;
}

int pa_start(pa_t pa)
{
	if(pa->started)
		return -1;

	pa->started = true;
	/* Starts the pa if there is things to do */
	pa_schedule(pa, 0);

	/* Register to iface */
	iface_register_user(&pa->ifu);

	L_NOTICE("Pa structure started");
	return 0;
}

void pa_destroy(pa_t pa)
{
	struct pa_iface *iface;
	struct pa_dp *dp;
	struct pa_eap *eap;
	struct pa_eap *seap;

	/* Uninit local assignments */
	pa_local_term(pa);

	/* Unregister everywhere */
	iface_unregister_user(&pa->ifu);

	/* Destroy all interfaces
	 * This will also delete all laps */
	while(!list_empty(&pa->ifaces)) {
		iface = list_first_entry(&pa->ifaces, struct pa_iface, le);
		pa_iface_destroy(pa, iface);
	}

	/* Destroy all dps */
	while(!list_empty(&pa->dps)) {
		dp = list_first_entry(&pa->dps, struct pa_dp, le);
		pa_dp_destroy(pa, dp);
	}

	/* Destroy all eaps */
	avl_for_each_element_safe(&pa->eaps, eap, avl_node, seap) {
		pa_eap_destroy(pa, eap);
	}

	if(pa->pa_short_timeout.pending)
		uloop_timeout_cancel(&pa->pa_short_timeout);

	if(pa->pa_dp_timeout.pending)
		uloop_timeout_cancel(&pa->pa_dp_timeout);

	L_NOTICE("Pa structure destroyed");
	free(pa);
}
