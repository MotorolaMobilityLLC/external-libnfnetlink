/* iftable - table of network interfaces
 *
 * (C) 2004 by Astaro AG, written by Harald Welte <hwelte@astaro.com>
 *
 * This software is Free Software and licensed under GNU GPLv2. 
 *
 */

/* IFINDEX handling */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <linux/netdevice.h>

#include <libnfnetlink/libnfnetlink.h>
#include "rtnl.h"

#define iftb_log(x, ...)

struct ifindex_map {
	struct ifindex_map *next;

	u_int32_t	index;
	u_int32_t	type;
	u_int32_t	alen;
	u_int32_t	flags;
	char		addr[8];
	char		name[16];
};

struct nlif_handle {
	struct ifindex_map *ifindex_map[16];
	struct rtnl_handle *rtnl_handle;
	struct rtnl_handler *ifadd_handler;
	struct rtnl_handler *ifdel_handler;
};

/* iftable_add - Add/Update an entry to/in the interface table
 * @n:		netlink message header of a RTM_NEWLINK message
 * @arg:	not used
 *
 * This function adds/updates an entry in the intrface table.
 * Returns -1 on error, 1 on success.
 */
int iftable_add(struct nlmsghdr *n, void *arg)
{
	unsigned int hash;
	struct ifinfomsg *ifi_msg = NLMSG_DATA(n);
	struct ifindex_map *im, **imp;
	struct rtattr *cb[IFLA_MAX+1];
	struct nlif_handle *nlif_handle = (struct nlif_handle *)arg;

	if (n->nlmsg_type != RTM_NEWLINK)
		return -1;

	if (n->nlmsg_len < NLMSG_LENGTH(sizeof(ifi_msg))) {
		iftb_log(LOG_ERROR, "short message (%u < %u)",
			 n->nlmsg_len, NLMSG_LENGTH(sizeof(ifi_msg)));
		return -1;
	}

	memset(&cb, 0, sizeof(cb));
	rtnl_parse_rtattr(cb, IFLA_MAX, IFLA_RTA(ifi_msg), IFLA_PAYLOAD(n));
	if (!cb[IFLA_IFNAME]) {
		iftb_log(LOG_ERROR, "interface without name?");
		return -1;
	}

	hash = ifi_msg->ifi_index&0xF;
	for (imp = &((nlif_handle->ifindex_map)[hash]); 
	     (im=*imp)!=NULL; imp = &im->next) {
		if (im->index == ifi_msg->ifi_index) {
			iftb_log(LOG_DEBUG,
				 "updating iftable (ifindex=%u)", im->index);
			break;
		}
	}

	if (!im) {
		im = malloc(sizeof(*im));
		if (!im) {
			iftb_log(LOG_ERROR,
				 "ENOMEM while allocating ifindex_map");
			return 0;
		}
		im->next = *imp;
		im->index = ifi_msg->ifi_index;
		*imp = im;
		iftb_log(LOG_DEBUG, "creating new iftable (ifindex=%u)",
			 im->index);
	}
	
	im->type = ifi_msg->ifi_type;
	im->flags = ifi_msg->ifi_flags;
	if (cb[IFLA_ADDRESS]) {
		unsigned int alen;
		im->alen = alen = RTA_PAYLOAD(cb[IFLA_ADDRESS]);
		if (alen > sizeof(im->addr))
			alen = sizeof(im->addr);
		memcpy(im->addr, RTA_DATA(cb[IFLA_ADDRESS]), alen);
	} else {
		im->alen = 0;
		memset(im->addr, 0, sizeof(im->addr));
	}
	strcpy(im->name, RTA_DATA(cb[IFLA_IFNAME]));
	return 1;
}

/* iftable_del - Delete an entry from the interface table
 * @n:		netlink message header of a RTM_DELLINK nlmsg
 * @arg:	not used
 *
 * Delete an entry from the interface table.  
 * Returns -1 on error, 0 if no matching entry was found or 1 on success.
 */
int iftable_del(struct nlmsghdr *n, void *arg)
{
	struct ifinfomsg *ifi_msg = NLMSG_DATA(n);
	struct rtattr *cb[IFLA_MAX+1];
	struct nlif_handle *nlif_handle = (struct nlif_handle *)arg;
	struct ifindex_map *im, *ima, **imp;
	unsigned int hash;

	if (n->nlmsg_type != RTM_DELLINK) {
		iftb_log(LOG_ERROR,
			 "called with wrong nlmsg_type %u", n->nlmsg_type);
		return -1;
	}

	if (n->nlmsg_len < NLMSG_LENGTH(sizeof(ifi_msg))) {
		iftb_log(LOG_ERROR, "short message (%u < %u)",
			 n->nlmsg_len, NLMSG_LENGTH(sizeof(ifi_msg)));
		return -1;
	}

	memset(&cb, 0, sizeof(cb));
	rtnl_parse_rtattr(cb, IFLA_MAX, IFLA_RTA(ifi_msg), IFLA_PAYLOAD(n));

	/* \todo Really suppress entry */
	hash = ifi_msg->ifi_index&0xF;
	for (ima = NULL, imp = &((nlif_handle->ifindex_map)[hash]); 
	     (im=*imp)!=NULL; imp = &im->next, ima=im) {
		if (im->index == ifi_msg->ifi_index) {
			iftb_log(LOG_DEBUG,
				 "deleting iftable (ifindex=%u)", im->index);
			break;
		}
	}

	if (!im)
		return 0;

	if (ima)
		ima->next = *imp;
	else
		(nlif_handle->ifindex_map)[hash] = *imp;
	free(im);

	return 1;
}

/** Get the name for an ifindex
 *
 * \param nlif_handle A pointer to a ::nlif_handle created
 * \param index ifindex to be resolved
 * \param name interface name, pass a buffer of IFNAMSIZ size
 * \return -1 on error, 1 on success 
 */
int nlif_index2name(struct nlif_handle *nlif_handle, 
		    unsigned int index,
		    char *name)
{
	struct ifindex_map *im;

	if (index == 0) {
		strcpy(name, "*");
		return 1;
	}
	for (im = (nlif_handle->ifindex_map)[index&0xF]; im; im = im->next)
		if (im->index == index) {
			strcpy(name, im->name);
			return 1;
		}

	return -1;
}

/* iftable_up - Determine whether a given interface is UP
 * @index:	ifindex of interface
 *
 * Return value: -1 if interface unknown, 1 if interface up, 0 if not.
 */
int iftable_up(struct nlif_handle *nlif_handle, unsigned int index)
{
	struct ifindex_map *im;

	for (im = nlif_handle->ifindex_map[index&0xF]; im; im = im->next) {
		if (im->index == index) {
			if (im->flags & IFF_UP)
				return 1;
			else
				return 0;
		}
	}
	return -1;
}

static struct nlif_handle *init_or_fini(struct nlif_handle *orig)
{
	struct nlif_handle *nlif_handle;
	int ret = 0;

	if (orig) {
		nlif_handle = orig;
		goto cleanup;
	}

		
	nlif_handle = calloc(1,  sizeof(struct nlif_handle));
	if (!nlif_handle)
		goto cleanup_none;

	nlif_handle->ifadd_handler = calloc(1, sizeof(struct rtnl_handler));
	nlif_handle->ifadd_handler->nlmsg_type = RTM_NEWLINK;
	nlif_handle->ifadd_handler->handlefn = &iftable_add;
	nlif_handle->ifadd_handler->arg = nlif_handle;
	nlif_handle->ifdel_handler = calloc(1, sizeof(struct rtnl_handler));
	nlif_handle->ifdel_handler->nlmsg_type = RTM_DELLINK;
	nlif_handle->ifdel_handler->handlefn = &iftable_del;
	nlif_handle->ifdel_handler->arg = nlif_handle;

	nlif_handle->rtnl_handle = rtnl_init();

	if (! nlif_handle->rtnl_handle)
		goto cleanup_none;

	if (rtnl_handler_register(nlif_handle->rtnl_handle, 
				  nlif_handle->ifadd_handler) < 0) {
		ret = -1;
		goto cleanup_none;
	}

	if (rtnl_handler_register(nlif_handle->rtnl_handle,
				  nlif_handle->ifdel_handler) < 0) {
		ret = -1;
		goto cleanup_0;
	}


	return nlif_handle;

#if 0
	if (rtnl_wilddump_requet(rtnl_fd, AF_UNSPEC, RTM_GETLINK) < 0) {
		iftb_log(LOG_ERROR, "unable to send dump request");
		return -1;
	}

#endif

cleanup:
	rtnl_handler_unregister(nlif_handle->rtnl_handle,
				nlif_handle->ifadd_handler);
	free(nlif_handle->ifadd_handler);
cleanup_0:
	rtnl_handler_unregister(nlif_handle->rtnl_handle,
				nlif_handle->ifdel_handler);
	free(nlif_handle->ifdel_handler);
	rtnl_fini(nlif_handle->rtnl_handle);
	free(nlif_handle);

cleanup_none:
	return nlif_handle;
}

/** Initialize interface table
 *
 * Initialize rtnl interface and interface table
 * Call this before any nlif_* function
 *
 * \return file descriptor to netlink socket
 */
struct nlif_handle *nlif_open(void)
{
	iftb_log(LOG_DEBUG, "%s", __FUNCTION__);
	return init_or_fini(NULL);
}

/** Destructor of interface table
 *
 * \param nlif_handle A pointer to a ::nlif_handle created 
 * via nlif_open()
 */
void nlif_close(struct nlif_handle *nlif_handle)
{
	init_or_fini(nlif_handle);
}

/** Receive message from netlink and update interface table
 *
 * \param nlif_handle A pointer to a ::nlif_handle created
 * \return 0 if OK
 */
int nlif_catch(struct nlif_handle *nlif_handle)
{
	if (nlif_handle && nlif_handle->rtnl_handle)
		return rtnl_receive(nlif_handle->rtnl_handle);
	else
		return -1;
}

/** 
 * nlif_query - request a dump of interfaces available in the system
 * @h: pointer to a valid nlif_handler
 */
int nlif_query(struct nlif_handle *h)
{
	if (rtnl_dump_type(h->rtnl_handle, RTM_GETLINK) < 0)
		return -1;

	return nlif_catch(h);
}

/** Returns socket descriptor for the netlink socket
 *
 * \param nlif_handle A pointer to a ::nlif_handle created
 * \return The fd or -1 if there's an error
 */
int nlif_fd(struct nlif_handle *nlif_handle)
{
	if (nlif_handle && nlif_handle->rtnl_handle)
		return nlif_handle->rtnl_handle->rtnl_fd;
	else
		return -1;
}
