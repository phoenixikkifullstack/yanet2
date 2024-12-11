/*
 * The file is under construction.
 * The first tought was  about provinding device and queue identifiers into
 * worker. Worker processes infinite loop consisting of:
 *  - queue read
 *  - pipepline invocation
 *  - device routing and queue write
 * However the concept is not good as may be because of:
 *  - worker should know about device mapping
 *  - worker should implement logic for logical device routing as vlan/vxlan/
 *    bond/lagg/etc
 *  - multiple pipelines are hard to implement here as the are configured
 *    inside dataplane and shared across workers
 *  - inter-worker-communication to pass packets between numa-nodes
 *
 * New idea is:
 *  - dataplane is responsible for RX/TX and provides function callback to
 *    workers
 *  - concept of logical devices and multiple pipelines, each pipeline could
 *    be assigned to multiple logical devices whereas one logical device
 *    may have only one assigned pipeline. This will reduce pipeline
 *    configuration in case of virtual routers.
 *  - dataplane is reposnsible for L2 processing and routing packets between
 *    logical devices, merging and balancing laggs and so on
 *  - read and writ callbacks should return packages with information about
 *    pipeline assigned to each packet.
 *
 * The only question is how pipeline should be attached to packet/mbuf
 * readings:
 *  - inside packet metadata
 *  - packets are clustered by pipeline and separate array with pipeline range
 *    provided
 *  - anything else
 */

#include "worker.h"

#include "pipeline.h"

#include "dataplane.h"

#include "common/data_pipe.h"
#include <rte_ethdev.h>


static void
worker_read(struct dataplane_worker *worker, struct packet_list *packets)
{
	struct worker_read_ctx *ctx = &worker->read_ctx;
	struct rte_mbuf *mbufs[ctx->read_size];


	uint16_t read =
		rte_eth_rx_burst(
			worker->port_id,
			worker->queue_id,
			mbufs,
			ctx->read_size);
	for (uint32_t idx = 0; idx < read; ++idx) {
		struct packet *packet = mbuf_to_packet(mbufs[idx]);
		memset(packet, 0, sizeof(struct packet));
		//FIXME update packet fields
		packet->mbuf = mbufs[idx];

		packet->rx_device_id = worker->device_id;
		// Preserve device by default
		packet->tx_device_id = worker->device_id;

		parse_packet(packet);
		packet_list_add(packets, packet);
	}
}

static size_t
worker_connection_push_cb(
	void **item,
	size_t count,
	void *data)
{
	if (count > 0) {
		struct packet *packet = (struct packet *)data;
		struct rte_mbuf *mbuf = packet_to_mbuf(packet);
		rte_mbuf_refcnt_update(mbuf, 1);
		memcpy(item, &data, sizeof(struct packet *));
		return 1;
	}
	return 0;
}

static size_t
worker_rx_pipe_pop_cb(
	void **item,
	size_t count,
	void *data)
{
	struct dataplane_worker *worker = (struct dataplane_worker *)data;
	struct packet **packets = (struct packet **)item;

	struct rte_mbuf *mbufs[count];
	for (size_t idx = 0; idx < count; ++idx) {
		mbufs[idx] = packet_to_mbuf(packets[idx]);
	}

	size_t written =
		rte_eth_tx_burst(
			worker->port_id,
			worker->queue_id,
			mbufs,
			count);

	for (size_t idx = 0; idx < written; ++idx) {
		packets[idx]->tx_result = 0;
	}

	for (size_t idx = written; idx < count; ++idx) {
		packets[idx]->tx_result = -1;
	}

	return count;
}

static size_t
worker_connection_free_cb(
	void **item,
	size_t count,
	void *data)
{
	struct packet_list *failed = (struct packet_list *)data;

	for (size_t idx = 0; idx < count; ++idx) {
		struct packet *packet = ((struct packet **)item)[idx];
		if (packet->tx_result) {
			packet_list_add(failed, packet);
		} else {
			struct rte_mbuf *mbuf = packet_to_mbuf(packet);
			rte_pktmbuf_free(mbuf);
		}
	}
	return count;
}

/*
 * FIXME: the function bellow sends a packet to a different worker
 * using corresponding data pipe so the routine name might be confusing.
 */
static int
worker_send_to_port(
	struct worker_write_ctx *ctx,
	struct packet *packet)
{
	struct worker_tx_connection *tx_conn =
		ctx->tx_connections + packet->tx_device_id;

	if (!tx_conn->count) {
		fprintf(stderr, "no conn\n");
		// No available data pipe to the port
		return -1;
	}

	if (data_pipe_item_push(
		tx_conn->pipes + packet->hash % tx_conn->count,
		worker_connection_push_cb,
		packet) != 1) {
		fprintf(stderr, "no space\n");
		return -1;
	}

	return 0;
}

static void
worker_collect_from_port(
	struct dataplane_worker *worker,
	struct packet_list *failed)
{
	for (uint32_t conn_idx = 0;
	     conn_idx < worker->dataplane->device_count;
	     ++conn_idx) {
		struct worker_tx_connection *tx_conn =
			worker->write_ctx.tx_connections + conn_idx;
		for (uint32_t pipe_idx = 0;
		     pipe_idx < tx_conn->count;
		     ++pipe_idx) {
			data_pipe_item_free(
				tx_conn->pipes + pipe_idx,
				worker_connection_free_cb,
				failed);
		}

	}
}

static void
worker_submit_burst(
	struct dataplane_worker *worker,
	struct rte_mbuf **mbufs,
	uint16_t count,
	struct packet_list *failed)
{
	uint16_t written = rte_eth_tx_burst(
		worker->port_id,
		worker->queue_id,
		mbufs,
		count);

	if (written < count)
		fprintf(stderr, "brst fld %d:%d %d\n", worker->port_id, worker->queue_id, count - written);

	for (uint16_t idx = written; idx < count; ++idx) {
		packet_list_add(failed, mbuf_to_packet(mbufs[idx]));
	}
}

static void
worker_write(struct dataplane_worker *worker, struct packet_list *packets)
{
	struct packet_list failed;
	packet_list_init(&failed);

	struct worker_write_ctx *ctx = &worker->write_ctx;
	struct rte_mbuf *mbufs[ctx->write_size];

	uint16_t to_write = 0;

	struct packet *packet;
	while ((packet = packet_list_pop(packets)) != NULL) {
		if (to_write == ctx->write_size) {
			worker_submit_burst(worker, mbufs, to_write, &failed);
			to_write = 0;
		}

		if (packet->tx_device_id == worker->device_id) {
			mbufs[to_write] = packet_to_mbuf(packet);
			++to_write;
		} else {
			if (worker_send_to_port(ctx, packet)) {
				fprintf(stderr, "send fld %d %d to %d\n", worker->device_id, worker->queue_id, packet->tx_device_id);
				packet_list_add(&failed, packet);
			}
		}
	}

	if (to_write > 0) {
		worker_submit_burst(worker, mbufs, to_write, &failed);
	}

	worker_collect_from_port(worker, &failed);

	*packets = failed;

	for (uint32_t pipe_idx = 0;
	     pipe_idx < ctx->rx_pipe_count;
	     ++pipe_idx) {
		data_pipe_item_pop(
			ctx->rx_pipes + pipe_idx,
			worker_rx_pipe_pop_cb,
			worker);
	}

}

static void
worker_loop_round(struct dataplane_worker *worker)
{
	struct packet_list input_packets;
	packet_list_init(&input_packets);

	struct packet_list output_packets;
	packet_list_init(&output_packets);

	struct packet_list drop_packets;
	packet_list_init(&drop_packets);

	worker_read(worker, &input_packets);

	// Determine pipelines
	dataplane_route_pipeline(worker->dataplane, &input_packets);

	// Now group packets by pipeline and build pipeline_front
	while (packet_list_first(&input_packets)) {
		struct pipeline *pipeline =
			packet_list_first(&input_packets)->pipeline;

		struct pipeline_front pipeline_front;
		pipeline_front_init(&pipeline_front);

		// List of packets with different pipeline assigned
		struct packet_list ready_packets;
		packet_list_init(&ready_packets);

		struct packet *packet;
		while ((packet = packet_list_pop(&input_packets))) {
			if (packet->pipeline == pipeline) {
				pipeline_front_output(
					&pipeline_front,
					packet);
			} else {
				packet_list_add(&ready_packets, packet);
			}
		}

		// Process pipeline and push packets into drop and write lists

		pipeline_process(pipeline, &pipeline_front);

		packet_list_concat(&drop_packets, &pipeline_front.drop);
		packet_list_concat(&output_packets, &pipeline_front.output);
		packet_list_concat(&output_packets, &pipeline_front.bypass);

		input_packets = ready_packets;
	}

	worker_write(worker, &output_packets);

	/*
	 * `output_packets` now contains failed-to-transmit packets which
	 * should be freed.
	 */
	packet_list_concat(&drop_packets, &output_packets);
	dataplane_drop_packets(worker->dataplane, &drop_packets);
}

void
worker_exec(struct dataplane_worker *worker)
{
	while (1) {
		worker_loop_round(worker);
	}

}

int
dataplane_worker_init(
	struct dataplane *dataplane,
	struct dataplane_device *device,
	struct dataplane_worker *worker,
	int queue_id)
{
	worker->dataplane = dataplane;
	worker->device = device;
	worker->device_id = device->device_id;
	worker->port_id = device->port_id;
	worker->queue_id = queue_id;

	worker->read_ctx.read_size = 32;
	worker->write_ctx.write_size = 32;
	worker->write_ctx.rx_pipes = NULL;

	// Initialize device rx and tx queue
	if (rte_eth_tx_queue_setup(
		device->port_id,
		queue_id,
		4096,
		0,
		NULL)) {
		return -1;
	}

	char mempool_name[80];
	snprintf(
		mempool_name,
		sizeof(mempool_name) - 1,
		"wrk_rx_pool_%d_%d",
		device->port_id,
		queue_id);

	worker->rx_mempool = rte_mempool_create(
		mempool_name,
		4096,
		8192,
		0,
		sizeof(struct rte_pktmbuf_pool_private),
		rte_pktmbuf_pool_init,
		NULL,
		rte_pktmbuf_init,
		NULL,
		0,
		MEMPOOL_F_SP_PUT | MEMPOOL_F_SC_GET);
	if (worker->rx_mempool == NULL) {
		return -1;
	}

	if (rte_eth_rx_queue_setup(
		device->port_id,
		queue_id,
		4096,
		0,
		NULL,
		worker->rx_mempool)) {
		goto error_mempool;
	}

	// Allocate connection data for each dataplane device
	worker->write_ctx.tx_connections = (struct worker_tx_connection *)
		malloc(
			sizeof(struct worker_tx_connection) *
			dataplane->device_count);
	if (worker->write_ctx.tx_connections == NULL) {
		goto error_mempool;
	}

	memset(
		worker->write_ctx.tx_connections,
		0,
		sizeof(struct worker_tx_connection) *
		dataplane->device_count);

	// Initialize zero rx connections
	worker->write_ctx.rx_pipe_count = 0;

	return 0;

error_mempool:
	rte_mempool_free(worker->rx_mempool);

	return -1;
}


