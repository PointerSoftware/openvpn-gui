/**
 * @file packet_distributor.c
 * @brief Packet distribution implementation
 * 
 * Implements user-space packet forwarding engine for Windows TAP bonding
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <setupapi.h>
#include <winioctl.h>
#include <tchar.h>
#include <shlwapi.h>

#include "packet_distributor.h"
#include "bonding_types.h"
#include "bonding_config.h"

/* External logging function */
extern void bonding_log(int level, const char *format, ...);

/* Helper function to duplicate string */
static char* duplicate_string(const char *str)
{
    char *dup;
    size_t len;
    
    if (!str)
        return NULL;
    
    len = strlen(str) + 1;
    dup = (char*)calloc(len, 1);
    if (!dup)
        return NULL;
    
    memcpy(dup, str, len - 1);
    return dup;
}

/* TAP-Windows driver GUID */
#define TAP_COMPONENT_ID "tap0901"
#define TAP_GUID_PREFIX "\\\\.\\Global\\"

/* Bonding header structure */
#define BONDING_HEADER_MAGIC 0xB0ND
#define BONDING_HEADER_SIZE 20

typedef struct {
    uint32_t magic;          /* Magic number: 0xB0ND */
    uint32_t sequence;       /* Sequence number */
    uint32_t tunnel_id;      /* Tunnel ID */
    uint32_t timestamp;      /* Timestamp (seconds since epoch) */
    uint32_t packet_length;  /* Original packet length */
    uint32_t checksum;       /* Header checksum */
} bonding_header_t;

/* Packet buffer structure */
typedef struct packet_buffer {
    uint8_t *data;
    size_t length;
    time_t timestamp;
    uint32_t sequence;
    struct packet_buffer *next;
} packet_buffer_t;

/* Packet queue structure */
typedef struct packet_queue {
    packet_buffer_t *head;
    packet_buffer_t *tail;
    size_t current_size;
    size_t max_size;
    CRITICAL_SECTION mutex;
    CONDITION_VARIABLE not_empty;
    CONDITION_VARIABLE not_full;
} packet_queue_t;

/* Tunnel health structure */
typedef struct tunnel_health {
    int healthy;                 /* 1 if healthy, 0 if unhealthy */
    time_t last_heartbeat;       /* Last heartbeat timestamp */
    int consecutive_failures;    /* Consecutive failure count */
    uint64_t bandwidth_bytes;    /* Bandwidth estimation (bytes) */
    time_t bandwidth_timestamp;  /* Last bandwidth measurement */
} tunnel_health_t;

/* Packet distributor structure */
struct packet_distributor {
    bonding_mode_t mode;                 /* Bonding mode */
    int tunnel_count;                    /* Number of tunnels */
    tunnel_health_t *tunnel_health;      /* Per-tunnel health status */
    int *tunnel_weights;                 /* Per-tunnel weights */
    int *tunnel_weight_counters;         /* Current weight counters for weighted RR */
    int current_rr_index;                /* Current round-robin index */
    uint32_t sequence_counter;           /* Global sequence counter */
    
    /* Statistics */
    uint64_t *packets_sent;              /* Packets sent per tunnel */
    uint64_t *bytes_sent;                 /* Bytes sent per tunnel */
    uint64_t *errors;                    /* Error count per tunnel */
    
    /* TAP adapter handles */
    HANDLE master_tap_handle;             /* Master bonding TAP adapter */
    HANDLE *tunnel_tap_handles;           /* Array of tunnel TAP adapter handles */
    char **tunnel_tap_names;              /* Array of tunnel TAP adapter names */
    
    /* Packet queue */
    packet_queue_t *packet_queue;
    
    /* Threading */
    HANDLE forwarding_thread;            /* Forwarding thread handle */
    HANDLE shutdown_event;                /* Shutdown event */
    CRITICAL_SECTION mutex;               /* Main mutex for thread safety */
    
    /* Overlapped I/O */
    OVERLAPPED master_read_overlapped;    /* Overlapped structure for master TAP read */
    HANDLE master_read_event;             /* Event for master TAP read completion */
    
    /* Configuration */
    int queue_size;                       /* Maximum queue size */
    int io_timeout;                       /* I/O timeout in milliseconds */
    int sequencing_enabled;               /* Enable packet sequencing */
    int flow_control_enabled;             /* Enable flow control */
};

/* Forward declarations */
static int create_master_tap_adapter(struct packet_distributor *dist);
static int open_tap_adapter(const char *adapter_name, HANDLE *handle);
static int read_packet_from_tap(HANDLE handle, uint8_t *buffer, size_t buffer_size, size_t *bytes_read, OVERLAPPED *overlapped);
static int write_packet_to_tap(HANDLE handle, const uint8_t *buffer, size_t length, OVERLAPPED *overlapped);
static int select_tunnel_round_robin(struct packet_distributor *dist);
static int select_tunnel_weighted(struct packet_distributor *dist);
static int add_bonding_header(struct packet_distributor *dist, const uint8_t *packet, size_t packet_len, uint8_t *output, size_t *output_len, int tunnel_id);
static packet_queue_t* packet_queue_create(size_t max_size);
static int packet_queue_enqueue(packet_queue_t *queue, const uint8_t *data, size_t length, uint32_t sequence);
static int packet_queue_dequeue(packet_queue_t *queue, uint8_t *data, size_t *length, uint32_t *sequence, DWORD timeout_ms);
static void packet_queue_destroy(packet_queue_t *queue);
static DWORD WINAPI forwarding_thread_proc(LPVOID lpParam);
static int start_forwarding_thread(struct packet_distributor *dist);
static int stop_forwarding_thread(struct packet_distributor *dist);

/* Logging macros */
#define BONDING_LOG_ERROR(...) bonding_log(3, __VA_ARGS__)
#define BONDING_LOG_WARN(...) bonding_log(2, __VA_ARGS__)
#define BONDING_LOG_INFO(...) bonding_log(1, __VA_ARGS__)
#define BONDING_LOG_DEBUG(...) bonding_log(0, __VA_ARGS__)

/**
 * @brief Create master TAP adapter
 */
static int create_master_tap_adapter(struct packet_distributor *dist)
{
    TCHAR addtap_path[MAX_PATH];
    TCHAR cmdline[MAX_PATH * 2];
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    DWORD exit_code;
    HKEY regkey;
    HANDLE handle = INVALID_HANDLE_VALUE;
    PIP_ADAPTER_INFO adapter_list = NULL;
    PIP_ADAPTER_INFO adapter = NULL;
    ULONG adapter_list_size = 0;
    DWORD status;
    char device_path[256];
    GUID tap_guid;
    char guid_str[64];
    int i;
    
    if (!dist)
        return -1;
    
    /* Check if master TAP adapter already exists */
    status = GetAdaptersInfo(NULL, &adapter_list_size);
    if (status == ERROR_BUFFER_OVERFLOW) {
        adapter_list = (PIP_ADAPTER_INFO)malloc(adapter_list_size);
        if (adapter_list) {
            status = GetAdaptersInfo(adapter_list, &adapter_list_size);
            if (status == ERROR_SUCCESS) {
                adapter = adapter_list;
                while (adapter) {
                    if (strstr(adapter->Description, "TAP") != NULL ||
                        strstr(adapter->Description, "TAP-Windows") != NULL) {
                        if (strstr(adapter->Description, "Bonding-Master") != NULL ||
                            strcmp(adapter->AdapterName, "TAP-Bonding-Master") == 0) {
                            /* Found existing master adapter */
                            BONDING_LOG_INFO("Master TAP adapter already exists: %s", adapter->AdapterName);
                            free(adapter_list);
                            
                            /* Try to open it */
                            if (open_tap_adapter("TAP-Bonding-Master", &dist->master_tap_handle) == 0) {
                                return 0;
                            }
                            break;
                        }
                    }
                    adapter = adapter->Next;
                }
            }
            free(adapter_list);
        }
    }
    
    /* Master adapter not found - attempt to create it */
    BONDING_LOG_INFO("Creating master TAP adapter: TAP-Bonding-Master");
    
    /* Try to find addtap.bat in OpenVPN installation */
    addtap_path[0] = '\0';
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
    
    /* Run addtap.bat to create TAP adapter */
    _sntprintf_s(cmdline, _countof(cmdline), _TRUNCATE, _T("cmd.exe /c \"%ls\""), addtap_path);
    
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    
    memset(&pi, 0, sizeof(pi));
    
    if (!CreateProcess(NULL, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        BONDING_LOG_ERROR("Failed to run addtap.bat (error %lu). Admin privileges may be required.", GetLastError());
        return -1;
    }
    
    /* Wait for process to complete */
    WaitForSingleObject(pi.hProcess, 30000);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    
    if (exit_code != 0) {
        BONDING_LOG_ERROR("addtap.bat exited with code %lu", exit_code);
        return -1;
    }
    
    /* Wait for adapter to be registered */
    Sleep(2000);
    
    /* Try to open the master adapter */
    if (open_tap_adapter("TAP-Bonding-Master", &dist->master_tap_handle) != 0) {
        BONDING_LOG_ERROR("Failed to open master TAP adapter after creation");
        return -1;
    }
    
    BONDING_LOG_INFO("Successfully created and opened master TAP adapter");
    return 0;
}

/**
 * @brief Open TAP adapter by name
 */
static int open_tap_adapter(const char *adapter_name, HANDLE *handle)
{
    HDEVINFO device_info_set;
    SP_DEVICE_INTERFACE_DATA device_interface_data;
    PSP_DEVICE_INTERFACE_DETAIL_DATA device_interface_detail_data;
    DWORD required_size;
    GUID tap_guid;
    char device_path[256];
    int i;
    
    if (!adapter_name || !handle)
        return -1;
    
    /* Initialize TAP GUID (this is a simplified approach - in production, enumerate all TAP adapters) */
    /* For now, try common TAP adapter paths */
    const char *tap_paths[] = {
        "\\\\.\\Global\\{8E0F1E0E-4F1A-4B2C-8E0F-1E0E4F1A4B2C}.tap",
        "\\\\.\\Global\\{00000000-0000-0000-0000-000000000000}.tap"
    };
    
    /* Try to find TAP adapter by enumerating network adapters */
    PIP_ADAPTER_INFO adapter_list = NULL;
    PIP_ADAPTER_INFO adapter = NULL;
    ULONG adapter_list_size = 0;
    DWORD status;
    
    status = GetAdaptersInfo(NULL, &adapter_list_size);
    if (status == ERROR_BUFFER_OVERFLOW) {
        adapter_list = (PIP_ADAPTER_INFO)malloc(adapter_list_size);
        if (adapter_list) {
            status = GetAdaptersInfo(adapter_list, &adapter_list_size);
            if (status == ERROR_SUCCESS) {
                adapter = adapter_list;
                while (adapter) {
                    if (strstr(adapter->Description, "TAP") != NULL ||
                        strstr(adapter->Description, "TAP-Windows") != NULL) {
                        if (strcmp(adapter->AdapterName, adapter_name) == 0 ||
                            strstr(adapter->Description, adapter_name) != NULL) {
                            /* Found matching adapter - construct device path */
                            /* Note: This is simplified - in production, use SetupAPI to get exact GUID */
                            snprintf(device_path, sizeof(device_path), "\\\\.\\Global\\%s.tap", adapter->AdapterName);
                            
                            /* Try to open device */
                            *handle = CreateFileA(device_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
                            if (*handle != INVALID_HANDLE_VALUE) {
                                free(adapter_list);
                                BONDING_LOG_INFO("Opened TAP adapter: %s", adapter_name);
                                return 0;
                            }
                        }
                    }
                    adapter = adapter->Next;
                }
            }
            free(adapter_list);
        }
    }
    
    /* Fallback: try direct path construction */
    snprintf(device_path, sizeof(device_path), "\\\\.\\Global\\%s.tap", adapter_name);
    *handle = CreateFileA(device_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (*handle != INVALID_HANDLE_VALUE) {
        BONDING_LOG_INFO("Opened TAP adapter via direct path: %s", adapter_name);
        return 0;
    }
    
    BONDING_LOG_ERROR("Failed to open TAP adapter: %s (error %lu)", adapter_name, GetLastError());
    return -1;
}

/**
 * @brief Read packet from TAP adapter
 */
static int read_packet_from_tap(HANDLE handle, uint8_t *buffer, size_t buffer_size, size_t *bytes_read, OVERLAPPED *overlapped)
{
    DWORD bytes_read_dw = 0;
    BOOL result;
    DWORD wait_result;
    
    if (!handle || handle == INVALID_HANDLE_VALUE || !buffer || !bytes_read || !overlapped)
        return -1;
    
    /* Issue asynchronous read */
    result = ReadFile(handle, buffer, (DWORD)buffer_size, NULL, overlapped);
    
    if (!result) {
        DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            /* I/O is pending - wait for completion */
            wait_result = WaitForSingleObject(overlapped->hEvent, 100); /* 100ms timeout */
            if (wait_result == WAIT_OBJECT_0) {
                /* I/O completed */
                result = GetOverlappedResult(handle, overlapped, &bytes_read_dw, FALSE);
                if (result) {
                    *bytes_read = bytes_read_dw;
                    return 0;
                }
            } else if (wait_result == WAIT_TIMEOUT) {
                /* Timeout - cancel I/O */
                CancelIo(handle);
                return -1; /* Timeout */
            }
        }
        return -1;
    } else {
        /* I/O completed immediately */
        result = GetOverlappedResult(handle, overlapped, &bytes_read_dw, FALSE);
        if (result) {
            *bytes_read = bytes_read_dw;
            return 0;
        }
    }
    
    return -1;
}

/**
 * @brief Write packet to TAP adapter
 */
static int write_packet_to_tap(HANDLE handle, const uint8_t *buffer, size_t length, OVERLAPPED *overlapped)
{
    DWORD bytes_written = 0;
    BOOL result;
    
    if (!handle || handle == INVALID_HANDLE_VALUE || !buffer || !overlapped)
        return -1;
    
    /* Issue asynchronous write */
    result = WriteFile(handle, buffer, (DWORD)length, NULL, overlapped);
    
    if (!result) {
        DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            /* I/O is pending - wait for completion with timeout */
            DWORD wait_result = WaitForSingleObject(overlapped->hEvent, 100);
            if (wait_result == WAIT_OBJECT_0) {
                result = GetOverlappedResult(handle, overlapped, &bytes_written, FALSE);
                if (result && bytes_written == length) {
                    return 0;
                }
            } else {
                CancelIo(handle);
                return -1;
            }
        }
        return -1;
    } else {
        /* I/O completed immediately */
        result = GetOverlappedResult(handle, overlapped, &bytes_written, FALSE);
        if (result && bytes_written == length) {
            return 0;
        }
    }
    
    return -1;
}

/**
 * @brief Select tunnel using round-robin algorithm
 */
static int select_tunnel_round_robin(struct packet_distributor *dist)
{
    int start_index;
    int attempts = 0;
    
    if (!dist || dist->tunnel_count <= 0)
        return -1;
    
    start_index = dist->current_rr_index;
    
    /* Find next healthy tunnel */
    do {
        if (dist->tunnel_health[dist->current_rr_index].healthy) {
            int selected = dist->current_rr_index;
            dist->current_rr_index = (dist->current_rr_index + 1) % dist->tunnel_count;
            return selected;
        }
        
        dist->current_rr_index = (dist->current_rr_index + 1) % dist->tunnel_count;
        attempts++;
    } while (dist->current_rr_index != start_index && attempts < dist->tunnel_count);
    
    /* No healthy tunnels found */
    return -1;
}

/**
 * @brief Select tunnel using weighted round-robin algorithm
 */
static int select_tunnel_weighted(struct packet_distributor *dist)
{
    int i;
    int max_weight_index = -1;
    int max_weight = 0;
    
    if (!dist || dist->tunnel_count <= 0)
        return -1;
    
    /* Find tunnel with highest remaining weight */
    for (i = 0; i < dist->tunnel_count; i++) {
        if (dist->tunnel_health[i].healthy && 
            dist->tunnel_weight_counters[i] > max_weight) {
            max_weight = dist->tunnel_weight_counters[i];
            max_weight_index = i;
        }
    }
    
    if (max_weight_index < 0) {
        /* No healthy tunnels */
        return -1;
    }
    
    /* Decrement weight counter */
    dist->tunnel_weight_counters[max_weight_index]--;
    
    /* Check if all counters are zero - reset if so */
    int all_zero = 1;
    for (i = 0; i < dist->tunnel_count; i++) {
        if (dist->tunnel_health[i].healthy && dist->tunnel_weight_counters[i] > 0) {
            all_zero = 0;
            break;
        }
    }
    
    if (all_zero) {
        /* Reset all weight counters */
        for (i = 0; i < dist->tunnel_count; i++) {
            if (dist->tunnel_health[i].healthy) {
                dist->tunnel_weight_counters[i] = dist->tunnel_weights[i];
            }
        }
    }
    
    return max_weight_index;
}

/**
 * @brief Add bonding header to packet
 */
static int add_bonding_header(struct packet_distributor *dist, const uint8_t *packet, size_t packet_len, uint8_t *output, size_t *output_len, int tunnel_id)
{
    bonding_header_t header;
    uint32_t checksum = 0;
    int i;
    
    if (!dist || !packet || !output || !output_len || *output_len < packet_len + BONDING_HEADER_SIZE)
        return -1;
    
    /* Increment sequence counter atomically */
    dist->sequence_counter = InterlockedIncrement((LONG*)&dist->sequence_counter);
    
    /* Build header */
    header.magic = htonl(BONDING_HEADER_MAGIC);
    header.sequence = htonl(dist->sequence_counter);
    header.tunnel_id = htonl((uint32_t)tunnel_id);
    header.timestamp = htonl((uint32_t)time(NULL));
    header.packet_length = htonl((uint32_t)packet_len);
    
    /* Calculate simple XOR checksum */
    checksum = 0;
    for (i = 0; i < (int)sizeof(bonding_header_t) - sizeof(uint32_t); i++) {
        checksum ^= ((uint8_t*)&header)[i];
    }
    header.checksum = htonl(checksum);
    
    /* Copy header and packet to output */
    memcpy(output, &header, BONDING_HEADER_SIZE);
    memcpy(output + BONDING_HEADER_SIZE, packet, packet_len);
    
    *output_len = packet_len + BONDING_HEADER_SIZE;
    
    return 0;
}

/**
 * @brief Create packet queue
 */
static packet_queue_t* packet_queue_create(size_t max_size)
{
    packet_queue_t *queue;
    
    queue = (packet_queue_t*)calloc(1, sizeof(packet_queue_t));
    if (!queue)
        return NULL;
    
    queue->max_size = max_size;
    queue->current_size = 0;
    queue->head = NULL;
    queue->tail = NULL;
    
    InitializeCriticalSection(&queue->mutex);
    InitializeConditionVariable(&queue->not_empty);
    InitializeConditionVariable(&queue->not_full);
    
    return queue;
}

/**
 * @brief Enqueue packet
 */
static int packet_queue_enqueue(packet_queue_t *queue, const uint8_t *data, size_t length, uint32_t sequence)
{
    packet_buffer_t *buffer;
    
    if (!queue || !data || length == 0)
        return -1;
    
    EnterCriticalSection(&queue->mutex);
    
    /* Check if queue is full */
    if (queue->current_size >= queue->max_size) {
        /* Drop oldest packet (tail-drop policy) */
        if (queue->tail) {
            packet_buffer_t *old_tail = queue->tail;
            if (queue->head == queue->tail) {
                queue->head = NULL;
                queue->tail = NULL;
            } else {
                packet_buffer_t *prev = queue->head;
                while (prev && prev->next != queue->tail) {
                    prev = prev->next;
                }
                queue->tail = prev;
                if (queue->tail) {
                    queue->tail->next = NULL;
                }
            }
            if (old_tail->data) {
                free(old_tail->data);
            }
            free(old_tail);
            queue->current_size--;
        }
    }
    
    /* Allocate new buffer */
    buffer = (packet_buffer_t*)calloc(1, sizeof(packet_buffer_t));
    if (!buffer) {
        LeaveCriticalSection(&queue->mutex);
        return -1;
    }
    
    buffer->data = (uint8_t*)malloc(length);
    if (!buffer->data) {
        free(buffer);
        LeaveCriticalSection(&queue->mutex);
        return -1;
    }
    
    memcpy(buffer->data, data, length);
    buffer->length = length;
    buffer->sequence = sequence;
    buffer->timestamp = time(NULL);
    buffer->next = NULL;
    
    /* Add to queue */
    if (!queue->head) {
        queue->head = buffer;
        queue->tail = buffer;
    } else {
        queue->head->next = buffer;
        queue->head = buffer;
    }
    
    queue->current_size++;
    
    /* Signal waiting threads */
    WakeConditionVariable(&queue->not_empty);
    
    LeaveCriticalSection(&queue->mutex);
    
    return 0;
}

/**
 * @brief Dequeue packet
 */
static int packet_queue_dequeue(packet_queue_t *queue, uint8_t *data, size_t *length, uint32_t *sequence, DWORD timeout_ms)
{
    packet_buffer_t *buffer;
    DWORD wait_result;
    
    if (!queue || !data || !length || !sequence)
        return -1;
    
    EnterCriticalSection(&queue->mutex);
    
    /* Wait for packet if queue is empty */
    while (queue->current_size == 0) {
        wait_result = SleepConditionVariableCS(&queue->not_empty, &queue->mutex, timeout_ms);
        if (wait_result == 0) {
            /* Timeout */
            LeaveCriticalSection(&queue->mutex);
            return -1;
        }
    }
    
    /* Remove from tail */
    buffer = queue->tail;
    if (!buffer) {
        LeaveCriticalSection(&queue->mutex);
        return -1;
    }
    
    if (queue->head == queue->tail) {
        queue->head = NULL;
        queue->tail = NULL;
    } else {
        queue->tail = buffer->next;
    }
    
    queue->current_size--;
    
    /* Copy data */
    if (*length >= buffer->length) {
        memcpy(data, buffer->data, buffer->length);
        *length = buffer->length;
        *sequence = buffer->sequence;
    } else {
        /* Buffer too small */
        *length = buffer->length;
        LeaveCriticalSection(&queue->mutex);
        return -1;
    }
    
    /* Free buffer */
    if (buffer->data) {
        free(buffer->data);
    }
    free(buffer);
    
    /* Signal waiting threads */
    WakeConditionVariable(&queue->not_full);
    
    LeaveCriticalSection(&queue->mutex);
    
    return 0;
}

/**
 * @brief Destroy packet queue
 */
static void packet_queue_destroy(packet_queue_t *queue)
{
    packet_buffer_t *buffer;
    
    if (!queue)
        return;
    
    EnterCriticalSection(&queue->mutex);
    
    /* Free all buffers */
    while (queue->tail) {
        buffer = queue->tail;
        queue->tail = buffer->next;
        
        if (buffer->data) {
            free(buffer->data);
        }
        free(buffer);
    }
    
    LeaveCriticalSection(&queue->mutex);
    
    DeleteCriticalSection(&queue->mutex);
    free(queue);
}

/**
 * @brief Forwarding thread procedure
 */
static DWORD WINAPI forwarding_thread_proc(LPVOID lpParam)
{
    struct packet_distributor *dist = (struct packet_distributor*)lpParam;
    uint8_t read_buffer[2048];
    uint8_t write_buffer[2048 + BONDING_HEADER_SIZE];
    size_t bytes_read;
    size_t write_length;
    int selected_tunnel;
    HANDLE wait_handles[2];
    DWORD wait_result;
    int retry_count;
    
    if (!dist)
        return 1;
    
    wait_handles[0] = dist->shutdown_event;
    wait_handles[1] = dist->master_read_event;
    
    BONDING_LOG_INFO("Forwarding thread started");
    
    /* Main processing loop */
    while (1) {
        /* Check for shutdown */
        if (WaitForSingleObject(dist->shutdown_event, 0) == WAIT_OBJECT_0) {
            break;
        }
        
        /* Read packet from master TAP adapter */
        bytes_read = 0;
        if (read_packet_from_tap(dist->master_tap_handle, read_buffer, sizeof(read_buffer), 
                                 &bytes_read, &dist->master_read_overlapped) == 0 && bytes_read > 0) {
            
            /* Validate packet size (minimum Ethernet frame) */
            if (bytes_read < 14) {
                BONDING_LOG_DEBUG("Packet too small: %zu bytes", bytes_read);
                continue;
            }
            
            /* Select target tunnel */
            EnterCriticalSection(&dist->mutex);
            
            if (dist->mode == BONDING_MODE_WEIGHTED || dist->mode == BONDING_MODE_ADAPTIVE) {
                selected_tunnel = select_tunnel_weighted(dist);
            } else {
                selected_tunnel = select_tunnel_round_robin(dist);
            }
            
            if (selected_tunnel < 0 || selected_tunnel >= dist->tunnel_count) {
                BONDING_LOG_WARN("No healthy tunnel available for packet forwarding");
                LeaveCriticalSection(&dist->mutex);
                continue;
            }
            
            /* Add sequencing header if enabled */
            write_length = sizeof(write_buffer);
            if (dist->sequencing_enabled) {
                if (add_bonding_header(dist, read_buffer, bytes_read, write_buffer, &write_length, selected_tunnel) != 0) {
                    BONDING_LOG_ERROR("Failed to add bonding header");
                    LeaveCriticalSection(&dist->mutex);
                    continue;
                }
            } else {
                memcpy(write_buffer, read_buffer, bytes_read);
                write_length = bytes_read;
            }
            
            /* Update statistics */
            dist->packets_sent[selected_tunnel]++;
            dist->bytes_sent[selected_tunnel] += write_length;
            
            LeaveCriticalSection(&dist->mutex);
            
            /* Write packet to selected tunnel's TAP adapter */
            if (dist->tunnel_tap_handles[selected_tunnel] != INVALID_HANDLE_VALUE) {
                OVERLAPPED write_overlapped = {0};
                write_overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
                
                retry_count = 0;
                while (retry_count < 3) {
                    if (write_packet_to_tap(dist->tunnel_tap_handles[selected_tunnel], 
                                           write_buffer, write_length, &write_overlapped) == 0) {
                        break; /* Success */
                    }
                    
                    retry_count++;
                    if (retry_count < 3) {
                        Sleep(10); /* Brief delay before retry */
                    }
                }
                
                if (write_overlapped.hEvent) {
                    CloseHandle(write_overlapped.hEvent);
                }
                
                if (retry_count >= 3) {
                    /* Mark tunnel as unhealthy after persistent errors */
                    EnterCriticalSection(&dist->mutex);
                    dist->tunnel_health[selected_tunnel].consecutive_failures++;
                    if (dist->tunnel_health[selected_tunnel].consecutive_failures >= 3) {
                        dist->tunnel_health[selected_tunnel].healthy = 0;
                        BONDING_LOG_WARN("Tunnel %d marked as unhealthy after %d consecutive failures",
                                        selected_tunnel, dist->tunnel_health[selected_tunnel].consecutive_failures);
                    }
                    dist->errors[selected_tunnel]++;
                    LeaveCriticalSection(&dist->mutex);
                } else {
                    /* Reset failure count on success */
                    EnterCriticalSection(&dist->mutex);
                    dist->tunnel_health[selected_tunnel].consecutive_failures = 0;
                    LeaveCriticalSection(&dist->mutex);
                }
            } else {
                BONDING_LOG_ERROR("Tunnel %d TAP handle is invalid", selected_tunnel);
            }
        } else {
            /* Read failed or timeout - check for shutdown */
            Sleep(10); /* Brief sleep to avoid busy-waiting */
        }
    }
    
    BONDING_LOG_INFO("Forwarding thread stopped");
    return 0;
}

/**
 * @brief Start forwarding thread
 */
static int start_forwarding_thread(struct packet_distributor *dist)
{
    if (!dist)
        return -1;
    
    /* Initialize overlapped structure for master TAP read */
    memset(&dist->master_read_overlapped, 0, sizeof(dist->master_read_overlapped));
    dist->master_read_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!dist->master_read_event) {
        BONDING_LOG_ERROR("Failed to create master read event");
        return -1;
    }
    dist->master_read_overlapped.hEvent = dist->master_read_event;
    
    /* Create shutdown event */
    dist->shutdown_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!dist->shutdown_event) {
        BONDING_LOG_ERROR("Failed to create shutdown event");
        CloseHandle(dist->master_read_event);
        return -1;
    }
    
    /* Create forwarding thread */
    dist->forwarding_thread = CreateThread(NULL, 0, forwarding_thread_proc, dist, 0, NULL);
    if (!dist->forwarding_thread) {
        BONDING_LOG_ERROR("Failed to create forwarding thread");
        CloseHandle(dist->shutdown_event);
        CloseHandle(dist->master_read_event);
        return -1;
    }
    
    /* Set thread priority */
    SetThreadPriority(dist->forwarding_thread, THREAD_PRIORITY_ABOVE_NORMAL);
    
    BONDING_LOG_INFO("Forwarding thread started");
    return 0;
}

/**
 * @brief Stop forwarding thread
 */
static int stop_forwarding_thread(struct packet_distributor *dist)
{
    DWORD wait_result;
    
    if (!dist)
        return -1;
    
    if (dist->shutdown_event) {
        /* Signal shutdown */
        SetEvent(dist->shutdown_event);
        
        /* Cancel pending I/O */
        if (dist->master_tap_handle != INVALID_HANDLE_VALUE) {
            CancelIo(dist->master_tap_handle);
        }
        
        /* Wait for thread termination */
        if (dist->forwarding_thread) {
            wait_result = WaitForSingleObject(dist->forwarding_thread, 5000);
            if (wait_result != WAIT_OBJECT_0) {
                BONDING_LOG_WARN("Forwarding thread did not terminate gracefully");
                TerminateThread(dist->forwarding_thread, 1);
            }
            CloseHandle(dist->forwarding_thread);
            dist->forwarding_thread = NULL;
        }
        
        CloseHandle(dist->shutdown_event);
        dist->shutdown_event = NULL;
    }
    
    if (dist->master_read_event) {
        CloseHandle(dist->master_read_event);
        dist->master_read_event = NULL;
    }
    
    BONDING_LOG_INFO("Forwarding thread stopped");
    return 0;
}

/**
 * @brief Create packet distributor
 */
packet_distributor_t* packet_distributor_create(bonding_mode_t mode, int tunnel_count)
{
    struct packet_distributor *dist;
    int i;
    
    if (tunnel_count <= 0 || tunnel_count > MAX_TUNNEL_COUNT)
        return NULL;
    
    /* Allocate distributor structure */
    dist = (struct packet_distributor*)calloc(1, sizeof(struct packet_distributor));
    if (!dist) {
        BONDING_LOG_ERROR("Out of memory allocating packet distributor");
        return NULL;
    }
    
    /* Initialize basic fields */
    dist->mode = mode;
    dist->tunnel_count = tunnel_count;
    dist->current_rr_index = 0;
    dist->sequence_counter = 0;
    
    /* Allocate arrays */
    dist->tunnel_health = (tunnel_health_t*)calloc(tunnel_count, sizeof(tunnel_health_t));
    dist->tunnel_weights = (int*)calloc(tunnel_count, sizeof(int));
    dist->tunnel_weight_counters = (int*)calloc(tunnel_count, sizeof(int));
    dist->packets_sent = (uint64_t*)calloc(tunnel_count, sizeof(uint64_t));
    dist->bytes_sent = (uint64_t*)calloc(tunnel_count, sizeof(uint64_t));
    dist->errors = (uint64_t*)calloc(tunnel_count, sizeof(uint64_t));
    dist->tunnel_tap_handles = (HANDLE*)calloc(tunnel_count, sizeof(HANDLE));
    dist->tunnel_tap_names = (char**)calloc(tunnel_count, sizeof(char*));
    
    if (!dist->tunnel_health || !dist->tunnel_weights || !dist->tunnel_weight_counters ||
        !dist->packets_sent || !dist->bytes_sent || !dist->errors ||
        !dist->tunnel_tap_handles || !dist->tunnel_tap_names) {
        BONDING_LOG_ERROR("Out of memory allocating tunnel arrays");
        packet_distributor_destroy((packet_distributor_t*)dist);
        return NULL;
    }
    
    /* Initialize tunnel health and weights */
    for (i = 0; i < tunnel_count; i++) {
        dist->tunnel_health[i].healthy = 1;
        dist->tunnel_health[i].last_heartbeat = time(NULL);
        dist->tunnel_health[i].consecutive_failures = 0;
        dist->tunnel_weights[i] = 1; /* Default weight */
        dist->tunnel_weight_counters[i] = 1;
        dist->tunnel_tap_handles[i] = INVALID_HANDLE_VALUE;
    }
    
    /* Initialize synchronization primitives */
    InitializeCriticalSection(&dist->mutex);
    
    /* Initialize configuration defaults */
    dist->queue_size = 1000;
    dist->io_timeout = 100;
    dist->sequencing_enabled = 1;
    dist->flow_control_enabled = 1;
    
    /* Create packet queue */
    dist->packet_queue = packet_queue_create(dist->queue_size);
    if (!dist->packet_queue) {
        BONDING_LOG_ERROR("Failed to create packet queue");
        packet_distributor_destroy((packet_distributor_t*)dist);
        return NULL;
    }
    
    /* Create master TAP adapter */
    if (create_master_tap_adapter(dist) != 0) {
        BONDING_LOG_ERROR("Failed to create master TAP adapter");
        packet_distributor_destroy((packet_distributor_t*)dist);
        return NULL;
    }
    
    /* Note: Tunnel TAP adapters should be opened by caller after OpenVPN manager creates them */
    /* For now, we'll open them when set_tunnel_health is called with adapter names */
    
    /* Start forwarding thread */
    if (start_forwarding_thread(dist) != 0) {
        BONDING_LOG_ERROR("Failed to start forwarding thread");
        packet_distributor_destroy((packet_distributor_t*)dist);
        return NULL;
    }
    
    BONDING_LOG_INFO("Packet distributor created successfully (mode=%d, tunnels=%d)", mode, tunnel_count);
    return (packet_distributor_t*)dist;
}

/**
 * @brief Select tunnel for packet forwarding
 */
int packet_distributor_select_tunnel(packet_distributor_t *dist, int *tunnel_id)
{
    struct packet_distributor *d = (struct packet_distributor*)dist;
    int selected;
    
    if (!d || !tunnel_id)
        return -1;
    
    EnterCriticalSection(&d->mutex);
    
    if (d->mode == BONDING_MODE_WEIGHTED || d->mode == BONDING_MODE_ADAPTIVE) {
        selected = select_tunnel_weighted(d);
    } else {
        selected = select_tunnel_round_robin(d);
    }
    
    if (selected >= 0) {
        *tunnel_id = selected;
        d->packets_sent[selected]++; /* Update statistics */
    }
    
    LeaveCriticalSection(&d->mutex);
    
    return (selected >= 0) ? 0 : -1;
}

/**
 * @brief Set tunnel weight
 */
int packet_distributor_set_tunnel_weight(packet_distributor_t *dist, int tunnel_id, int weight)
{
    struct packet_distributor *d = (struct packet_distributor*)dist;
    
    if (!d || tunnel_id < 0 || tunnel_id >= d->tunnel_count || weight <= 0)
        return -1;
    
    EnterCriticalSection(&d->mutex);
    
    d->tunnel_weights[tunnel_id] = weight;
    d->tunnel_weight_counters[tunnel_id] = weight;
    
    LeaveCriticalSection(&d->mutex);
    
    BONDING_LOG_INFO("Set tunnel %d weight to %d", tunnel_id, weight);
    return 0;
}

/**
 * @brief Set tunnel health status
 */
int packet_distributor_set_tunnel_health(packet_distributor_t *dist, int tunnel_id, int healthy)
{
    struct packet_distributor *d = (struct packet_distributor*)dist;
    
    if (!d || tunnel_id < 0 || tunnel_id >= d->tunnel_count)
        return -1;
    
    EnterCriticalSection(&d->mutex);
    
    d->tunnel_health[tunnel_id].healthy = healthy ? 1 : 0;
    d->tunnel_health[tunnel_id].last_heartbeat = time(NULL);
    
    if (healthy) {
        d->tunnel_health[tunnel_id].consecutive_failures = 0;
    }
    
    LeaveCriticalSection(&d->mutex);
    
    BONDING_LOG_INFO("Set tunnel %d health to %s", tunnel_id, healthy ? "healthy" : "unhealthy");
    return 0;
}

/**
 * @brief Set tunnel TAP adapter name (helper function for integration)
 */
int packet_distributor_set_tunnel_tap_adapter(packet_distributor_t *dist, int tunnel_id, const char *tap_adapter_name)
{
    struct packet_distributor *d = (struct packet_distributor*)dist;
    HANDLE handle;
    
    if (!d || tunnel_id < 0 || tunnel_id >= d->tunnel_count || !tap_adapter_name)
        return -1;
    
    /* Close existing handle if any */
    if (d->tunnel_tap_handles[tunnel_id] != INVALID_HANDLE_VALUE) {
        CloseHandle(d->tunnel_tap_handles[tunnel_id]);
        d->tunnel_tap_handles[tunnel_id] = INVALID_HANDLE_VALUE;
    }
    
    /* Free existing name if any */
    if (d->tunnel_tap_names[tunnel_id]) {
        free(d->tunnel_tap_names[tunnel_id]);
        d->tunnel_tap_names[tunnel_id] = NULL;
    }
    
    /* Open TAP adapter */
    if (open_tap_adapter(tap_adapter_name, &handle) == 0) {
        EnterCriticalSection(&d->mutex);
        d->tunnel_tap_handles[tunnel_id] = handle;
        d->tunnel_tap_names[tunnel_id] = duplicate_string(tap_adapter_name);
        LeaveCriticalSection(&d->mutex);
        
        BONDING_LOG_INFO("Opened TAP adapter for tunnel %d: %s", tunnel_id, tap_adapter_name);
        return 0;
    }
    
    BONDING_LOG_ERROR("Failed to open TAP adapter for tunnel %d: %s", tunnel_id, tap_adapter_name);
    return -1;
}

/**
 * @brief Get statistics for a tunnel
 */
int packet_distributor_get_stats(packet_distributor_t *dist, int tunnel_id, packet_distributor_stats_t *stats)
{
    struct packet_distributor *d = (struct packet_distributor*)dist;
    
    if (!d || tunnel_id < 0 || tunnel_id >= d->tunnel_count || !stats)
        return -1;
    
    EnterCriticalSection(&d->mutex);
    
    stats->packets_sent = d->packets_sent[tunnel_id];
    stats->bytes_sent = d->bytes_sent[tunnel_id];
    stats->errors = d->errors[tunnel_id];
    stats->queue_depth = (int)d->packet_queue->current_size;
    stats->average_latency_ms = 0.0; /* TODO: Calculate from timestamps */
    
    LeaveCriticalSection(&d->mutex);
    
    return 0;
}

/**
 * @brief Configure packet distributor
 */
int packet_distributor_configure(packet_distributor_t *dist, const packet_distributor_config_t *config)
{
    struct packet_distributor *d = (struct packet_distributor*)dist;
    
    if (!d || !config)
        return -1;
    
    EnterCriticalSection(&d->mutex);
    
    if (config->queue_size > 0 && config->queue_size <= 10000) {
        d->queue_size = config->queue_size;
    }
    
    if (config->io_timeout_ms > 0) {
        d->io_timeout = config->io_timeout_ms;
    }
    
    d->sequencing_enabled = config->sequencing_enabled ? 1 : 0;
    d->flow_control_enabled = config->flow_control_enabled ? 1 : 0;
    
    LeaveCriticalSection(&d->mutex);
    
    BONDING_LOG_INFO("Packet distributor configured (queue_size=%d, timeout=%d, sequencing=%d, flow_control=%d)",
                     d->queue_size, d->io_timeout, d->sequencing_enabled, d->flow_control_enabled);
    
    return 0;
}

/**
 * @brief Destroy packet distributor
 */
void packet_distributor_destroy(packet_distributor_t *dist)
{
    struct packet_distributor *d = (struct packet_distributor*)dist;
    int i;
    
    if (!d)
        return;
    
    /* Stop forwarding thread */
    stop_forwarding_thread(d);
    
    /* Close TAP adapter handles */
    if (d->master_tap_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(d->master_tap_handle);
        d->master_tap_handle = INVALID_HANDLE_VALUE;
    }
    
    if (d->tunnel_tap_handles) {
        for (i = 0; i < d->tunnel_count; i++) {
            if (d->tunnel_tap_handles[i] != INVALID_HANDLE_VALUE) {
                CloseHandle(d->tunnel_tap_handles[i]);
            }
        }
        free(d->tunnel_tap_handles);
    }
    
    /* Free TAP adapter names */
    if (d->tunnel_tap_names) {
        for (i = 0; i < d->tunnel_count; i++) {
            if (d->tunnel_tap_names[i]) {
                free(d->tunnel_tap_names[i]);
            }
        }
        free(d->tunnel_tap_names);
    }
    
    /* Destroy packet queue */
    if (d->packet_queue) {
        packet_queue_destroy(d->packet_queue);
    }
    
    /* Free arrays */
    if (d->tunnel_health) free(d->tunnel_health);
    if (d->tunnel_weights) free(d->tunnel_weights);
    if (d->tunnel_weight_counters) free(d->tunnel_weight_counters);
    if (d->packets_sent) free(d->packets_sent);
    if (d->bytes_sent) free(d->bytes_sent);
    if (d->errors) free(d->errors);
    
    /* Delete critical section */
    DeleteCriticalSection(&d->mutex);
    
    /* Free distributor structure */
    free(d);
    
    BONDING_LOG_INFO("Packet distributor destroyed");
}
