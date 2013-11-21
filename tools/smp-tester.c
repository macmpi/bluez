/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2013  Intel Corporation. All rights reserved.
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/socket.h>

#include <glib.h>

#include "lib/bluetooth.h"
#include "lib/mgmt.h"

#include "monitor/bt.h"
#include "emulator/bthost.h"

#include "src/shared/tester.h"
#include "src/shared/mgmt.h"
#include "src/shared/hciemu.h"

#ifndef SOL_ALG
#define SOL_ALG 279
#endif

#ifndef AF_ALG
#define AF_ALG  38
#define PF_ALG  AF_ALG

#include <linux/types.h>

struct sockaddr_alg {
        __u16   salg_family;
        __u8    salg_type[14];
        __u32   salg_feat;
        __u32   salg_mask;
        __u8    salg_name[64];
};

struct af_alg_iv {
        __u32   ivlen;
        __u8    iv[0];
};

#define ALG_SET_KEY                     1
#define ALG_SET_IV                      2
#define ALG_SET_OP                      3

#define ALG_OP_DECRYPT                  0
#define ALG_OP_ENCRYPT                  1

#else
#include <linux/if_alg.h>
#endif

#define SMP_CID 0x0006

struct test_data {
	const void *test_data;
	struct mgmt *mgmt;
	uint16_t mgmt_index;
	struct hciemu *hciemu;
	enum hciemu_type hciemu_type;
	unsigned int io_id;
	uint16_t handle;
	size_t counter;
	int alg_sk;
};

struct smp_req_rsp {
	const void *req;
	uint16_t req_len;
	const void *rsp;
	uint16_t rsp_len;
};

struct smp_server_data {
	const struct smp_req_rsp *req;
	size_t req_count;
};

struct smp_client_data {
	const struct smp_req_rsp *req;
	size_t req_count;
};

static int alg_setup(void)
{
	struct sockaddr_alg salg;
	int sk;

	sk = socket(PF_ALG, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (sk < 0) {
		fprintf(stderr, "socket(AF_ALG): %s\n", strerror(errno));
		return -1;
	}

	memset(&salg, 0, sizeof(salg));
	salg.salg_family = AF_ALG;
	strcpy((char *) salg.salg_type, "skcipher");
	strcpy((char *) salg.salg_name, "ecb(aes)");

	if (bind(sk, (struct sockaddr *) &salg, sizeof(salg)) < 0) {
		fprintf(stderr, "bind(AF_ALG): %s\n", strerror(errno));
		close(sk);
		return -1;
	}

	return sk;
}

static void mgmt_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	tester_print("%s%s", prefix, str);
}

static void read_info_callback(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	struct test_data *data = tester_get_data();
	const struct mgmt_rp_read_info *rp = param;
	char addr[18];
	uint16_t manufacturer;
	uint32_t supported_settings, current_settings;

	tester_print("Read Info callback");
	tester_print("  Status: 0x%02x", status);

	if (status || !param) {
		tester_pre_setup_failed();
		return;
	}

	ba2str(&rp->bdaddr, addr);
	manufacturer = btohs(rp->manufacturer);
	supported_settings = btohl(rp->supported_settings);
	current_settings = btohl(rp->current_settings);

	tester_print("  Address: %s", addr);
	tester_print("  Version: 0x%02x", rp->version);
	tester_print("  Manufacturer: 0x%04x", manufacturer);
	tester_print("  Supported settings: 0x%08x", supported_settings);
	tester_print("  Current settings: 0x%08x", current_settings);
	tester_print("  Class: 0x%02x%02x%02x",
			rp->dev_class[2], rp->dev_class[1], rp->dev_class[0]);
	tester_print("  Name: %s", rp->name);
	tester_print("  Short name: %s", rp->short_name);

	if (strcmp(hciemu_get_address(data->hciemu), addr)) {
		tester_pre_setup_failed();
		return;
	}

	tester_pre_setup_complete();
}

static void index_added_callback(uint16_t index, uint16_t length,
					const void *param, void *user_data)
{
	struct test_data *data = tester_get_data();

	tester_print("Index Added callback");
	tester_print("  Index: 0x%04x", index);

	data->mgmt_index = index;

	mgmt_send(data->mgmt, MGMT_OP_READ_INFO, data->mgmt_index, 0, NULL,
					read_info_callback, NULL, NULL);
}

static void index_removed_callback(uint16_t index, uint16_t length,
					const void *param, void *user_data)
{
	struct test_data *data = tester_get_data();

	tester_print("Index Removed callback");
	tester_print("  Index: 0x%04x", index);

	if (index != data->mgmt_index)
		return;

	mgmt_unregister_index(data->mgmt, data->mgmt_index);

	mgmt_unref(data->mgmt);
	data->mgmt = NULL;

	tester_post_teardown_complete();
}

static void read_index_list_callback(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	struct test_data *data = tester_get_data();

	tester_print("Read Index List callback");
	tester_print("  Status: 0x%02x", status);

	if (status || !param) {
		tester_pre_setup_failed();
		return;
	}

	mgmt_register(data->mgmt, MGMT_EV_INDEX_ADDED, MGMT_INDEX_NONE,
					index_added_callback, NULL, NULL);

	mgmt_register(data->mgmt, MGMT_EV_INDEX_REMOVED, MGMT_INDEX_NONE,
					index_removed_callback, NULL, NULL);

	data->hciemu = hciemu_new(data->hciemu_type);
	if (!data->hciemu) {
		tester_warn("Failed to setup HCI emulation");
		tester_pre_setup_failed();
	}

	tester_print("New hciemu instance created");
}

static void test_pre_setup(const void *test_data)
{
	struct test_data *data = tester_get_data();

	data->alg_sk = alg_setup();
	if (data->alg_sk < 0) {
		tester_warn("Failed to setup AF_ALG socket");
		tester_pre_setup_failed();
		return;
	}

	data->mgmt = mgmt_new_default();
	if (!data->mgmt) {
		tester_warn("Failed to setup management interface");
		tester_pre_setup_failed();
		return;
	}

	if (tester_use_debug())
		mgmt_set_debug(data->mgmt, mgmt_debug, "mgmt: ", NULL);

	mgmt_send(data->mgmt, MGMT_OP_READ_INDEX_LIST, MGMT_INDEX_NONE, 0, NULL,
					read_index_list_callback, NULL, NULL);
}

static void test_post_teardown(const void *test_data)
{
	struct test_data *data = tester_get_data();

	if (data->io_id > 0) {
		g_source_remove(data->io_id);
		data->io_id = 0;
	}

	if (data->alg_sk >= 0) {
		close(data->alg_sk);
		data->alg_sk = -1;
	}

	hciemu_unref(data->hciemu);
	data->hciemu = NULL;
}

static void test_data_free(void *test_data)
{
	struct test_data *data = test_data;

	free(data);
}

#define test_smp(name, data, setup, func) \
	do { \
		struct test_data *user; \
		user = malloc(sizeof(struct test_data)); \
		if (!user) \
			break; \
		user->hciemu_type = HCIEMU_TYPE_LE; \
		user->io_id = 0; \
		user->counter = 0; \
		user->alg_sk = -1; \
		user->test_data = data; \
		tester_add_full(name, data, \
				test_pre_setup, setup, func, NULL, \
				test_post_teardown, 2, user, test_data_free); \
	} while (0)

static const uint8_t smp_nval_req_1[] = { 0x0b, 0x00 };
static const uint8_t smp_nval_req_1_rsp[] = { 0x05, 0x07 };

static const struct smp_req_rsp nval_req_1[] = {
	{ smp_nval_req_1, sizeof(smp_nval_req_1),
			smp_nval_req_1_rsp, sizeof(smp_nval_req_1_rsp) },
};

static const struct smp_server_data smp_server_nval_req_1_test = {
	.req = nval_req_1,
	.req_count = G_N_ELEMENTS(nval_req_1),
};

static const uint8_t smp_nval_req_2[7] = { 0x01 };
static const uint8_t smp_nval_req_2_rsp[] = { 0x05, 0x06 };

static const struct smp_req_rsp srv_nval_req_1[] = {
	{ smp_nval_req_2, sizeof(smp_nval_req_2),
			smp_nval_req_2_rsp, sizeof(smp_nval_req_2_rsp) },
};

static const struct smp_server_data smp_server_nval_req_2_test = {
	.req = srv_nval_req_1,
	.req_count = G_N_ELEMENTS(srv_nval_req_1),
};

static const uint8_t smp_basic_req_1[] = {	0x01,	/* Pairing Request */
						0x03,	/* NoInputNoOutput */
						0x00,	/* OOB Flag */
						0x01,	/* Bonding - no MITM */
						0x10,	/* Max key size */
						0x00,	/* Init. key dist. */
						0x01,	/* Rsp. key dist. */
};
static const uint8_t smp_basic_req_1_rsp[] = {	0x02,	/* Pairing Response */
						0x03,	/* NoInputNoOutput */
						0x00,	/* OOB Flag */
						0x01,	/* Bonding - no MITM */
						0x10,	/* Max key size */
						0x00,	/* Init. key dist. */
						0x01,	/* Rsp. key dist. */
};

static const uint8_t smp_confirm_req_1[17] = { 0x03 };
static const uint8_t smp_random_req_1[17] = { 0x04 };

static const struct smp_req_rsp srv_basic_req_1[] = {
	{ smp_basic_req_1, sizeof(smp_basic_req_1),
			smp_basic_req_1_rsp, sizeof(smp_basic_req_1_rsp) },
	{ smp_confirm_req_1, sizeof(smp_confirm_req_1),
			smp_confirm_req_1, sizeof(smp_confirm_req_1) },
	{ smp_random_req_1, sizeof(smp_random_req_1), NULL, 0 },
};

static const struct smp_server_data smp_server_basic_req_1_test = {
	.req = srv_basic_req_1,
	.req_count = G_N_ELEMENTS(srv_basic_req_1),
};

static const struct smp_req_rsp cli_basic_req_1[] = {
	{ smp_basic_req_1, sizeof(smp_basic_req_1),
			smp_basic_req_1_rsp, sizeof(smp_basic_req_1_rsp) },
	{ smp_confirm_req_1, sizeof(smp_confirm_req_1),
			smp_confirm_req_1, sizeof(smp_confirm_req_1) },
};

static const struct smp_client_data smp_client_basic_req_1_test = {
	.req = cli_basic_req_1,
	.req_count = G_N_ELEMENTS(cli_basic_req_1),
};

static void client_connectable_complete(uint16_t opcode, uint8_t status,
					const void *param, uint8_t len,
					void *user_data)
{
	if (opcode != BT_HCI_CMD_LE_SET_ADV_ENABLE)
		return;

	tester_print("Client set connectable status 0x%02x", status);

	if (status)
		tester_setup_failed();
	else
		tester_setup_complete();
}

static void setup_powered_client_callback(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	struct test_data *data = tester_get_data();
	struct bthost *bthost;

	if (status != MGMT_STATUS_SUCCESS) {
		tester_setup_failed();
		return;
	}

	tester_print("Controller powered on");

	bthost = hciemu_client_get_host(data->hciemu);
	bthost_set_cmd_complete_cb(bthost, client_connectable_complete, data);
	bthost_set_adv_enable(bthost, 0x01);
}

static void setup_powered_client(const void *test_data)
{
	struct test_data *data = tester_get_data();
	unsigned char param[] = { 0x01 };

	tester_print("Powering on controller");

	mgmt_send(data->mgmt, MGMT_OP_SET_LE, data->mgmt_index,
				sizeof(param), param, NULL, NULL, NULL);
	mgmt_send(data->mgmt, MGMT_OP_SET_PAIRABLE, data->mgmt_index,
				sizeof(param), param, NULL, NULL, NULL);
	mgmt_send(data->mgmt, MGMT_OP_SET_POWERED, data->mgmt_index,
			sizeof(param), param, setup_powered_client_callback,
			NULL, NULL);
}

static void pair_device_complete(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	if (status != MGMT_STATUS_SUCCESS) {
		tester_warn("Pairing failed: %s", mgmt_errstr(status));
		tester_test_failed();
		return;
	}

	tester_print("Pairing succeedded");
	tester_test_passed();
}

static const void *get_pdu(const uint8_t *data)
{
	uint8_t opcode = data[0];
	static uint16_t buf[17];

	switch (opcode) {
	case 0x03: /* Pairing Confirm */
		memcpy(buf, data, sizeof(buf));
		return buf;
	case 0x04: /* Pairing Random */
		memcpy(buf, data, sizeof(buf));
		return buf;
	default:
		return data;
	}
}

static void smp_server(const void *data, uint16_t len, void *user_data)
{
	struct test_data *test_data = tester_get_data();
	const struct smp_client_data *cli = test_data->test_data;
	const struct smp_req_rsp *req;
	uint8_t opcode;

	tester_print("Received SMP request");

	if (test_data->counter >= cli->req_count) {
		tester_test_passed();
		return;
	}

	req = &cli->req[test_data->counter++];

	if (req->req_len != len) {
		tester_warn("Unexpected SMP request length (%u != %u)",
							len, req->req_len);
		goto failed;
	}

	opcode = *((const uint8_t *) data);

	switch (opcode) {
	case 0x03: /* Pairing Confirm */
		break;
	case 0x04: /* Pairing Random */
		break;
	default:
		if (memcmp(req->req, data, len) != 0) {
			tester_warn("Unexpected SMP request");
			goto failed;
		}

		break;
	}

	if (req->rsp) {
		struct bthost *bthost;
		const void *rsp = get_pdu(req->rsp);

		bthost = hciemu_client_get_host(test_data->hciemu);
		bthost_send_cid(bthost, test_data->handle, SMP_CID,
							rsp, req->rsp_len);

		if (cli->req_count > test_data->counter)
			return;
	}

	tester_test_passed();
	return;

failed:
	tester_test_failed();
}

static void smp_server_new_conn(uint16_t handle, void *user_data)
{
	struct test_data *data = user_data;
	struct bthost *bthost = hciemu_client_get_host(data->hciemu);

	tester_print("New server connection with handle 0x%04x", handle);

	data->handle = handle;

	bthost_add_cid_hook(bthost, handle, SMP_CID, smp_server, NULL);
}

static void test_client(const void *test_data)
{
	struct test_data *data = tester_get_data();
	const uint8_t *client_bdaddr;
	struct mgmt_cp_pair_device cp;
	struct bthost *bthost;

	client_bdaddr = hciemu_get_client_bdaddr(data->hciemu);
	if (!client_bdaddr) {
		tester_warn("No client bdaddr");
		tester_test_failed();
		return;
	}

	bthost = hciemu_client_get_host(data->hciemu);
	bthost_set_connect_cb(bthost, smp_server_new_conn, data);

	memcpy(&cp.addr.bdaddr, client_bdaddr, sizeof(bdaddr_t));
	cp.addr.type = BDADDR_LE_PUBLIC;
	cp.io_cap = 0x03; /* NoInputNoOutput */

	mgmt_send(data->mgmt, MGMT_OP_PAIR_DEVICE, data->mgmt_index,
			sizeof(cp), &cp, pair_device_complete, NULL, NULL);

	tester_print("Pairing in progress");
}

static void setup_powered_server_callback(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	if (status != MGMT_STATUS_SUCCESS) {
		tester_setup_failed();
		return;
	}

	tester_print("Controller powered on");

	tester_setup_complete();
}

static void setup_powered_server(const void *test_data)
{
	struct test_data *data = tester_get_data();
	unsigned char param[] = { 0x01 };

	tester_print("Powering on controller");

	mgmt_send(data->mgmt, MGMT_OP_SET_LE, data->mgmt_index,
				sizeof(param), param, NULL, NULL, NULL);
	mgmt_send(data->mgmt, MGMT_OP_SET_PAIRABLE, data->mgmt_index,
				sizeof(param), param, NULL, NULL, NULL);
	mgmt_send(data->mgmt, MGMT_OP_SET_ADVERTISING, data->mgmt_index,
				sizeof(param), param, NULL, NULL, NULL);
	mgmt_send(data->mgmt, MGMT_OP_SET_POWERED, data->mgmt_index,
			sizeof(param), param, setup_powered_server_callback,
			NULL, NULL);
}

static void smp_client(const void *data, uint16_t len, void *user_data)
{
	struct test_data *test_data = user_data;
	struct bthost *bthost = hciemu_client_get_host(test_data->hciemu);
	const struct smp_server_data *srv = test_data->test_data;
	const struct smp_req_rsp *req;
	const void *pdu;
	uint8_t opcode;

	tester_print("SMP client received response");

	if (test_data->counter >= srv->req_count) {
		tester_test_passed();
		return;
	}

	req = &srv->req[test_data->counter++];
	if (!req->rsp)
		goto next;

	if (req->rsp_len != len) {
		tester_warn("Unexpected SMP response length (%u != %u)",
							len, req->rsp_len);
		goto failed;
	}

	opcode = *((const uint8_t *) data);

	switch (opcode) {
	case 0x03: /* Pairing Confirm */
		break;
	case 0x04: /* Pairing Random */
		break;
	default:
		if (memcmp(req->rsp, data, len) != 0) {
			tester_warn("Unexpected SMP response");
			goto failed;
		}
		break;
	}

next:
	if (srv->req_count == test_data->counter) {
		tester_test_passed();
		return;
	}

	req = &srv->req[test_data->counter];
	pdu = get_pdu(req->req);
	bthost_send_cid(bthost, test_data->handle, SMP_CID, pdu, req->req_len);

	return;

failed:
	tester_test_failed();
}

static void smp_client_new_conn(uint16_t handle, void *user_data)
{
	struct test_data *data = user_data;
	const struct smp_server_data *srv = data->test_data;
	struct bthost *bthost = hciemu_client_get_host(data->hciemu);
	const struct smp_req_rsp *req;
	const void *pdu;

	tester_print("New SMP client connection with handle 0x%04x", handle);

	data->handle = handle;

	bthost_add_cid_hook(bthost, handle, SMP_CID, smp_client, data);

	if (srv->req_count == data->counter)
		return;

	req = &srv->req[data->counter];

	tester_print("Sending SMP Request from client");

	pdu = get_pdu(req->req);
	bthost_send_cid(bthost, handle, SMP_CID, pdu, req->req_len);
}

static void test_server(const void *test_data)
{
	struct test_data *data = tester_get_data();
	const uint8_t *master_bdaddr;
	struct bthost *bthost;

	master_bdaddr = hciemu_get_master_bdaddr(data->hciemu);
	if (!master_bdaddr) {
		tester_warn("No master bdaddr");
		tester_test_failed();
		return;
	}

	bthost = hciemu_client_get_host(data->hciemu);
	bthost_set_connect_cb(bthost, smp_client_new_conn, data);

	bthost_hci_connect(bthost, master_bdaddr, BDADDR_LE_PUBLIC);
}

int main(int argc, char *argv[])
{
	tester_init(&argc, &argv);

	test_smp("SMP Server - Basic Request 1",
					&smp_server_basic_req_1_test,
					setup_powered_server, test_server);
	test_smp("SMP Server - Invalid Request 1",
					&smp_server_nval_req_1_test,
					setup_powered_server, test_server);
	test_smp("SMP Server - Invalid Request 2",
					&smp_server_nval_req_2_test,
					setup_powered_server, test_server);

	test_smp("SMP Client - Basic Request 1",
					&smp_client_basic_req_1_test,
					setup_powered_client, test_client);

	return tester_run();
}
