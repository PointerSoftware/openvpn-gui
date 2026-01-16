/**
 * @file openvpn_manager.c
 * @brief Multi-instance OpenVPN process manager implementation
 */

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <tchar.h>
#include <shlwapi.h>

#include "openvpn_manager.h"
#include "bonding_config.h"
#include "bonding_types.h"
#include "nic_detector.h"
#include "../utils/routing_helper.h"

/* External functions from OpenVPN GUI */
extern void MsgToEventLog(WORD type, wchar_t *format, ...);
extern bool GetRandomPassword(char *buf, size_t len);
extern WCHAR* Widen(const char *utf8);
extern char* WCharToUTF8(const WCHAR *wstr);
extern BOOL CheckFileAccess(const TCHAR *path, int access);
extern BOOL find_free_tcp_port(SOCKADDR_IN *addr);
extern options_t o;

#ifdef DEBUG
extern void PrintDebug(TCHAR *format, ...);
#else
#define PrintDebug(...) do {} while(0)
#endif

/* Logging macros */
#define BONDING_LOG_ERROR(...) do { \
    WCHAR *wmsg = Widen(__VA_ARGS__); \
    if (wmsg) { \
        MsgToEventLog(EVENTLOG_ERROR_TYPE, L"[Bonding] %ls", wmsg); \
        free(wmsg); \
    } \
} while(0)

#define BONDING_LOG_INFO(...) do { \
    WCHAR *wmsg = Widen(__VA_ARGS__); \
    if (wmsg) { \
        MsgToEventLog(EVENTLOG_INFORMATION_TYPE, L"[Bonding] %ls", wmsg); \
        free(wmsg); \
    } \
} while(0)

#ifdef DEBUG
#define BONDING_LOG_DEBUG(...) PrintDebug(L"[Bonding] " __VA_ARGS__)
#else
#define BONDING_LOG_DEBUG(...) do {} while(0)
#endif

/* Constants */
#define MAX_RESTARTS 3
#define HEARTBEAT_TIMEOUT 30
#define MONITOR_INTERVAL 5000
#define PROCESS_TERMINATE_TIMEOUT 10000
#define MANAGEMENT_CONNECT_TIMEOUT 5000
#define BASE_MANAGEMENT_PORT 25340
#define INITIAL_INSTANCE_CAPACITY 32

/**
 * @brief OpenVPN process instance structure
 */
typedef struct {
    int tunnel_id;                      /* Unique identifier matching tunnel index */
    HANDLE hProcess;                    /* Windows process handle */
    HANDLE hThread;                     /* Monitoring thread handle */
    HANDLE exit_event;                  /* Event for graceful shutdown */
    int management_port;                /* Unique management interface port */
    SOCKET management_socket;           /* Socket for management communication */
    tunnel_state_t state;               /* Current tunnel state */
    TCHAR config_path[MAX_PATH];         /* Path to OpenVPN config file */
    TCHAR tap_adapter_name[256];        /* TAP adapter identifier */
    char nic_name[256];                 /* Physical NIC name for binding */
    char server_host[256];              /* Server hostname/IP for routing and restart */
    DWORD pid;                          /* Process ID */
    int restart_count;                  /* Number of automatic restarts */
    time_t last_heartbeat;              /* Last successful management interface response */
    TCHAR cmdline[2048];                /* Constructed command line for process */
    MIB_IPFORWARDROW route_entry;       /* Routing entry for cleanup */
    BOOL route_configured;              /* Whether route was configured */
} openvpn_instance_t;

/**
 * @brief OpenVPN manager structure
 */
struct openvpn_manager {
    openvpn_instance_t *instances;      /* Dynamic array of process instances */
    int instance_count;                 /* Number of active instances */
    int max_instances;                  /* Allocated capacity */
    int base_management_port;           /* Starting port for management interfaces */
    HANDLE monitor_thread;              /* Global health monitoring thread */
    HANDLE monitor_exit_event;          /* Event to stop monitoring thread */
    CRITICAL_SECTION mutex;              /* Thread synchronization for instance array */
    TCHAR openvpn_exe_path[MAX_PATH];   /* Path to openvpn.exe */
};

/* Forward declarations */
static DWORD WINAPI MonitorThreadProc(LPVOID lpParam);
static int BindProcessToNIC(openvpn_instance_t *instance, const char *nic_name, const char *server_host);
static int InitManagementInterface(openvpn_instance_t *instance);
static int SendManagementCommand(openvpn_instance_t *instance, const char *command, char *response, size_t response_size);
static int EnsureTAPAdapterExists(const char *tap_adapter_name);
static int GetTAPAdapterName(int tunnel_id, TCHAR *adapter_name, size_t adapter_name_size);
static void RestartFailedInstance(openvpn_manager_t *mgr, openvpn_instance_t *instance);
static int FindInstanceIndex(openvpn_manager_t *mgr, int tunnel_id);
static int openvpn_manager_stop_instance_locked(openvpn_manager_t *mgr, int tunnel_id);

/**
 * @brief Find instance index by tunnel_id
 */
static int FindInstanceIndex(openvpn_manager_t *mgr, int tunnel_id)
{
    int i;
    if (!mgr)
        return -1;
    
    for (i = 0; i < mgr->instance_count; i++) {
        if (mgr->instances[i].tunnel_id == tunnel_id)
            return i;
    }
    return -1;
}

/**
 * @brief Bind process to specific NIC using routing rules
 */
static int BindProcessToNIC(openvpn_instance_t *instance, const char *nic_name, const char *server_host)
{
    nic_info_t nic_info;
    nic_quality_t quality;
    DWORD status;
    
    if (!instance || !nic_name || !server_host)
        return -1;
    
    /* Get NIC information using NIC detector */
    if (nic_detector_get_by_name(nic_name, &nic_info) != 0) {
        BONDING_LOG_ERROR("BindProcessToNIC: NIC not found: %s", nic_name);
        return -1;
    }
    
    /* Check NIC quality before binding */
    if (nic_detector_get_quality(nic_name, &quality) == 0) {
        if (quality.quality_score < 50) {
            BONDING_LOG_WARN("BindProcessToNIC: NIC %s has poor quality (score: %d), binding anyway", 
                            nic_name, quality.quality_score);
        }
    }
    
    /* Use routing helper to bind traffic */
    if (routing_helper_bind_to_nic(nic_name, server_host) != 0) {
        BONDING_LOG_ERROR("BindProcessToNIC: Failed to bind to NIC: %s", nic_name);
        nic_detector_free_info(&nic_info);
        return -1;
    }
    
    /* Store route information for cleanup (get from routing helper) */
    /* Note: routing_helper tracks routes internally, so we just mark as configured */
    instance->route_configured = TRUE;
    instance->route_entry.dwForwardIfIndex = nic_info.index;
    
    BONDING_LOG_INFO("BindProcessToNIC: Route created for tunnel %d to %s via %s (index: %d)",
                     instance->tunnel_id, server_host, nic_name, nic_info.index);
    
    /* Free NIC info */
    nic_detector_free_info(&nic_info);
    return 0;
}

/**
 * @brief Initialize management interface connection
 */
static int InitManagementInterface(openvpn_instance_t *instance)
{
    struct sockaddr_in addr;
    fd_set write_fds;
    struct timeval timeout;
    int result;
    unsigned long mode = 1;
    char buffer[4096];
    int bytes_received;
    WSADATA wsaData;
    
    if (!instance)
        return -1;
    
    /* Initialize Winsock */
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        BONDING_LOG_ERROR("InitManagementInterface: WSAStartup failed");
        return -1;
    }
    
    /* Create TCP socket */
    instance->management_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (instance->management_socket == INVALID_SOCKET) {
        BONDING_LOG_ERROR("InitManagementInterface: Failed to create socket");
        return -1;
    }
    
    /* Set socket to non-blocking */
    if (ioctlsocket(instance->management_socket, FIONBIO, &mode) != 0) {
        BONDING_LOG_ERROR("InitManagementInterface: Failed to set non-blocking mode");
        closesocket(instance->management_socket);
        instance->management_socket = INVALID_SOCKET;
        return -1;
    }
    
    /* Connect to management interface */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(instance->management_port);
    
    result = connect(instance->management_socket, (struct sockaddr*)&addr, sizeof(addr));
    if (result == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK) {
            BONDING_LOG_ERROR("InitManagementInterface: Connect failed (error %d)", error);
            closesocket(instance->management_socket);
            instance->management_socket = INVALID_SOCKET;
            return -1;
        }
    }
    
    /* Wait for connection with timeout */
    FD_ZERO(&write_fds);
    FD_SET(instance->management_socket, &write_fds);
    timeout.tv_sec = MANAGEMENT_CONNECT_TIMEOUT / 1000;
    timeout.tv_usec = (MANAGEMENT_CONNECT_TIMEOUT % 1000) * 1000;
    
    result = select(0, NULL, &write_fds, NULL, &timeout);
    if (result <= 0) {
        BONDING_LOG_ERROR("InitManagementInterface: Connection timeout");
        closesocket(instance->management_socket);
        instance->management_socket = INVALID_SOCKET;
        return -1;
    }
    
    /* Read initial greeting */
    bytes_received = recv(instance->management_socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes_received <= 0) {
        BONDING_LOG_ERROR("InitManagementInterface: Failed to read greeting");
        closesocket(instance->management_socket);
        instance->management_socket = INVALID_SOCKET;
        return -1;
    }
    
    buffer[bytes_received] = '\0';
    BONDING_LOG_DEBUG("InitManagementInterface: Greeting received: %s", buffer);
    
    /* Send hold release command */
    if (SendManagementCommand(instance, "hold release\n", NULL, 0) != 0) {
        BONDING_LOG_ERROR("InitManagementInterface: Failed to send hold release");
        closesocket(instance->management_socket);
        instance->management_socket = INVALID_SOCKET;
        return -1;
    }
    
    instance->last_heartbeat = time(NULL);
    return 0;
}

/**
 * @brief Send command to management interface
 */
static int SendManagementCommand(openvpn_instance_t *instance, const char *command, char *response, size_t response_size)
{
    fd_set read_fds;
    struct timeval timeout;
    int result;
    int bytes_sent;
    int bytes_received;
    
    if (!instance || !command || instance->management_socket == INVALID_SOCKET)
        return -1;
    
    /* Send command */
    bytes_sent = send(instance->management_socket, command, (int)strlen(command), 0);
    if (bytes_sent < 0) {
        BONDING_LOG_ERROR("SendManagementCommand: Failed to send command");
        return -1;
    }
    
    /* Wait for response */
    if (response && response_size > 0) {
        FD_ZERO(&read_fds);
        FD_SET(instance->management_socket, &read_fds);
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;
        
        result = select(0, &read_fds, NULL, NULL, &timeout);
        if (result > 0 && FD_ISSET(instance->management_socket, &read_fds)) {
            bytes_received = recv(instance->management_socket, response, (int)(response_size - 1), 0);
            if (bytes_received > 0) {
                response[bytes_received] = '\0';
                return 0;
            }
        }
    }
    
    return 0;
}

/**
 * @brief Ensure TAP adapter exists, create if necessary
 */
static int EnsureTAPAdapterExists(const char *tap_adapter_name)
{
    PIP_ADAPTER_INFO adapter_list = NULL;
    PIP_ADAPTER_INFO adapter = NULL;
    ULONG adapter_list_size = 0;
    DWORD status;
    TCHAR tap_adapter_w[256];
    WCHAR *wpath = NULL;
    TCHAR addtap_path[MAX_PATH];
    TCHAR cmdline[MAX_PATH * 2];
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    DWORD exit_code;
    BOOL found = FALSE;
    
    if (!tap_adapter_name)
        return -1;
    
    /* Get adapter information */
    status = GetAdaptersInfo(NULL, &adapter_list_size);
    if (status != ERROR_BUFFER_OVERFLOW) {
        return -1;
    }
    
    adapter_list = (PIP_ADAPTER_INFO)malloc(adapter_list_size);
    if (!adapter_list)
        return -1;
    
    status = GetAdaptersInfo(adapter_list, &adapter_list_size);
    if (status != ERROR_SUCCESS) {
        free(adapter_list);
        return -1;
    }
    
    /* Search for TAP adapter */
    adapter = adapter_list;
    while (adapter) {
        if (strstr(adapter->Description, "TAP") != NULL ||
            strstr(adapter->Description, "TAP-Windows") != NULL) {
            if (strcmp(adapter->AdapterName, tap_adapter_name) == 0 ||
                strcmp(adapter->Description, tap_adapter_name) == 0) {
                found = TRUE;
                break;
            }
        }
        adapter = adapter->Next;
    }
    
    free(adapter_list);
    
    if (found) {
        return 0; /* Adapter exists */
    }
    
    /* TAP adapter not found - attempt to create it */
    BONDING_LOG_INFO("EnsureTAPAdapterExists: TAP adapter not found, attempting to create: %s", tap_adapter_name);
    
    /* Convert adapter name to wide string */
    wpath = Widen(tap_adapter_name);
    if (!wpath) {
        BONDING_LOG_ERROR("EnsureTAPAdapterExists: Failed to convert adapter name");
        return -1;
    }
    _tcsncpy_s(tap_adapter_w, _countof(tap_adapter_w), wpath, _TRUNCATE);
    free(wpath);
    
    /* Try to find addtap.bat in OpenVPN installation */
    HKEY regkey;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, _T("SOFTWARE\\OpenVPN"), 0, KEY_READ, &regkey) == ERROR_SUCCESS) {
        TCHAR install_path[MAX_PATH];
        DWORD install_path_size = sizeof(install_path) / sizeof(TCHAR);
        
        if (RegQueryValueEx(regkey, _T(""), NULL, NULL, (LPBYTE)install_path, &install_path_size) == ERROR_SUCCESS) {
            _sntprintf_s(addtap_path, _countof(addtap_path), _TRUNCATE,
                        _T("%lsbin\\addtap.bat"), install_path);
        }
        RegCloseKey(regkey);
    }
    
    /* If addtap.bat not found, try default location */
    if (addtap_path[0] == '\0') {
        _tcsncpy_s(addtap_path, _countof(addtap_path),
                  _T("C:\\Program Files\\OpenVPN\\bin\\addtap.bat"), _TRUNCATE);
    }
    
    /* Check if addtap.bat exists */
    if (!CheckFileAccess(addtap_path, GENERIC_READ)) {
        BONDING_LOG_ERROR("EnsureTAPAdapterExists: addtap.bat not found at %ls", addtap_path);
        BONDING_LOG_ERROR("EnsureTAPAdapterExists: TAP adapter creation requires admin privileges and TAP-Windows driver");
        return -1;
    }
    
    /* Run addtap.bat to create TAP adapter */
    /* Note: This requires admin privileges. In production, this should be done via a service */
    _sntprintf_s(cmdline, _countof(cmdline), _TRUNCATE, _T("cmd.exe /c \"%ls\""), addtap_path);
    
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    
    memset(&pi, 0, sizeof(pi));
    
    if (!CreateProcess(NULL, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        DWORD error = GetLastError();
        BONDING_LOG_ERROR("EnsureTAPAdapterExists: Failed to run addtap.bat (error %lu). Admin privileges may be required.", error);
        return -1;
    }
    
    /* Wait for process to complete */
    WaitForSingleObject(pi.hProcess, 30000); /* 30 second timeout */
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    
    if (exit_code != 0) {
        BONDING_LOG_ERROR("EnsureTAPAdapterExists: addtap.bat exited with code %lu", exit_code);
        return -1;
    }
    
    /* Wait a moment for adapter to be registered */
    Sleep(2000);
    
    /* Verify adapter was created by checking again */
    status = GetAdaptersInfo(NULL, &adapter_list_size);
    if (status != ERROR_BUFFER_OVERFLOW) {
        BONDING_LOG_ERROR("EnsureTAPAdapterExists: Failed to verify adapter creation");
        return -1;
    }
    
    adapter_list = (PIP_ADAPTER_INFO)malloc(adapter_list_size);
    if (!adapter_list)
        return -1;
    
    status = GetAdaptersInfo(adapter_list, &adapter_list_size);
    if (status != ERROR_SUCCESS) {
        free(adapter_list);
        return -1;
    }
    
    adapter = adapter_list;
    while (adapter) {
        if (strstr(adapter->Description, "TAP") != NULL ||
            strstr(adapter->Description, "TAP-Windows") != NULL) {
            if (strcmp(adapter->AdapterName, tap_adapter_name) == 0 ||
                strcmp(adapter->Description, tap_adapter_name) == 0) {
                free(adapter_list);
                BONDING_LOG_INFO("EnsureTAPAdapterExists: Successfully created TAP adapter: %s", tap_adapter_name);
                return 0;
            }
        }
        adapter = adapter->Next;
    }
    
    free(adapter_list);
    BONDING_LOG_ERROR("EnsureTAPAdapterExists: TAP adapter creation failed or adapter not found after creation: %s", tap_adapter_name);
    return -1;
}

/**
 * @brief Get TAP adapter name for tunnel
 */
static int GetTAPAdapterName(int tunnel_id, TCHAR *adapter_name, size_t adapter_name_size)
{
    if (!adapter_name || adapter_name_size < 32)
        return -1;
    
    _sntprintf_s(adapter_name, adapter_name_size, _TRUNCATE, _T("TAP-Bonding-%d"), tunnel_id);
    return 0;
}

/**
 * @brief Restart failed instance
 */
static void RestartFailedInstance(openvpn_manager_t *mgr, openvpn_instance_t *instance)
{
    char config_path_utf8[MAX_PATH];
    char tap_adapter_utf8[256];
    DWORD wait_result;
    
    if (!mgr || !instance)
        return;
    
    if (instance->restart_count >= MAX_RESTARTS) {
        BONDING_LOG_ERROR("RestartFailedInstance: Max restarts reached for tunnel %d", instance->tunnel_id);
        instance->state = TUNNEL_STATE_FAILED;
        return;
    }
    
    instance->restart_count++;
    BONDING_LOG_INFO("RestartFailedInstance: Restarting tunnel %d (attempt %d/%d)",
                     instance->tunnel_id, instance->restart_count, MAX_RESTARTS);
    
    /* Terminate any lingering process */
    if (instance->hProcess != NULL) {
        /* Signal graceful shutdown first */
        if (instance->exit_event) {
            SetEvent(instance->exit_event);
        }
        
        /* Send SIGTERM via management interface if connected */
        if (instance->management_socket != INVALID_SOCKET) {
            SendManagementCommand(instance, "signal SIGTERM\n", NULL, 0);
        }
        
        /* Wait for process termination */
        wait_result = WaitForSingleObject(instance->hProcess, PROCESS_TERMINATE_TIMEOUT);
        if (wait_result != WAIT_OBJECT_0) {
            /* Force terminate */
            BONDING_LOG_INFO("RestartFailedInstance: Force terminating lingering process for tunnel %d", instance->tunnel_id);
            TerminateProcess(instance->hProcess, 1);
        }
        
        CloseHandle(instance->hProcess);
        instance->hProcess = NULL;
    }
    
    /* Close management socket */
    if (instance->management_socket != INVALID_SOCKET) {
        closesocket(instance->management_socket);
        instance->management_socket = INVALID_SOCKET;
    }
    
    /* Remove routing rules */
    if (instance->route_configured && instance->nic_name[0] != '\0' && instance->server_host[0] != '\0') {
        if (routing_helper_remove_binding(instance->nic_name, instance->server_host) == 0) {
            BONDING_LOG_INFO("RestartFailedInstance: Removed route for tunnel %d", instance->tunnel_id);
        }
        instance->route_configured = FALSE;
    }
    
    /* Close exit event handle */
    if (instance->exit_event) {
        CloseHandle(instance->exit_event);
        instance->exit_event = NULL;
    }
    
    /* Wait with exponential backoff */
    int wait_time = 5 * (1 << (instance->restart_count - 1));
    if (wait_time > 20)
        wait_time = 20;
    Sleep(wait_time * 1000);
    
    /* Convert stored paths to UTF-8 for spawn function */
    char *config_path_ptr = WCharToUTF8(instance->config_path);
    char *tap_adapter_ptr = WCharToUTF8(instance->tap_adapter_name);
    
    if (!config_path_ptr || !tap_adapter_ptr) {
        BONDING_LOG_ERROR("RestartFailedInstance: Failed to convert paths for tunnel %d", instance->tunnel_id);
        instance->state = TUNNEL_STATE_FAILED;
        if (config_path_ptr) free(config_path_ptr);
        if (tap_adapter_ptr) free(tap_adapter_ptr);
        return;
    }
    
    /* Find instance index in array */
    int instance_index = FindInstanceIndex(mgr, instance->tunnel_id);
    if (instance_index < 0) {
        BONDING_LOG_ERROR("RestartFailedInstance: Instance not found in array for tunnel %d", instance->tunnel_id);
        instance->state = TUNNEL_STATE_FAILED;
        free(config_path_ptr);
        free(tap_adapter_ptr);
        return;
    }
    
    /* Temporarily remove instance from array so spawn can add it back */
    int i;
    for (i = instance_index; i < mgr->instance_count - 1; i++) {
        mgr->instances[i] = mgr->instances[i + 1];
    }
    mgr->instance_count--;
    
    /* Reset instance state for respawn */
    instance->state = TUNNEL_STATE_CONNECTING;
    instance->pid = 0;
    instance->last_heartbeat = 0;
    
    /* Leave critical section before calling spawn (spawn will acquire its own lock) */
    LeaveCriticalSection(&mgr->mutex);
    
    /* Actually respawn the process using stored parameters */
    int result = openvpn_manager_spawn_instance(mgr, instance->tunnel_id,
                                               config_path_ptr,
                                               tap_adapter_ptr,
                                               instance->nic_name[0] != '\0' ? instance->nic_name : NULL,
                                               instance->server_host[0] != '\0' ? instance->server_host : NULL);
    
    /* Re-enter critical section */
    EnterCriticalSection(&mgr->mutex);
    
    if (result != 0) {
        BONDING_LOG_ERROR("RestartFailedInstance: Failed to respawn tunnel %d", instance->tunnel_id);
        /* Instance was not re-added by spawn, mark as failed */
        /* Note: instance pointer may be invalid now, but we can't access it anyway */
    } else {
        BONDING_LOG_INFO("RestartFailedInstance: Successfully respawned tunnel %d", instance->tunnel_id);
        /* Reinitialize management interface will be done by monitor thread */
    }
    
    free(config_path_ptr);
    free(tap_adapter_ptr);
}

/**
 * @brief Global monitoring thread procedure
 */
static DWORD WINAPI MonitorThreadProc(LPVOID lpParam)
{
    openvpn_manager_t *mgr = (openvpn_manager_t*)lpParam;
    DWORD wait_result;
    int i;
    DWORD exit_code;
    time_t current_time;
    char response[4096];
    
    if (!mgr)
        return 1;
    
    while (1) {
        /* Wait for exit event or timeout */
        wait_result = WaitForSingleObject(mgr->monitor_exit_event, MONITOR_INTERVAL);
        if (wait_result == WAIT_OBJECT_0) {
            /* Exit event signaled */
            break;
        }
        
        /* Enter critical section to access instance array */
        EnterCriticalSection(&mgr->mutex);
        
        current_time = time(NULL);
        
        /* Iterate through all instances */
        for (i = 0; i < mgr->instance_count; i++) {
            openvpn_instance_t *instance = &mgr->instances[i];
            
            if (instance->hProcess == NULL)
                continue;
            
            /* Check if process is still running */
            if (GetExitCodeProcess(instance->hProcess, &exit_code)) {
                if (exit_code != STILL_ACTIVE) {
                    /* Process exited unexpectedly */
                    BONDING_LOG_ERROR("MonitorThreadProc: Process for tunnel %d exited with code %lu",
                                     instance->tunnel_id, exit_code);
                    instance->state = TUNNEL_STATE_FAILED;
                    
                    /* Attempt automatic restart */
                    if (instance->restart_count < MAX_RESTARTS) {
                        RestartFailedInstance(mgr, instance);
                    }
                    continue;
                }
            }
            
            /* Send management interface heartbeat */
            if (instance->management_socket != INVALID_SOCKET) {
                if (SendManagementCommand(instance, "state\n", response, sizeof(response)) == 0) {
                    instance->last_heartbeat = current_time;
                } else {
                    /* Management interface not responding */
                    if (current_time - instance->last_heartbeat > HEARTBEAT_TIMEOUT) {
                        BONDING_LOG_ERROR("MonitorThreadProc: No heartbeat from tunnel %d for %ld seconds",
                                         instance->tunnel_id, current_time - instance->last_heartbeat);
                        instance->state = TUNNEL_STATE_FAILED;
                    }
                }
            } else {
                /* Try to initialize management interface if not connected */
                if (instance->state == TUNNEL_STATE_CONNECTING) {
                    if (InitManagementInterface(instance) == 0) {
                        instance->state = TUNNEL_STATE_CONNECTED;
                        BONDING_LOG_INFO("MonitorThreadProc: Management interface connected for tunnel %d",
                                        instance->tunnel_id);
                    }
                }
            }
        }
        
        /* Leave critical section */
        LeaveCriticalSection(&mgr->mutex);
    }
    
    return 0;
}

/**
 * @brief Create OpenVPN manager
 */
openvpn_manager_t* openvpn_manager_create(void)
{
    openvpn_manager_t *mgr;
    HKEY regkey;
    DWORD path_size;
    
    /* Allocate manager structure */
    mgr = (openvpn_manager_t*)calloc(1, sizeof(openvpn_manager_t));
    if (!mgr) {
        BONDING_LOG_ERROR("openvpn_manager_create: Out of memory");
        return NULL;
    }
    
    /* Initialize critical section */
    InitializeCriticalSection(&mgr->mutex);
    
    /* Set base management port */
    mgr->base_management_port = BASE_MANAGEMENT_PORT;
    
    /* Allocate initial instances array */
    mgr->max_instances = INITIAL_INSTANCE_CAPACITY;
    mgr->instances = (openvpn_instance_t*)calloc(mgr->max_instances, sizeof(openvpn_instance_t));
    if (!mgr->instances) {
        BONDING_LOG_ERROR("openvpn_manager_create: Out of memory allocating instances");
        DeleteCriticalSection(&mgr->mutex);
        free(mgr);
        return NULL;
    }
    
    /* Retrieve OpenVPN executable path from registry or use default */
    path_size = sizeof(mgr->openvpn_exe_path) / sizeof(TCHAR);
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, _T("SOFTWARE\\OpenVPN"), 0, KEY_READ, &regkey) == ERROR_SUCCESS) {
        TCHAR install_path[MAX_PATH];
        DWORD install_path_size = sizeof(install_path) / sizeof(TCHAR);
        
        if (RegQueryValueEx(regkey, _T(""), NULL, NULL, (LPBYTE)install_path, &install_path_size) == ERROR_SUCCESS) {
            TCHAR exe_path[MAX_PATH];
            DWORD exe_path_size = sizeof(exe_path) / sizeof(TCHAR);
            
            if (RegQueryValueEx(regkey, _T("exe_path"), NULL, NULL, (LPBYTE)exe_path, &exe_path_size) == ERROR_SUCCESS) {
                _tcsncpy_s(mgr->openvpn_exe_path, _countof(mgr->openvpn_exe_path), exe_path, _TRUNCATE);
            } else {
                _sntprintf_s(mgr->openvpn_exe_path, _countof(mgr->openvpn_exe_path), _TRUNCATE,
                            _T("%lsbin\\openvpn.exe"), install_path);
            }
        } else {
            _tcsncpy_s(mgr->openvpn_exe_path, _countof(mgr->openvpn_exe_path),
                      _T("C:\\Program Files\\OpenVPN\\bin\\openvpn.exe"), _TRUNCATE);
        }
        RegCloseKey(regkey);
    } else {
        /* Use default path */
        _tcsncpy_s(mgr->openvpn_exe_path, _countof(mgr->openvpn_exe_path),
                  _T("C:\\Program Files\\OpenVPN\\bin\\openvpn.exe"), _TRUNCATE);
    }
    
    /* Create monitor exit event */
    mgr->monitor_exit_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!mgr->monitor_exit_event) {
        BONDING_LOG_ERROR("openvpn_manager_create: Failed to create monitor exit event");
        free(mgr->instances);
        DeleteCriticalSection(&mgr->mutex);
        free(mgr);
        return NULL;
    }
    
    /* Start global monitoring thread */
    mgr->monitor_thread = CreateThread(NULL, 0, MonitorThreadProc, mgr, 0, NULL);
    if (!mgr->monitor_thread) {
        BONDING_LOG_ERROR("openvpn_manager_create: Failed to create monitor thread");
        CloseHandle(mgr->monitor_exit_event);
        free(mgr->instances);
        DeleteCriticalSection(&mgr->mutex);
        free(mgr);
        return NULL;
    }
    
    BONDING_LOG_INFO("openvpn_manager_create: Manager created successfully");
    return mgr;
}

/**
 * @brief Spawn OpenVPN instance
 */
int openvpn_manager_spawn_instance(openvpn_manager_t *mgr, int tunnel_id, const char *config_path, const char *tap_adapter, const char *nic_name, const char *server_host)
{
    openvpn_instance_t *instance = NULL;
    TCHAR exit_event_name[64];
    TCHAR wconfig_path[MAX_PATH];
    TCHAR wconfig_dir[MAX_PATH];
    TCHAR log_path[MAX_PATH];
    char management_password[17];
    HANDLE hStdInRead = NULL, hStdInWrite = NULL;
    HANDLE hNul = NULL;
    DWORD written;
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    SECURITY_DESCRIPTOR sd;
    SECURITY_ATTRIBUTES sa;
    DWORD priority = NORMAL_PRIORITY_CLASS;
    int instance_index = -1;
    TCHAR cmdline[2048];
    TCHAR *cmdline_options;
    int result = -1;
    BOOL process_created = FALSE;
    WCHAR *wpath = NULL;
    
    if (!mgr || tunnel_id < 0 || !config_path || !tap_adapter) {
        BONDING_LOG_ERROR("openvpn_manager_spawn_instance: Invalid parameters");
        return -1;
    }
    
    /* Validate config file exists */
    wpath = Widen(config_path);
    if (!wpath || !CheckFileAccess(wpath, GENERIC_READ)) {
        BONDING_LOG_ERROR("openvpn_manager_spawn_instance: Config file not accessible: %s", config_path);
        if (wpath) free(wpath);
        return -1;
    }
    free(wpath);
    wpath = NULL;
    
    /* Enter critical section */
    EnterCriticalSection(&mgr->mutex);
    
    /* Check if tunnel_id already exists */
    instance_index = FindInstanceIndex(mgr, tunnel_id);
    if (instance_index >= 0) {
        BONDING_LOG_ERROR("openvpn_manager_spawn_instance: Tunnel %d already exists", tunnel_id);
        LeaveCriticalSection(&mgr->mutex);
        return -1;
    }
    
    /* Check if we need to expand array */
    if (mgr->instance_count >= mgr->max_instances) {
        int new_capacity = mgr->max_instances * 2;
        openvpn_instance_t *new_instances = (openvpn_instance_t*)realloc(mgr->instances,
                                                                         new_capacity * sizeof(openvpn_instance_t));
        if (!new_instances) {
            BONDING_LOG_ERROR("openvpn_manager_spawn_instance: Out of memory expanding instances array");
            LeaveCriticalSection(&mgr->mutex);
            return -1;
        }
        mgr->instances = new_instances;
        mgr->max_instances = new_capacity;
        /* Initialize new instances */
        memset(&mgr->instances[mgr->instance_count], 0,
               (new_capacity - mgr->instance_count) * sizeof(openvpn_instance_t));
    }
    
    /* Get instance pointer */
    instance = &mgr->instances[mgr->instance_count];
    instance_index = mgr->instance_count;
    
    /* Initialize instance */
    memset(instance, 0, sizeof(openvpn_instance_t));
    instance->tunnel_id = tunnel_id;
    instance->management_port = mgr->base_management_port + tunnel_id;
    instance->state = TUNNEL_STATE_CONNECTING;
    instance->management_socket = INVALID_SOCKET;
    instance->restart_count = 0;
    instance->route_configured = FALSE;
    
    /* Copy config path */
    wpath = Widen(config_path);
    if (wpath) {
        _tcsncpy_s(instance->config_path, _countof(instance->config_path), wpath, _TRUNCATE);
        free(wpath);
    } else {
        _tcsncpy_s(instance->config_path, _countof(instance->config_path), _T(""), _TRUNCATE);
    }
    
    /* Copy TAP adapter name */
    wpath = Widen(tap_adapter);
    if (wpath) {
        _tcsncpy_s(instance->tap_adapter_name, _countof(instance->tap_adapter_name), wpath, _TRUNCATE);
        free(wpath);
    } else {
        _tcsncpy_s(instance->tap_adapter_name, _countof(instance->tap_adapter_name), _T(""), _TRUNCATE);
    }
    
    /* Store NIC name and server host from parameters (already parsed from bonding config) */
    if (nic_name) {
        strncpy_s(instance->nic_name, _countof(instance->nic_name), nic_name, _TRUNCATE);
    }
    if (server_host) {
        strncpy_s(instance->server_host, _countof(instance->server_host), server_host, _TRUNCATE);
    }
    
    /* Generate exit event name */
    _sntprintf_s(exit_event_name, _countof(exit_event_name), _TRUNCATE,
                 _T("ovpnbond_%x_%d"), GetCurrentProcessId(), tunnel_id);
    
    /* Create exit event */
    instance->exit_event = CreateEvent(NULL, TRUE, FALSE, exit_event_name);
    if (!instance->exit_event) {
        BONDING_LOG_ERROR("openvpn_manager_spawn_instance: Failed to create exit event");
        goto cleanup;
    }
    
    /* Generate random management password */
    if (!GetRandomPassword(management_password, 16)) {
        BONDING_LOG_ERROR("openvpn_manager_spawn_instance: Failed to generate password");
        goto cleanup;
    }
    management_password[16] = '\0';
    
    /* Generate log path */
    _sntprintf_s(log_path, _countof(log_path), _TRUNCATE,
                _T("%%USERPROFILE%%\\OpenVPN\\log\\bonding-tunnel-%d.log"), tunnel_id);
    
    /* Get config directory */
    wpath = Widen(config_path);
    if (wpath) {
        _tcsncpy_s(wconfig_path, _countof(wconfig_path), wpath, _TRUNCATE);
        free(wpath);
    } else {
        _tcsncpy_s(wconfig_path, _countof(wconfig_path), _T(""), _TRUNCATE);
    }
    _tcsncpy_s(wconfig_dir, _countof(wconfig_dir), wconfig_path, _TRUNCATE);
    PathRemoveFileSpec(wconfig_dir);
    
    /* Ensure TAP adapter exists before creating process */
    if (EnsureTAPAdapterExists(tap_adapter) != 0) {
        BONDING_LOG_ERROR("openvpn_manager_spawn_instance: TAP adapter not found or could not be created: %s", tap_adapter);
        goto cleanup;
    }
    
    /* Construct command line */
    cmdline_options = cmdline + 8; /* Skip "openvpn " */
    _sntprintf_s(cmdline, _countof(cmdline), _TRUNCATE,
                _T("openvpn --config \"%ls\" --dev-node \"%ls\" "
                   "--management 127.0.0.1 %d stdin --management-hold "
                   "--management-query-passwords --service %ls 0 "
                   "--log \"%ls\" --setenv IV_GUI_VER \"OpenVPN-GUI-Bonding 1.0\" "
                   "--auth-retry interact"),
                wconfig_path, instance->tap_adapter_name, instance->management_port,
                exit_event_name, log_path);
    
    /* Setup security attributes for inheritable handles */
    if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)) {
        BONDING_LOG_ERROR("openvpn_manager_spawn_instance: Failed to initialize security descriptor");
        goto cleanup;
    }
    if (!SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE)) {
        BONDING_LOG_ERROR("openvpn_manager_spawn_instance: Failed to set security descriptor ACL");
        goto cleanup;
    }
    
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = TRUE;
    
    /* Get NUL device handle */
    hNul = CreateFile(_T("NUL"), GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    if (hNul == INVALID_HANDLE_VALUE) {
        BONDING_LOG_ERROR("openvpn_manager_spawn_instance: Failed to open NUL device");
        goto cleanup;
    }
    
    /* Create STDIN pipe */
    if (!CreatePipe(&hStdInRead, &hStdInWrite, &sa, 0)) {
        BONDING_LOG_ERROR("openvpn_manager_spawn_instance: Failed to create STDIN pipe");
        goto cleanup;
    }
    if (!SetHandleInformation(hStdInWrite, HANDLE_FLAG_INHERIT, 0)) {
        BONDING_LOG_ERROR("openvpn_manager_spawn_instance: Failed to set handle information");
        goto cleanup;
    }
    
    /* Setup STARTUPINFO */
    GetStartupInfo(&si);
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hStdInRead;
    si.hStdOutput = hNul;
    si.hStdError = hNul;
    
    /* Create process */
    if (!CreateProcess(mgr->openvpn_exe_path, cmdline, NULL, NULL, TRUE,
                      priority | CREATE_NO_WINDOW, NULL, wconfig_dir, &si, &pi)) {
        BONDING_LOG_ERROR("openvpn_manager_spawn_instance: Failed to create process (error %lu)", GetLastError());
        goto cleanup;
    }
    
    process_created = TRUE;
    
    /* Store process handle and PID */
    instance->hProcess = pi.hProcess;
    instance->pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    
    /* Write management password to STDIN */
    management_password[16] = '\n';
    if (!WriteFile(hStdInWrite, management_password, 17, &written, NULL)) {
        BONDING_LOG_ERROR("openvpn_manager_spawn_instance: Failed to write password");
        goto cleanup;
    }
    management_password[16] = '\0';
    
    /* Close pipe handles */
    CloseHandle(hStdInRead);
    hStdInRead = NULL;
    CloseHandle(hStdInWrite);
    hStdInWrite = NULL;
    CloseHandle(hNul);
    hNul = NULL;
    
    /* Bind process to NIC if server host and NIC name are available */
    if (server_host && instance->nic_name[0] != '\0') {
        if (BindProcessToNIC(instance, instance->nic_name, server_host) != 0) {
            BONDING_LOG_ERROR("openvpn_manager_spawn_instance: Failed to bind to NIC (continuing anyway)");
        } else {
            /* Update route metric based on current NIC priority */
            int nic_priority = nic_detector_get_priority(instance->nic_name);
            int metric = (11 - nic_priority); /* Convert priority to metric */
            if (metric < 1) metric = 1;
            if (metric > 10) metric = 10;
            
            if (routing_helper_set_metric(instance->nic_name, server_host, metric) != 0) {
                BONDING_LOG_DEBUG("openvpn_manager_spawn_instance: Failed to set route metric (continuing anyway)");
            }
        }
    } else {
        BONDING_LOG_DEBUG("openvpn_manager_spawn_instance: Skipping NIC binding (missing server_host or nic_name)");
    }
    
    /* Store command line */
    _tcsncpy_s(instance->cmdline, _countof(instance->cmdline), cmdline, _TRUNCATE);
    
    /* Add instance to array */
    mgr->instance_count++;
    
    /* Leave critical section */
    LeaveCriticalSection(&mgr->mutex);
    
    BONDING_LOG_INFO("openvpn_manager_spawn_instance: Spawned tunnel %d (PID %lu)", tunnel_id, instance->pid);
    
    return 0;
    
cleanup:
    /* Cleanup on error */
    if (hStdInRead) CloseHandle(hStdInRead);
    if (hStdInWrite) CloseHandle(hStdInWrite);
    if (hNul) CloseHandle(hNul);
    if (instance && instance->exit_event) CloseHandle(instance->exit_event);
    
    /* If process was created but later steps failed, terminate it */
    if (process_created && instance && instance->hProcess) {
        BONDING_LOG_INFO("openvpn_manager_spawn_instance: Terminating orphaned process for tunnel %d", tunnel_id);
        if (instance->exit_event) {
            SetEvent(instance->exit_event);
        }
        DWORD wait_result = WaitForSingleObject(instance->hProcess, PROCESS_TERMINATE_TIMEOUT);
        if (wait_result != WAIT_OBJECT_0) {
            TerminateProcess(instance->hProcess, 1);
        }
        CloseHandle(instance->hProcess);
        instance->hProcess = NULL;
    }
    
    /* Remove routing rules if configured */
    if (instance && instance->route_configured && instance->nic_name[0] != '\0' && instance->server_host[0] != '\0') {
        if (routing_helper_remove_binding(instance->nic_name, instance->server_host) == 0) {
            BONDING_LOG_INFO("openvpn_manager_spawn_instance: Removed route for tunnel %d", tunnel_id);
        }
        instance->route_configured = FALSE;
    }
    
    /* Remove instance from array if it was added */
    if (instance_index >= 0 && instance_index < mgr->instance_count) {
        /* Shift remaining instances */
        int i;
        for (i = instance_index; i < mgr->instance_count - 1; i++) {
            mgr->instances[i] = mgr->instances[i + 1];
        }
        mgr->instance_count--;
    }
    
    LeaveCriticalSection(&mgr->mutex);
    
    return -1;
}

/**
 * @brief Stop OpenVPN instance (assumes mutex is already held)
 */
static int openvpn_manager_stop_instance_locked(openvpn_manager_t *mgr, int tunnel_id)
{
    int instance_index;
    openvpn_instance_t *instance;
    DWORD wait_result;
    
    if (!mgr || tunnel_id < 0) {
        BONDING_LOG_ERROR("openvpn_manager_stop_instance_locked: Invalid parameters");
        return -1;
    }
    
    /* Find instance */
    instance_index = FindInstanceIndex(mgr, tunnel_id);
    if (instance_index < 0) {
        BONDING_LOG_ERROR("openvpn_manager_stop_instance_locked: Tunnel %d not found", tunnel_id);
        return -1;
    }
    
    instance = &mgr->instances[instance_index];
    
    /* Check if already disconnected */
    if (instance->state == TUNNEL_STATE_DISCONNECTED) {
        return 0;
    }
    
    /* Set state to disconnected */
    instance->state = TUNNEL_STATE_DISCONNECTED;
    
    /* Signal graceful shutdown */
    if (instance->exit_event) {
        SetEvent(instance->exit_event);
    }
    
    /* Send SIGTERM via management interface if connected */
    if (instance->management_socket != INVALID_SOCKET) {
        SendManagementCommand(instance, "signal SIGTERM\n", NULL, 0);
    }
    
    /* Wait for process termination */
    if (instance->hProcess) {
        wait_result = WaitForSingleObject(instance->hProcess, PROCESS_TERMINATE_TIMEOUT);
        if (wait_result != WAIT_OBJECT_0) {
            /* Force terminate */
            BONDING_LOG_INFO("openvpn_manager_stop_instance_locked: Force terminating tunnel %d", tunnel_id);
            TerminateProcess(instance->hProcess, 1);
        }
    }
    
    /* Close management socket */
    if (instance->management_socket != INVALID_SOCKET) {
        closesocket(instance->management_socket);
        instance->management_socket = INVALID_SOCKET;
    }
    
    /* Remove routing rules */
    if (instance->route_configured && instance->nic_name[0] != '\0' && instance->server_host[0] != '\0') {
        if (routing_helper_remove_binding(instance->nic_name, instance->server_host) == 0) {
            BONDING_LOG_INFO("openvpn_manager_stop_instance_locked: Removed route for tunnel %d", tunnel_id);
        } else {
            BONDING_LOG_ERROR("openvpn_manager_stop_instance_locked: Failed to remove route for tunnel %d", tunnel_id);
        }
        instance->route_configured = FALSE;
    }
    
    /* Close handles */
    if (instance->hProcess) {
        CloseHandle(instance->hProcess);
        instance->hProcess = NULL;
    }
    if (instance->hThread) {
        CloseHandle(instance->hThread);
        instance->hThread = NULL;
    }
    if (instance->exit_event) {
        CloseHandle(instance->exit_event);
        instance->exit_event = NULL;
    }
    
    /* Remove instance from array (shift remaining elements) */
    int i;
    for (i = instance_index; i < mgr->instance_count - 1; i++) {
        mgr->instances[i] = mgr->instances[i + 1];
    }
    mgr->instance_count--;
    
    BONDING_LOG_INFO("openvpn_manager_stop_instance_locked: Stopped tunnel %d", tunnel_id);
    return 0;
}

/**
 * @brief Stop OpenVPN instance
 */
int openvpn_manager_stop_instance(openvpn_manager_t *mgr, int tunnel_id)
{
    int result;
    
    if (!mgr || tunnel_id < 0) {
        BONDING_LOG_ERROR("openvpn_manager_stop_instance: Invalid parameters");
        return -1;
    }
    
    /* Enter critical section */
    EnterCriticalSection(&mgr->mutex);
    
    result = openvpn_manager_stop_instance_locked(mgr, tunnel_id);
    
    /* Leave critical section */
    LeaveCriticalSection(&mgr->mutex);
    
    return result;
}

/**
 * @brief Get instance status
 */
int openvpn_manager_get_instance_status(openvpn_manager_t *mgr, int tunnel_id, tunnel_state_t *state)
{
    int instance_index;
    openvpn_instance_t *instance;
    
    if (!mgr || tunnel_id < 0 || !state) {
        BONDING_LOG_ERROR("openvpn_manager_get_instance_status: Invalid parameters");
        return -1;
    }
    
    /* Enter critical section */
    EnterCriticalSection(&mgr->mutex);
    
    /* Find instance */
    instance_index = FindInstanceIndex(mgr, tunnel_id);
    if (instance_index < 0) {
        LeaveCriticalSection(&mgr->mutex);
        return -1;
    }
    
    instance = &mgr->instances[instance_index];
    
    /* Copy state */
    *state = instance->state;
    
    /* Leave critical section */
    LeaveCriticalSection(&mgr->mutex);
    
    return 0;
}

/**
 * @brief Destroy OpenVPN manager
 */
void openvpn_manager_destroy(openvpn_manager_t *mgr)
{
    int i;
    
    if (!mgr)
        return;
    
    /* Signal monitoring thread to exit */
    if (mgr->monitor_exit_event) {
        SetEvent(mgr->monitor_exit_event);
        
        /* Wait for monitoring thread termination */
        if (mgr->monitor_thread) {
            WaitForSingleObject(mgr->monitor_thread, 5000);
            CloseHandle(mgr->monitor_thread);
        }
        
        CloseHandle(mgr->monitor_exit_event);
    }
    
    /* Stop all instances */
    EnterCriticalSection(&mgr->mutex);
    
    for (i = mgr->instance_count - 1; i >= 0; i--) {
        openvpn_manager_stop_instance_locked(mgr, mgr->instances[i].tunnel_id);
    }
    
    LeaveCriticalSection(&mgr->mutex);
    
    /* Restore all routes */
    routing_helper_restore_routes();
    
    /* Free instances array */
    if (mgr->instances) {
        free(mgr->instances);
    }
    
    /* Delete critical section */
    DeleteCriticalSection(&mgr->mutex);
    
    /* Free manager structure */
    free(mgr);
    
    BONDING_LOG_INFO("openvpn_manager_destroy: Manager destroyed");
}
