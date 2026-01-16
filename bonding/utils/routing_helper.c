/**
 * @file routing_helper.c
 * @brief Windows routing API helper functions
 * 
 * Implements routing table manipulation for binding traffic to specific NICs
 */

#include "routing_helper.h"
#include "nic_detector.h"
#include <windows.h>
#include <iphlpapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* External logging functions */
extern void MsgToEventLog(WORD type, wchar_t *format, ...);
extern WCHAR* Widen(const char *utf8);

#ifdef DEBUG
extern void PrintDebug(TCHAR *format, ...);
#else
#define PrintDebug(...) do {} while(0)
#endif

/* Logging macros */
#define BONDING_LOG_ERROR(...) do { \
    WCHAR *wmsg = Widen(__VA_ARGS__); \
    if (wmsg) { \
        MsgToEventLog(EVENTLOG_ERROR_TYPE, L"[Routing Helper] %ls", wmsg); \
        free(wmsg); \
    } \
} while(0)

#define BONDING_LOG_INFO(...) do { \
    WCHAR *wmsg = Widen(__VA_ARGS__); \
    if (wmsg) { \
        MsgToEventLog(EVENTLOG_INFORMATION_TYPE, L"[Routing Helper] %ls", wmsg); \
        free(wmsg); \
    } \
} while(0)

#ifdef DEBUG
#define BONDING_LOG_DEBUG(...) PrintDebug(L"[Routing Helper] " __VA_ARGS__)
#else
#define BONDING_LOG_DEBUG(...) do {} while(0)
#endif

/* Internal route tracking structure */
typedef struct {
    char *nic_name;
    char *destination;
    MIB_IPFORWARDROW route;
    int valid;
} route_entry_t;

/* Module state */
static struct {
    route_entry_t *routes;
    int route_count;
    int route_capacity;
    CRITICAL_SECTION mutex;
} g_routing_helper = {0};

/**
 * @brief Initialize routing helper (called automatically on first use)
 */
static void InitRoutingHelper(void)
{
    if (g_routing_helper.mutex.DebugInfo) {
        return; /* Already initialized */
    }
    
    InitializeCriticalSection(&g_routing_helper.mutex);
    g_routing_helper.route_capacity = 32;
    g_routing_helper.routes = (route_entry_t*)calloc(g_routing_helper.route_capacity, sizeof(route_entry_t));
    g_routing_helper.route_count = 0;
}

/**
 * @brief Find route entry
 */
static route_entry_t* FindRouteEntry(const char *nic_name, const char *destination)
{
    int i;
    
    if (!g_routing_helper.mutex.DebugInfo) {
        InitRoutingHelper();
    }
    
    EnterCriticalSection(&g_routing_helper.mutex);
    
    for (i = 0; i < g_routing_helper.route_count; i++) {
        if (g_routing_helper.routes[i].valid &&
            strcmp(g_routing_helper.routes[i].nic_name, nic_name) == 0 &&
            strcmp(g_routing_helper.routes[i].destination, destination) == 0) {
            LeaveCriticalSection(&g_routing_helper.mutex);
            return &g_routing_helper.routes[i];
        }
    }
    
    LeaveCriticalSection(&g_routing_helper.mutex);
    return NULL;
}

/**
 * @brief Add route entry to tracking list
 */
static int AddRouteEntry(const char *nic_name, const char *destination, const MIB_IPFORWARDROW *route)
{
    route_entry_t *entry;
    
    if (!g_routing_helper.mutex.DebugInfo) {
        InitRoutingHelper();
    }
    
    EnterCriticalSection(&g_routing_helper.mutex);
    
    /* Check if already exists */
    entry = FindRouteEntry(nic_name, destination);
    if (entry) {
        LeaveCriticalSection(&g_routing_helper.mutex);
        return 0; /* Already exists */
    }
    
    /* Expand array if needed */
    if (g_routing_helper.route_count >= g_routing_helper.route_capacity) {
        int new_capacity = g_routing_helper.route_capacity * 2;
        route_entry_t *new_routes = (route_entry_t*)realloc(g_routing_helper.routes,
                                                             new_capacity * sizeof(route_entry_t));
        if (!new_routes) {
            BONDING_LOG_ERROR("AddRouteEntry: Out of memory");
            LeaveCriticalSection(&g_routing_helper.mutex);
            return -1;
        }
        g_routing_helper.routes = new_routes;
        g_routing_helper.route_capacity = new_capacity;
        /* Initialize new entries */
        memset(&g_routing_helper.routes[g_routing_helper.route_count], 0,
               (new_capacity - g_routing_helper.route_count) * sizeof(route_entry_t));
    }
    
    /* Add new entry */
    entry = &g_routing_helper.routes[g_routing_helper.route_count];
    entry->nic_name = (char*)malloc(strlen(nic_name) + 1);
    entry->destination = (char*)malloc(strlen(destination) + 1);
    
    if (!entry->nic_name || !entry->destination) {
        if (entry->nic_name) free(entry->nic_name);
        if (entry->destination) free(entry->destination);
        BONDING_LOG_ERROR("AddRouteEntry: Out of memory");
        LeaveCriticalSection(&g_routing_helper.mutex);
        return -1;
    }
    
    strcpy(entry->nic_name, nic_name);
    strcpy(entry->destination, destination);
    memcpy(&entry->route, route, sizeof(MIB_IPFORWARDROW));
    entry->valid = 1;
    g_routing_helper.route_count++;
    
    LeaveCriticalSection(&g_routing_helper.mutex);
    return 0;
}

/**
 * @brief Remove route entry from tracking list
 */
static void RemoveRouteEntry(const char *nic_name, const char *destination)
{
    int i;
    
    if (!g_routing_helper.mutex.DebugInfo) {
        return;
    }
    
    EnterCriticalSection(&g_routing_helper.mutex);
    
    for (i = 0; i < g_routing_helper.route_count; i++) {
        if (g_routing_helper.routes[i].valid &&
            strcmp(g_routing_helper.routes[i].nic_name, nic_name) == 0 &&
            strcmp(g_routing_helper.routes[i].destination, destination) == 0) {
            /* Mark as invalid */
            g_routing_helper.routes[i].valid = 0;
            if (g_routing_helper.routes[i].nic_name) {
                free(g_routing_helper.routes[i].nic_name);
                g_routing_helper.routes[i].nic_name = NULL;
            }
            if (g_routing_helper.routes[i].destination) {
                free(g_routing_helper.routes[i].destination);
                g_routing_helper.routes[i].destination = NULL;
            }
            break;
        }
    }
    
    LeaveCriticalSection(&g_routing_helper.mutex);
}

/**
 * @brief Bind traffic to specific NIC using routing rules
 */
int routing_helper_bind_to_nic(const char *nic_name, const char *destination)
{
    PIP_ADAPTER_INFO adapter_list = NULL;
    PIP_ADAPTER_INFO adapter = NULL;
    ULONG adapter_list_size = 0;
    DWORD status;
    MIB_IPFORWARDROW route;
    DWORD route_metric = 1;
    struct sockaddr_in dest_addr;
    struct hostent *host = NULL;
    int nic_priority;
    
    if (!nic_name || !destination) {
        BONDING_LOG_ERROR("routing_helper_bind_to_nic: Invalid parameters");
        return -1;
    }
    
    if (!g_routing_helper.mutex.DebugInfo) {
        InitRoutingHelper();
    }
    
    /* Get adapter information */
    status = GetAdaptersInfo(NULL, &adapter_list_size);
    if (status != ERROR_BUFFER_OVERFLOW) {
        BONDING_LOG_ERROR("routing_helper_bind_to_nic: Failed to get adapter list size");
        return -1;
    }
    
    adapter_list = (PIP_ADAPTER_INFO)malloc(adapter_list_size);
    if (!adapter_list) {
        BONDING_LOG_ERROR("routing_helper_bind_to_nic: Out of memory");
        return -1;
    }
    
    status = GetAdaptersInfo(adapter_list, &adapter_list_size);
    if (status != ERROR_SUCCESS) {
        BONDING_LOG_ERROR("routing_helper_bind_to_nic: Failed to get adapter list");
        free(adapter_list);
        return -1;
    }
    
    /* Find adapter matching nic_name */
    adapter = adapter_list;
    while (adapter) {
        if (strcmp(adapter->AdapterName, nic_name) == 0 ||
            (adapter->Description && strcmp(adapter->Description, nic_name) == 0)) {
            break;
        }
        adapter = adapter->Next;
    }
    
    if (!adapter) {
        BONDING_LOG_ERROR("routing_helper_bind_to_nic: NIC not found: %s", nic_name);
        free(adapter_list);
        return -1;
    }
    
    /* Get NIC priority to derive metric (lower priority = higher metric = lower preference) */
    nic_priority = nic_detector_get_priority(nic_name);
    /* Convert priority (1-10) to metric: priority 10 = metric 1, priority 1 = metric 10 */
    /* Use inverse relationship so higher priority NICs get lower metrics */
    route_metric = (DWORD)(11 - nic_priority);
    if (route_metric < 1) route_metric = 1;
    if (route_metric > 10) route_metric = 10;
    
    /* Convert destination to IP address */
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    
    /* Try to parse as IP address first */
    dest_addr.sin_addr.s_addr = inet_addr(destination);
    if (dest_addr.sin_addr.s_addr == INADDR_NONE) {
        /* Try to resolve hostname */
        host = gethostbyname(destination);
        if (host && host->h_addr_list[0]) {
            memcpy(&dest_addr.sin_addr, host->h_addr_list[0], sizeof(struct in_addr));
        } else {
            BONDING_LOG_ERROR("routing_helper_bind_to_nic: Invalid destination or failed to resolve: %s", destination);
            free(adapter_list);
            return -1;
        }
    }
    
    /* Initialize route entry */
    memset(&route, 0, sizeof(route));
    route.dwForwardDest = dest_addr.sin_addr.s_addr;
    route.dwForwardMask = 0xFFFFFFFF; /* 255.255.255.255 - host route */
    route.dwForwardIfIndex = adapter->Index;
    route.dwForwardType = MIB_IPROUTE_TYPE_INDIRECT;
    route.dwForwardProto = MIB_IPPROTO_NETMGMT;
    route.dwForwardAge = 0;
    
    /* Get gateway IP - use first gateway if available */
    if (adapter->GatewayList.IpAddress.String[0] != '\0') {
        route.dwForwardNextHop = inet_addr(adapter->GatewayList.IpAddress.String);
    } else {
        /* No gateway - use adapter's IP as next hop for direct connection */
        if (adapter->IpAddressList.IpAddress.String[0] != '\0') {
            route.dwForwardNextHop = inet_addr(adapter->IpAddressList.IpAddress.String);
        } else {
            BONDING_LOG_ERROR("routing_helper_bind_to_nic: No IP address or gateway for NIC: %s", nic_name);
            free(adapter_list);
            return -1;
        }
    }
    
    route.dwForwardMetric1 = route_metric;
    
    /* Create route */
    status = CreateIpForwardEntry(&route);
    if (status != ERROR_SUCCESS && status != ERROR_OBJECT_ALREADY_EXISTS) {
        BONDING_LOG_ERROR("routing_helper_bind_to_nic: Failed to create route (error %lu)", status);
        free(adapter_list);
        return -1;
    }
    
    /* Add to tracking list */
    if (AddRouteEntry(nic_name, destination, &route) == 0) {
        BONDING_LOG_INFO("routing_helper_bind_to_nic: Route created for %s via %s", destination, nic_name);
    }
    
    free(adapter_list);
    return 0;
}

/**
 * @brief Remove binding for specific NIC and destination
 */
int routing_helper_remove_binding(const char *nic_name, const char *destination)
{
    route_entry_t *entry;
    DWORD status;
    
    if (!nic_name || !destination) {
        BONDING_LOG_ERROR("routing_helper_remove_binding: Invalid parameters");
        return -1;
    }
    
    if (!g_routing_helper.mutex.DebugInfo) {
        return 0; /* Nothing to remove */
    }
    
    /* Find route entry */
    entry = FindRouteEntry(nic_name, destination);
    if (!entry) {
        BONDING_LOG_DEBUG("routing_helper_remove_binding: Route not found for %s via %s", destination, nic_name);
        return 0; /* Not an error if route doesn't exist */
    }
    
    /* Delete route */
    status = DeleteIpForwardEntry(&entry->route);
    if (status != ERROR_SUCCESS) {
        BONDING_LOG_ERROR("routing_helper_remove_binding: Failed to delete route (error %lu)", status);
        /* Continue to remove from tracking list anyway */
    } else {
        BONDING_LOG_INFO("routing_helper_remove_binding: Route removed for %s via %s", destination, nic_name);
    }
    
    /* Remove from tracking list */
    RemoveRouteEntry(nic_name, destination);
    
    return 0;
}

/**
 * @brief Restore all routes (cleanup on shutdown)
 */
int routing_helper_restore_routes(void)
{
    int i;
    DWORD status;
    int removed_count = 0;
    
    if (!g_routing_helper.mutex.DebugInfo) {
        return 0; /* Nothing to restore */
    }
    
    EnterCriticalSection(&g_routing_helper.mutex);
    
    for (i = 0; i < g_routing_helper.route_count; i++) {
        if (g_routing_helper.routes[i].valid) {
            status = DeleteIpForwardEntry(&g_routing_helper.routes[i].route);
            if (status == ERROR_SUCCESS) {
                removed_count++;
            }
            
            /* Free memory */
            if (g_routing_helper.routes[i].nic_name) {
                free(g_routing_helper.routes[i].nic_name);
            }
            if (g_routing_helper.routes[i].destination) {
                free(g_routing_helper.routes[i].destination);
            }
        }
    }
    
    /* Free routes array */
    if (g_routing_helper.routes) {
        free(g_routing_helper.routes);
        g_routing_helper.routes = NULL;
    }
    
    g_routing_helper.route_count = 0;
    g_routing_helper.route_capacity = 0;
    
    LeaveCriticalSection(&g_routing_helper.mutex);
    
    /* Delete critical section */
    DeleteCriticalSection(&g_routing_helper.mutex);
    memset(&g_routing_helper.mutex, 0, sizeof(CRITICAL_SECTION));
    
    BONDING_LOG_INFO("routing_helper_restore_routes: Restored %d routes", removed_count);
    return 0;
}

/**
 * @brief Set route metric (adjust priority)
 */
int routing_helper_set_metric(const char *nic_name, const char *destination, int metric)
{
    route_entry_t *entry;
    DWORD status;
    
    if (!nic_name || !destination || metric < 1) {
        BONDING_LOG_ERROR("routing_helper_set_metric: Invalid parameters");
        return -1;
    }
    
    if (!g_routing_helper.mutex.DebugInfo) {
        return -1;
    }
    
    /* Find route entry */
    entry = FindRouteEntry(nic_name, destination);
    if (!entry) {
        BONDING_LOG_ERROR("routing_helper_set_metric: Route not found for %s via %s", destination, nic_name);
        return -1;
    }
    
    /* Update metric */
    entry->route.dwForwardMetric1 = (DWORD)metric;
    
    /* Update route */
    status = SetIpForwardEntry(&entry->route);
    if (status != ERROR_SUCCESS) {
        BONDING_LOG_ERROR("routing_helper_set_metric: Failed to set route metric (error %lu)", status);
        return -1;
    }
    
    BONDING_LOG_INFO("routing_helper_set_metric: Set metric %d for %s via %s", metric, destination, nic_name);
    return 0;
}

/**
 * @brief Update all route metrics for a given NIC based on its priority
 */
int routing_helper_update_nic_metrics(const char *nic_name, int priority)
{
    int i;
    int metric;
    int updated_count = 0;
    
    if (!nic_name || priority < 1 || priority > 10) {
        BONDING_LOG_ERROR("routing_helper_update_nic_metrics: Invalid parameters");
        return -1;
    }
    
    if (!g_routing_helper.mutex.DebugInfo) {
        return 0; /* No routes to update */
    }
    
    /* Convert priority to metric */
    metric = 11 - priority;
    if (metric < 1) metric = 1;
    if (metric > 10) metric = 10;
    
    EnterCriticalSection(&g_routing_helper.mutex);
    
    /* Update all routes for this NIC */
    for (i = 0; i < g_routing_helper.route_count; i++) {
        if (g_routing_helper.routes[i].valid &&
            strcmp(g_routing_helper.routes[i].nic_name, nic_name) == 0) {
            /* Update metric */
            g_routing_helper.routes[i].route.dwForwardMetric1 = (DWORD)metric;
            
            /* Update route in system */
            DWORD status = SetIpForwardEntry(&g_routing_helper.routes[i].route);
            if (status == ERROR_SUCCESS) {
                updated_count++;
            } else {
                BONDING_LOG_ERROR("routing_helper_update_nic_metrics: Failed to update route metric (error %lu)", status);
            }
        }
    }
    
    LeaveCriticalSection(&g_routing_helper.mutex);
    
    if (updated_count > 0) {
        BONDING_LOG_INFO("routing_helper_update_nic_metrics: Updated %d route metrics for %s to %d", 
                        updated_count, nic_name, metric);
    }
    
    return 0;
}
