/**
 * @file bonding_manager.c
 * @brief Main bonding manager implementation
 * 
 * Coordinates OpenVPN manager and packet distributor
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bonding_manager.h"
#include "openvpn_manager.h"
#include "packet_distributor.h"
#include "bonding_config.h"
#include "bonding_types.h"
#include "nic_detector.h"
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

/* External functions */
extern void MsgToEventLog(WORD type, wchar_t *format, ...);
extern WCHAR* Widen(const char *utf8);
extern char* WCharToUTF8(const WCHAR *wstr);

#ifdef DEBUG
extern void PrintDebug(TCHAR *format, ...);
#else
#define PrintDebug(...) do {} while(0)
#endif

/* Logging macros */
#define BONDING_LOG_ERROR(...) bonding_log(3, __VA_ARGS__)
#define BONDING_LOG_WARN(...) bonding_log(2, __VA_ARGS__)
#define BONDING_LOG_INFO(...) bonding_log(1, __VA_ARGS__)
#define BONDING_LOG_DEBUG(...) bonding_log(0, __VA_ARGS__)

/* Forward declarations */
static DWORD WINAPI HealthMonitorThread(LPVOID lpParam);
static void NICEventCallback(nic_event_type_t event, const char *nic_name, void *user_data);

/**
 * @brief Bonding manager structure
 */
struct bonding_manager {
    openvpn_manager_t *openvpn_mgr;      /* OpenVPN manager instance */
    packet_distributor_t *packet_dist;    /* Packet distributor instance */
    bonding_profile_t *profile;          /* Current bonding profile */
    int initialized;                      /* Initialization flag */
    bonding_state_t state;                /* Current bonding state */
    HANDLE health_monitor_thread;         /* Health monitoring thread handle */
    HANDLE health_monitor_stop_event;     /* Event to stop health monitor */
    CRITICAL_SECTION mutex;               /* Thread synchronization */
};

/**
 * @brief NIC event callback for failover
 */
static void NICEventCallback(nic_event_type_t event, const char *nic_name, void *user_data)
{
    struct bonding_manager *mgr = (struct bonding_manager*)user_data;
    
    if (!mgr || !mgr->initialized) {
        return;
    }
    
    BONDING_LOG_INFO("NIC event: %d for NIC: %s", event, nic_name ? nic_name : "unknown");
    
    /* Handle disconnect events */
    if (event == NIC_EVENT_DISCONNECTED && nic_name) {
        EnterCriticalSection(&mgr->mutex);
        
        /* Find tunnels using this NIC and trigger failover */
        if (mgr->profile) {
            for (int i = 0; i < mgr->profile->tunnel_count; i++) {
                tunnel_config_t *tunnel = &mgr->profile->tunnels[i];
                if (tunnel->nic_name && strcmp(tunnel->nic_name, nic_name) == 0) {
                    BONDING_LOG_WARN("NIC %s disconnected, triggering failover for tunnel %d", nic_name, i);
                    /* Mark tunnel as failed - health monitor will handle failover */
                    if (mgr->packet_dist) {
                        packet_distributor_set_tunnel_health(mgr->packet_dist, i, 0);
                    }
                }
            }
        }
        
        LeaveCriticalSection(&mgr->mutex);
    }
}

/**
 * @brief Create bonding manager
 */
bonding_manager_t* bonding_manager_create(void)
{
    struct bonding_manager *mgr;
    
    /* Initialize NIC detector */
    if (nic_detector_init() != 0) {
        BONDING_LOG_ERROR("Failed to initialize NIC detector");
        return NULL;
    }
    
    /* Start quality monitoring */
    if (nic_detector_monitor_quality() != 0) {
        BONDING_LOG_WARN("Failed to start quality monitoring");
    }
    
    /* Allocate manager structure */
    mgr = (struct bonding_manager*)calloc(1, sizeof(struct bonding_manager));
    if (!mgr) {
        BONDING_LOG_ERROR("Out of memory allocating bonding manager");
        nic_detector_cleanup();
        return NULL;
    }
    
    /* Initialize critical section */
    InitializeCriticalSection(&mgr->mutex);
    
    /* Create OpenVPN manager */
    mgr->openvpn_mgr = openvpn_manager_create();
    if (!mgr->openvpn_mgr) {
        BONDING_LOG_ERROR("Failed to create OpenVPN manager");
        DeleteCriticalSection(&mgr->mutex);
        free(mgr);
        nic_detector_cleanup();
        return NULL;
    }
    
    /* Register NIC event callback */
    nic_detector_register_callback(NICEventCallback, mgr);
    
    mgr->initialized = 0;
    mgr->state = BONDING_STATE_STOPPED;
    mgr->health_monitor_thread = NULL;
    mgr->health_monitor_stop_event = NULL;
    
    BONDING_LOG_INFO("Bonding manager created successfully");
    return mgr;
}

/**
 * @brief Start bonding manager with profile
 */
int bonding_manager_start(bonding_manager_t *mgr, bonding_profile_t *profile)
{
    struct bonding_manager *m = (struct bonding_manager*)mgr;
    int i;
    tunnel_state_t tunnel_state;
    char tap_adapter_name[256];
    
    if (!m || !profile) {
        BONDING_LOG_ERROR("Invalid parameters for bonding_manager_start");
        return -1;
    }
    
    EnterCriticalSection(&m->mutex);
    
    if (m->initialized) {
        BONDING_LOG_WARN("Bonding manager already started");
        LeaveCriticalSection(&m->mutex);
        return -1;
    }
    
    /* Update state to starting */
    m->state = BONDING_STATE_STARTING;
    
    /* Store profile */
    m->profile = profile;
    
    /* Create packet distributor */
    m->packet_dist = packet_distributor_create(profile->mode, profile->tunnel_count);
    if (!m->packet_dist) {
        BONDING_LOG_ERROR("Failed to create packet distributor");
        m->state = BONDING_STATE_FAILED;
        LeaveCriticalSection(&m->mutex);
        return -1;
    }
    
    /* Configure packet distributor from profile */
    packet_distributor_config_t dist_config = {0};
    dist_config.queue_size = profile->packet_queue_size;
    dist_config.io_timeout_ms = profile->packet_timeout;
    dist_config.sequencing_enabled = profile->sequencing_enabled;
    dist_config.flow_control_enabled = profile->flow_control_enabled;
    dist_config.retry_count = 3;
    
    packet_distributor_configure(m->packet_dist, &dist_config);
    
    /* Spawn OpenVPN instances for each tunnel */
    for (i = 0; i < profile->tunnel_count; i++) {
        tunnel_config_t *tunnel = &profile->tunnels[i];
        
        /* Generate TAP adapter name */
        snprintf(tap_adapter_name, sizeof(tap_adapter_name), "TAP-Bonding-%d", i);
        
        /* Store TAP adapter name in tunnel config */
        if (tunnel->tap_adapter) {
            free(tunnel->tap_adapter);
        }
        tunnel->tap_adapter = duplicate_string(tap_adapter_name);
        
        /* Spawn OpenVPN instance */
        if (openvpn_manager_spawn_instance(m->openvpn_mgr, i,
                                          tunnel->config_file,
                                          tap_adapter_name,
                                          tunnel->nic_name,
                                          tunnel->server_host) != 0) {
            BONDING_LOG_ERROR("Failed to spawn OpenVPN instance for tunnel %d", i);
            /* Continue with other tunnels */
            continue;
        }
        
        /* Set tunnel weight in packet distributor */
        if (profile->mode == BONDING_MODE_WEIGHTED || profile->mode == BONDING_MODE_ADAPTIVE) {
            packet_distributor_set_tunnel_weight(m->packet_dist, i, tunnel->weight);
        }
        
        /* Open TAP adapter in packet distributor */
        /* Wait a bit for TAP adapter to be created by OpenVPN */
        Sleep(2000);
        if (packet_distributor_set_tunnel_tap_adapter(m->packet_dist, i, tap_adapter_name) != 0) {
            BONDING_LOG_WARN("Failed to open TAP adapter for tunnel %d (will retry)", i);
            /* Mark tunnel as unhealthy until TAP adapter is successfully opened */
            packet_distributor_set_tunnel_health(m->packet_dist, i, 0);
        } else {
            /* Only set health to healthy once TAP adapter handle has been successfully opened */
            packet_distributor_set_tunnel_health(m->packet_dist, i, 1);
        }
    }
    
    m->initialized = 1;
    
    /* Create health monitor stop event */
    m->health_monitor_stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!m->health_monitor_stop_event) {
        BONDING_LOG_ERROR("Failed to create health monitor stop event");
        m->state = BONDING_STATE_FAILED;
        LeaveCriticalSection(&m->mutex);
        return -1;
    }
    
    /* Start health monitoring thread */
    m->health_monitor_thread = CreateThread(NULL, 0, HealthMonitorThread, m, 0, NULL);
    if (!m->health_monitor_thread) {
        BONDING_LOG_ERROR("Failed to create health monitor thread");
        CloseHandle(m->health_monitor_stop_event);
        m->health_monitor_stop_event = NULL;
        m->state = BONDING_STATE_FAILED;
        LeaveCriticalSection(&m->mutex);
        return -1;
    }
    
    /* Update state to running */
    m->state = BONDING_STATE_RUNNING;
    
    LeaveCriticalSection(&m->mutex);
    
    BONDING_LOG_INFO("Bonding manager started with %d tunnels", profile->tunnel_count);
    return 0;
}

/**
 * @brief Health monitoring thread function
 */
static DWORD WINAPI HealthMonitorThread(LPVOID lpParam)
{
    struct bonding_manager *m = (struct bonding_manager*)lpParam;
    tunnel_state_t states[32];
    int tunnel_count;
    int i;
    int healthy_count;
    int failed_count;
    time_t last_recovery_attempt = 0;
    const int RECOVERY_INTERVAL = 30; /* Wait 30 seconds between recovery attempts */
    
    BONDING_LOG_INFO("Health monitor thread started");
    
    while (WaitForSingleObject(m->health_monitor_stop_event, 5000) == WAIT_TIMEOUT) {
        EnterCriticalSection(&m->mutex);
        
        if (!m->initialized || !m->profile) {
            LeaveCriticalSection(&m->mutex);
            continue;
        }
        
        /* Get status of all tunnels */
        tunnel_count = bonding_manager_get_status((bonding_manager_t*)m, states, 32);
        if (tunnel_count < 0) {
            LeaveCriticalSection(&m->mutex);
            continue;
        }
        
        healthy_count = 0;
        failed_count = 0;
        
        /* Check each tunnel */
        for (i = 0; i < tunnel_count; i++) {
            if (states[i] == TUNNEL_STATE_CONNECTED) {
                healthy_count++;
                
                /* Check NIC quality for connected tunnels */
                if (m->profile && i < m->profile->tunnel_count) {
                    tunnel_config_t *tunnel = &m->profile->tunnels[i];
                    if (tunnel->nic_name) {
                        nic_quality_t quality;
                        if (nic_detector_get_quality(tunnel->nic_name, &quality) == 0) {
                            /* Trigger failover if quality is poor */
                            if (quality.quality_score < 30) {
                                BONDING_LOG_WARN("Tunnel %d NIC %s has poor quality (score: %d), marking unhealthy", 
                                                i, tunnel->nic_name, quality.quality_score);
                                if (m->packet_dist) {
                                    packet_distributor_set_tunnel_health(m->packet_dist, i, 0);
                                }
                                healthy_count--;
                                failed_count++;
                            }
                        }
                    }
                }
            } else if (states[i] == TUNNEL_STATE_FAILED) {
                failed_count++;
                
                /* Mark tunnel as unhealthy in packet distributor */
                if (m->packet_dist) {
                    packet_distributor_set_tunnel_health(m->packet_dist, i, 0);
                }
            }
        }
        
        /* Update bonding state based on tunnel health */
        if (healthy_count == 0 && tunnel_count > 0) {
            /* All tunnels failed */
            if (m->state != BONDING_STATE_FAILED) {
                m->state = BONDING_STATE_FAILED;
                BONDING_LOG_ERROR("All tunnels failed - bonding state changed to FAILED");
            }
        } else if (failed_count > 0 && healthy_count > 0) {
            /* Some tunnels failed, but at least one is healthy */
            if (m->state != BONDING_STATE_RECOVERING) {
                m->state = BONDING_STATE_RECOVERING;
                BONDING_LOG_WARN("Some tunnels failed - bonding state changed to RECOVERING");
            }
            
            /* Attempt recovery for failed tunnels */
            time_t now = time(NULL);
            if (now - last_recovery_attempt >= RECOVERY_INTERVAL) {
                for (i = 0; i < tunnel_count; i++) {
                    if (states[i] == TUNNEL_STATE_FAILED || states[i] == TUNNEL_STATE_DISCONNECTED) {
                        BONDING_LOG_INFO("Attempting recovery for tunnel %d", i);
                        /* Try to restart the tunnel */
                        if (m->profile && i < m->profile->tunnel_count) {
                            tunnel_config_t *tunnel = &m->profile->tunnels[i];
                            if (tunnel->tap_adapter) {
                                /* Retry opening TAP adapter */
                                if (packet_distributor_set_tunnel_tap_adapter(m->packet_dist, i, tunnel->tap_adapter) == 0) {
                                    BONDING_LOG_INFO("Successfully recovered TAP adapter for tunnel %d", i);
                                    packet_distributor_set_tunnel_health(m->packet_dist, i, 1);
                                }
                            }
                        }
                    }
                }
                last_recovery_attempt = now;
            }
        } else if (healthy_count == tunnel_count && tunnel_count > 0) {
            /* All tunnels healthy */
            if (m->state != BONDING_STATE_RUNNING) {
                m->state = BONDING_STATE_RUNNING;
                BONDING_LOG_INFO("All tunnels healthy - bonding state changed to RUNNING");
            }
        }
        
        LeaveCriticalSection(&m->mutex);
    }
    
    BONDING_LOG_INFO("Health monitor thread stopped");
    return 0;
}

/**
 * @brief Stop bonding manager
 */
int bonding_manager_stop(bonding_manager_t *mgr)
{
    struct bonding_manager *m = (struct bonding_manager*)mgr;
    int i;
    
    if (!m) {
        BONDING_LOG_ERROR("Invalid parameters for bonding_manager_stop");
        return -1;
    }
    
    EnterCriticalSection(&m->mutex);
    
    if (!m->initialized) {
        BONDING_LOG_WARN("Bonding manager not started");
        LeaveCriticalSection(&m->mutex);
        return -1;
    }
    
    /* Update state to stopping */
    m->state = BONDING_STATE_STOPPING;
    
    /* Stop health monitoring thread */
    if (m->health_monitor_stop_event) {
        SetEvent(m->health_monitor_stop_event);
    }
    
    LeaveCriticalSection(&m->mutex);
    
    /* Wait for health monitor thread to finish */
    if (m->health_monitor_thread) {
        WaitForSingleObject(m->health_monitor_thread, 5000);
        CloseHandle(m->health_monitor_thread);
        m->health_monitor_thread = NULL;
    }
    
    if (m->health_monitor_stop_event) {
        CloseHandle(m->health_monitor_stop_event);
        m->health_monitor_stop_event = NULL;
    }
    
    EnterCriticalSection(&m->mutex);
    
    /* Stop packet distributor first (flushes queues) */
    if (m->packet_dist) {
        packet_distributor_destroy(m->packet_dist);
        m->packet_dist = NULL;
    }
    
    /* Stop all OpenVPN instances */
    if (m->openvpn_mgr && m->profile) {
        for (i = 0; i < m->profile->tunnel_count; i++) {
            openvpn_manager_stop_instance(m->openvpn_mgr, i);
        }
    }
    
    m->initialized = 0;
    m->profile = NULL;
    m->state = BONDING_STATE_STOPPED;
    
    LeaveCriticalSection(&m->mutex);
    
    BONDING_LOG_INFO("Bonding manager stopped");
    return 0;
}

/**
 * @brief Get current bonding state
 */
bonding_state_t bonding_manager_get_state(bonding_manager_t *mgr)
{
    struct bonding_manager *m = (struct bonding_manager*)mgr;
    bonding_state_t state;
    
    if (!m) {
        return BONDING_STATE_STOPPED;
    }
    
    EnterCriticalSection(&m->mutex);
    state = m->state;
    LeaveCriticalSection(&m->mutex);
    
    return state;
}

/**
 * @brief Get status of all tunnels
 */
int bonding_manager_get_status(bonding_manager_t *mgr, tunnel_state_t *states, int max_tunnels)
{
    struct bonding_manager *m = (struct bonding_manager*)mgr;
    int i;
    tunnel_state_t state;
    
    if (!m || !states || max_tunnels <= 0) {
        BONDING_LOG_ERROR("Invalid parameters for bonding_manager_get_status");
        return -1;
    }
    
    EnterCriticalSection(&m->mutex);
    
    if (!m->initialized || !m->profile) {
        LeaveCriticalSection(&m->mutex);
        return -1;
    }
    
    int tunnel_count = (m->profile->tunnel_count < max_tunnels) ? m->profile->tunnel_count : max_tunnels;
    
    /* Get status from OpenVPN manager */
    for (i = 0; i < tunnel_count; i++) {
        tunnel_config_t *tunnel = &m->profile->tunnels[i];
        int tap_adapter_open = 0;
        
        /* Retry TAP adapter opening for unhealthy tunnels */
        if (m->packet_dist && tunnel->tap_adapter) {
            /* Try to open TAP adapter if it failed previously (retry on status poll) */
            if (packet_distributor_set_tunnel_tap_adapter(m->packet_dist, i, tunnel->tap_adapter) == 0) {
                tap_adapter_open = 1;
                /* TAP adapter successfully opened */
                if (openvpn_manager_get_instance_status(m->openvpn_mgr, i, &state) == 0 && 
                    state == TUNNEL_STATE_CONNECTED) {
                    BONDING_LOG_INFO("Successfully opened TAP adapter for tunnel %d on retry", i);
                }
            } else {
                /* TAP adapter still failed - mark unhealthy */
                tap_adapter_open = 0;
                packet_distributor_set_tunnel_health(m->packet_dist, i, 0);
            }
        }
        
        if (openvpn_manager_get_instance_status(m->openvpn_mgr, i, &state) == 0) {
            states[i] = state;
            
            /* Update packet distributor health status */
            /* Only mark healthy if both TAP adapter is open AND OpenVPN is connected */
            if (m->packet_dist) {
                int healthy = (state == TUNNEL_STATE_CONNECTED && tap_adapter_open) ? 1 : 0;
                packet_distributor_set_tunnel_health(m->packet_dist, i, healthy);
            }
        } else {
            states[i] = TUNNEL_STATE_DISCONNECTED;
            /* Mark unhealthy if OpenVPN is disconnected */
            if (m->packet_dist) {
                packet_distributor_set_tunnel_health(m->packet_dist, i, 0);
            }
        }
    }
    
    LeaveCriticalSection(&m->mutex);
    
    return tunnel_count;
}

/**
 * @brief Destroy bonding manager
 */
void bonding_manager_destroy(bonding_manager_t *mgr)
{
    struct bonding_manager *m = (struct bonding_manager*)mgr;
    
    if (!m)
        return;
    
    /* Unregister NIC event callback */
    nic_detector_unregister_callback(NICEventCallback);
    
    /* Stop manager if still running */
    if (m->initialized) {
        bonding_manager_stop(mgr);
    }
    
    /* Clean up health monitor thread if still exists */
    if (m->health_monitor_thread) {
        if (m->health_monitor_stop_event) {
            SetEvent(m->health_monitor_stop_event);
        }
        WaitForSingleObject(m->health_monitor_thread, 2000);
        CloseHandle(m->health_monitor_thread);
        m->health_monitor_thread = NULL;
    }
    
    if (m->health_monitor_stop_event) {
        CloseHandle(m->health_monitor_stop_event);
        m->health_monitor_stop_event = NULL;
    }
    
    /* Destroy OpenVPN manager */
    if (m->openvpn_mgr) {
        openvpn_manager_destroy(m->openvpn_mgr);
        m->openvpn_mgr = NULL;
    }
    
    /* Delete critical section */
    DeleteCriticalSection(&m->mutex);
    
    /* Free manager structure */
    free(m);
    
    /* Cleanup NIC detector */
    nic_detector_cleanup();
    
    BONDING_LOG_INFO("Bonding manager destroyed");
}
