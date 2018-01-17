/*
 * Copyright (c) 2016-2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define SYS_LOG_DOMAIN "fota/hawkbit"
#define SYS_LOG_LEVEL CONFIG_SYS_LOG_FOTA_LEVEL
#include <logging/sys_log.h>

#include <zephyr/types.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <misc/byteorder.h>
#include <flash.h>
#include <zephyr.h>
#include <misc/reboot.h>
#include <misc/stack.h>
#include <net/http.h>
#include <net/net_app.h>
#include <net/net_event.h>
#include <net/net_mgmt.h>
#include <json.h>

#include <soc.h>

#include "hawkbit.h"
#include "hawkbit_priv.h"
#include "mcuboot.h"
#include "flash_block.h"
#include "product_id.h"
#include "tcp.h"

#define HAWKBIT_MAX_SERVER_FAIL	5

/* Network configuration checks */
#if defined(CONFIG_NET_IPV6)
BUILD_ASSERT_MSG(sizeof(CONFIG_NET_APP_PEER_IPV6_ADDR) > 1,
		"CONFIG_NET_APP_PEER_IPV6_ADDR must be defined in boards/$(BOARD)-local.conf");
#define HAWKBIT_SERVER_ADDR    CONFIG_NET_APP_PEER_IPV6_ADDR
#elif defined(CONFIG_NET_IPV4)
#if !defined(CONFIG_NET_DHCPV4)
BUILD_ASSERT_MSG(sizeof(CONFIG_NET_APP_MY_IPV4_ADDR) > 1,
		"DHCPv4 must be enabled, or CONFIG_NET_APP_MY_IPV4_ADDR must be defined, in boards/$(BOARD)-local.conf");
#endif
BUILD_ASSERT_MSG(sizeof(CONFIG_NET_APP_PEER_IPV4_ADDR) > 1,
		"CONFIG_NET_APP_PEER_IPV4_ADDR must be defined in boards/$(BOARD)-local.conf");
#define HAWKBIT_SERVER_ADDR    CONFIG_NET_APP_PEER_IPV4_ADDR
#endif

/*
 * TODO:
 * create a transfer lifecycle structure
 * to contain the following vars:
 * TCP receive buffer
 * tracking indexes
 * status
 *
 */

/*
 * Buffer sizes were calculated by watching the actual usage on HW and then
 * adding a "safe" cushion (or round up to a nice big number).
 */
/* TODO: optimize these values */
#define TCP_RECV_BUFFER_SIZE	2048
#define URL_BUFFER_SIZE		128
#define STATUS_BUFFER_SIZE	128
#define HTTP_HEADER_BUFFER_SIZE	512

struct hawkbit_context {
	struct http_client_ctx http_ctx;
	struct http_client_request http_req;
	u8_t tcp_buffer[TCP_RECV_BUFFER_SIZE];
	size_t tcp_buffer_size;
	u8_t url_buffer[URL_BUFFER_SIZE];
	size_t url_buffer_size;
	u8_t status_buffer[STATUS_BUFFER_SIZE];
	size_t status_buffer_size;
};

struct hawkbit_download {
	size_t http_content_size;
	size_t downloaded_size;
	int download_progress;
	int download_status;
	struct k_sem *download_waitp;
};

struct json_data_t {
	char *data;
	size_t len;
};

typedef enum {
	HAWKBIT_RESULT_SUCCESS = 0,
	HAWKBIT_RESULT_FAILURE,
	HAWKBIT_RESULT_NONE,
} hawkbit_result_status_t;

typedef enum {
	HAWKBIT_EXEC_CLOSED = 0,
	HAWKBIT_EXEC_PROCEEDING,
	HAWKBIT_EXEC_CANCELED,
	HAWKBIT_EXEC_SCHEDULED,
	HAWKBIT_EXEC_REJECTED,
	HAWKBIT_EXEC_RESUMED,
} hawkbit_exec_status_t;

typedef enum {
	HAWKBIT_ACID_CURRENT = 0,
	HAWKBIT_ACID_UPDATE,
} hawkbit_dev_acid_t;

#define HAWKBIT_RX_TIMEOUT	K_SECONDS(10)

#define HAWKBIT_STACK_SIZE 3840
static K_THREAD_STACK_DEFINE(hawkbit_thread_stack, HAWKBIT_STACK_SIZE);
static struct k_thread hawkbit_thread_data;

int poll_sleep = K_SECONDS(30);
static bool connection_ready;
#if defined(CONFIG_NET_MGMT_EVENT)
static struct net_mgmt_event_callback cb;
#endif

#define HAWKBIT_DOWNLOAD_TIMEOUT	K_SECONDS(10)

#define HTTP_HEADER_CONTENT_TYPE_JSON		"application/json"
#define HTTP_HEADER_CONNECTION_CLOSE_CRLF	"Connection: close\r\n"

static struct hawkbit_context hbc;
static struct k_sem download_wait_sem;

#if defined(CONFIG_NET_CONTEXT_NET_PKT_POOL)
NET_PKT_TX_SLAB_DEFINE(http_client_tx, 15);
NET_PKT_DATA_POOL_DEFINE(http_client_data, 30);

static struct k_mem_slab *tx_slab(void)
{
	return &http_client_tx;
}

static struct net_buf_pool *data_pool(void)
{
	return &http_client_data;
}
#else
#if defined(CONFIG_NET_L2_BT)
#error "TCP connections over Bluetooth need CONFIG_NET_CONTEXT_NET_PKT_POOL "\
	"defined."
#endif /* CONFIG_NET_L2_BT */

#define tx_slab NULL
#define data_pool NULL
#endif /* CONFIG_NET_CONTEXT_NET_PKT_POOL */

/*
 * Descriptors for mapping between JSON and structure representations.
 */

static const struct json_obj_descr json_href_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_href, href, JSON_TOK_STRING),
};

static const struct json_obj_descr json_ctl_res_sleep_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_ctl_res_sleep, sleep,
			    JSON_TOK_STRING),
};

static const struct json_obj_descr json_ctl_res_polling_descr[] = {
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_ctl_res_polling, polling,
			      json_ctl_res_sleep_descr),
};

static const struct json_obj_descr json_ctl_res_links_descr[] = {
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_ctl_res_links, deploymentBase,
			      json_href_descr),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_ctl_res_links, cancelAction,
			      json_href_descr),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_ctl_res_links, configData,
			      json_href_descr),
};

static const struct json_obj_descr json_ctl_res_descr[] = {
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_ctl_res, config,
			      json_ctl_res_polling_descr),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_ctl_res, _links,
			      json_ctl_res_links_descr),
};

static const struct json_obj_descr json_dep_res_hashes_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_hashes, sha1,
			    JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_hashes, md5,
			    JSON_TOK_STRING),
};

static const struct json_obj_descr json_dep_res_links_descr[] = {
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_dep_res_links, download,
			      json_href_descr),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_dep_res_links, md5sum,
			      json_href_descr),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct hawkbit_dep_res_links,
				    "download-http", download_http,
				    json_href_descr),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct hawkbit_dep_res_links,
				    "md5sum-http", md5sum_http,
				    json_href_descr),
};

static const struct json_obj_descr json_dep_res_arts_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_arts, filename,
			    JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_arts, size,
			    JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_dep_res_arts, hashes,
			      json_dep_res_hashes_descr),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_dep_res_arts, _links,
			      json_dep_res_links_descr),
};

static const struct json_obj_descr json_dep_res_chunk_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_chunk, part,
			    JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_chunk, name,
			    JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_chunk, version,
			    JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJ_ARRAY(struct hawkbit_dep_res_chunk, artifacts,
				 HAWKBIT_DEP_MAX_CHUNK_ARTS, num_artifacts,
				 json_dep_res_arts_descr,
				 ARRAY_SIZE(json_dep_res_arts_descr)),
};

static const struct json_obj_descr json_dep_res_deploy_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_deploy, download,
			    JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_deploy, update,
			    JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJ_ARRAY(struct hawkbit_dep_res_deploy, chunks,
				 HAWKBIT_DEP_MAX_CHUNKS, num_chunks,
				 json_dep_res_chunk_descr,
				 ARRAY_SIZE(json_dep_res_chunk_descr)),
};

static const struct json_obj_descr json_dep_res_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res, id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_dep_res, deployment,
			      json_dep_res_deploy_descr),
};

/* Utils */
static int atoi_n(const char *s, int len)
{
        int i, val = 0;

	for (i = 0; i < len; i++) {
		if (*s < '0' || *s > '9')
			return val;
		val = (val * 10) + (*s - '0');
		s++;
	}

        return val;
}

static int hawkbit_time2sec(const char *s)
{
        int sec = 0;

	/* Time: HH:MM:SS */
	sec = atoi_n(s, 2) * 60 * 60;
	sec += atoi_n(s + 3, 2) * 60;
	sec += atoi_n(s + 6, 2);

	if (sec < 0) {
		return -1;
	} else {
		return sec;
	}
}

void hawkbit_device_acid_read(struct hawkbit_device_acid *device_acid)
{
	flash_read(flash_dev, FLASH_AREA_APPLICATION_STATE_OFFSET, device_acid,
		   sizeof(*device_acid));
}

/**
 * @brief Update an ACID of a given type on flash.
 *
 * @param type ACID type to update
 * @param acid New ACID value
 * @return 0 on success, negative on error.
 */
static int hawkbit_device_acid_update(hawkbit_dev_acid_t type,
				      u32_t new_value)
{
	struct hawkbit_device_acid device_acid;
	int ret;

	flash_read(flash_dev, FLASH_AREA_APPLICATION_STATE_OFFSET, &device_acid,
		   sizeof(device_acid));
	if (type == HAWKBIT_ACID_UPDATE) {
		device_acid.update = new_value;
	} else {
		device_acid.current = new_value;
	}

	flash_write_protection_set(flash_dev, false);
	ret = flash_erase(flash_dev, FLASH_AREA_APPLICATION_STATE_OFFSET,
			  FLASH_AREA_APPLICATION_STATE_SIZE);
	flash_write_protection_set(flash_dev, true);
	if (ret) {
		return ret;
	}

	flash_write_protection_set(flash_dev, false);
	ret = flash_write(flash_dev, FLASH_AREA_APPLICATION_STATE_OFFSET,
			  &device_acid, sizeof(device_acid));
	flash_write_protection_set(flash_dev, true);
	return ret;
}

static int hawkbit_start(void)
{
	int ret = 0;
	struct hawkbit_device_acid init_acid;
	u8_t boot_status;

	/* Update boot status and acid */
	hawkbit_device_acid_read(&init_acid);
	SYS_LOG_INF("ACID: current %d, update %d",
		    init_acid.current, init_acid.update);
	boot_status = boot_status_read();
	SYS_LOG_INF("Current boot status %x", boot_status);
	if (boot_status == BOOT_STATUS_ONGOING) {
		boot_status_update();
		SYS_LOG_INF("Updated boot status to %x", boot_status_read());
		ret = boot_erase_flash_bank(FLASH_AREA_IMAGE_1_OFFSET);
		if (ret) {
			SYS_LOG_ERR("Flash bank erase at offset %x: error %d",
				    FLASH_AREA_IMAGE_1_OFFSET, ret);
			return ret;
		} else {
			SYS_LOG_DBG("Erased flash bank at offset %x",
				    FLASH_AREA_IMAGE_1_OFFSET);
		}
		if (init_acid.update != -1) {
			ret = hawkbit_device_acid_update(HAWKBIT_ACID_CURRENT,
						  init_acid.update);
		}
		if (!ret) {
			hawkbit_device_acid_read(&init_acid);
			SYS_LOG_INF("ACID updated, current %d, update %d",
				    init_acid.current, init_acid.update);
		} else {
			SYS_LOG_ERR("Failed to update ACID: %d", ret);
		}
	}
	return ret;
}

/* http_client doesn't callback until the HTTP body has started */
static void install_update_cb(struct http_client_ctx *ctx,
			      u8_t *data, size_t data_size,
			      size_t data_len,
			      enum http_final_call final_data,
			      void *user_data)
{
	struct hawkbit_download *hbd = user_data;
	int downloaded, ret = 0;
	u8_t *body_data = NULL;
	size_t body_len = 0;

	/* HTTP error */
	if (ctx->parser.status_code != 200) {
		SYS_LOG_ERR("HTTP error: %d!", ctx->parser.status_code);
		goto error;
	}

	/* header hasn't been read yet */
	if (hbd->http_content_size == 0) {
		if (ctx->rsp.body_found == 0) {
			SYS_LOG_ERR("Callback called w/o HTTP header found!");
			goto error;
		}

		body_data = ctx->rsp.body_start;
		body_len = data_len;
		body_len -= (ctx->rsp.body_start - ctx->rsp.response_buf);
		hbd->http_content_size = ctx->rsp.content_length;
	}

	if (body_data == NULL) {
		body_data = ctx->rsp.response_buf;
		body_len = data_len;
	}

	/* everything looks good: flash */
	ret = flash_block_write(flash_dev,
				FLASH_AREA_IMAGE_1_OFFSET,
				&hbd->downloaded_size,
				body_data, body_len,
				final_data == HTTP_DATA_FINAL);
	if (ret < 0) {
		SYS_LOG_ERR("Flash write error: %d", ret);
		goto error;
	}

	downloaded = hbd->downloaded_size * 100 /
		     hbd->http_content_size;
	if (downloaded > hbd->download_progress) {
		hbd->download_progress = downloaded;
		SYS_LOG_DBG("%d%%", hbd->download_progress);
	}

	if (final_data == HTTP_DATA_FINAL) {
		hbd->download_status = 1;
		k_sem_give(hbd->download_waitp);
	}

	return;

error:
	hbd->download_status = -1;
	k_sem_give(hbd->download_waitp);
}

static int hawkbit_install_update(struct hawkbit_context *hb_ctx,
				  const char *download_http,
				  size_t file_size)
{
	struct hawkbit_download hbd;
	int ret = 0;
	size_t last_downloaded_size = 0;

	if (!download_http || !file_size) {
		return -EINVAL;
	}

	flash_write_protection_set(flash_dev, false);
	ret = flash_erase(flash_dev, FLASH_AREA_IMAGE_1_OFFSET,
			  FLASH_BANK_SIZE);
	flash_write_protection_set(flash_dev, true);
	if (ret != 0) {
		SYS_LOG_ERR("Failed to erase flash at offset %x, size %d",
			    FLASH_AREA_IMAGE_1_OFFSET, FLASH_BANK_SIZE);
		return -EIO;
	}

	SYS_LOG_INF("Starting the download and flash process");

	/* Receive is special for download, since it writes to flash */
	memset(hb_ctx->tcp_buffer, 0, hb_ctx->tcp_buffer_size);
	memset(&hbd, 0, sizeof(struct hawkbit_download));
	hbd.download_waitp = &download_wait_sem;
	/* reset download semaphore */
	k_sem_init(hbd.download_waitp, 0, 1);

	ret = http_client_init(&hbc.http_ctx,
			       HAWKBIT_SERVER_ADDR, HAWKBIT_PORT);
	if (ret < 0) {
		SYS_LOG_ERR("Failed to init http ctx, err %d", ret);
		return ret;
	}

#if defined(CONFIG_NET_CONTEXT_NET_PKT_POOL)
	http_client_set_net_pkt_pool(&hbc.http_ctx, tx_slab, data_pool);
#endif

	ret = http_client_send_get_req(&hb_ctx->http_ctx, download_http,
				       HAWKBIT_HOST,
				       HTTP_HEADER_CONNECTION_CLOSE_CRLF,
				       install_update_cb,
				       hb_ctx->tcp_buffer,
				       hb_ctx->tcp_buffer_size,
				       (void *)&hbd, K_NO_WAIT);
	/* http_client returns EINPROGRESS for get_req w/ K_NO_WAIT */
	if (ret < 0 && ret != -EINPROGRESS) {
		SYS_LOG_ERR("Failed to send request, err %d", ret);
		return ret;
	}

	while (k_sem_take(hbd.download_waitp, HAWKBIT_DOWNLOAD_TIMEOUT)) {
		/* wait timeout: check for download activity */
		if (last_downloaded_size == hbd.downloaded_size) {
			/* no activity: break loop */
			break;
		} else {
			last_downloaded_size = hbd.downloaded_size;
		}
	}

	/* clean up TCP context */
	http_client_release(&hb_ctx->http_ctx);

	if (hbd.download_status < 0) {
		SYS_LOG_ERR("Unable to finish the download process %d",
			    hbd.download_status);
		return -1;
	}

	if (hbd.downloaded_size != hbd.http_content_size) {
		SYS_LOG_ERR("Download: downloaded image size mismatch, "
			    "downloaded %zu, expecting %zu",
			    hbd.downloaded_size, hbd.http_content_size);
		return -1;
	}

	if (hbd.downloaded_size != file_size) {
		SYS_LOG_ERR("Download: downloaded image size mismatch, "
			    "downloaded %zu, expecting from JSON %zu",
			    hbd.downloaded_size, file_size);
		return -1;
	}

	SYS_LOG_INF("Download: downloaded bytes %zu", hbd.downloaded_size);
	return 0;
}

static int hawkbit_query(struct hawkbit_context *hb_ctx,
			 struct json_data_t *json)
{
	int ret = 0;

	SYS_LOG_DBG("[%s] HOST:%s URL:%s",
		    http_method_str(hb_ctx->http_req.method),
		    hb_ctx->http_req.host, hb_ctx->http_req.url);

	memset(hb_ctx->tcp_buffer, 0, hb_ctx->tcp_buffer_size);

	ret = http_client_init(&hbc.http_ctx,
			       HAWKBIT_SERVER_ADDR, HAWKBIT_PORT);
	if (ret < 0) {
		SYS_LOG_ERR("Failed to init http ctx, err %d", ret);
		return ret;
	}

#if defined(CONFIG_NET_CONTEXT_NET_PKT_POOL)
	http_client_set_net_pkt_pool(&hbc.http_ctx, tx_slab, data_pool);
#endif

	ret = http_client_send_req(&hb_ctx->http_ctx, &hb_ctx->http_req, NULL,
				   hb_ctx->tcp_buffer, hb_ctx->tcp_buffer_size,
				   NULL, HAWKBIT_RX_TIMEOUT);
	if (ret < 0) {
		SYS_LOG_ERR("Failed to send buffer, err %d", ret);
		goto cleanup;
	}

	if (hb_ctx->http_ctx.rsp.data_len == 0) {
		SYS_LOG_ERR("No received data (rsp.data_len: %zu)",
			    hb_ctx->http_ctx.rsp.data_len);
		ret = -EIO;
		goto cleanup;
	}

	if (hb_ctx->http_ctx.parser.status_code != 200) {
		SYS_LOG_ERR("Invalid HTTP status code [%d]",
			    hb_ctx->http_ctx.parser.status_code);
		ret = -1;
		goto cleanup;
	}

	if (json) {
		json->data = hb_ctx->http_ctx.rsp.body_start;
		json->len = strlen(hb_ctx->http_ctx.rsp.response_buf);
		json->len -= hb_ctx->http_ctx.rsp.body_start -
			     hb_ctx->http_ctx.rsp.response_buf;

		/* FIXME: Each poll needs a new connection, this saves
		 * us from using content from a previous package.
		 */
		json->data[json->len] = '\0';
		SYS_LOG_DBG("JSON DATA:\n%s", json->data);
	}

	SYS_LOG_DBG("Hawkbit query completed");

cleanup:
	/* clean up TCP context */
	http_client_release(&hb_ctx->http_ctx);
	return ret;
}

/*
 * Update sleep interval, based on results from hawkBit base polling
 * resource.
 */
static void hawkbit_update_sleep(struct hawkbit_ctl_res *hawkbit_res)
{
	const char *sleep = hawkbit_res->config.polling.sleep;
	int len;

	if (strlen(sleep) != HAWKBIT_SLEEP_LENGTH) {
		SYS_LOG_ERR("invalid poll sleep: %s", sleep);
	} else {
		len = hawkbit_time2sec(sleep);
		if (len > 0 && poll_sleep != K_SECONDS(len)) {
			SYS_LOG_INF("New poll sleep %d seconds", len);
			poll_sleep = K_SECONDS(len);
		}
	}
}

static int hawkbit_report_config_data(struct hawkbit_context *hb_ctx)
{
	const struct product_id_t *product_id = product_id_get();

	SYS_LOG_INF("Reporting target config data to Hawkbit");

	/* Build URL */
	snprintf(hb_ctx->url_buffer, hb_ctx->url_buffer_size,
		 "%s/%s-%x/configData", HAWKBIT_JSON_URL,
		 product_id->name, product_id->number);

	/* Build JSON */
	snprintf(hb_ctx->status_buffer, hb_ctx->status_buffer_size, "{"
			"\"data\":{"
				"\"board\":\"%s\","
				"\"serial\":\"%x\"},"
			"\"status\":{"
				"\"result\":{\"finished\":\"success\"},"
				"\"execution\":\"closed\"}"
			"}", product_id->name, product_id->number);

	memset(&hb_ctx->http_req, 0, sizeof(hb_ctx->http_req));
	hb_ctx->http_req.method = HTTP_PUT;
	hb_ctx->http_req.url = hb_ctx->url_buffer;
	hb_ctx->http_req.host = HAWKBIT_HOST;
	hb_ctx->http_req.protocol = " " HTTP_PROTOCOL HTTP_CRLF;
	hb_ctx->http_req.header_fields = HTTP_HEADER_CONNECTION_CLOSE_CRLF;
	hb_ctx->http_req.content_type_value = "application/json";
	hb_ctx->http_req.payload = hb_ctx->status_buffer;
	hb_ctx->http_req.payload_size = strlen(hb_ctx->status_buffer);

	if (hawkbit_query(hb_ctx, NULL)) {
		SYS_LOG_ERR("Error when reporting config data to Hawkbit");
		return -1;
	}

	return 0;
}

/*
 * Find URL component for this device's deployment operations
 * resource.
 */
static int hawkbit_find_deployment_base(struct hawkbit_ctl_res *res,
					char *deployment_base,
					size_t deployment_base_size)
{
	const char *href;
	const char *helper;
	size_t len;

	href = res->_links.deploymentBase.href;
	if (!href) {
		/* A missing deployment base is not an error. */
		*deployment_base = '\0';
		return 0;
	}
	helper = strstr(href, "deploymentBase/");
	if (!helper) {
		/* A badly formatted deployment base is a server error. */
		SYS_LOG_ERR("missing deploymentBase/ in href %s", href);
		return -EINVAL;
	}
	len = strlen(helper);
	if (len > deployment_base_size - 1) {
		/* Lack of memory is an application error. */
		SYS_LOG_ERR("deploymentBase %s is too big (len %zu, max %zu)",
			    helper, len, deployment_base_size - 1);
		return -ENOMEM;
	}
	strncpy(deployment_base, helper, deployment_base_size);
	return 0;
}

/*
 * Parse the results of polling the deployment operations resource.
 */
static int hawkbit_parse_deployment(struct hawkbit_dep_res *res,
				    int *json_acid,
				    char *download_http,
				    size_t download_http_size,
				    s32_t *file_size)
{
	const char *href;
	const char *helper;
	size_t len;
	struct hawkbit_dep_res_chunk *chunk;
	struct hawkbit_dep_res_arts *artifact;
	size_t num_chunks, num_artifacts;
	s32_t acid, size;

	acid = strtol(res->id, NULL, 10);
	if (acid < 0) {
		SYS_LOG_ERR("negative action ID %d", acid);
		return -EINVAL;
	}
	num_chunks = res->deployment.num_chunks;
	if (num_chunks != 1) {
		SYS_LOG_ERR("expecting one chunk (got %d)", num_chunks);
		return -ENOSPC;
	}
	chunk = &res->deployment.chunks[0];
	if (strcmp("os", chunk->part)) {
		SYS_LOG_ERR("only part 'os' is supported; got %s", chunk->part);
		return -EINVAL;
	}
	num_artifacts = chunk->num_artifacts;
	if (num_artifacts != 1) {
		SYS_LOG_ERR("expecting one artifact (got %d)", num_artifacts);
		return -EINVAL;
	}
	artifact = &chunk->artifacts[0];
	size = artifact->size;
	if (size > FLASH_BANK_SIZE) {
		SYS_LOG_ERR("artifact file size too big (got %d, max is %d)",
			    size, FLASH_BANK_SIZE);
		return -ENOSPC;
	}
	/*
	 * Find the download-http href. We only support the DEFAULT
	 * tenant on the same hawkBit server.
	 */
	href = artifact->_links.download_http.href;
	if (!href) {
		SYS_LOG_ERR("missing expected download-http href");
		return -EINVAL;
	}
	helper = strstr(href, "/DEFAULT/controller/v1");
	if (!helper) {
		SYS_LOG_ERR("unexpected download-http href format: %s", helper);
		return -EINVAL;
	}
	len = strlen(helper);
	if (len == 0) {
		SYS_LOG_ERR("empty download-http");
		return -EINVAL;
	} else if (len > download_http_size - 1) {
		SYS_LOG_ERR("download-http %s is too big (len: %zu, max: %zu)",
			    helper, len, download_http_size - 1);
		return -ENOMEM;
	}
	/* Success. */
	*json_acid = acid;
	strncpy(download_http, helper, download_http_size);
	*file_size = size;
	return 0;
}

static int hawkbit_report_update_status(struct hawkbit_context *hb_ctx,
					int acid,
					hawkbit_result_status_t status,
					hawkbit_exec_status_t exec)
{
	const struct product_id_t *product_id = product_id_get();
	char finished[8];	/* 'success', 'failure', 'none' */
	char execution[11];

	switch (status) {
	case HAWKBIT_RESULT_SUCCESS:
		snprintf(finished, sizeof(finished), "success");
		break;
	case HAWKBIT_RESULT_FAILURE:
		snprintf(finished, sizeof(finished), "failure");
		break;
	case HAWKBIT_RESULT_NONE:
		snprintf(finished, sizeof(finished), "none");
		break;
	}

	/* 'closed', 'proceeding', 'canceled', 'scheduled',
	 * 'rejected', 'resumed'
	 */
	switch (exec) {
	case HAWKBIT_EXEC_CLOSED:
		snprintf(execution, sizeof(execution), "closed");
		break;
	case HAWKBIT_EXEC_PROCEEDING:
		snprintf(execution, sizeof(execution), "proceeding");
		break;
	case HAWKBIT_EXEC_CANCELED:
		snprintf(execution, sizeof(execution), "canceled");
		break;
	case HAWKBIT_EXEC_SCHEDULED:
		snprintf(execution, sizeof(execution), "scheduled");
		break;
	case HAWKBIT_EXEC_REJECTED:
		snprintf(execution, sizeof(execution), "rejected");
		break;
	case HAWKBIT_EXEC_RESUMED:
		snprintf(execution, sizeof(execution), "resumed");
		break;
	}

	SYS_LOG_INF("Reporting action ID feedback: %s", finished);

	/* Build URL */
	snprintf(hb_ctx->url_buffer, hb_ctx->url_buffer_size,
		 "%s/%s-%x/deploymentBase/%d/feedback",
		 HAWKBIT_JSON_URL, product_id->name, product_id->number, acid);

	/* Build JSON */
	snprintf(hb_ctx->status_buffer, hb_ctx->status_buffer_size, "{"
			"\"id\":\"%d\","
			"\"status\":{"
				"\"result\":{\"finished\":\"%s\"},"
				"\"execution\":\"%s\"}"
			"}", acid, finished, execution);

	memset(&hb_ctx->http_req, 0, sizeof(hb_ctx->http_req));
	hb_ctx->http_req.method = HTTP_POST;
	hb_ctx->http_req.url = hb_ctx->url_buffer;
	hb_ctx->http_req.host = HAWKBIT_HOST;
	hb_ctx->http_req.protocol = " " HTTP_PROTOCOL HTTP_CRLF;
	hb_ctx->http_req.header_fields = HTTP_HEADER_CONNECTION_CLOSE_CRLF;
	hb_ctx->http_req.content_type_value = "application/json";
	hb_ctx->http_req.payload = hb_ctx->status_buffer;
	hb_ctx->http_req.payload_size = strlen(hb_ctx->status_buffer);

	if (hawkbit_query(hb_ctx, NULL) < 0) {
		SYS_LOG_ERR("Error when reporting acId feedback to Hawkbit");
		return -1;
	}

	return 0;
}

static int hawkbit_ddi_poll(struct hawkbit_context *hb_ctx)
{
	/*
	 * "Raw" decoded JSON objects.
	 */
	union {
		struct hawkbit_ctl_res base; /* Base resource. */
		struct hawkbit_dep_res dep;  /* Deployment operations. */
	} hawkbit_results;
	/*
	 * Cached hawkBit base resource results.
	 */
	char deployment_base[40];	/* TODO: Find a better value */
	char download_http[200];	/* TODO: Find a better value */
	static int json_acid;
	s32_t file_size = 0;
	/*
	 * Etc.
	 */
	struct hawkbit_device_acid device_acid;
	struct json_data_t json = { NULL, 0 };
	const struct product_id_t *product_id = product_id_get();
	int ret;

	/*
	 * Query the hawkBit base polling resource.
	 */
	SYS_LOG_DBG("Polling target data from Hawkbit");

	/* Build URL */
	snprintf(hb_ctx->url_buffer, hb_ctx->url_buffer_size, "%s/%s-%x",
		 HAWKBIT_JSON_URL, product_id->name, product_id->number);

	memset(&hb_ctx->http_req, 0, sizeof(hb_ctx->http_req));
	hb_ctx->http_req.method = HTTP_GET;
	hb_ctx->http_req.url = hb_ctx->url_buffer;
	hb_ctx->http_req.host = HAWKBIT_HOST;
	hb_ctx->http_req.protocol = " " HTTP_PROTOCOL HTTP_CRLF;
	hb_ctx->http_req.header_fields = HTTP_HEADER_CONNECTION_CLOSE_CRLF;

	ret = hawkbit_query(hb_ctx, &json);
	if (ret < 0) {
		SYS_LOG_ERR("Error when polling from Hawkbit");
		return ret;
	}

	/*
	 * Decode the results from the base polling resource, finding
	 * the hawkBit DDI v1 deployment base in the returned result.
	 */
	memset(&hawkbit_results.base, 0, sizeof(hawkbit_results.base));
	ret = json_obj_parse(json.data, json.len, json_ctl_res_descr,
			     ARRAY_SIZE(json_ctl_res_descr),
			     &hawkbit_results.base);
	if (ret < 0) {
		SYS_LOG_ERR("JSON parse error %d polling base resource", ret);
		return ret;
	}
	if (hawkbit_results.base.config.polling.sleep) {
		/* Update the sleep time. */
		hawkbit_update_sleep(&hawkbit_results.base);
	}
	if (hawkbit_results.base._links.cancelAction.href) {
		/* TODO: implement cancelAction logic. */
		SYS_LOG_WRN("Ignoring cancelAction (href %s)",
			    hawkbit_results.base._links.cancelAction.href);
	}
	ret = hawkbit_find_deployment_base(&hawkbit_results.base,
					   deployment_base,
					   sizeof(deployment_base));
	if (ret < 0) {
		return ret;
	}

	/* Provide this device's config data if the server asked for it. */
	if (hawkbit_results.base._links.configData.href) {
		hawkbit_report_config_data(hb_ctx);
	}

	/*
	 * If one was found, poll the deployment base discovered
	 * earlier. If there was no deployment base, there is nothing
	 * else to do.
	 */
	if (strlen(deployment_base) == 0) {
		SYS_LOG_DBG("No deployment base found, no actions to take");
		return 0;
	}

	memset(&json, 0, sizeof(struct json_data_t));

	/* Build URL: Hawkbit DDI v1 deploymentBase */
	snprintf(hb_ctx->url_buffer, hb_ctx->url_buffer_size, "%s/%s-%x/%s",
		 HAWKBIT_JSON_URL, product_id->name, product_id->number,
		 deployment_base);

	memset(&hb_ctx->http_req, 0, sizeof(hb_ctx->http_req));
	hb_ctx->http_req.method = HTTP_GET;
	hb_ctx->http_req.url = hb_ctx->url_buffer;
	hb_ctx->http_req.host = HAWKBIT_HOST;
	hb_ctx->http_req.protocol = " " HTTP_PROTOCOL HTTP_CRLF;
	hb_ctx->http_req.header_fields = HTTP_HEADER_CONNECTION_CLOSE_CRLF;

	if (hawkbit_query(hb_ctx, &json) < 0) {
		SYS_LOG_ERR("Error when querying from Hawkbit");
		return -1;
	}

	/*
	 * Decode results from the deployment operations resource.
	 */

	memset(&hawkbit_results.dep, 0, sizeof(hawkbit_results.dep));
	ret = json_obj_parse(json.data, json.len, json_dep_res_descr,
			     ARRAY_SIZE(json_dep_res_descr),
			     &hawkbit_results.dep);
	if (ret < 0) {
		SYS_LOG_ERR("deploymentBase JSON parse error %d", ret);
		goto report_error;
	} else if (ret != (1 << ARRAY_SIZE(json_dep_res_descr)) - 1) {
		SYS_LOG_ERR("deploymentBase JSON mismatch"
			    " (expected 0x%x, got 0x%x)",
			    (1 << ARRAY_SIZE(json_dep_res_descr)) - 1, ret);
		ret = -EINVAL;
		goto report_error;
	}

	ret = hawkbit_parse_deployment(&hawkbit_results.dep, &json_acid,
				       download_http, sizeof(download_http),
				       &file_size);
	if (ret) {
		goto report_error;
	}

	/* TODO: handle download/update. */
	SYS_LOG_DBG("action ID: %d", json_acid);
	SYS_LOG_DBG("deployment: download %s, update %s (ignored)",
		    hawkbit_results.dep.deployment.download,
		    hawkbit_results.dep.deployment.update);
	SYS_LOG_DBG("artifact address: %s", download_http);
	SYS_LOG_DBG("artifact file size: %d", file_size);

	hawkbit_device_acid_read(&device_acid);
	if (device_acid.current == json_acid) {
		/* We are coming from a successful flash, update the server */
		hawkbit_report_update_status(hb_ctx, json_acid,
					     HAWKBIT_RESULT_SUCCESS,
					     HAWKBIT_EXEC_CLOSED);
		return 0;
	}

	/*
	 * Check for errors.
	 */
	if (device_acid.update == (u32_t)json_acid) {
		SYS_LOG_ERR("Preventing repeated attempt to install %d",
			    json_acid);
		ret = -EALREADY;
		goto report_error;
	}

	/* Here we should have everything we need to apply the action */
	SYS_LOG_INF("Valid action ID %d found, proceeding with the update",
					json_acid);
	hawkbit_report_update_status(hb_ctx, json_acid,
				     HAWKBIT_RESULT_SUCCESS,
				     HAWKBIT_EXEC_PROCEEDING);
	ret = hawkbit_install_update(hb_ctx, download_http, file_size);
	if (ret != 0) {
		SYS_LOG_ERR("Failed to install the update for action ID %d",
			    json_acid);
		goto report_error;
	}

	SYS_LOG_INF("Triggering OTA update.");
	boot_trigger_ota();
	ret = hawkbit_device_acid_update(HAWKBIT_ACID_UPDATE, json_acid);
	if (ret != 0) {
		SYS_LOG_ERR("Failed to update ACID: %d", ret);
		goto report_error;
	}
	SYS_LOG_INF("Image id %d flashed successfuly, rebooting now",
					json_acid);

	/* Reboot and let the bootloader take care of the swap process */
	sys_reboot(0);

	return 0;

 report_error:
	hawkbit_report_update_status(hb_ctx, json_acid,
				     HAWKBIT_RESULT_FAILURE,
				     HAWKBIT_EXEC_CLOSED);
	return ret;
}

/* Firmware OTA thread (Hawkbit) */
static void hawkbit_service(void)
{
	u32_t hawkbit_failures = 0;
	int ret;

	SYS_LOG_INF("Starting FOTA Service Thread");

	do {
		k_sleep(poll_sleep);

		if (!connection_ready) {
			SYS_LOG_DBG("Network interface is not ready");
			continue;
		}

		tcp_interface_lock();

		ret = hawkbit_ddi_poll(&hbc);
		if (ret < 0) {
			hawkbit_failures++;
			if (hawkbit_failures == HAWKBIT_MAX_SERVER_FAIL) {
				SYS_LOG_ERR("Too many unsuccessful poll"
					    " attempts, rebooting!");
				sys_reboot(0);
			}
		} else {
			/* restart the failed attempt counter */
			hawkbit_failures = 0;
		}

		tcp_interface_unlock();

		STACK_ANALYZE("Hawkbit Thread", hawkbit_thread_stack);
	} while (1);
}

static void event_iface_up(struct net_mgmt_event_callback *cb,
			   u32_t mgmt_event, struct net_if *iface)
{
	connection_ready = true;
}

int hawkbit_init(void)
{
	/* TODO: default interface may not always be the one we want */
	struct net_if *iface = net_if_get_default();
	int ret;

	ret = hawkbit_start();
	if (ret) {
		SYS_LOG_ERR("Hawkbit Client initialization generated "
			    "an error: %d", ret);
		return ret;
	}

	memset(&hbc, 0, sizeof(hbc));
	hbc.tcp_buffer_size = TCP_RECV_BUFFER_SIZE;
	hbc.url_buffer_size = URL_BUFFER_SIZE;
	hbc.status_buffer_size = STATUS_BUFFER_SIZE;

	k_thread_create(&hawkbit_thread_data, &hawkbit_thread_stack[0],
			K_THREAD_STACK_SIZEOF(hawkbit_thread_stack),
			(k_thread_entry_t) hawkbit_service,
			NULL, NULL, NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);

#if defined(CONFIG_NET_MGMT_EVENT)
	/* Subscribe to NET_IF_UP if interface is not ready */
	if (!atomic_test_bit(iface->flags, NET_IF_UP)) {
		net_mgmt_init_event_callback(&cb, event_iface_up,
					     NET_EVENT_IF_UP);
		net_mgmt_add_event_callback(&cb);
		return 0;
	}
#endif

	event_iface_up(NULL, NET_EVENT_IF_UP, iface);

	return ret;
}
