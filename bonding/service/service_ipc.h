#ifndef SERVICE_IPC_H
#define SERVICE_IPC_H

#include <windows.h>
#include "bonding_config.h"
#include "bonding_types.h"
#include "bonding_service.h"

/**
 * @file service_ipc.h
 * @brief IPC communication between GUI and service
 */

/* Named pipe name constant */
#define BONDING_IPC_PIPE_NAME L"\\\\.\\pipe\\OpenVPNBonding"

/* IPC message types */
typedef enum {
    IPC_CMD_START = 1,
    IPC_CMD_STOP = 2,
    IPC_CMD_GET_STATUS = 3,
    IPC_CMD_SET_CONFIG = 4,
    IPC_CMD_FAILOVER = 5,
    IPC_RESP_SUCCESS = 100,
    IPC_RESP_ERROR = 101,
    IPC_RESP_STATUS = 102
} ipc_message_type_t;

/* IPC message structure */
typedef struct {
    ipc_message_type_t message_type;
    DWORD payload_length;
    BYTE payload[8192];  /* Maximum payload size */
} ipc_message_t;

/* Status response structure */
typedef struct {
    bonding_service_state_t service_state;
    bonding_state_t bonding_state;
    int tunnel_count;
    tunnel_state_t tunnel_states[32];
    char error_message[512];
} ipc_status_response_t;

/* Server-side functions (service) */
int service_ipc_init(void);
int service_ipc_accept_connection(void);
int service_ipc_process_message(HANDLE hPipe, ipc_message_t *request, ipc_message_t *response);
void service_ipc_cleanup(void);

/* Client-side functions (GUI) */
HANDLE service_ipc_connect(void);
int service_ipc_send_command(HANDLE hPipe, ipc_message_type_t cmd, const void *payload, DWORD payload_size);
int service_ipc_receive_response(HANDLE hPipe, ipc_message_t *response, DWORD timeout_ms);
void service_ipc_disconnect(HANDLE hPipe);

#endif /* SERVICE_IPC_H */
