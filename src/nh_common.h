/*
 * Copyright (c) 2020, AT&T Intellectual Property.  All rights reserved.
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */
#ifndef NH_COMMON_H
#define NH_COMMON_H

#include <netinet/in.h>
#include <rte_fbk_hash.h>

#include "compiler.h"
#include "fal_plugin.h"
#include "ip_addr.h"
#include "mpls/mpls.h"
#include "pd_show.h"
#include "route_flags.h"
#include "urcu.h"

struct ifnet;
struct llentry;

/* Output information associated with a single nexthop */
struct next_hop {
	union {
		struct ifnet *ifp;     /* target interface */
		struct llentry *lle;   /* lle entry to use when sending */
	} u;
	uint32_t      flags;   /* routing flags */
	union next_hop_outlabels outlabels;
	union {
		in_addr_t       gateway4; /* nexthop IPv4 address */
		struct in6_addr gateway6; /* nexthop IPv6 address */
	};
};

/*
 * This is the nexthop information result of route lookup - allows for
 * multiple nexthops in the case of ECMP
 */
struct next_hop_list {
	struct next_hop      *siblings;	/* array of next_hop */
	uint8_t              nsiblings;	/* # of next_hops */
	uint8_t              proto;	/* routing protocol */
	uint16_t             padding;
	uint32_t             index;
	struct next_hop      hop0;      /* optimization for non-ECMP */
	uint32_t             refcount;	/* # of LPM's referring */
	enum pd_obj_state    pd_state;
	struct cds_lfht_node nh_node;
	fal_object_t         nhg_fal_obj;   /* FAL handle for next_hop_group */
	fal_object_t         *nh_fal_obj; /* Per-nh FAL handles */
	struct rcu_head      rcu;
} __rte_cache_aligned;

/*
 * key for hashing an array of NHs. Size is the number of NHs in the array.
 */
struct nexthop_hash_key {
	const struct next_hop *nh;
	size_t		       size;
	uint8_t		       proto;
};

/*

 * The nexthop in LPM is 22 bits but dpdk hash tables currently have a
 * limit of 2^20 entries.
 */
#define NEXTHOP_HASH_TBL_SIZE RTE_FBK_HASH_ENTRIES_MAX
#define NEXTHOP_HASH_TBL_MIN  (UINT8_MAX + 1)

struct nexthop_table {
	uint32_t in_use;  /* # of entries used */
	uint32_t rover;   /* next free slot to look at */
	struct next_hop_list *entry[NEXTHOP_HASH_TBL_SIZE]; /* entry array */
	uint32_t neigh_present;
	uint32_t neigh_created;
};

enum nh_type {
	NH_TYPE_V4GW, /* struct next_hop  */
	NH_TYPE_V6GW, /* struct next_hop_v6 */
};

void nh_set_ifp(struct next_hop *next_hop, struct ifnet *ifp);

struct next_hop_list *nexthop_alloc(int size);

void __nexthop_destroy(struct next_hop_list *nextl);

void nexthop_destroy(struct rcu_head *head);

int nexthop_new(int family, const struct next_hop *nh, uint16_t size,
		uint8_t proto, uint32_t *slot);

/*
 * Create a next_hop based on the given information.  This nexthop will then
 * be used as the argument to nexthop_new.
 *
 * @param[in] ifp The interface the nexthop uses.
 * @param[in] gw  The gateway for the nexthop
 * @param[in] flags The flags to set in the nexthop
 * @param[in] num_labels The number of labels for the nexthop
 * @param[in] labels An array of labels, of size 'num_labels'
 *
 * @return A next_hop on success
 * @return NULL on failure.
 */
struct next_hop *
nexthop_create(struct ifnet *ifp, struct ip_addr *gw, uint32_t flags,
	       uint16_t num_labels, label_t *labels);

void nexthop_put(int family, uint32_t idx);

/*
 * Given an nexthop_u create a copy of the nexthops in an array
 *
 * @param[in] nhl The fully formed nhl
 * @param[out] size Store the size of the created array here.
 *
 * @return Pointer to array of nexthops on success
 * @return NULL on failure
 */
struct next_hop *nexthop_create_copy(struct next_hop_list *nhl, int *size);

/*
 * Remove the old NH from the hash and add the new one. Can not
 * use a call to cds_lfht_add_replace() or any of the variants
 * as the key for the new NH may be very different in the case
 * where there are a different number of paths.
 *
 * @param[in] family The address family for this nexthop
 * @param[in] old_nu The old nexthop_u to remove from the hash
 * @param[in] new_nu The new nexthop_u to add to the hash
 *
 * @retval 0 on success
 *         -ve on failure
 */
int
nexthop_hash_del_add(int family,
		     struct next_hop_list *old_nhl,
		     struct next_hop_list *new_nhl);

/*
 * Modify a NH to mark it as neigh present. This is done in a non atomic
 * way, so this must be atomically swapped into the forwarding state when
 * ready.
 *
 * @param[in] family The family the nh is using.
 * @param[out] nh The next_hop to modify
 * @param[in] lle The lle entry that the next_hop needs to link to.
 *
 */
void nh_set_neigh_present(int family __unused,
			  struct next_hop *next_hop,
			  struct llentry *lle);

/*
 * Modify a NH to clear the neigh.
 *
 * @param[in] family The family the nh is using.
 * @param[out] nh The next_hop to modify
 */
void nh_clear_neigh_present(int family,
			    struct next_hop *next_hop);

/*
 * Modify a NH to mark it as neigh created. This is done in a non atomic
 * way, so this must be atomically swapped into the forwarding state when
 * ready.
 *
 * @param[in] family The family the nh is using.
 * @param[out] nh The next_hop to modify
 * @param[in] lle The lle entry that the next_hop needs to link to.
 */
void nh_set_neigh_created(int family,
			  struct next_hop *next_hop,
			  struct llentry *lle);

/*
 * Modify a NH to clear the fact that it was neigh created.
 *
 * @param[in] family The family the nh is using.
 * @param[out] nh The next_hop to modify
 */
void nh_clear_neigh_created(int family,
			    struct next_hop *next_hop);

/*
 * Get the number of neighbour created entries in the next_hop_list
 *
 * @param[in] nhl The next_hop_list to check
 *
 * @return the count of neighbour created entries in the nhl.
 */
int next_hop_list_nc_count(const struct next_hop_list *nhl);

/*
 * Given a next_hop_list and an ifp, find the next_hop within the
 * next_hop_list that uses the given interface.
 *
 * @param[in] nhl The next_hop_list to check
 * @param[in] ifp The ifp to look for
 * @param[out] sibling Store the index of the returned nexthop
 *
 * @return A ptr to the next_hop if one matched
 *         Null if no match found.
 */
struct next_hop *next_hop_list_find_path_using_ifp(struct next_hop_list *nhl,
						   struct ifnet *ifp,
						   int *sibling);

/*
 * Given a next_hop_list check if any of the hops are connected
 *
 * @param[in] nh The next_hop_list to check
 *
 * @return True if there is a connected nexthop
 * @return False if there is not a connected nexthop
 */
bool next_hop_list_is_any_connected(const struct next_hop_list *nhl);

struct next_hop *nexthop_mp_select(struct next_hop *next,
				   uint32_t size,
				   uint32_t hash);

struct next_hop *nexthop_select(int family, uint32_t nh_idx,
				const struct rte_mbuf *m,
				uint16_t ether_type);

bool nh_is_connected(const struct next_hop *nh);
bool nh_is_local(const struct next_hop *nh);
bool nh_is_gw(const struct next_hop *nh);

static ALWAYS_INLINE bool
nh_is_neigh_present(const struct next_hop *next_hop)
{
	return next_hop->flags & RTF_NEIGH_PRESENT;
}

static ALWAYS_INLINE bool
nh_is_neigh_created(const struct next_hop *next_hop)
{
	return next_hop->flags & RTF_NEIGH_CREATED;
}

static ALWAYS_INLINE struct llentry *
nh_get_lle(const struct next_hop *next_hop)
{
	if (next_hop->flags & (RTF_NEIGH_CREATED | RTF_NEIGH_PRESENT))
		return rcu_dereference(next_hop->u.lle);

	return NULL;
}

static inline const union next_hop_outlabels *
nh_get_labels(struct next_hop *nh)
{
	return &nh->outlabels;
}

static inline uint32_t
nh_get_flags(struct next_hop *nh)
{
	return nh->flags;
}

/*
 * Per AF hash function for a nexthop.
 */
typedef int (nh_common_hash_fn)(const struct nexthop_hash_key *key,
				unsigned long seed);

/*
 * Per AF function to compare nexthops
 */
typedef int (nh_common_cmp_fn)(struct cds_lfht_node *node, const void *key);

/*
 * get the hash table used to track NHs and if a new can be reused.
 */
typedef struct cds_lfht *(nh_common_get_hash_tbl_fn)(void);

/*
 * Get the table that the NHs are stored in.
 */
typedef struct nexthop_table *(nh_common_get_nh_tbl_fn)(void);

/*
 * Structure to hold all the function pointers required to do the
 * NH processing that differs between address families.
 */
struct nh_common {
	nh_common_hash_fn *nh_hash;
	nh_common_cmp_fn *nh_compare;
	nh_common_get_hash_tbl_fn *nh_get_hash_tbl;
	nh_common_get_nh_tbl_fn *nh_get_nh_tbl;
};

/*
 * Register AF specific behaviour for processing NHs.
 */
void nh_common_register(int family, struct nh_common *nh_common);

#endif /* NH_COMMON_H */