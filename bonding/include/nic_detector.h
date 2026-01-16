#ifndef NIC_DETECTOR_H
#define NIC_DETECTOR_H

#include "bonding_types.h"

/**
 * @file nic_detector.h
 * @brief Physical NIC detection and enumeration
 */

/* Forward declaration */
struct nic_info;

/**
 * @brief NIC information structure
 */
typedef struct nic_info {
    char *name;                  /* NIC name */
    char *description;           /* NIC description */
    char *guid;                  /* NIC GUID */
    nic_type_t type;             /* NIC type */
    nic_status_t status;          /* Connection status */
    unsigned long speed;          /* Link speed (Mbps) */
    int index;                   /* Interface index */
} nic_info_t;

/**
 * @brief NIC quality metrics structure
 */
typedef struct {
    unsigned long latency_ms;      /* Average latency in milliseconds */
    float packet_loss_percent;     /* Packet loss percentage */
    unsigned long jitter_ms;       /* Jitter in milliseconds */
    unsigned long bandwidth_mbps;  /* Available bandwidth */
    int quality_score;             /* Overall quality score (0-100) */
} nic_quality_t;

/**
 * @brief NIC capabilities structure
 */
typedef struct {
    int supports_full_duplex;
    int supports_jumbo_frames;
    unsigned long max_speed_mbps;
    int supports_wake_on_lan;
    int supports_offload;
} nic_capabilities_t;

/**
 * @brief NIC event types
 */
typedef enum {
    NIC_EVENT_CONNECTED = 0,
    NIC_EVENT_DISCONNECTED = 1,
    NIC_EVENT_IP_CHANGED = 2,
    NIC_EVENT_SPEED_CHANGED = 3,
    NIC_EVENT_ADDED = 4,
    NIC_EVENT_REMOVED = 5
} nic_event_type_t;

/**
 * @brief NIC event callback function type
 */
typedef void (*nic_event_callback_t)(nic_event_type_t event, const char *nic_name, void *user_data);

/* Core detection functions */
int nic_detector_get_count(void);
int nic_detector_enumerate(nic_info_t *nics, int max_count);
int nic_detector_get_by_name(const char *name, nic_info_t *nic);
int nic_detector_is_physical(const char *name);

/* Quality monitoring functions */
int nic_detector_get_quality(const char *nic_name, nic_quality_t *quality);
int nic_detector_monitor_quality(void);

/* Priority management functions */
int nic_detector_set_priority(const char *nic_name, int priority);
int nic_detector_get_priority(const char *nic_name);
const char* nic_detector_get_best_nic(void);

/* Capability detection */
int nic_detector_get_capabilities(const char *nic_name, nic_capabilities_t *capabilities);

/* Event monitoring */
int nic_detector_register_callback(nic_event_callback_t callback, void *user_data);
int nic_detector_unregister_callback(nic_event_callback_t callback);

/* Memory management */
int nic_detector_init(void);
void nic_detector_cleanup(void);
void nic_detector_free_info(nic_info_t *nic);

#endif /* NIC_DETECTOR_H */
