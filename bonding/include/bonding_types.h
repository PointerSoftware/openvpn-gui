#ifndef BONDING_TYPES_H
#define BONDING_TYPES_H

/**
 * @file bonding_types.h
 * @brief Common types and enums for bonding module
 */

/* Bonding modes */
typedef enum {
    BONDING_MODE_ROUND_ROBIN = 0,    /* Round-robin packet distribution */
    BONDING_MODE_WEIGHTED = 1,       /* Weighted distribution */
    BONDING_MODE_ACTIVE_BACKUP = 2,  /* Active/backup redundancy */
    BONDING_MODE_ADAPTIVE = 3        /* Adaptive bonding mode */
} bonding_mode_t;

/* NIC types */
typedef enum {
    NIC_TYPE_ETHERNET = 0,
    NIC_TYPE_WIFI = 1,
    NIC_TYPE_LTE = 2,
    NIC_TYPE_UNKNOWN = 3
} nic_type_t;

/* Tunnel states */
typedef enum {
    TUNNEL_STATE_DISCONNECTED = 0,
    TUNNEL_STATE_CONNECTING = 1,
    TUNNEL_STATE_CONNECTED = 2,
    TUNNEL_STATE_FAILED = 3
} tunnel_state_t;

/* NIC connection status */
typedef enum {
    NIC_STATUS_DISCONNECTED = 0,
    NIC_STATUS_CONNECTED = 1,
    NIC_STATUS_UNKNOWN = 2
} nic_status_t;

/* Bonding manager state */
typedef enum {
    BONDING_STATE_STOPPED = 0,
    BONDING_STATE_STARTING = 1,
    BONDING_STATE_RUNNING = 2,
    BONDING_STATE_STOPPING = 3,
    BONDING_STATE_FAILED = 4,
    BONDING_STATE_RECOVERING = 5
} bonding_state_t;

#endif /* BONDING_TYPES_H */
