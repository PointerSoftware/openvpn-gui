/**
 * @file nic_detector.c
 * @brief NIC detection and enumeration implementation
 * 
 * Implements comprehensive NIC detection using Windows IP Helper API
 */

#include "nic_detector.h"
#include "../utils/routing_helper.h"
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>
#include <time.h>

/* External logging functions */
extern void MsgToEventLog(WORD type, wchar_t *format, ...);
extern WCHAR* Widen(const char *utf8);
extern char* WCharToUTF8(const WCHAR *wstr);

#ifdef DEBUG
extern void PrintDebug(TCHAR *format, ...);
#else
#define PrintDebug(...) do {} while(0)
#endif

/* Logging macros */
#define BONDING_LOG_ERROR(...) do { \
    WCHAR *wmsg = Widen(__VA_ARGS__); \
    if (wmsg) { \
        MsgToEventLog(EVENTLOG_ERROR_TYPE, L"[NIC Detector] %ls", wmsg); \
        free(wmsg); \
    } \
} while(0)

#define BONDING_LOG_INFO(...) do { \
    WCHAR *wmsg = Widen(__VA_ARGS__); \
    if (wmsg) { \
        MsgToEventLog(EVENTLOG_INFORMATION_TYPE, L"[NIC Detector] %ls", wmsg); \
        free(wmsg); \
    } \
} while(0)

#ifdef DEBUG
#define BONDING_LOG_DEBUG(...) PrintDebug(L"[NIC Detector] " __VA_ARGS__)
#else
#define BONDING_LOG_DEBUG(...) do {} while(0)
#endif

/* Constants */
#define MAX_QUALITY_PROBES 10
#define QUALITY_MONITOR_INTERVAL 30000  /* 30 seconds */
#define MAX_PRIORITY 10
#define MIN_PRIORITY 1

/* Internal structures */
typedef struct {
    char *nic_name;
    int priority;
    nic_quality_t quality;
    time_t last_update;
} nic_priority_entry_t;

typedef struct {
    nic_event_callback_t callback;
    void *user_data;
} nic_callback_entry_t;

/* Track previous NIC status for change detection */
typedef struct {
    DWORD if_index;
    IF_OPER_STATUS oper_status;
    char nic_name[256];
} prev_status_entry_t;

/* Module state */
static struct {
    int initialized;
    CRITICAL_SECTION mutex;
    HANDLE quality_monitor_thread;
    HANDLE quality_monitor_stop_event;
    HANDLE event_monitor_thread;
    HANDLE event_monitor_stop_event;
    nic_priority_entry_t *priorities;
    int priority_count;
    nic_callback_entry_t *callbacks;
    int callback_count;
    char *ping_target;
    /* Track previous NIC status for change detection */
    prev_status_entry_t *prev_statuses;
    int prev_status_count;
} g_nic_detector = {0};

/* Forward declarations */
static nic_type_t DetectNICType(DWORD adapter_index, const char *description);
static unsigned long GetNICSpeed(DWORD adapter_index);
static nic_status_t GetNICStatus(DWORD adapter_index);
static int IsPhysicalAdapter(PIP_ADAPTER_INFO adapter);
static DWORD WINAPI QualityMonitorThread(LPVOID lpParam);
static DWORD WINAPI EventMonitorThread(LPVOID lpParam);
static void FreeNICInfo(nic_info_t *nic);

/**
 * @brief Detect NIC type from adapter index and description
 */
static nic_type_t DetectNICType(DWORD adapter_index, const char *description)
{
    MIB_IFROW ifRow;
    char desc_lower[256];
    int i;
    
    if (!description)
        return NIC_TYPE_UNKNOWN;
    
    /* Convert description to lowercase for comparison */
    for (i = 0; description[i] && i < 255; i++) {
        desc_lower[i] = (char)tolower((unsigned char)description[i]);
    }
    desc_lower[i] = '\0';
    
    /* Try to get interface row */
    memset(&ifRow, 0, sizeof(ifRow));
    ifRow.dwIndex = adapter_index;
    
    if (GetIfEntry(&ifRow) == NO_ERROR) {
        /* Check interface type */
        if (ifRow.dwType == IF_TYPE_ETHERNET_CSMACD) {
            return NIC_TYPE_ETHERNET;
        } else if (ifRow.dwType == IF_TYPE_IEEE80211) {
            return NIC_TYPE_WIFI;
        } else if (ifRow.dwType == IF_TYPE_WWANPP || ifRow.dwType == IF_TYPE_WWANPP2) {
            return NIC_TYPE_LTE;
        }
    }
    
    /* Fallback to description-based detection */
    if (strstr(desc_lower, "wireless") || strstr(desc_lower, "wi-fi") || strstr(desc_lower, "802.11")) {
        return NIC_TYPE_WIFI;
    }
    
    if (strstr(desc_lower, "mobile") || strstr(desc_lower, "cellular") || 
        strstr(desc_lower, "lte") || strstr(desc_lower, "4g") || 
        strstr(desc_lower, "5g") || strstr(desc_lower, "wwan")) {
        return NIC_TYPE_LTE;
    }
    
    if (strstr(desc_lower, "ethernet") || strstr(desc_lower, "lan")) {
        return NIC_TYPE_ETHERNET;
    }
    
    return NIC_TYPE_UNKNOWN;
}

/**
 * @brief Get NIC link speed in Mbps
 */
static unsigned long GetNICSpeed(DWORD adapter_index)
{
    MIB_IFROW ifRow;
    unsigned long speed_mbps = 0;
    
    memset(&ifRow, 0, sizeof(ifRow));
    ifRow.dwIndex = adapter_index;
    
    if (GetIfEntry(&ifRow) == NO_ERROR) {
        /* Speed is in bits per second, convert to Mbps */
        if (ifRow.dwSpeed > 0 && ifRow.dwSpeed != 0xFFFFFFFF) {
            speed_mbps = ifRow.dwSpeed / 1000000;
        }
    }
    
    return speed_mbps;
}

/**
 * @brief Get NIC connection status
 */
static nic_status_t GetNICStatus(DWORD adapter_index)
{
    MIB_IFROW ifRow;
    
    memset(&ifRow, 0, sizeof(ifRow));
    ifRow.dwIndex = adapter_index;
    
    if (GetIfEntry(&ifRow) == NO_ERROR) {
        if (ifRow.dwOperStatus == IF_OPER_STATUS_OPERATIONAL) {
            return NIC_STATUS_CONNECTED;
        } else if (ifRow.dwOperStatus == IF_OPER_STATUS_NON_OPERATIONAL) {
            return NIC_STATUS_DISCONNECTED;
        }
    }
    
    return NIC_STATUS_UNKNOWN;
}

/**
 * @brief Check if adapter is physical (not virtual)
 */
static int IsPhysicalAdapter(PIP_ADAPTER_INFO adapter)
{
    char desc_lower[256];
    int i;
    
    if (!adapter || !adapter->Description)
        return 0;
    
    /* Convert description to lowercase */
    for (i = 0; adapter->Description[i] && i < 255; i++) {
        desc_lower[i] = (char)tolower((unsigned char)adapter->Description[i]);
    }
    desc_lower[i] = '\0';
    
    /* Filter out virtual adapters */
    if (strstr(desc_lower, "tap-windows") || strstr(desc_lower, "tap-win32") ||
        strstr(desc_lower, "openvpn") || strstr(desc_lower, "microsoft kernel debug") ||
        strstr(desc_lower, "loopback") || strstr(desc_lower, "virtual") ||
        strstr(desc_lower, "vmware") || strstr(desc_lower, "virtualbox") ||
        strstr(desc_lower, "hyper-v") || strstr(desc_lower, "hyperv")) {
        return 0;
    }
    
    /* Check adapter type */
    if (adapter->Type == IF_TYPE_ETHERNET_CSMACD ||
        adapter->Type == IF_TYPE_IEEE80211 ||
        adapter->Type == IF_TYPE_WWANPP ||
        adapter->Type == IF_TYPE_WWANPP2) {
        return 1;
    }
    
    return 0;
}

/**
 * @brief Free NIC info structure
 */
static void FreeNICInfo(nic_info_t *nic)
{
    if (!nic)
        return;
    
    if (nic->name)
        free(nic->name);
    if (nic->description)
        free(nic->description);
    if (nic->guid)
        free(nic->guid);
    
    memset(nic, 0, sizeof(nic_info_t));
}

/**
 * @brief Get count of physical NICs
 */
int nic_detector_get_count(void)
{
    PIP_ADAPTER_INFO adapter_list = NULL;
    PIP_ADAPTER_INFO adapter = NULL;
    ULONG adapter_list_size = 0;
    DWORD status;
    int count = 0;
    
    /* Get required buffer size */
    status = GetAdaptersInfo(NULL, &adapter_list_size);
    if (status != ERROR_BUFFER_OVERFLOW) {
        BONDING_LOG_ERROR("nic_detector_get_count: Failed to get adapter list size");
        return 0;
    }
    
    /* Allocate buffer */
    adapter_list = (PIP_ADAPTER_INFO)malloc(adapter_list_size);
    if (!adapter_list) {
        BONDING_LOG_ERROR("nic_detector_get_count: Out of memory");
        return 0;
    }
    
    /* Get adapter list */
    status = GetAdaptersInfo(adapter_list, &adapter_list_size);
    if (status != ERROR_SUCCESS) {
        BONDING_LOG_ERROR("nic_detector_get_count: Failed to get adapter list");
        free(adapter_list);
        return 0;
    }
    
    /* Count physical adapters */
    adapter = adapter_list;
    while (adapter) {
        if (IsPhysicalAdapter(adapter)) {
            count++;
        }
        adapter = adapter->Next;
    }
    
    free(adapter_list);
    return count;
}

/**
 * @brief Enumerate physical NICs
 */
int nic_detector_enumerate(nic_info_t *nics, int max_count)
{
    PIP_ADAPTER_INFO adapter_list = NULL;
    PIP_ADAPTER_INFO adapter = NULL;
    ULONG adapter_list_size = 0;
    DWORD status;
    int count = 0;
    char *name_copy = NULL;
    char *desc_copy = NULL;
    char *guid_copy = NULL;
    
    if (!nics || max_count <= 0) {
        BONDING_LOG_ERROR("nic_detector_enumerate: Invalid parameters");
        return -1;
    }
    
    /* Get required buffer size */
    status = GetAdaptersInfo(NULL, &adapter_list_size);
    if (status != ERROR_BUFFER_OVERFLOW) {
        BONDING_LOG_ERROR("nic_detector_enumerate: Failed to get adapter list size");
        return -1;
    }
    
    /* Allocate buffer */
    adapter_list = (PIP_ADAPTER_INFO)malloc(adapter_list_size);
    if (!adapter_list) {
        BONDING_LOG_ERROR("nic_detector_enumerate: Out of memory");
        return -1;
    }
    
    /* Get adapter list */
    status = GetAdaptersInfo(adapter_list, &adapter_list_size);
    if (status != ERROR_SUCCESS) {
        BONDING_LOG_ERROR("nic_detector_enumerate: Failed to get adapter list");
        free(adapter_list);
        return -1;
    }
    
    /* Enumerate physical adapters */
    adapter = adapter_list;
    while (adapter && count < max_count) {
        if (IsPhysicalAdapter(adapter)) {
            nic_info_t *nic = &nics[count];
            
            /* Initialize structure */
            memset(nic, 0, sizeof(nic_info_t));
            
            /* Copy adapter name (GUID format) */
            name_copy = (char*)malloc(strlen(adapter->AdapterName) + 1);
            if (name_copy) {
                strcpy(name_copy, adapter->AdapterName);
                nic->name = name_copy;
            }
            
            /* Copy description */
            if (adapter->Description) {
                desc_copy = (char*)malloc(strlen(adapter->Description) + 1);
                if (desc_copy) {
                    strcpy(desc_copy, adapter->Description);
                    nic->description = desc_copy;
                }
            }
            
            /* Generate GUID string from adapter name */
            guid_copy = (char*)malloc(strlen(adapter->AdapterName) + 1);
            if (guid_copy) {
                strcpy(guid_copy, adapter->AdapterName);
                nic->guid = guid_copy;
            }
            
            /* Get interface index */
            nic->index = adapter->Index;
            
            /* Detect NIC type */
            nic->type = DetectNICType(adapter->Index, adapter->Description);
            
            /* Get link speed */
            nic->speed = GetNICSpeed(adapter->Index);
            
            /* Get status */
            nic->status = GetNICStatus(adapter->Index);
            
            count++;
        }
        adapter = adapter->Next;
    }
    
    free(adapter_list);
    BONDING_LOG_INFO("nic_detector_enumerate: Enumerated %d physical NICs", count);
    return count;
}

/**
 * @brief Get NIC information by name
 */
int nic_detector_get_by_name(const char *name, nic_info_t *nic)
{
    PIP_ADAPTER_INFO adapter_list = NULL;
    PIP_ADAPTER_INFO adapter = NULL;
    ULONG adapter_list_size = 0;
    DWORD status;
    int found = 0;
    char *name_copy = NULL;
    char *desc_copy = NULL;
    char *guid_copy = NULL;
    
    if (!name || !nic) {
        BONDING_LOG_ERROR("nic_detector_get_by_name: Invalid parameters");
        return -1;
    }
    
    /* Get required buffer size */
    status = GetAdaptersInfo(NULL, &adapter_list_size);
    if (status != ERROR_BUFFER_OVERFLOW) {
        BONDING_LOG_ERROR("nic_detector_get_by_name: Failed to get adapter list size");
        return -1;
    }
    
    /* Allocate buffer */
    adapter_list = (PIP_ADAPTER_INFO)malloc(adapter_list_size);
    if (!adapter_list) {
        BONDING_LOG_ERROR("nic_detector_get_by_name: Out of memory");
        return -1;
    }
    
    /* Get adapter list */
    status = GetAdaptersInfo(adapter_list, &adapter_list_size);
    if (status != ERROR_SUCCESS) {
        BONDING_LOG_ERROR("nic_detector_get_by_name: Failed to get adapter list");
        free(adapter_list);
        return -1;
    }
    
    /* Search for matching adapter */
    adapter = adapter_list;
    while (adapter) {
        /* Case-insensitive comparison */
        if (_stricmp(adapter->AdapterName, name) == 0 ||
            (adapter->Description && _stricmp(adapter->Description, name) == 0)) {
            
            if (IsPhysicalAdapter(adapter)) {
                /* Initialize structure */
                memset(nic, 0, sizeof(nic_info_t));
                
                /* Copy adapter name */
                name_copy = (char*)malloc(strlen(adapter->AdapterName) + 1);
                if (name_copy) {
                    strcpy(name_copy, adapter->AdapterName);
                    nic->name = name_copy;
                }
                
                /* Copy description */
                if (adapter->Description) {
                    desc_copy = (char*)malloc(strlen(adapter->Description) + 1);
                    if (desc_copy) {
                        strcpy(desc_copy, adapter->Description);
                        nic->description = desc_copy;
                    }
                }
                
                /* Generate GUID */
                guid_copy = (char*)malloc(strlen(adapter->AdapterName) + 1);
                if (guid_copy) {
                    strcpy(guid_copy, adapter->AdapterName);
                    nic->guid = guid_copy;
                }
                
                /* Get interface index */
                nic->index = adapter->Index;
                
                /* Detect NIC type */
                nic->type = DetectNICType(adapter->Index, adapter->Description);
                
                /* Get link speed */
                nic->speed = GetNICSpeed(adapter->Index);
                
                /* Get status */
                nic->status = GetNICStatus(adapter->Index);
                
                found = 1;
                break;
            }
        }
        adapter = adapter->Next;
    }
    
    free(adapter_list);
    
    if (!found) {
        BONDING_LOG_ERROR("nic_detector_get_by_name: NIC not found: %s", name);
        return -1;
    }
    
    return 0;
}

/**
 * @brief Check if NIC is physical
 */
int nic_detector_is_physical(const char *name)
{
    PIP_ADAPTER_INFO adapter_list = NULL;
    PIP_ADAPTER_INFO adapter = NULL;
    ULONG adapter_list_size = 0;
    DWORD status;
    int is_physical = 0;
    
    if (!name) {
        return 0;
    }
    
    /* Get required buffer size */
    status = GetAdaptersInfo(NULL, &adapter_list_size);
    if (status != ERROR_BUFFER_OVERFLOW) {
        return 0;
    }
    
    /* Allocate buffer */
    adapter_list = (PIP_ADAPTER_INFO)malloc(adapter_list_size);
    if (!adapter_list) {
        return 0;
    }
    
    /* Get adapter list */
    status = GetAdaptersInfo(adapter_list, &adapter_list_size);
    if (status != ERROR_SUCCESS) {
        free(adapter_list);
        return 0;
    }
    
    /* Search for matching adapter */
    adapter = adapter_list;
    while (adapter) {
        if (_stricmp(adapter->AdapterName, name) == 0 ||
            (adapter->Description && _stricmp(adapter->Description, name) == 0)) {
            is_physical = IsPhysicalAdapter(adapter);
            break;
        }
        adapter = adapter->Next;
    }
    
    free(adapter_list);
    return is_physical;
}

/**
 * @brief Get NIC quality metrics
 */
int nic_detector_get_quality(const char *nic_name, nic_quality_t *quality)
{
    HANDLE icmp_handle;
    IPAddr target_ip;
    IPAddr source_ip = 0;
    char reply_buffer[sizeof(ICMP_ECHO_REPLY) + 32];
    PIP_OPTION_INFORMATION options = NULL;
    DWORD reply_count;
    unsigned long total_latency = 0;
    int success_count = 0;
    unsigned long min_latency = 0xFFFFFFFF;
    unsigned long max_latency = 0;
    PIP_ADAPTER_INFO adapter_list = NULL;
    PIP_ADAPTER_INFO adapter = NULL;
    ULONG adapter_list_size = 0;
    DWORD status;
    char gateway_ip[16] = {0};
    
    if (!nic_name || !quality) {
        BONDING_LOG_ERROR("nic_detector_get_quality: Invalid parameters");
        return -1;
    }
    
    memset(quality, 0, sizeof(nic_quality_t));
    
    /* Get adapter information to find gateway and source IP */
    status = GetAdaptersInfo(NULL, &adapter_list_size);
    if (status == ERROR_BUFFER_OVERFLOW) {
        adapter_list = (PIP_ADAPTER_INFO)malloc(adapter_list_size);
        if (adapter_list) {
            if (GetAdaptersInfo(adapter_list, &adapter_list_size) == ERROR_SUCCESS) {
                adapter = adapter_list;
                while (adapter) {
                    if (_stricmp(adapter->AdapterName, nic_name) == 0 ||
                        (adapter->Description && _stricmp(adapter->Description, nic_name) == 0)) {
                        if (adapter->GatewayList.IpAddress.String[0] != '\0') {
                            strncpy_s(gateway_ip, sizeof(gateway_ip), adapter->GatewayList.IpAddress.String, _TRUNCATE);
                        }
                        /* Resolve source IP from adapter's IP address list */
                        if (adapter->IpAddressList.IpAddress.String[0] != '\0') {
                            source_ip = inet_addr(adapter->IpAddressList.IpAddress.String);
                        }
                        break;
                    }
                    adapter = adapter->Next;
                }
            }
            free(adapter_list);
        }
    }
    
    /* If source IP not found from GetAdaptersInfo, try GetAdaptersAddresses */
    if (source_ip == 0 || source_ip == INADDR_NONE) {
        PIP_ADAPTER_ADDRESSES adapter_addresses = NULL;
        ULONG adapter_addresses_size = 0;
        
        status = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_ALL_INTERFACES, 
                                       NULL, NULL, &adapter_addresses_size);
        if (status == ERROR_BUFFER_OVERFLOW) {
            adapter_addresses = (PIP_ADAPTER_ADDRESSES)malloc(adapter_addresses_size);
            if (adapter_addresses) {
                if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_ALL_INTERFACES,
                                           NULL, adapter_addresses, &adapter_addresses_size) == ERROR_SUCCESS) {
                    PIP_ADAPTER_ADDRESSES addr_adapter = adapter_addresses;
                    while (addr_adapter) {
                        char adapter_name[256];
                        WideCharToMultiByte(CP_UTF8, 0, addr_adapter->FriendlyName, -1, adapter_name, sizeof(adapter_name), NULL, NULL);
                        
                        if (_stricmp(adapter_name, nic_name) == 0) {
                            /* Get first IPv4 address */
                            PIP_ADAPTER_UNICAST_ADDRESS unicast = addr_adapter->FirstUnicastAddress;
                            while (unicast) {
                                if (unicast->Address.lpSockaddr->sa_family == AF_INET) {
                                    struct sockaddr_in *sin = (struct sockaddr_in*)unicast->Address.lpSockaddr;
                                    source_ip = sin->sin_addr.s_addr;
                                    break;
                                }
                                unicast = unicast->Next;
                            }
                            break;
                        }
                        addr_adapter = addr_adapter->Next;
                    }
                }
                free(adapter_addresses);
            }
        }
    }
    
    /* Use ping target if configured, otherwise use gateway */
    const char *ping_target = g_nic_detector.ping_target ? g_nic_detector.ping_target : gateway_ip;
    if (ping_target[0] == '\0') {
        ping_target = "8.8.8.8"; /* Default to Google DNS */
    }
    
    target_ip = inet_addr(ping_target);
    if (target_ip == INADDR_NONE) {
        BONDING_LOG_ERROR("nic_detector_get_quality: Invalid ping target: %s", ping_target);
        return -1;
    }
    
    /* Create ICMP handle */
    icmp_handle = IcmpCreateFile();
    if (icmp_handle == INVALID_HANDLE_VALUE) {
        BONDING_LOG_ERROR("nic_detector_get_quality: Failed to create ICMP handle");
        return -1;
    }
    
    /* Send multiple probes with source IP binding */
    for (int i = 0; i < MAX_QUALITY_PROBES; i++) {
        if (source_ip != 0 && source_ip != INADDR_NONE) {
            /* Use IcmpSendEcho2Ex with source IP to force traffic through target NIC */
            reply_count = IcmpSendEcho2Ex(icmp_handle, NULL, NULL, NULL, source_ip, target_ip, 
                                         NULL, 0, options, reply_buffer, sizeof(reply_buffer), 1000);
        } else {
            /* Fallback to IcmpSendEcho if source IP not available */
            reply_count = IcmpSendEcho(icmp_handle, target_ip, NULL, 0, options, 
                                      reply_buffer, sizeof(reply_buffer), 1000);
        }
        
        if (reply_count > 0) {
            PIP_ECHO_REPLY echo_reply = (PIP_ECHO_REPLY)reply_buffer;
            if (echo_reply->Status == IP_SUCCESS) {
                unsigned long latency = echo_reply->RoundTripTime;
                total_latency += latency;
                success_count++;
                
                if (latency < min_latency)
                    min_latency = latency;
                if (latency > max_latency)
                    max_latency = latency;
            }
        }
        
        Sleep(100); /* Small delay between probes */
    }
    
    IcmpCloseHandle(icmp_handle);
    
    /* Calculate metrics */
    if (success_count > 0) {
        quality->latency_ms = total_latency / success_count;
        quality->packet_loss_percent = ((float)(MAX_QUALITY_PROBES - success_count) / MAX_QUALITY_PROBES) * 100.0f;
        quality->jitter_ms = max_latency - min_latency;
        
        /* Calculate quality score (0-100) */
        /* Latency: 40% weight (lower is better, max 200ms = 0 score) */
        float latency_score = (quality->latency_ms < 200) ? (1.0f - (quality->latency_ms / 200.0f)) * 40.0f : 0.0f;
        
        /* Packet loss: 40% weight (lower is better) */
        float loss_score = (1.0f - (quality->packet_loss_percent / 100.0f)) * 40.0f;
        
        /* Jitter: 20% weight (lower is better, max 100ms = 0 score) */
        float jitter_score = (quality->jitter_ms < 100) ? (1.0f - (quality->jitter_ms / 100.0f)) * 20.0f : 0.0f;
        
        quality->quality_score = (int)(latency_score + loss_score + jitter_score);
        if (quality->quality_score > 100)
            quality->quality_score = 100;
        if (quality->quality_score < 0)
            quality->quality_score = 0;
    } else {
        /* All probes failed */
        quality->packet_loss_percent = 100.0f;
        quality->quality_score = 0;
    }
    
    return 0;
}

/**
 * @brief Quality monitoring thread
 */
static DWORD WINAPI QualityMonitorThread(LPVOID lpParam)
{
    nic_info_t nics[32];
    int nic_count;
    int i;
    
    BONDING_LOG_INFO("Quality monitor thread started");
    
    while (WaitForSingleObject(g_nic_detector.quality_monitor_stop_event, QUALITY_MONITOR_INTERVAL) == WAIT_TIMEOUT) {
        EnterCriticalSection(&g_nic_detector.mutex);
        
        /* Enumerate all NICs */
        nic_count = nic_detector_enumerate(nics, 32);
        
        /* Update quality for each NIC */
        for (i = 0; i < nic_count; i++) {
            if (nics[i].status == NIC_STATUS_CONNECTED) {
                nic_quality_t quality;
                if (nic_detector_get_quality(nics[i].name, &quality) == 0) {
                    /* Update priority entry if exists */
                    int j;
                    for (j = 0; j < g_nic_detector.priority_count; j++) {
                        if (strcmp(g_nic_detector.priorities[j].nic_name, nics[i].name) == 0) {
                            g_nic_detector.priorities[j].quality = quality;
                            g_nic_detector.priorities[j].last_update = time(NULL);
                            break;
                        }
                    }
                }
            }
            
            /* Free NIC info */
            FreeNICInfo(&nics[i]);
        }
        
        LeaveCriticalSection(&g_nic_detector.mutex);
    }
    
    BONDING_LOG_INFO("Quality monitor thread stopped");
    return 0;
}

/**
 * @brief Event monitoring thread
 */
static DWORD WINAPI EventMonitorThread(LPVOID lpParam)
{
    HANDLE notify_handle = NULL;
    OVERLAPPED overlapped = {0};
    HANDLE event_handle = NULL;
    DWORD status;
    PMIB_IF_TABLE2 if_table = NULL;
    const int POLL_INTERVAL = 5000; /* Poll every 5 seconds */
    
    BONDING_LOG_INFO("Event monitor thread started");
    
    event_handle = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!event_handle) {
        BONDING_LOG_ERROR("EventMonitorThread: Failed to create event");
        return 1;
    }
    
    overlapped.hEvent = event_handle;
    
    /* Register for address change notifications */
    status = NotifyAddrChange(&notify_handle, &overlapped);
    if (status != ERROR_IO_PENDING) {
        BONDING_LOG_ERROR("EventMonitorThread: Failed to register for notifications");
        CloseHandle(event_handle);
        return 1;
    }
    
    /* Initialize previous status tracking */
    EnterCriticalSection(&g_nic_detector.mutex);
    g_nic_detector.prev_statuses = NULL;
    g_nic_detector.prev_status_count = 0;
    LeaveCriticalSection(&g_nic_detector.mutex);
    
    while (1) {
        HANDLE handles[2] = {g_nic_detector.event_monitor_stop_event, event_handle};
        DWORD wait_result = WaitForMultipleObjects(2, handles, FALSE, POLL_INTERVAL);
        
        if (wait_result == WAIT_OBJECT_0) {
            /* Stop event signaled */
            break;
        } else if (wait_result == WAIT_OBJECT_0 + 1) {
            /* Address change detected */
            EnterCriticalSection(&g_nic_detector.mutex);
            
            /* Notify all callbacks */
            for (int i = 0; i < g_nic_detector.callback_count; i++) {
                if (g_nic_detector.callbacks[i].callback) {
                    g_nic_detector.callbacks[i].callback(NIC_EVENT_IP_CHANGED, NULL, 
                                                         g_nic_detector.callbacks[i].user_data);
                }
            }
            
            LeaveCriticalSection(&g_nic_detector.mutex);
            
            /* Reset event and re-register */
            ResetEvent(event_handle);
            status = NotifyAddrChange(&notify_handle, &overlapped);
            if (status != ERROR_IO_PENDING) {
                BONDING_LOG_ERROR("EventMonitorThread: Failed to re-register for notifications");
                break;
            }
        }
        
        /* Periodic polling for interface status changes */
        if (wait_result == WAIT_TIMEOUT) {
            /* Get current interface table */
            status = GetIfTable2(&if_table);
            if (status == NO_ERROR && if_table) {
                EnterCriticalSection(&g_nic_detector.mutex);
                
                /* Check each interface for status changes */
                for (ULONG i = 0; i < if_table->NumEntries; i++) {
                    MIB_IF_ROW2 *if_row = &if_table->Table[i];
                    char nic_name[256] = {0};
                    
                    /* Skip non-physical interfaces */
                    if (if_row->Type != IF_TYPE_ETHERNET_CSMACD &&
                        if_row->Type != IF_TYPE_IEEE80211 &&
                        if_row->Type != IF_TYPE_WWANPP &&
                        if_row->Type != IF_TYPE_WWANPP2) {
                        continue;
                    }
                    
                    /* Get NIC name from adapter addresses */
                    PIP_ADAPTER_ADDRESSES adapter_addresses = NULL;
                    ULONG adapter_addresses_size = 0;
                    status = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_ALL_INTERFACES, 
                                                   NULL, NULL, &adapter_addresses_size);
                    if (status == ERROR_BUFFER_OVERFLOW) {
                        adapter_addresses = (PIP_ADAPTER_ADDRESSES)malloc(adapter_addresses_size);
                        if (adapter_addresses) {
                            if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_ALL_INTERFACES,
                                                       NULL, adapter_addresses, &adapter_addresses_size) == ERROR_SUCCESS) {
                                PIP_ADAPTER_ADDRESSES adapter = adapter_addresses;
                                while (adapter) {
                                    if (adapter->IfIndex == if_row->InterfaceIndex) {
                                        WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1, 
                                                           nic_name, sizeof(nic_name), NULL, NULL);
                                        break;
                                    }
                                    adapter = adapter->Next;
                                }
                            }
                            free(adapter_addresses);
                        }
                    }
                    
                    if (nic_name[0] == '\0') {
                        continue; /* Skip if we can't get NIC name */
                    }
                    
                    /* Find previous status for this interface */
                    int prev_idx = -1;
                    for (int j = 0; j < g_nic_detector.prev_status_count; j++) {
                        if (g_nic_detector.prev_statuses[j].if_index == if_row->InterfaceIndex) {
                            prev_idx = j;
                            break;
                        }
                    }
                    
                    /* Check for status change */
                    if (prev_idx >= 0) {
                        IF_OPER_STATUS prev_status = g_nic_detector.prev_statuses[prev_idx].oper_status;
                        IF_OPER_STATUS curr_status = if_row->OperStatus;
                        
                        if (prev_status != curr_status) {
                            /* Status changed - notify callbacks */
                            nic_event_type_t event_type;
                            if (curr_status == IF_OPER_STATUS_OPERATIONAL && 
                                prev_status != IF_OPER_STATUS_OPERATIONAL) {
                                event_type = NIC_EVENT_CONNECTED;
                            } else if (curr_status != IF_OPER_STATUS_OPERATIONAL && 
                                      prev_status == IF_OPER_STATUS_OPERATIONAL) {
                                event_type = NIC_EVENT_DISCONNECTED;
                            } else {
                                event_type = NIC_EVENT_IP_CHANGED; /* Other status change */
                            }
                            
                            /* Notify all callbacks with NIC name */
                            for (int k = 0; k < g_nic_detector.callback_count; k++) {
                                if (g_nic_detector.callbacks[k].callback) {
                                    g_nic_detector.callbacks[k].callback(event_type, nic_name, 
                                                                         g_nic_detector.callbacks[k].user_data);
                                }
                            }
                            
                            /* Update previous status */
                            g_nic_detector.prev_statuses[prev_idx].oper_status = curr_status;
                        }
                    } else {
                        /* New interface - add to tracking */
                        prev_status_entry_t *new_statuses = (prev_status_entry_t*)realloc(g_nic_detector.prev_statuses,
                                                                        (g_nic_detector.prev_status_count + 1) * sizeof(prev_status_entry_t));
                        if (new_statuses) {
                            g_nic_detector.prev_statuses = new_statuses;
                            g_nic_detector.prev_statuses[g_nic_detector.prev_status_count].if_index = if_row->InterfaceIndex;
                            g_nic_detector.prev_statuses[g_nic_detector.prev_status_count].oper_status = if_row->OperStatus;
                            strncpy_s(g_nic_detector.prev_statuses[g_nic_detector.prev_status_count].nic_name,
                                     sizeof(g_nic_detector.prev_statuses[g_nic_detector.prev_status_count].nic_name),
                                     nic_name, _TRUNCATE);
                            g_nic_detector.prev_status_count++;
                        }
                    }
                }
                
                LeaveCriticalSection(&g_nic_detector.mutex);
                
                /* Free interface table */
                FreeMibTable(if_table);
                if_table = NULL;
            }
        }
    }
    
    /* Cleanup */
    if (if_table) {
        FreeMibTable(if_table);
    }
    
    EnterCriticalSection(&g_nic_detector.mutex);
    if (g_nic_detector.prev_statuses) {
        free(g_nic_detector.prev_statuses);
        g_nic_detector.prev_statuses = NULL;
        g_nic_detector.prev_status_count = 0;
    }
    LeaveCriticalSection(&g_nic_detector.mutex);
    
    /* Note: NotifyAddrChange handle is automatically cleaned up when thread exits */
    /* The overlapped operation will be cancelled when the event handle is closed */
    
    CloseHandle(event_handle);
    BONDING_LOG_INFO("Event monitor thread stopped");
    return 0;
}

/**
 * @brief Start quality monitoring
 */
int nic_detector_monitor_quality(void)
{
    if (!g_nic_detector.initialized) {
        BONDING_LOG_ERROR("nic_detector_monitor_quality: Module not initialized");
        return -1;
    }
    
    if (g_nic_detector.quality_monitor_thread) {
        /* Already monitoring */
        return 0;
    }
    
    /* Create stop event */
    g_nic_detector.quality_monitor_stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_nic_detector.quality_monitor_stop_event) {
        BONDING_LOG_ERROR("nic_detector_monitor_quality: Failed to create stop event");
        return -1;
    }
    
    /* Start monitoring thread */
    g_nic_detector.quality_monitor_thread = CreateThread(NULL, 0, QualityMonitorThread, NULL, 0, NULL);
    if (!g_nic_detector.quality_monitor_thread) {
        BONDING_LOG_ERROR("nic_detector_monitor_quality: Failed to create thread");
        CloseHandle(g_nic_detector.quality_monitor_stop_event);
        g_nic_detector.quality_monitor_stop_event = NULL;
        return -1;
    }
    
    return 0;
}

/**
 * @brief Set NIC priority
 */
int nic_detector_set_priority(const char *nic_name, int priority)
{
    int i;
    
    if (!nic_name || priority < MIN_PRIORITY || priority > MAX_PRIORITY) {
        BONDING_LOG_ERROR("nic_detector_set_priority: Invalid parameters");
        return -1;
    }
    
    EnterCriticalSection(&g_nic_detector.mutex);
    
    /* Find existing entry */
    for (i = 0; i < g_nic_detector.priority_count; i++) {
        if (g_nic_detector.priorities[i].nic_name && 
            strcmp(g_nic_detector.priorities[i].nic_name, nic_name) == 0) {
            int old_priority = g_nic_detector.priorities[i].priority;
            g_nic_detector.priorities[i].priority = priority;
            LeaveCriticalSection(&g_nic_detector.mutex);
            
            /* Update route metrics if priority changed */
            if (old_priority != priority) {
                routing_helper_update_nic_metrics(nic_name, priority);
            }
            return 0;
        }
    }
    
    /* Add new entry */
    nic_priority_entry_t *new_priorities = (nic_priority_entry_t*)realloc(
        g_nic_detector.priorities, 
        (g_nic_detector.priority_count + 1) * sizeof(nic_priority_entry_t));
    if (!new_priorities) {
        BONDING_LOG_ERROR("nic_detector_set_priority: Out of memory");
        LeaveCriticalSection(&g_nic_detector.mutex);
        return -1;
    }
    
    g_nic_detector.priorities = new_priorities;
    i = g_nic_detector.priority_count;
    
    /* Allocate and check nic_name - roll back on failure */
    g_nic_detector.priorities[i].nic_name = (char*)malloc(strlen(nic_name) + 1);
    if (!g_nic_detector.priorities[i].nic_name) {
        BONDING_LOG_ERROR("nic_detector_set_priority: Out of memory allocating nic_name");
        /* Roll back: shrink array back to original size */
        nic_priority_entry_t *rollback_priorities = (nic_priority_entry_t*)realloc(
            g_nic_detector.priorities,
            g_nic_detector.priority_count * sizeof(nic_priority_entry_t));
        if (rollback_priorities) {
            g_nic_detector.priorities = rollback_priorities;
        }
        LeaveCriticalSection(&g_nic_detector.mutex);
        return -1;
    }
    
    strcpy(g_nic_detector.priorities[i].nic_name, nic_name);
    g_nic_detector.priorities[i].priority = priority;
    memset(&g_nic_detector.priorities[i].quality, 0, sizeof(nic_quality_t));
    g_nic_detector.priorities[i].last_update = 0;
    g_nic_detector.priority_count++;
    
    LeaveCriticalSection(&g_nic_detector.mutex);
    return 0;
}

/**
 * @brief Get NIC priority
 */
int nic_detector_get_priority(const char *nic_name)
{
    int i;
    int priority = 5; /* Default priority */
    
    if (!nic_name) {
        return priority;
    }
    
    EnterCriticalSection(&g_nic_detector.mutex);
    
    for (i = 0; i < g_nic_detector.priority_count; i++) {
        if (strcmp(g_nic_detector.priorities[i].nic_name, nic_name) == 0) {
            priority = g_nic_detector.priorities[i].priority;
            break;
        }
    }
    
    LeaveCriticalSection(&g_nic_detector.mutex);
    return priority;
}

/**
 * @brief Get best NIC based on priority and quality
 */
const char* nic_detector_get_best_nic(void)
{
    int i;
    int best_priority = 0;
    int best_quality = 0;
    const char *best_nic = NULL;
    time_t now = time(NULL);
    
    EnterCriticalSection(&g_nic_detector.mutex);
    
    for (i = 0; i < g_nic_detector.priority_count; i++) {
        /* Skip stale entries (older than 60 seconds) */
        if (now - g_nic_detector.priorities[i].last_update > 60) {
            continue;
        }
        
        /* Check quality score */
        if (g_nic_detector.priorities[i].quality.quality_score < 50) {
            continue; /* Skip poor quality NICs */
        }
        
        /* Prefer higher priority, then higher quality */
        if (g_nic_detector.priorities[i].priority > best_priority ||
            (g_nic_detector.priorities[i].priority == best_priority &&
             g_nic_detector.priorities[i].quality.quality_score > best_quality)) {
            best_priority = g_nic_detector.priorities[i].priority;
            best_quality = g_nic_detector.priorities[i].quality.quality_score;
            best_nic = g_nic_detector.priorities[i].nic_name;
        }
    }
    
    LeaveCriticalSection(&g_nic_detector.mutex);
    return best_nic;
}

/**
 * @brief Get NIC capabilities
 */
int nic_detector_get_capabilities(const char *nic_name, nic_capabilities_t *capabilities)
{
    PIP_ADAPTER_ADDRESSES adapter_addresses = NULL;
    PIP_ADAPTER_ADDRESSES adapter = NULL;
    ULONG adapter_addresses_size = 0;
    DWORD status;
    int found = 0;
    MIB_IF_ROW2 if_row;
    
    if (!nic_name || !capabilities) {
        BONDING_LOG_ERROR("nic_detector_get_capabilities: Invalid parameters");
        return -1;
    }
    
    memset(capabilities, 0, sizeof(nic_capabilities_t));
    
    /* Get required buffer size */
    status = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_ALL_INTERFACES, 
                                   NULL, NULL, &adapter_addresses_size);
    if (status != ERROR_BUFFER_OVERFLOW) {
        BONDING_LOG_ERROR("nic_detector_get_capabilities: Failed to get adapter addresses size");
        return -1;
    }
    
    /* Allocate buffer */
    adapter_addresses = (PIP_ADAPTER_ADDRESSES)malloc(adapter_addresses_size);
    if (!adapter_addresses) {
        BONDING_LOG_ERROR("nic_detector_get_capabilities: Out of memory");
        return -1;
    }
    
    /* Get adapter addresses */
    status = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_ALL_INTERFACES,
                                   NULL, adapter_addresses, &adapter_addresses_size);
    if (status != ERROR_SUCCESS) {
        BONDING_LOG_ERROR("nic_detector_get_capabilities: Failed to get adapter addresses");
        free(adapter_addresses);
        return -1;
    }
    
    /* Search for matching adapter */
    adapter = adapter_addresses;
    while (adapter) {
        char adapter_name[256];
        WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1, adapter_name, sizeof(adapter_name), NULL, NULL);
        
        if (_stricmp(adapter_name, nic_name) == 0) {
            /* Get interface row to query real capabilities */
            memset(&if_row, 0, sizeof(if_row));
            if_row.InterfaceIndex = adapter->IfIndex;
            
            status = GetIfEntry2(&if_row);
            if (status == NO_ERROR) {
                /* Extract capabilities from interface row */
                capabilities->max_speed_mbps = GetNICSpeed(adapter->IfIndex);
                
                /* Query duplex mode from interface properties */
                /* MIB_IF_ROW2 doesn't directly expose duplex, but we can infer from AdminStatus and MediaConnectState */
                /* For Ethernet interfaces, check if it's operational and supports full duplex */
                if (if_row.Type == IF_TYPE_ETHERNET_CSMACD) {
                    /* Modern Ethernet interfaces typically support full duplex */
                    /* Check if interface is operational - if so, it likely supports full duplex */
                    if (if_row.OperStatus == IF_OPER_STATUS_OPERATIONAL &&
                        if_row.MediaConnectState == MediaConnectStateConnected) {
                        capabilities->supports_full_duplex = 1;
                    } else {
                        /* Interface not connected, cannot determine duplex */
                        capabilities->supports_full_duplex = 0;
                    }
                } else {
                    /* For non-Ethernet interfaces, assume full duplex if operational */
                    capabilities->supports_full_duplex = 
                        (if_row.OperStatus == IF_OPER_STATUS_OPERATIONAL) ? 1 : 0;
                }
                
                /* Query jumbo frames support from interface MTU */
                /* If MTU > 1500, jumbo frames are likely supported */
                capabilities->supports_jumbo_frames = (if_row.Mtu > 1500) ? 1 : 0;
                
                /* Advanced features require WMI queries - set to 0 for now */
                capabilities->supports_wake_on_lan = 0; /* Would need WMI query */
                capabilities->supports_offload = 0; /* Would need WMI query */
            } else {
                /* Fallback if GetIfEntry2 fails */
                capabilities->max_speed_mbps = GetNICSpeed(adapter->IfIndex);
                capabilities->supports_full_duplex = 0;
                capabilities->supports_jumbo_frames = 0;
                capabilities->supports_wake_on_lan = 0;
                capabilities->supports_offload = 0;
            }
            
            found = 1;
            break;
        }
        adapter = adapter->Next;
    }
    
    free(adapter_addresses);
    
    if (!found) {
        BONDING_LOG_ERROR("nic_detector_get_capabilities: NIC not found: %s", nic_name);
        return -1;
    }
    
    return 0;
}

/**
 * @brief Register callback for NIC events
 */
int nic_detector_register_callback(nic_event_callback_t callback, void *user_data)
{
    int i;
    
    if (!callback) {
        BONDING_LOG_ERROR("nic_detector_register_callback: Invalid callback");
        return -1;
    }
    
    EnterCriticalSection(&g_nic_detector.mutex);
    
    /* Check if already registered */
    for (i = 0; i < g_nic_detector.callback_count; i++) {
        if (g_nic_detector.callbacks[i].callback == callback) {
            LeaveCriticalSection(&g_nic_detector.mutex);
            return 0; /* Already registered */
        }
    }
    
    /* Add new callback */
    nic_callback_entry_t *new_callbacks = (nic_callback_entry_t*)realloc(
        g_nic_detector.callbacks,
        (g_nic_detector.callback_count + 1) * sizeof(nic_callback_entry_t));
    if (!new_callbacks) {
        BONDING_LOG_ERROR("nic_detector_register_callback: Out of memory");
        LeaveCriticalSection(&g_nic_detector.mutex);
        return -1;
    }
    
    g_nic_detector.callbacks = new_callbacks;
    g_nic_detector.callbacks[g_nic_detector.callback_count].callback = callback;
    g_nic_detector.callbacks[g_nic_detector.callback_count].user_data = user_data;
    g_nic_detector.callback_count++;
    
    LeaveCriticalSection(&g_nic_detector.mutex);
    return 0;
}

/**
 * @brief Unregister callback for NIC events
 */
int nic_detector_unregister_callback(nic_event_callback_t callback)
{
    int i;
    
    if (!callback) {
        return -1;
    }
    
    EnterCriticalSection(&g_nic_detector.mutex);
    
    for (i = 0; i < g_nic_detector.callback_count; i++) {
        if (g_nic_detector.callbacks[i].callback == callback) {
            /* Remove callback by shifting array */
            int j;
            for (j = i; j < g_nic_detector.callback_count - 1; j++) {
                g_nic_detector.callbacks[j] = g_nic_detector.callbacks[j + 1];
            }
            g_nic_detector.callback_count--;
            LeaveCriticalSection(&g_nic_detector.mutex);
            return 0;
        }
    }
    
    LeaveCriticalSection(&g_nic_detector.mutex);
    return -1;
}

/**
 * @brief Initialize NIC detector module
 */
int nic_detector_init(void)
{
    if (g_nic_detector.initialized) {
        return 0; /* Already initialized */
    }
    
    /* Initialize critical section */
    InitializeCriticalSection(&g_nic_detector.mutex);
    
    /* Initialize priority list */
    g_nic_detector.priorities = NULL;
    g_nic_detector.priority_count = 0;
    
    /* Initialize callback list */
    g_nic_detector.callbacks = NULL;
    g_nic_detector.callback_count = 0;
    
    /* Create event monitor stop event */
    g_nic_detector.event_monitor_stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_nic_detector.event_monitor_stop_event) {
        BONDING_LOG_ERROR("nic_detector_init: Failed to create event monitor stop event");
        DeleteCriticalSection(&g_nic_detector.mutex);
        return -1;
    }
    
    /* Start event monitoring thread */
    g_nic_detector.event_monitor_thread = CreateThread(NULL, 0, EventMonitorThread, NULL, 0, NULL);
    if (!g_nic_detector.event_monitor_thread) {
        BONDING_LOG_ERROR("nic_detector_init: Failed to create event monitor thread");
        CloseHandle(g_nic_detector.event_monitor_stop_event);
        g_nic_detector.event_monitor_stop_event = NULL;
        DeleteCriticalSection(&g_nic_detector.mutex);
        return -1;
    }
    
    g_nic_detector.initialized = 1;
    BONDING_LOG_INFO("NIC detector initialized");
    return 0;
}

/**
 * @brief Cleanup NIC detector module
 */
void nic_detector_cleanup(void)
{
    int i;
    
    if (!g_nic_detector.initialized) {
        return;
    }
    
    /* Stop quality monitoring */
    if (g_nic_detector.quality_monitor_stop_event) {
        SetEvent(g_nic_detector.quality_monitor_stop_event);
    }
    if (g_nic_detector.quality_monitor_thread) {
        WaitForSingleObject(g_nic_detector.quality_monitor_thread, 5000);
        CloseHandle(g_nic_detector.quality_monitor_thread);
        g_nic_detector.quality_monitor_thread = NULL;
    }
    if (g_nic_detector.quality_monitor_stop_event) {
        CloseHandle(g_nic_detector.quality_monitor_stop_event);
        g_nic_detector.quality_monitor_stop_event = NULL;
    }
    
    /* Stop event monitoring */
    if (g_nic_detector.event_monitor_stop_event) {
        SetEvent(g_nic_detector.event_monitor_stop_event);
    }
    if (g_nic_detector.event_monitor_thread) {
        WaitForSingleObject(g_nic_detector.event_monitor_thread, 5000);
        CloseHandle(g_nic_detector.event_monitor_thread);
        g_nic_detector.event_monitor_thread = NULL;
    }
    if (g_nic_detector.event_monitor_stop_event) {
        CloseHandle(g_nic_detector.event_monitor_stop_event);
        g_nic_detector.event_monitor_stop_event = NULL;
    }
    
    EnterCriticalSection(&g_nic_detector.mutex);
    
    /* Free priorities */
    if (g_nic_detector.priorities) {
        for (i = 0; i < g_nic_detector.priority_count; i++) {
            if (g_nic_detector.priorities[i].nic_name) {
                free(g_nic_detector.priorities[i].nic_name);
            }
        }
        free(g_nic_detector.priorities);
        g_nic_detector.priorities = NULL;
        g_nic_detector.priority_count = 0;
    }
    
    /* Free callbacks */
    if (g_nic_detector.callbacks) {
        free(g_nic_detector.callbacks);
        g_nic_detector.callbacks = NULL;
        g_nic_detector.callback_count = 0;
    }
    
    /* Free ping target */
    if (g_nic_detector.ping_target) {
        free(g_nic_detector.ping_target);
        g_nic_detector.ping_target = NULL;
    }
    
    LeaveCriticalSection(&g_nic_detector.mutex);
    
    /* Delete critical section */
    DeleteCriticalSection(&g_nic_detector.mutex);
    
    g_nic_detector.initialized = 0;
    BONDING_LOG_INFO("NIC detector cleaned up");
}

/**
 * @brief Free NIC info structure (public API)
 */
void nic_detector_free_info(nic_info_t *nic)
{
    FreeNICInfo(nic);
}
