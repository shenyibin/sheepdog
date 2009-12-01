/*
 * Copyright (C) 2009 Nippon Telegraph and Telephone Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef __COLLIE_H__
#define __COLLIE_H__

#include <inttypes.h>
#include <corosync/cpg.h>

#include "sheepdog_proto.h"
#include "event.h"
#include "logger.h"
#include "work.h"
#include "net.h"

#define SD_MSG_JOIN             0x01
#define SD_MSG_VDI_OP           0x02
#define SD_MSG_MASTER_CHANGED   0x03

struct client_info {
	struct connection conn;

	struct request *rx_req;

	struct request *tx_req;

	struct list_head reqs;
	struct list_head done_reqs;

	struct cluster_info *cluster;
};

struct request;

typedef void (*req_end_t) (struct request *);

struct request {
	struct sd_req rq;
	struct sd_rsp rp;

	void *data;

	struct client_info *ci;
	struct list_head r_siblings;
	struct list_head r_wlist;
	struct list_head pending_list;

	req_end_t done;
	struct work work;
};

struct cluster_info {
	cpg_handle_t handle;
	int synchronized;
	uint32_t this_nodeid;
	uint32_t this_pid;
	struct sheepdog_node_list_entry this_node;

	uint64_t epoch;

	struct list_head node_list;
	struct list_head vm_list;
	struct list_head pending_list;
};

int create_listen_port(int port, void *data);

int init_store(char *dir);

int add_vdi(struct cluster_info *cluster,
	    char *name, int len, uint64_t size, uint64_t * added_oid,
	    uint64_t base_oid, uint32_t tag);

int lookup_vdi(struct cluster_info *cluster, char *filename, uint64_t * oid,
	       uint32_t tag, int do_lock, int *current);

int build_node_list(struct list_head *node_list,
		    struct sheepdog_node_list_entry *entries);

struct cluster_info *create_cluster(int port);

void store_queue_request(struct work *work, int idx);

void cluster_queue_request(struct work *work, int idx);

#endif