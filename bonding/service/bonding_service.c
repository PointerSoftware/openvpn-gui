/**
 * @file bonding_service.c
 * @brief Windows service entry point for bonding manager
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "bonding_manager.h"
#include "bonding_service.h"
#include "service_ipc.h"
#include "bonding_config.h"

/* Global service state */
SERVICE_STATUS g_ServiceStatus = {0};
SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
HANDLE g_ServiceStopEvent = NULL;
bonding_manager_t *g_BondingManager = NULL;
CRITICAL_SECTION g_ServiceMutex;

/* External logging function */
extern void bonding_log(int level, const char *format, ...);
#define BONDING_LOG_ERROR(...) bonding_log(3, __VA_ARGS__)
#define BONDING_LOG_WARN(...) bonding_log(2, __VA_ARGS__)
#define BONDING_LOG_INFO(...) bonding_log(1, __VA_ARGS__)

/* Forward declarations */
static void ReportServiceStatus(DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint);
static DWORD WINAPI ServiceWorkerThread(LPVOID lpParam);

/**
 * @brief Report service status to SCM
 */
static void ReportServiceStatus(DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint)
{
    static DWORD dwCheckPoint = 1;

    g_ServiceStatus.dwCurrentState = dwCurrentState;
    g_ServiceStatus.dwWin32ExitCode = dwWin32ExitCode;
    g_ServiceStatus.dwWaitHint = dwWaitHint;

    if (dwCurrentState == SERVICE_START_PENDING)
        g_ServiceStatus.dwControlsAccepted = 0;
    else
        g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PAUSE_CONTINUE;

    if ((dwCurrentState == SERVICE_RUNNING) || (dwCurrentState == SERVICE_STOPPED))
        g_ServiceStatus.dwCheckPoint = 0;
    else
        g_ServiceStatus.dwCheckPoint = dwCheckPoint++;

    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
}

/**
 * @brief Service control handler
 */
DWORD WINAPI ServiceCtrlHandler(DWORD dwCtrl, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext)
{
    switch (dwCtrl)
    {
        case SERVICE_CONTROL_STOP:
            BONDING_LOG_INFO("Service stop requested");
            ReportServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 0);
            SetEvent(g_ServiceStopEvent);
            return NO_ERROR;

        case SERVICE_CONTROL_PAUSE:
            BONDING_LOG_INFO("Service pause requested");
            ReportServiceStatus(SERVICE_PAUSE_PENDING, NO_ERROR, 0);
            EnterCriticalSection(&g_ServiceMutex);
            if (g_BondingManager) {
                bonding_manager_stop(g_BondingManager);
            }
            LeaveCriticalSection(&g_ServiceMutex);
            g_ServiceStatus.dwCurrentState = SERVICE_PAUSED;
            ReportServiceStatus(SERVICE_PAUSED, NO_ERROR, 0);
            return NO_ERROR;

        case SERVICE_CONTROL_CONTINUE:
            BONDING_LOG_INFO("Service continue requested");
            ReportServiceStatus(SERVICE_CONTINUE_PENDING, NO_ERROR, 0);
            /* Note: Service will need to be restarted via IPC command */
            g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
            ReportServiceStatus(SERVICE_RUNNING, NO_ERROR, 0);
            return NO_ERROR;

        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;

        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

/**
 * @brief Service worker thread (handles IPC and bonding manager)
 */
static DWORD WINAPI ServiceWorkerThread(LPVOID lpParam)
{
    /* Initialize IPC server */
    if (service_ipc_init() != 0) {
        BONDING_LOG_ERROR("Failed to initialize IPC server");
        ReportServiceStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 0);
        return 1;
    }

    /* Create bonding manager */
    EnterCriticalSection(&g_ServiceMutex);
    g_BondingManager = bonding_manager_create();
    if (!g_BondingManager) {
        BONDING_LOG_ERROR("Failed to create bonding manager");
        LeaveCriticalSection(&g_ServiceMutex);
        service_ipc_cleanup();
        ReportServiceStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 0);
        return 1;
    }
    LeaveCriticalSection(&g_ServiceMutex);

    BONDING_LOG_INFO("Service worker thread started");

    /* Main service loop */
    while (WaitForSingleObject(g_ServiceStopEvent, 100) == WAIT_TIMEOUT)
    {
        /* Accept and process IPC connections */
        HANDLE hPipe = (HANDLE)service_ipc_accept_connection();
        if (hPipe != INVALID_HANDLE_VALUE && hPipe != NULL)
        {
            ipc_message_t request, response;
            if (service_ipc_process_message(hPipe, &request, &response) == 0)
            {
                /* Message processed successfully */
            }
            CloseHandle(hPipe);
        }
    }

    /* Cleanup */
    EnterCriticalSection(&g_ServiceMutex);
    if (g_BondingManager) {
        bonding_manager_stop(g_BondingManager);
        bonding_manager_destroy(g_BondingManager);
        g_BondingManager = NULL;
    }
    LeaveCriticalSection(&g_ServiceMutex);

    service_ipc_cleanup();
    BONDING_LOG_INFO("Service worker thread stopped");
    return 0;
}

/**
 * @brief Service main entry point (called by Windows SCM)
 */
void WINAPI ServiceMain(DWORD argc, LPTSTR *argv)
{
    HANDLE hThread = NULL;

    /* Register service control handler */
    g_StatusHandle = RegisterServiceCtrlHandlerEx(BONDING_SERVICE_NAME, ServiceCtrlHandler, NULL);
    if (!g_StatusHandle) {
        return;
    }

    /* Initialize service status */
    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwServiceSpecificExitCode = 0;

    /* Report starting status */
    ReportServiceStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    /* Create stop event */
    g_ServiceStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_ServiceStopEvent) {
        ReportServiceStatus(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    /* Initialize critical section */
    InitializeCriticalSection(&g_ServiceMutex);

    /* Report running status */
    ReportServiceStatus(SERVICE_RUNNING, NO_ERROR, 0);

    /* Start worker thread */
    hThread = CreateThread(NULL, 0, ServiceWorkerThread, NULL, 0, NULL);
    if (!hThread) {
        ReportServiceStatus(SERVICE_STOPPED, GetLastError(), 0);
        CloseHandle(g_ServiceStopEvent);
        DeleteCriticalSection(&g_ServiceMutex);
        return;
    }

    /* Wait for stop event */
    WaitForSingleObject(g_ServiceStopEvent, INFINITE);

    /* Wait for worker thread to finish */
    WaitForSingleObject(hThread, 5000);
    CloseHandle(hThread);

    /* Cleanup */
    CloseHandle(g_ServiceStopEvent);
    DeleteCriticalSection(&g_ServiceMutex);

    /* Report stopped status */
    ReportServiceStatus(SERVICE_STOPPED, NO_ERROR, 0);
}

/**
 * @brief Install the bonding service
 */
int InstallBondingService(void)
{
    SC_HANDLE schSCManager = NULL;
    SC_HANDLE schService = NULL;
    TCHAR szPath[MAX_PATH];
    int result = -1;

    if (!GetModuleFileName(NULL, szPath, MAX_PATH)) {
        BONDING_LOG_ERROR("GetModuleFileName failed (error %d)", GetLastError());
        return -1;
    }

    /* Open service control manager */
    schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!schSCManager) {
        BONDING_LOG_ERROR("OpenSCManager failed (error %d)", GetLastError());
        return -1;
    }

    /* Create service */
    schService = CreateService(
        schSCManager,
        BONDING_SERVICE_NAME,
        BONDING_SERVICE_DISPLAY_NAME,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        szPath,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL
    );

    if (schService) {
        /* Set service description */
        SERVICE_DESCRIPTION sd;
        sd.lpDescription = (LPTSTR)BONDING_SERVICE_DESCRIPTION;
        ChangeServiceConfig2(schService, SERVICE_CONFIG_DESCRIPTION, &sd);

        BONDING_LOG_INFO("Service installed successfully");
        result = 0;
        CloseServiceHandle(schService);
    } else {
        DWORD error = GetLastError();
        if (error == ERROR_SERVICE_EXISTS) {
            BONDING_LOG_WARN("Service already exists");
            result = 0;
        } else {
            BONDING_LOG_ERROR("CreateService failed (error %d)", error);
        }
    }

    CloseServiceHandle(schSCManager);
    return result;
}

/**
 * @brief Uninstall the bonding service
 */
int UninstallBondingService(void)
{
    SC_HANDLE schSCManager = NULL;
    SC_HANDLE schService = NULL;
    int result = -1;

    /* Open service control manager */
    schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!schSCManager) {
        BONDING_LOG_ERROR("OpenSCManager failed (error %d)", GetLastError());
        return -1;
    }

    /* Open service */
    schService = OpenService(schSCManager, BONDING_SERVICE_NAME, DELETE);
    if (!schService) {
        BONDING_LOG_ERROR("OpenService failed (error %d)", GetLastError());
        CloseServiceHandle(schSCManager);
        return -1;
    }

    /* Delete service */
    if (DeleteService(schService)) {
        BONDING_LOG_INFO("Service uninstalled successfully");
        result = 0;
    } else {
        BONDING_LOG_ERROR("DeleteService failed (error %d)", GetLastError());
    }

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
    return result;
}

/**
 * @brief Start the bonding service
 */
int StartBondingService(void)
{
    SC_HANDLE schSCManager = NULL;
    SC_HANDLE schService = NULL;
    int result = -1;

    /* Open service control manager */
    schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
    if (!schSCManager) {
        BONDING_LOG_ERROR("OpenSCManager failed (error %d)", GetLastError());
        return -1;
    }

    /* Open service */
    schService = OpenService(schSCManager, BONDING_SERVICE_NAME, SERVICE_START);
    if (!schService) {
        BONDING_LOG_ERROR("OpenService failed (error %d)", GetLastError());
        CloseServiceHandle(schSCManager);
        return -1;
    }

    /* Start service */
    if (StartService(schService, 0, NULL)) {
        BONDING_LOG_INFO("Service start requested");
        result = 0;
    } else {
        DWORD error = GetLastError();
        if (error == ERROR_SERVICE_ALREADY_RUNNING) {
            BONDING_LOG_WARN("Service already running");
            result = 0;
        } else {
            BONDING_LOG_ERROR("StartService failed (error %d)", error);
        }
    }

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
    return result;
}

/**
 * @brief Stop the bonding service
 */
int StopBondingService(void)
{
    SC_HANDLE schSCManager = NULL;
    SC_HANDLE schService = NULL;
    SERVICE_STATUS_PROCESS ssp;
    DWORD dwBytesNeeded;
    int result = -1;

    /* Open service control manager */
    schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
    if (!schSCManager) {
        BONDING_LOG_ERROR("OpenSCManager failed (error %d)", GetLastError());
        return -1;
    }

    /* Open service */
    schService = OpenService(schSCManager, BONDING_SERVICE_NAME, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!schService) {
        BONDING_LOG_ERROR("OpenService failed (error %d)", GetLastError());
        CloseServiceHandle(schSCManager);
        return -1;
    }

    /* Query service status */
    if (!QueryServiceStatusEx(schService, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(SERVICE_STATUS_PROCESS), &dwBytesNeeded)) {
        BONDING_LOG_ERROR("QueryServiceStatusEx failed (error %d)", GetLastError());
        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
        return -1;
    }

    /* Stop service if running */
    if (ssp.dwCurrentState != SERVICE_STOPPED) {
        if (ControlService(schService, SERVICE_CONTROL_STOP, (LPSERVICE_STATUS)&ssp)) {
            /* Wait for service to stop */
            while (ssp.dwCurrentState != SERVICE_STOPPED) {
                Sleep(ssp.dwWaitHint);
                if (!QueryServiceStatusEx(schService, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(SERVICE_STATUS_PROCESS), &dwBytesNeeded)) {
                    break;
                }
            }
            BONDING_LOG_INFO("Service stopped");
            result = 0;
        } else {
            BONDING_LOG_ERROR("ControlService failed (error %d)", GetLastError());
        }
    } else {
        BONDING_LOG_WARN("Service already stopped");
        result = 0;
    }

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
    return result;
}

/**
 * @brief Main entry point
 */
int main(int argc, char *argv[])
{
    SERVICE_TABLE_ENTRY DispatchTable[] = {
        { (LPTSTR)BONDING_SERVICE_NAME, (LPSERVICE_MAIN_FUNCTION)ServiceMain },
        { NULL, NULL }
    };

    /* Handle command line arguments */
    if (argc > 1) {
        if (strcmp(argv[1], "install") == 0) {
            return InstallBondingService() == 0 ? 0 : 1;
        } else if (strcmp(argv[1], "uninstall") == 0) {
            return UninstallBondingService() == 0 ? 0 : 1;
        } else if (strcmp(argv[1], "start") == 0) {
            return StartBondingService() == 0 ? 0 : 1;
        } else if (strcmp(argv[1], "stop") == 0) {
            return StopBondingService() == 0 ? 0 : 1;
        } else {
            printf("Usage: %s [install|uninstall|start|stop]\n", argv[0]);
            return 1;
        }
    }

    /* Start service control dispatcher */
    if (!StartServiceCtrlDispatcher(DispatchTable)) {
        BONDING_LOG_ERROR("StartServiceCtrlDispatcher failed (error %d)", GetLastError());
        return 1;
    }

    return 0;
}
