#ifndef BONDING_SERVICE_H
#define BONDING_SERVICE_H

#include <windows.h>

/**
 * @file bonding_service.h
 * @brief Windows service definitions for bonding manager
 */

/* Service name constant */
#define BONDING_SERVICE_NAME L"OpenVPNBondingService"

/* Service display name and description */
#define BONDING_SERVICE_DISPLAY_NAME L"OpenVPN Bonding Service"
#define BONDING_SERVICE_DESCRIPTION L"Manages OpenVPN tunnel bonding with automatic failover and load balancing"

/* Service control codes for custom commands */
#define SERVICE_CONTROL_START_BONDING 128
#define SERVICE_CONTROL_STOP_BONDING 129
#define SERVICE_CONTROL_GET_STATUS 130
#define SERVICE_CONTROL_SET_CONFIG 131
#define SERVICE_CONTROL_FAILOVER 132

/**
 * @brief Service state enumeration
 */
typedef enum {
    BONDING_SERVICE_STATE_STOPPED = 0,
    BONDING_SERVICE_STATE_STARTING = 1,
    BONDING_SERVICE_STATE_RUNNING = 2,
    BONDING_SERVICE_STATE_STOPPING = 3,
    BONDING_SERVICE_STATE_PAUSED = 4
} bonding_service_state_t;

/* Service installation and management functions */
int InstallBondingService(void);
int UninstallBondingService(void);
int StartBondingService(void);
int StopBondingService(void);

/* Service main entry point (called by Windows SCM) */
void WINAPI ServiceMain(DWORD argc, LPTSTR *argv);

/* Service control handler */
DWORD WINAPI ServiceCtrlHandler(DWORD dwCtrl, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext);

#endif /* BONDING_SERVICE_H */
