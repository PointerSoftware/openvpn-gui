#ifndef BONDING_CONFIG_H
#define BONDING_CONFIG_H

#include "bonding_types.h"

/**
 * @file bonding_config.h
 * @brief Configuration structures for bonding profiles
 */

/* Constants */
#define MAX_TUNNEL_COUNT 32
#define MAX_LINE_LENGTH 1024
#define MAX_PARAM_LENGTH 256
#define MAX_ERROR_MESSAGE 512

/* Forward declarations */
struct bonding_profile;
struct tunnel_config;

/**
 * @brief Error codes for configuration operations
 */
typedef enum {
    BONDING_ERROR_SUCCESS = 0,
    BONDING_ERROR_FILE_NOT_FOUND,
    BONDING_ERROR_PARSE_ERROR,
    BONDING_ERROR_VALIDATION_ERROR,
    BONDING_ERROR_MEMORY_ERROR,
    BONDING_ERROR_INVALID_MODE,
    BONDING_ERROR_INVALID_TUNNEL_COUNT,
    BONDING_ERROR_DUPLICATE_PORT,
    BONDING_ERROR_MISSING_NIC,
    BONDING_ERROR_MISSING_PARAMETER,
    BONDING_ERROR_INVALID_PORT,
    BONDING_ERROR_INVALID_WEIGHT,
    BONDING_ERROR_FILE_ACCESS,
    BONDING_ERROR_WRITE_ERROR
} bonding_error_t;

/**
 * @brief Validation result structure
 */
typedef struct {
    bonding_error_t error_code;
    int line_number;
    char error_message[MAX_ERROR_MESSAGE];
} validation_result_t;

/**
 * @brief Configuration structure for a single tunnel
 */
typedef struct tunnel_config {
    char *nic_name;              /* Physical NIC name */
    char *tap_adapter;           /* TAP adapter name */
    char *server_host;           /* Server hostname/IP */
    int server_port;             /* Server port */
    char *config_file;           /* OpenVPN config file path */
    int weight;                  /* Distribution weight (for weighted mode) */
    tunnel_state_t state;        /* Current tunnel state */
} tunnel_config_t;

/**
 * @brief Main bonding profile configuration
 */
typedef struct bonding_profile {
    char *profile_name;          /* Profile name */
    bonding_mode_t mode;         /* Bonding mode */
    int tunnel_count;            /* Number of tunnels */
    tunnel_config_t *tunnels;    /* Array of tunnel configurations */
    char *config_path;           /* Path to .ovpn-bond file */
    validation_result_t validation; /* Validation state and error information */
    /* Internal: Track parsed tunnel sections for validation */
    int parsed_tunnel_count;     /* Number of [tunnel-N] sections actually parsed */
    int parsed_tunnel_indices[MAX_TUNNEL_COUNT]; /* Indices of parsed tunnel sections */
    /* Packet distributor configuration */
    int packet_queue_size;       /* Maximum queue size (100-10000, default: 1000) */
    int packet_timeout;          /* I/O timeout in milliseconds (default: 100) */
    int sequencing_enabled;      /* Enable packet sequencing (default: 1) */
    int flow_control_enabled;    /* Enable flow control (default: 1) */
} bonding_profile_t;

/* Configuration functions */
bonding_profile_t* bonding_config_load(const char *config_path);
int bonding_config_save(bonding_profile_t *profile, const char *config_path);
void bonding_config_free(bonding_profile_t *profile);

/* Validation function */
validation_result_t bonding_config_validate(bonding_profile_t *profile);

/* Helper functions */
bonding_mode_t bonding_mode_from_string(const char *mode_str);
const char* bonding_mode_to_string(bonding_mode_t mode);

#endif /* BONDING_CONFIG_H */
