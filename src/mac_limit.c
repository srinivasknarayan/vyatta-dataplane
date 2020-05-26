/*-
 * Copyright (c) 2020, AT&T Intellectual Property.
 * All rights reserved.
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * mac limit feature handling
 */

#include <errno.h>
#include <rte_jhash.h>
#include <dp_event.h>
#include <vplane_log.h>
#include <if_var.h>
#include <urcu/list.h>
#include "feature_commands.h"
#include "mac_limit.h"
#include "protobuf.h"
#include "protobuf/MacLimitConfig.pb-c.h"

static struct cds_lfht *mac_limit_profile_tbl;
static struct cds_list_head *mac_limit_list;

#define MAC_LIMIT_PROFILE_TABLE_MIN 8
#define MAC_LIMIT_PROFILE_TABLE_MAX 1024

struct mac_limit_profile {
	struct cds_lfht_node mlp_node;
	struct cds_list_head mlp_list;
	struct rcu_head      mlp_rcu;
	char                 *mlp_name;
	uint32_t             mlp_limit;
};

struct mac_limit_entry {
	struct cds_list_head     mle_list;
	struct cds_list_head     mle_profile_list;
	struct ifnet             *mle_ifp;
	uint16_t                 mle_vlan;
	struct mac_limit_profile *mle_profile;
	struct rcu_head          mle_rcu;
};

/* MAC limit profile functions*/
static int mac_limit_setup_profile_table(void)
{
	mac_limit_profile_tbl = cds_lfht_new(MAC_LIMIT_PROFILE_TABLE_MIN,
					     MAC_LIMIT_PROFILE_TABLE_MIN,
					     MAC_LIMIT_PROFILE_TABLE_MAX,
					     CDS_LFHT_AUTO_RESIZE,
					     NULL);
	if (!mac_limit_profile_tbl) {
		RTE_LOG(ERR, MAC_LIMIT,
			"Could not allocate mac limit profile table\n");
		return -ENOMEM;
	}
	return 0;
}

static inline uint32_t mac_limit_profile_hash(const char *profile_name)
{
	int len = strlen(profile_name);
	char copy[len+3];

	memcpy(copy, profile_name, len);
	return rte_jhash(copy, len, 0);
}

static inline int mac_limit_profile_match_fn(struct cds_lfht_node *node,
					     const void *arg)
{
	const char *profile_name = arg;
	const struct mac_limit_profile *profile;

	profile = caa_container_of(node, const struct mac_limit_profile,
				   mlp_node);

	if (strcmp(profile_name, profile->mlp_name) == 0)
		return 1;

	return 0;
}

static struct mac_limit_profile *
mac_limit_add_profile(const char *name)
{
	struct mac_limit_profile *profile = NULL;
	unsigned long name_hash;
	struct cds_lfht_node *ret_node;
	int rc;

	if (!mac_limit_profile_tbl) {
		rc = mac_limit_setup_profile_table();
		if (rc) {
			RTE_LOG(ERR, MAC_LIMIT,
				"Failed to add profile %s, no profile tbl\n",
				name);
			return NULL;
		}
	}

	profile = calloc(1, sizeof(*profile));
	if (!profile) {
		RTE_LOG(ERR, MAC_LIMIT,
			"Could not allocate mac limit profile %s\n",
			name);
		return NULL;
	}

	profile->mlp_name = strdup(name);
	if (!profile->mlp_name) {
		free(profile);
		RTE_LOG(ERR, MAC_LIMIT,
			"Could not allocate mac limit profile %s\n",
			name);
		return NULL;
	}

	cds_lfht_node_init(&profile->mlp_node);
	name_hash = mac_limit_profile_hash(name);
	ret_node = cds_lfht_add_unique(mac_limit_profile_tbl, name_hash,
				       mac_limit_profile_match_fn, name,
				       &profile->mlp_node);

	if (ret_node != &profile->mlp_node) {
		free(profile->mlp_name);
		free(profile);
		profile = caa_container_of(ret_node, struct mac_limit_profile,
					   mlp_node);
		RTE_LOG(DEBUG, MAC_LIMIT,
			"Found an existing profile %s (%lx)\n",
			name, (unsigned long)profile);
	} else {
		CDS_INIT_LIST_HEAD(&profile->mlp_list);
		RTE_LOG(DEBUG, MAC_LIMIT, "Added profile %s (%lx)\n", name,
			(unsigned long)profile);
	}

	return profile;
}


static void mac_limit_free_profile(struct rcu_head *head)
{
	struct mac_limit_profile *profile;

	profile = caa_container_of(head, struct mac_limit_profile, mlp_rcu);
	free(profile->mlp_name);
	free(profile);
}

static void mac_limit_delete_profile(struct mac_limit_profile *profile)
{
	RTE_LOG(INFO, MAC_LIMIT, "Delete profile %s\n", profile->mlp_name);

	cds_lfht_del(mac_limit_profile_tbl, &profile->mlp_node);
	call_rcu(&profile->mlp_rcu, mac_limit_free_profile);
}

static struct mac_limit_profile *
mac_limit_find_profile(const char *name)
{
	struct mac_limit_profile *profile = NULL;
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;

	if (!mac_limit_profile_tbl)
		return NULL;

	cds_lfht_lookup(mac_limit_profile_tbl,
			mac_limit_profile_hash(name),
			mac_limit_profile_match_fn,
			name, &iter);

	node = cds_lfht_iter_get_node(&iter);
	if (node)
		profile = caa_container_of(node, struct mac_limit_profile,
					   mlp_node);

	return profile;
}

static void
mac_limit_profile_set_limit(struct mac_limit_profile *profile, uint32_t limit)
{
	profile->mlp_limit = limit;

	/* TODO - update the list of attachment points */
}

static int mac_limit_set_profile(MacLimitConfig__MacLimitProfileConfig *cfg)
{
	char *profile_name;
	struct mac_limit_profile *profile;
	bool set = cfg->action == MAC_LIMIT_CONFIG__ACTION__SET;

	profile_name = cfg->profile;
	if (!profile_name) {
		RTE_LOG(ERR, MAC_LIMIT,
			"Missing profile name in profile update\n");
		return 0;
	}

	profile = mac_limit_find_profile(profile_name);
	if (!profile) {
		if (set) {
			profile = mac_limit_add_profile(profile_name);
			if (!profile) {
				RTE_LOG(INFO, MAC_LIMIT,
					"Could not create mac limit profile %s\n",
					profile_name);
				return -ENOMEM;
			}
		} else {
			RTE_LOG(INFO, MAC_LIMIT,
				"Could not find profile %s\n", profile_name);
			return -ENOENT;
		}
	}

	if (!set)
		mac_limit_profile_set_limit(profile, 0);
	else
		mac_limit_profile_set_limit(profile, cfg->limit);

	if (!set) {
		if (!cds_list_empty(&profile->mlp_list)) {
			RTE_LOG(DEBUG, MAC_LIMIT,
				"Not deleting profile %s, list not EMPTY\n",
				profile->mlp_name);
			return 0;
		}
		/* Delete the profile if it is not referred to by anythign */
		mac_limit_delete_profile(profile);
	}

	return 0;
}

/*
 * MAC limit Entry functions
 */
static struct mac_limit_entry *mle_find_entry(struct ifnet *ifp,
					      uint16_t vlan)
{
	struct mac_limit_entry *entry;
	struct mac_limit_entry *next;

	if (!mac_limit_list)
		return NULL;

	cds_list_for_each_entry_safe(entry, next, mac_limit_list, mle_list) {
		if ((entry->mle_ifp == ifp) && (entry->mle_vlan == vlan))
			return (entry);
	}
	return NULL;
}

static struct mac_limit_entry *mle_add_entry(struct ifnet *ifp,
					     uint16_t vlan)
{
	struct mac_limit_entry *entry;

	if (!mac_limit_list) {
		mac_limit_list = calloc(1, sizeof(*mac_limit_list));
		if (!mac_limit_list)
			return NULL;

		CDS_INIT_LIST_HEAD(mac_limit_list);
	}
	entry = calloc(1, sizeof(*entry));
	if (!entry) {
		RTE_LOG(ERR, MAC_LIMIT,
			"Failed to alloc mac limit_list entry");
		return NULL;
	}
	entry->mle_vlan = vlan;
	entry->mle_ifp = ifp;
	cds_list_add_tail(&entry->mle_list, mac_limit_list);
	RTE_LOG(DEBUG, MAC_LIMIT,
		"Allocated entry %lx for Intf: %s, vlan: %d\n",
		(unsigned long)entry, ifp->if_name, vlan);
	return entry;
}

static void mle_entry_free(struct rcu_head *head)
{
	struct mac_limit_entry *entry;

	entry = caa_container_of(head, struct mac_limit_entry, mle_rcu);
	free(entry);
}

static void mle_delete_entry(struct mac_limit_entry *entry)
{
	if (!entry)
		return;

	cds_list_del(&entry->mle_list);
	cds_list_del(&entry->mle_profile_list);
	RTE_LOG(DEBUG, MAC_LIMIT, "Freeing entry %lx\n",
		(unsigned long)entry);
	call_rcu(&entry->mle_rcu, mle_entry_free);
}

/*
 * mac-limit <SET|DELETE> <ifname> <vlan> <profile>
 */
static int mac_limit_set_intf_cfg(MacLimitConfig__MacLimitIfVLANConfig *cfg)
{
	bool set = cfg->action == MAC_LIMIT_CONFIG__ACTION__SET;
	char *ifname, *pname;
	struct ifnet *ifp = NULL;
	struct mac_limit_entry *entry = NULL;
	struct mac_limit_profile *profile = NULL;
	uint16_t vlan;

	ifname = cfg->ifname;
	vlan = cfg->vlan;
	pname = cfg->profile;

	RTE_LOG(DEBUG, MAC_LIMIT,
		"set_intf_cfg: intf %s vlan %u profile %s\n",
		ifname, vlan, pname);

	ifp = dp_ifnet_byifname(ifname);
	if (!ifp) {
		RTE_LOG(ERR, MAC_LIMIT, "No interface %s\n", ifname);
		return -1;
	}

	entry = mle_find_entry(ifp, vlan);

	if (set) {
		profile = mac_limit_find_profile(pname);
		if (!profile) {
			RTE_LOG(ERR, MAC_LIMIT, "Invalid profile %s\n", pname);
			return -1;
		}
		if (entry) {
			/*
			 * Existing entry. If the profile name differs from the
			 * one we are adding, need to undo the existing one
			 * first.
			 */
			if (entry->mle_profile != profile) {
				cds_list_del(&entry->mle_profile_list);
				entry->mle_profile = NULL;
			}
		} else {
			entry = mle_add_entry(ifp, vlan);
			if (!entry)
				return -1;
		}
		entry->mle_profile = profile;
		cds_list_add_rcu(&entry->mle_profile_list,
				 &profile->mlp_list);
		/* TODO - apply update to FAL */
	} else {
		/* Nothing to delete */
		if (!entry)
			return 0;

		/* TODO - unapply fal update */
		mle_delete_entry(entry);
	}

	return 0;
}

/*
 * mac-limit SET profile <profile> <limit>
 * mac-limit DELETE profile <profile> <limit>
 *
 * mac-limit SET <interface> <vlan> <name>
 * mac-limit DELETE <interface> <vlan> <name>
 *
 */
static int
cmd_mac_limit_cfg(struct pb_msg *msg)
{
	MacLimitConfig *mlmsg = mac_limit_config__unpack(NULL, msg->msg_len,
							 msg->msg);
	int ret;

	if (!mlmsg) {
		RTE_LOG(ERR, DATAPLANE,
			"failed to read MacLimitConfig protobuf command\n");
		return -1;
	}

	switch (mlmsg->mtype_case) {
	case MAC_LIMIT_CONFIG__MTYPE_PROFILE:
		ret = mac_limit_set_profile(mlmsg->profile);
		break;
	case MAC_LIMIT_CONFIG__MTYPE_IFVLAN:
		ret = mac_limit_set_intf_cfg(mlmsg->ifvlan);
		break;
	default:
		RTE_LOG(INFO, MAC_LIMIT,
			"unhandled MacLimitConfig message type %d\n",
			mlmsg->mtype_case);
		ret = 0;
		break;
	}

	mac_limit_config__free_unpacked(mlmsg, NULL);
	return ret;
}

PB_REGISTER_CMD(maclimit_cmd) = {
	.cmd = "vyatta:maclimit",
	.handler = cmd_mac_limit_cfg,
};
