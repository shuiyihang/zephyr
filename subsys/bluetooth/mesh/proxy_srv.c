/*
 * Copyright (c) 2017 Intel Corporation
 * Copyright (c) 2021 Lingao Meng
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <sys/byteorder.h>

#include <net/buf.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/conn.h>
#include <bluetooth/gatt.h>
#include <bluetooth/mesh.h>

#define BT_DBG_ENABLED IS_ENABLED(CONFIG_BT_MESH_DEBUG_PROXY)
#define LOG_MODULE_NAME bt_mesh_gatt
#include "common/log.h"

#include "mesh.h"
#include "adv.h"
#include "net.h"
#include "rpl.h"
#include "transport.h"
#include "host/ecc.h"
#include "prov.h"
#include "beacon.h"
#include "foundation.h"
#include "access.h"
#include "proxy.h"
#include "proxy_msg.h"

#if defined(CONFIG_BT_MESH_PROXY_USE_DEVICE_NAME)
#define ADV_OPT_USE_NAME BT_LE_ADV_OPT_USE_NAME
#else
#define ADV_OPT_USE_NAME 0
#endif

#define ADV_OPT_PROXY                                                           \
	(BT_LE_ADV_OPT_CONNECTABLE | BT_LE_ADV_OPT_SCANNABLE |                 \
	 BT_LE_ADV_OPT_ONE_TIME | ADV_OPT_USE_IDENTITY |                       \
	 ADV_OPT_USE_NAME)

static void proxy_send_beacons(struct k_work *work);
static int proxy_send(struct bt_conn *conn,
		      const void *data, uint16_t len,
		      bt_gatt_complete_func_t end, void *user_data);

static struct bt_mesh_proxy_client {
	struct bt_mesh_proxy_role *cli;
	uint16_t filter[CONFIG_BT_MESH_PROXY_FILTER_SIZE];
	enum __packed {
		NONE,
		ACCEPT,
		REJECT,
	} filter_type;
	struct k_work send_beacons;
} clients[CONFIG_BT_MAX_CONN] = {
	[0 ... (CONFIG_BT_MAX_CONN - 1)] = {
		.send_beacons = Z_WORK_INITIALIZER(proxy_send_beacons),
	},
};

static bool service_registered;
static int conn_count;

static struct bt_mesh_proxy_client *find_client(struct bt_conn *conn)
{
	return &clients[bt_conn_index(conn)];
}

static ssize_t gatt_recv(struct bt_conn *conn,
			 const struct bt_gatt_attr *attr, const void *buf,
			 uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *data = buf;
	struct bt_mesh_proxy_client *client = find_client(conn);

	if (len < 1) {
		BT_WARN("Too small Proxy PDU");
		return -EINVAL;
	}

	if (PDU_TYPE(data) == BT_MESH_PROXY_PROV) {
		BT_WARN("Proxy PDU type doesn't match GATT service");
		return -EINVAL;
	}

	return bt_mesh_proxy_msg_recv(client->cli, buf, len);
}

/* Next subnet in queue to be advertised */
static struct bt_mesh_subnet *beacon_sub;

static int filter_set(struct bt_mesh_proxy_client *client,
		      struct net_buf_simple *buf)
{
	uint8_t type;

	if (buf->len < 1) {
		BT_WARN("Too short Filter Set message");
		return -EINVAL;
	}

	type = net_buf_simple_pull_u8(buf);
	BT_DBG("type 0x%02x", type);

	switch (type) {
	case 0x00:
		(void)memset(client->filter, 0, sizeof(client->filter));
		client->filter_type = ACCEPT;
		break;
	case 0x01:
		(void)memset(client->filter, 0, sizeof(client->filter));
		client->filter_type = REJECT;
		break;
	default:
		BT_WARN("Prohibited Filter Type 0x%02x", type);
		return -EINVAL;
	}

	return 0;
}

static void filter_add(struct bt_mesh_proxy_client *client, uint16_t addr)
{
	int i;

	BT_DBG("addr 0x%04x", addr);

	if (addr == BT_MESH_ADDR_UNASSIGNED) {
		return;
	}

	for (i = 0; i < ARRAY_SIZE(client->filter); i++) {
		if (client->filter[i] == addr) {
			return;
		}
	}

	for (i = 0; i < ARRAY_SIZE(client->filter); i++) {
		if (client->filter[i] == BT_MESH_ADDR_UNASSIGNED) {
			client->filter[i] = addr;
			return;
		}
	}
}

static void filter_remove(struct bt_mesh_proxy_client *client, uint16_t addr)
{
	int i;

	BT_DBG("addr 0x%04x", addr);

	if (addr == BT_MESH_ADDR_UNASSIGNED) {
		return;
	}

	for (i = 0; i < ARRAY_SIZE(client->filter); i++) {
		if (client->filter[i] == addr) {
			client->filter[i] = BT_MESH_ADDR_UNASSIGNED;
			return;
		}
	}
}

static void send_filter_status(struct bt_mesh_proxy_client *client,
			       struct bt_mesh_net_rx *rx,
			       struct net_buf_simple *buf)
{
	struct bt_mesh_net_tx tx = {
		.sub = rx->sub,
		.ctx = &rx->ctx,
		.src = bt_mesh_primary_addr(),
	};
	uint16_t filter_size;
	int i, err;

	/* Configuration messages always have dst unassigned */
	tx.ctx->addr = BT_MESH_ADDR_UNASSIGNED;

	net_buf_simple_reset(buf);
	net_buf_simple_reserve(buf, 10);

	net_buf_simple_add_u8(buf, CFG_FILTER_STATUS);

	if (client->filter_type == ACCEPT) {
		net_buf_simple_add_u8(buf, 0x00);
	} else {
		net_buf_simple_add_u8(buf, 0x01);
	}

	for (filter_size = 0U, i = 0; i < ARRAY_SIZE(client->filter); i++) {
		if (client->filter[i] != BT_MESH_ADDR_UNASSIGNED) {
			filter_size++;
		}
	}

	net_buf_simple_add_be16(buf, filter_size);

	BT_DBG("%u bytes: %s", buf->len, bt_hex(buf->data, buf->len));

	err = bt_mesh_net_encode(&tx, buf, true);
	if (err) {
		BT_ERR("Encoding Proxy cfg message failed (err %d)", err);
		return;
	}

	err = bt_mesh_proxy_msg_send(client->cli, BT_MESH_PROXY_CONFIG,
				     buf, NULL, NULL);
	if (err) {
		BT_ERR("Failed to send proxy cfg message (err %d)", err);
	}
}

static void proxy_filter_recv(struct bt_conn *conn,
			      struct bt_mesh_net_rx *rx, struct net_buf_simple *buf)
{
	struct bt_mesh_proxy_client *client;
	uint8_t opcode;

	client = find_client(conn);

	opcode = net_buf_simple_pull_u8(buf);
	switch (opcode) {
	case CFG_FILTER_SET:
		filter_set(client, buf);
		send_filter_status(client, rx, buf);
		break;
	case CFG_FILTER_ADD:
		while (buf->len >= 2) {
			uint16_t addr;

			addr = net_buf_simple_pull_be16(buf);
			filter_add(client, addr);
		}
		send_filter_status(client, rx, buf);
		break;
	case CFG_FILTER_REMOVE:
		while (buf->len >= 2) {
			uint16_t addr;

			addr = net_buf_simple_pull_be16(buf);
			filter_remove(client, addr);
		}
		send_filter_status(client, rx, buf);
		break;
	default:
		BT_WARN("Unhandled configuration OpCode 0x%02x", opcode);
		break;
	}
}

static void proxy_cfg(struct bt_mesh_proxy_role *role)
{
	NET_BUF_SIMPLE_DEFINE(buf, BT_MESH_NET_MAX_PDU_LEN);
	struct bt_mesh_net_rx rx;
	int err;

	err = bt_mesh_net_decode(&role->buf, BT_MESH_NET_IF_PROXY_CFG,
				 &rx, &buf);
	if (err) {
		BT_ERR("Failed to decode Proxy Configuration (err %d)", err);
		return;
	}

	rx.local_match = 1U;

	if (bt_mesh_rpl_check(&rx, NULL)) {
		BT_WARN("Replay: src 0x%04x dst 0x%04x seq 0x%06x",
			rx.ctx.addr, rx.ctx.recv_dst, rx.seq);
		return;
	}

	/* Remove network headers */
	net_buf_simple_pull(&buf, BT_MESH_NET_HDR_LEN);

	BT_DBG("%u bytes: %s", buf.len, bt_hex(buf.data, buf.len));

	if (buf.len < 1) {
		BT_WARN("Too short proxy configuration PDU");
		return;
	}

	proxy_filter_recv(role->conn, &rx, &buf);
}

static void proxy_msg_recv(struct bt_mesh_proxy_role *role)
{
	switch (role->msg_type) {
	case BT_MESH_PROXY_NET_PDU:
		BT_DBG("Mesh Network PDU");
		bt_mesh_net_recv(&role->buf, 0, BT_MESH_NET_IF_PROXY);
		break;
	case BT_MESH_PROXY_BEACON:
		BT_DBG("Mesh Beacon PDU");
		bt_mesh_beacon_recv(&role->buf);
		break;
	case BT_MESH_PROXY_CONFIG:
		BT_DBG("Mesh Configuration PDU");
		proxy_cfg(role);
		break;
	default:
		BT_WARN("Unhandled Message Type 0x%02x", role->msg_type);
		break;
	}
}

static int beacon_send(struct bt_mesh_proxy_client *client,
		       struct bt_mesh_subnet *sub)
{
	NET_BUF_SIMPLE_DEFINE(buf, 23);

	net_buf_simple_reserve(&buf, 1);
	bt_mesh_beacon_create(sub, &buf);

	return bt_mesh_proxy_msg_send(client->cli, BT_MESH_PROXY_BEACON,
				      &buf, NULL, NULL);
}

static int send_beacon_cb(struct bt_mesh_subnet *sub, void *cb_data)
{
	struct bt_mesh_proxy_client *client = cb_data;

	return beacon_send(client, sub);
}

static void proxy_send_beacons(struct k_work *work)
{
	struct bt_mesh_proxy_client *client;

	client = CONTAINER_OF(work, struct bt_mesh_proxy_client, send_beacons);

	(void)bt_mesh_subnet_find(send_beacon_cb, client);
}

void bt_mesh_proxy_beacon_send(struct bt_mesh_subnet *sub)
{
	int i;

	if (!sub) {
		/* NULL means we send on all subnets */
		bt_mesh_subnet_foreach(bt_mesh_proxy_beacon_send);
		return;
	}

	for (i = 0; i < ARRAY_SIZE(clients); i++) {
		if (clients[i].cli) {
			beacon_send(&clients[i], sub);
		}
	}
}

static void node_id_start(struct bt_mesh_subnet *sub)
{
	sub->node_id = BT_MESH_NODE_IDENTITY_RUNNING;
	sub->node_id_start = k_uptime_get_32();

	STRUCT_SECTION_FOREACH(bt_mesh_proxy_cb, cb) {
		if (cb->identity_enabled) {
			cb->identity_enabled(sub->net_idx);
		}
	}
}

void bt_mesh_proxy_identity_start(struct bt_mesh_subnet *sub)
{
	node_id_start(sub);

	/* Prioritize the recently enabled subnet */
	beacon_sub = sub;
}

void bt_mesh_proxy_identity_stop(struct bt_mesh_subnet *sub)
{
	sub->node_id = BT_MESH_NODE_IDENTITY_STOPPED;
	sub->node_id_start = 0U;

	STRUCT_SECTION_FOREACH(bt_mesh_proxy_cb, cb) {
		if (cb->identity_disabled) {
			cb->identity_disabled(sub->net_idx);
		}
	}
}

int bt_mesh_proxy_identity_enable(void)
{
	BT_DBG("");

	if (!bt_mesh_is_provisioned()) {
		return -EAGAIN;
	}

	if (bt_mesh_subnet_foreach(node_id_start)) {
		bt_mesh_adv_update();
	}

	return 0;
}

#define ID_TYPE_NET  0x00
#define ID_TYPE_NODE 0x01

#define NODE_ID_LEN  19
#define NET_ID_LEN   11

#define NODE_ID_TIMEOUT (CONFIG_BT_MESH_NODE_ID_TIMEOUT * MSEC_PER_SEC)

static uint8_t proxy_svc_data[NODE_ID_LEN] = {
	BT_UUID_16_ENCODE(BT_UUID_MESH_PROXY_VAL),
};

static const struct bt_data node_id_ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL,
		      BT_UUID_16_ENCODE(BT_UUID_MESH_PROXY_VAL)),
	BT_DATA(BT_DATA_SVC_DATA16, proxy_svc_data, NODE_ID_LEN),
};

static const struct bt_data net_id_ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL,
		      BT_UUID_16_ENCODE(BT_UUID_MESH_PROXY_VAL)),
	BT_DATA(BT_DATA_SVC_DATA16, proxy_svc_data, NET_ID_LEN),
};

static int node_id_adv(struct bt_mesh_subnet *sub, int32_t duration)
{
	struct bt_le_adv_param fast_adv_param = {
		.options = ADV_OPT_PROXY,
		ADV_FAST_INT,
	};
	uint8_t tmp[16];
	int err;

	BT_DBG("");

	proxy_svc_data[2] = ID_TYPE_NODE;

	err = bt_rand(proxy_svc_data + 11, 8);
	if (err) {
		return err;
	}

	(void)memset(tmp, 0, 6);
	memcpy(tmp + 6, proxy_svc_data + 11, 8);
	sys_put_be16(bt_mesh_primary_addr(), tmp + 14);

	err = bt_encrypt_be(sub->keys[SUBNET_KEY_TX_IDX(sub)].identity, tmp,
			    tmp);
	if (err) {
		return err;
	}

	memcpy(proxy_svc_data + 3, tmp + 8, 8);

	err = bt_mesh_adv_start(&fast_adv_param, duration, node_id_ad,
				ARRAY_SIZE(node_id_ad), NULL, 0);
	if (err) {
		BT_WARN("Failed to advertise using Node ID (err %d)", err);
		return err;
	}

	return 0;
}

static int net_id_adv(struct bt_mesh_subnet *sub, int32_t duration)
{
	struct bt_le_adv_param slow_adv_param = {
		.options = ADV_OPT_PROXY,
		ADV_SLOW_INT,
	};
	int err;

	BT_DBG("");

	proxy_svc_data[2] = ID_TYPE_NET;

	BT_DBG("Advertising with NetId %s",
	       bt_hex(sub->keys[SUBNET_KEY_TX_IDX(sub)].net_id, 8));

	memcpy(proxy_svc_data + 3, sub->keys[SUBNET_KEY_TX_IDX(sub)].net_id, 8);

	err = bt_mesh_adv_start(&slow_adv_param, duration, net_id_ad,
				ARRAY_SIZE(net_id_ad), NULL, 0);
	if (err) {
		BT_WARN("Failed to advertise using Network ID (err %d)", err);
		return err;
	}

	return 0;
}

static bool advertise_subnet(struct bt_mesh_subnet *sub)
{
	if (sub->net_idx == BT_MESH_KEY_UNUSED) {
		return false;
	}

	return (sub->node_id == BT_MESH_NODE_IDENTITY_RUNNING ||
		bt_mesh_gatt_proxy_get() == BT_MESH_GATT_PROXY_ENABLED);
}

static struct bt_mesh_subnet *next_sub(void)
{
	struct bt_mesh_subnet *sub = NULL;

	if (!beacon_sub) {
		beacon_sub = bt_mesh_subnet_next(NULL);
		if (!beacon_sub) {
			/* No valid subnets */
			return NULL;
		}
	}

	sub = beacon_sub;
	do {
		if (advertise_subnet(sub)) {
			beacon_sub = sub;
			return sub;
		}

		sub = bt_mesh_subnet_next(sub);
	} while (sub != beacon_sub);

	/* No subnets to advertise on */
	return NULL;
}

static int sub_count_cb(struct bt_mesh_subnet *sub, void *cb_data)
{
	int *count = cb_data;

	if (advertise_subnet(sub)) {
		(*count)++;
	}

	return 0;
}

static int sub_count(void)
{
	int count = 0;

	(void)bt_mesh_subnet_find(sub_count_cb, &count);

	return count;
}

static int gatt_proxy_advertise(struct bt_mesh_subnet *sub)
{
	int32_t remaining = SYS_FOREVER_MS;
	int subnet_count;
	int err = -EBUSY;

	BT_DBG("");

	if (conn_count == CONFIG_BT_MAX_CONN) {
		BT_DBG("Connectable advertising deferred (max connections)");
		return -ENOMEM;
	}

	sub = beacon_sub ? beacon_sub : bt_mesh_subnet_next(beacon_sub);
	if (!sub) {
		BT_WARN("No subnets to advertise on");
		return -ENOENT;
	}

	subnet_count = sub_count();
	BT_DBG("sub_count %u", subnet_count);
	if (subnet_count > 1) {
		int32_t max_timeout;

		/* We use NODE_ID_TIMEOUT as a starting point since it may
		 * be less than 60 seconds. Divide this period into at least
		 * 6 slices, but make sure that a slice is at least one
		 * second long (to avoid excessive rotation).
		 */
		max_timeout = NODE_ID_TIMEOUT / MAX(subnet_count, 6);
		max_timeout = MAX(max_timeout, 1 * MSEC_PER_SEC);

		if (remaining > max_timeout || remaining == SYS_FOREVER_MS) {
			remaining = max_timeout;
		}
	}

	if (sub->node_id == BT_MESH_NODE_IDENTITY_RUNNING) {
		uint32_t active = k_uptime_get_32() - sub->node_id_start;

		if (active < NODE_ID_TIMEOUT) {
			remaining = NODE_ID_TIMEOUT - active;
			BT_DBG("Node ID active for %u ms, %d ms remaining",
			       active, remaining);
			err = node_id_adv(sub, remaining);
		} else {
			bt_mesh_proxy_identity_stop(sub);
			BT_DBG("Node ID stopped");
		}
	}

	if (sub->node_id == BT_MESH_NODE_IDENTITY_STOPPED) {
		err = net_id_adv(sub, remaining);
	}

	BT_DBG("Advertising %d ms for net_idx 0x%04x", remaining, sub->net_idx);

	beacon_sub = bt_mesh_subnet_next(beacon_sub);

	return err;
}

static void subnet_evt(struct bt_mesh_subnet *sub, enum bt_mesh_key_evt evt)
{
	if (evt == BT_MESH_KEY_DELETED) {
		if (sub == beacon_sub) {
			beacon_sub = NULL;
		}
	} else {
		bt_mesh_proxy_beacon_send(sub);
	}
}

BT_MESH_SUBNET_CB_DEFINE(gatt_services) = {
	.evt_handler = subnet_evt,
};

static void proxy_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	BT_DBG("value 0x%04x", value);
}

static ssize_t proxy_ccc_write(struct bt_conn *conn,
			       const struct bt_gatt_attr *attr, uint16_t value)
{
	struct bt_mesh_proxy_client *client;

	BT_DBG("value: 0x%04x", value);

	if (value != BT_GATT_CCC_NOTIFY) {
		BT_WARN("Client wrote 0x%04x instead enabling notify", value);
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	client = find_client(conn);
	if (client->filter_type == NONE) {
		client->filter_type = ACCEPT;
		k_work_submit(&client->send_beacons);
	}

	return sizeof(value);
}

/* Mesh Proxy Service Declaration */
static struct _bt_gatt_ccc proxy_ccc =
	BT_GATT_CCC_INITIALIZER(proxy_ccc_changed, proxy_ccc_write, NULL);

static struct bt_gatt_attr proxy_attrs[] = {
	BT_GATT_PRIMARY_SERVICE(BT_UUID_MESH_PROXY),

	BT_GATT_CHARACTERISTIC(BT_UUID_MESH_PROXY_DATA_IN,
			       BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE,
			       NULL, gatt_recv, NULL),

	BT_GATT_CHARACTERISTIC(BT_UUID_MESH_PROXY_DATA_OUT,
			       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE,
			       NULL, NULL, NULL),
	BT_GATT_CCC_MANAGED(&proxy_ccc,
			    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
};

static struct bt_gatt_service proxy_svc = BT_GATT_SERVICE(proxy_attrs);

int bt_mesh_proxy_gatt_enable(void)
{
	int i;

	BT_DBG("");

	if (!bt_mesh_is_provisioned()) {
		return -ENOTSUP;
	}

	if (service_registered) {
		return -EBUSY;
	}

	(void)bt_gatt_service_register(&proxy_svc);
	service_registered = true;

	for (i = 0; i < ARRAY_SIZE(clients); i++) {
		if (clients[i].cli) {
			clients[i].filter_type = ACCEPT;
		}
	}

	return 0;
}

void bt_mesh_proxy_gatt_disconnect(void)
{
	int i;

	BT_DBG("");

	for (i = 0; i < ARRAY_SIZE(clients); i++) {
		struct bt_mesh_proxy_client *client = &clients[i];

		if (client->cli && (client->filter_type == ACCEPT ||
				     client->filter_type == REJECT)) {
			client->filter_type = NONE;
			bt_conn_disconnect(client->cli->conn,
					   BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		}
	}
}

int bt_mesh_proxy_gatt_disable(void)
{
	BT_DBG("");

	if (!service_registered) {
		return -EALREADY;
	}

	bt_mesh_proxy_gatt_disconnect();

	bt_gatt_service_unregister(&proxy_svc);
	service_registered = false;

	return 0;
}

void bt_mesh_proxy_addr_add(struct net_buf_simple *buf, uint16_t addr)
{
	struct bt_mesh_proxy_client *client;
	struct bt_mesh_proxy_role *cli =
		CONTAINER_OF(buf, struct bt_mesh_proxy_role, buf);

	client = find_client(cli->conn);

	BT_DBG("filter_type %u addr 0x%04x", client->filter_type, addr);

	if (client->filter_type == ACCEPT) {
		filter_add(client, addr);
	} else if (client->filter_type == REJECT) {
		filter_remove(client, addr);
	}
}

static bool client_filter_match(struct bt_mesh_proxy_client *client,
				uint16_t addr)
{
	int i;

	BT_DBG("filter_type %u addr 0x%04x", client->filter_type, addr);

	if (client->filter_type == REJECT) {
		for (i = 0; i < ARRAY_SIZE(client->filter); i++) {
			if (client->filter[i] == addr) {
				return false;
			}
		}

		return true;
	}

	if (addr == BT_MESH_ADDR_ALL_NODES) {
		return true;
	}

	if (client->filter_type == ACCEPT) {
		for (i = 0; i < ARRAY_SIZE(client->filter); i++) {
			if (client->filter[i] == addr) {
				return true;
			}
		}
	}

	return false;
}

static void buf_send_end(struct bt_conn *conn, void *user_data)
{
	struct net_buf *buf = user_data;

	net_buf_unref(buf);
}

bool bt_mesh_proxy_relay(struct net_buf *buf, uint16_t dst)
{
	bool relayed = false;
	int i, err;

	BT_DBG("%u bytes to dst 0x%04x", buf->len, dst);

	for (i = 0; i < ARRAY_SIZE(clients); i++) {
		struct bt_mesh_proxy_client *client = &clients[i];

		NET_BUF_SIMPLE_DEFINE(msg, 32);

		if (!client->cli) {
			continue;
		}

		if (!client_filter_match(client, dst)) {
			continue;
		}

		/* Proxy PDU sending modifies the original buffer,
		 * so we need to make a copy.
		 */
		net_buf_simple_reserve(&msg, 1);
		net_buf_simple_add_mem(&msg, buf->data, buf->len);

		err = bt_mesh_proxy_msg_send(client->cli, BT_MESH_PROXY_NET_PDU,
					     &msg, buf_send_end, net_buf_ref(buf));

		bt_mesh_adv_send_start(0, err, BT_MESH_ADV(buf));
		if (err) {
			BT_ERR("Failed to send proxy message (err %d)", err);

			/* If segment_and_send() fails the buf_send_end() callback will
			 * not be called, so we need to clear the user data (net_buf,
			 * which is just opaque data to segment_and send) reference given
			 * to segment_and_send() here.
			 */
			net_buf_unref(buf);
			continue;
		}

		relayed = true;
	}

	return relayed;
}

static void gatt_connected(struct bt_conn *conn, uint8_t err)
{
	struct bt_mesh_proxy_client *client;
	struct bt_conn_info info;

	bt_conn_get_info(conn, &info);
	if (info.role != BT_CONN_ROLE_PERIPHERAL ||
	    !service_registered) {
		return;
	}

	BT_DBG("conn %p err 0x%02x", (void *)conn, err);

	conn_count++;

	client = find_client(conn);

	client->filter_type = NONE;
	(void)memset(client->filter, 0, sizeof(client->filter));
	client->cli = bt_mesh_proxy_role_setup(conn, proxy_send,
					       proxy_msg_recv);

	/* Try to re-enable advertising in case it's possible */
	if (conn_count < CONFIG_BT_MAX_CONN) {
		bt_mesh_adv_update();
	}
}

static void gatt_disconnected(struct bt_conn *conn, uint8_t reason)
{
	struct bt_conn_info info;
	struct bt_mesh_proxy_client *client;

	bt_conn_get_info(conn, &info);
	if (info.role != BT_CONN_ROLE_PERIPHERAL) {
		return;
	}

	if (!service_registered && bt_mesh_is_provisioned()) {
		(void)bt_mesh_proxy_gatt_enable();
		return;
	}

	conn_count--;

	client = find_client(conn);
	client->cli = NULL;
}

static int proxy_send(struct bt_conn *conn,
		      const void *data, uint16_t len,
		      bt_gatt_complete_func_t end, void *user_data)
{
	BT_DBG("%u bytes: %s", len, bt_hex(data, len));

	struct bt_gatt_notify_params params = {
		.data = data,
		.len = len,
		.attr = &proxy_attrs[3],
		.user_data = user_data,
		.func = end,
	};

	return bt_gatt_notify_cb(conn, &params);
}

int bt_mesh_proxy_adv_start(void)
{
	BT_DBG("");

	if (!service_registered || !bt_mesh_is_provisioned()) {
		return -ENOTSUP;
	}

	return gatt_proxy_advertise(next_sub());
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = gatt_connected,
	.disconnected = gatt_disconnected,
};
