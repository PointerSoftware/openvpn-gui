#ifndef PACKET_DISTRIBUTOR_H
#define PACKET_DISTRIBUTOR_H

#include "bonding_types.h"

/**
 * @file packet_distributor.h
 * @brief Packet distribution across tunnels
 */

/* Forward declaration */
struct packet_distributor;

/**
 * @brief Packet distributor handle
 */
typedef struct packet_distributor packet_distributor_t;

/**
 * @brief Statistics structure for packet distributor
 */
typedef struct {
    uint64_t packets_sent;      /* Packets sent per tunnel */
    uint64_t bytes_sent;         /* Bytes sent per tunnel */
    uint64_t errors;            /* Error count per tunnel */
    int queue_depth;            /* Current queue depth */
    double average_latency_ms;   /* Average latency in milliseconds */
} packet_distributor_stats_t;

/**
 * @brief Configuration structure for packet distributor
 */
typedef struct {
    int queue_size;              /* Maximum queue size (default: 1000) */
    int io_timeout_ms;           /* I/O timeout in milliseconds (default: 100) */
    int sequencing_enabled;      /* Enable packet sequencing (default: 1) */
    int flow_control_enabled;   /* Enable flow control (default: 1) */
    int retry_count;             /* Retry count for failed operations (default: 3) */
} packet_distributor_config_t;

/* Distribution functions */
packet_distributor_t* packet_distributor_create(bonding_mode_t mode, int tunnel_count);
int packet_distributor_select_tunnel(packet_distributor_t *dist, int *tunnel_id);
int packet_distributor_set_tunnel_weight(packet_distributor_t *dist, int tunnel_id, int weight);
int packet_distributor_set_tunnel_health(packet_distributor_t *dist, int tunnel_id, int healthy);
void packet_distributor_destroy(packet_distributor_t *dist);

/* Additional functions for integration */
int packet_distributor_set_tunnel_tap_adapter(packet_distributor_t *dist, int tunnel_id, const char *tap_adapter_name);
int packet_distributor_get_stats(packet_distributor_t *dist, int tunnel_id, packet_distributor_stats_t *stats);
int packet_distributor_configure(packet_distributor_t *dist, const packet_distributor_config_t *config);

#endif /* PACKET_DISTRIBUTOR_H */
