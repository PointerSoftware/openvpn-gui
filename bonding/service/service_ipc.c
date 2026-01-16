/**
 * @file service_ipc.c
 * @brief IPC communication between GUI and service
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "service_ipc.h"
#include "bonding_config.h"
#include "bonding_manager.h"
#include "bonding_service.h"
#include "bonding_types.h"

/* External bonding manager instance (from bonding_service.c) */
extern bonding_manager_t *g_BondingManager;
extern CRITICAL_SECTION g_ServiceMutex;

/* External logging function */
extern void bonding_log(int level, const char *format, ...);
#define BONDING_LOG_ERROR(...) bonding_log(3, __VA_ARGS__)
#define BONDING_LOG_WARN(...) bonding_log(2, __VA_ARGS__)
#define BONDING_LOG_INFO(...) bonding_log(1, __VA_ARGS__)

/* IPC server state */
static HANDLE g_hPipe = INVALID_HANDLE_VALUE;
static int g_ipc_initialized = 0;

/**
 * @brief Initialize named pipe server
 */
int service_ipc_init(void)
{
    SECURITY_ATTRIBUTES sa;
    SECURITY_DESCRIPTOR sd;

    /* Initialize security descriptor to allow all users */
    if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)) {
        BONDING_LOG_ERROR("InitializeSecurityDescriptor failed (error %d)", GetLastError());
        return -1;
    }

    if (!SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE)) {
        BONDING_LOG_ERROR("SetSecurityDescriptorDacl failed (error %d)", GetLastError());
        return -1;
    }

    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;

    g_ipc_initialized = 1;
    BONDING_LOG_INFO("IPC server initialized");
    return 0;
}

/**
 * @brief Accept a connection on the named pipe
 * @return Pipe handle on success, INVALID_HANDLE_VALUE on error
 */
int service_ipc_accept_connection(void)
{
    HANDLE hPipe;

    if (!g_ipc_initialized) {
        return (int)INVALID_HANDLE_VALUE;
    }

    /* Create named pipe instance */
    hPipe = CreateNamedPipe(
        BONDING_IPC_PIPE_NAME,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        8192,  /* Output buffer size */
        8192,  /* Input buffer size */
        0,     /* Default timeout */
        NULL   /* Default security attributes */
    );

    if (hPipe == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error != ERROR_PIPE_CONNECTED) {
            BONDING_LOG_ERROR("CreateNamedPipe failed (error %d)", error);
            return (int)INVALID_HANDLE_VALUE;
        }
    }

    /* Wait for client connection */
    if (ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED)) {
        BONDING_LOG_INFO("IPC client connected");
        return (int)hPipe;
    } else {
        BONDING_LOG_ERROR("ConnectNamedPipe failed (error %d)", GetLastError());
        CloseHandle(hPipe);
        return (int)INVALID_HANDLE_VALUE;
    }
}

/**
 * @brief Process an IPC message
 */
int service_ipc_process_message(HANDLE hPipe, ipc_message_t *request, ipc_message_t *response)
{
    DWORD dwBytesRead, dwBytesWritten;
    BOOL fSuccess;

    if (!hPipe || hPipe == INVALID_HANDLE_VALUE || !request || !response) {
        return -1;
    }

    /* Read request message */
    fSuccess = ReadFile(
        hPipe,
        request,
        sizeof(ipc_message_t),
        &dwBytesRead,
        NULL
    );

    if (!fSuccess || dwBytesRead == 0) {
        BONDING_LOG_ERROR("ReadFile failed (error %d)", GetLastError());
        return -1;
    }

    /* Initialize response */
    memset(response, 0, sizeof(ipc_message_t));

    /* Process command */
    switch (request->message_type) {
        case IPC_CMD_START: {
            /* Load profile from payload (config path) */
            if (request->payload_length > 0 && request->payload_length < 8192) {
                char config_path[8192];
                memcpy(config_path, request->payload, request->payload_length);
                config_path[request->payload_length] = '\0';

                EnterCriticalSection(&g_ServiceMutex);
                if (g_BondingManager) {
                    bonding_profile_t *profile = bonding_config_load(config_path);
                    if (profile) {
                        if (bonding_manager_start(g_BondingManager, profile) == 0) {
                            response->message_type = IPC_RESP_SUCCESS;
                            BONDING_LOG_INFO("Bonding started via IPC");
                        } else {
                            response->message_type = IPC_RESP_ERROR;
                            strncpy((char*)response->payload, "Failed to start bonding", sizeof(response->payload) - 1);
                            response->payload_length = strlen((char*)response->payload);
                            bonding_config_free(profile);
                        }
                    } else {
                        response->message_type = IPC_RESP_ERROR;
                        strncpy((char*)response->payload, "Failed to load profile", sizeof(response->payload) - 1);
                        response->payload_length = strlen((char*)response->payload);
                    }
                } else {
                    response->message_type = IPC_RESP_ERROR;
                    strncpy((char*)response->payload, "Bonding manager not initialized", sizeof(response->payload) - 1);
                    response->payload_length = strlen((char*)response->payload);
                }
                LeaveCriticalSection(&g_ServiceMutex);
            } else {
                response->message_type = IPC_RESP_ERROR;
                strncpy((char*)response->payload, "Invalid config path", sizeof(response->payload) - 1);
                response->payload_length = strlen((char*)response->payload);
            }
            break;
        }

        case IPC_CMD_STOP: {
            EnterCriticalSection(&g_ServiceMutex);
            if (g_BondingManager) {
                if (bonding_manager_stop(g_BondingManager) == 0) {
                    response->message_type = IPC_RESP_SUCCESS;
                    BONDING_LOG_INFO("Bonding stopped via IPC");
                } else {
                    response->message_type = IPC_RESP_ERROR;
                    strncpy((char*)response->payload, "Failed to stop bonding", sizeof(response->payload) - 1);
                    response->payload_length = strlen((char*)response->payload);
                }
            } else {
                response->message_type = IPC_RESP_ERROR;
                strncpy((char*)response->payload, "Bonding manager not initialized", sizeof(response->payload) - 1);
                response->payload_length = strlen((char*)response->payload);
            }
            LeaveCriticalSection(&g_ServiceMutex);
            break;
        }

        case IPC_CMD_GET_STATUS: {
            ipc_status_response_t status_resp = {0};
            tunnel_state_t states[32];
            int tunnel_count = 0;

            EnterCriticalSection(&g_ServiceMutex);
            if (g_BondingManager) {
                tunnel_count = bonding_manager_get_status(g_BondingManager, states, 32);
                if (tunnel_count >= 0) {
                    status_resp.service_state = BONDING_SERVICE_STATE_RUNNING;
                    status_resp.bonding_state = BONDING_STATE_RUNNING;
                    status_resp.tunnel_count = tunnel_count;
                    memcpy(status_resp.tunnel_states, states, sizeof(tunnel_state_t) * tunnel_count);
                    response->message_type = IPC_RESP_STATUS;
                    memcpy(response->payload, &status_resp, sizeof(status_resp));
                    response->payload_length = sizeof(status_resp);
                } else {
                    response->message_type = IPC_RESP_ERROR;
                    strncpy((char*)response->payload, "Failed to get status", sizeof(response->payload) - 1);
                    response->payload_length = strlen((char*)response->payload);
                }
            } else {
                status_resp.service_state = BONDING_SERVICE_STATE_STOPPED;
                status_resp.bonding_state = BONDING_STATE_STOPPED;
                status_resp.tunnel_count = 0;
                response->message_type = IPC_RESP_STATUS;
                memcpy(response->payload, &status_resp, sizeof(status_resp));
                response->payload_length = sizeof(status_resp);
            }
            LeaveCriticalSection(&g_ServiceMutex);
            break;
        }

        case IPC_CMD_SET_CONFIG: {
            /* Similar to START, but for updating existing config */
            response->message_type = IPC_RESP_ERROR;
            strncpy((char*)response->payload, "SET_CONFIG not yet implemented", sizeof(response->payload) - 1);
            response->payload_length = strlen((char*)response->payload);
            break;
        }

        case IPC_CMD_FAILOVER: {
            /* Trigger manual failover */
            response->message_type = IPC_RESP_ERROR;
            strncpy((char*)response->payload, "FAILOVER not yet implemented", sizeof(response->payload) - 1);
            response->payload_length = strlen((char*)response->payload);
            break;
        }

        default:
            response->message_type = IPC_RESP_ERROR;
            strncpy((char*)response->payload, "Unknown command", sizeof(response->payload) - 1);
            response->payload_length = strlen((char*)response->payload);
            break;
    }

    /* Send response */
    fSuccess = WriteFile(
        hPipe,
        response,
        sizeof(ipc_message_t),
        &dwBytesWritten,
        NULL
    );

    if (!fSuccess || dwBytesWritten != sizeof(ipc_message_t)) {
        BONDING_LOG_ERROR("WriteFile failed (error %d)", GetLastError());
        return -1;
    }

    FlushFileBuffers(hPipe);
    return 0;
}

/**
 * @brief Cleanup IPC server
 */
void service_ipc_cleanup(void)
{
    if (g_hPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_hPipe);
        g_hPipe = INVALID_HANDLE_VALUE;
    }
    g_ipc_initialized = 0;
    BONDING_LOG_INFO("IPC server cleaned up");
}

/* ========== Client-side functions (for GUI) ========== */

/**
 * @brief Connect to the service IPC pipe (client-side)
 */
HANDLE service_ipc_connect(void)
{
    HANDLE hPipe;
    DWORD dwMode;

    /* Try to connect to pipe */
    while (1) {
        hPipe = CreateFile(
            BONDING_IPC_PIPE_NAME,
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            0,
            NULL
        );

        if (hPipe != INVALID_HANDLE_VALUE) {
            break;
        }

        if (GetLastError() != ERROR_PIPE_BUSY) {
            return INVALID_HANDLE_VALUE;
        }

        /* Wait for pipe to become available */
        if (!WaitNamedPipe(BONDING_IPC_PIPE_NAME, 5000)) {
            return INVALID_HANDLE_VALUE;
        }
    }

    /* Set pipe to message mode */
    dwMode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(hPipe, &dwMode, NULL, NULL)) {
        CloseHandle(hPipe);
        return INVALID_HANDLE_VALUE;
    }

    return hPipe;
}

/**
 * @brief Send a command to the service (client-side)
 */
int service_ipc_send_command(HANDLE hPipe, ipc_message_type_t cmd, const void *payload, DWORD payload_size)
{
    ipc_message_t message;
    DWORD dwBytesWritten;
    BOOL fSuccess;

    if (hPipe == INVALID_HANDLE_VALUE || !hPipe) {
        return -1;
    }

    if (payload_size > sizeof(message.payload)) {
        return -1;
    }

    memset(&message, 0, sizeof(message));
    message.message_type = cmd;
    message.payload_length = payload_size;
    if (payload && payload_size > 0) {
        memcpy(message.payload, payload, payload_size);
    }

    fSuccess = WriteFile(
        hPipe,
        &message,
        sizeof(ipc_message_t),
        &dwBytesWritten,
        NULL
    );

    if (!fSuccess || dwBytesWritten != sizeof(ipc_message_t)) {
        return -1;
    }

    FlushFileBuffers(hPipe);
    return 0;
}

/**
 * @brief Receive a response from the service (client-side)
 */
int service_ipc_receive_response(HANDLE hPipe, ipc_message_t *response, DWORD timeout_ms)
{
    DWORD dwBytesRead;
    BOOL fSuccess;
    DWORD dwTotalBytesAvail = 0;

    if (hPipe == INVALID_HANDLE_VALUE || !hPipe || !response) {
        return -1;
    }

    /* Wait for data with timeout */
    if (timeout_ms > 0) {
        DWORD dwWaitResult = WaitForSingleObject(hPipe, timeout_ms);
        if (dwWaitResult != WAIT_OBJECT_0) {
            return -1;
        }
    }

    /* Check if data is available */
    if (!PeekNamedPipe(hPipe, NULL, 0, NULL, &dwTotalBytesAvail, NULL)) {
        return -1;
    }

    if (dwTotalBytesAvail == 0) {
        return -1;
    }

    /* Read response */
    fSuccess = ReadFile(
        hPipe,
        response,
        sizeof(ipc_message_t),
        &dwBytesRead,
        NULL
    );

    if (!fSuccess || dwBytesRead == 0) {
        return -1;
    }

    return 0;
}

/**
 * @brief Disconnect from the service IPC pipe (client-side)
 */
void service_ipc_disconnect(HANDLE hPipe)
{
    if (hPipe != INVALID_HANDLE_VALUE && hPipe != NULL) {
        CloseHandle(hPipe);
    }
}
