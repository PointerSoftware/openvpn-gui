/**
 * @file bonding_status.c
 * @brief Status display for bonding tunnels
 */

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>

#include "bonding_types.h"
#include "bonding_config.h"
#include "service_ipc.h"
#include "../../openvpn-gui-res.h"
#include "../../localization.h"
#include "../../options.h"

extern options_t o;

#define IDT_BONDING_STATUS_TIMER 1

/* Status dialog data */
typedef struct {
    connection_t *connection;
} bonding_status_data_t;

/* Forward declarations */
static void UpdateStatusDisplay(HWND hDlg);
static void UpdateTunnelList(HWND hDlg);
static void UpdateSummaryInfo(HWND hDlg);

/* Dialog procedure */
INT_PTR CALLBACK
BondingStatusDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    bonding_status_data_t *data = NULL;

    switch (message)
    {
        case WM_INITDIALOG:
        {
            data = (bonding_status_data_t *)calloc(1, sizeof(bonding_status_data_t));
            if (!data)
            {
                EndDialog(hDlg, IDCANCEL);
                return FALSE;
            }

            data->connection = (connection_t *)lParam;
            SetWindowLongPtr(hDlg, DWLP_USER, (LONG_PTR)data);

            /* Initialize list view columns */
            HWND hList = GetDlgItem(hDlg, ID_LST_TUNNEL_STATUS);
            LVCOLUMN lvc = {0};
            lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            lvc.cx = 80;

            lvc.pszText = L"Tunnel";
            ListView_InsertColumn(hList, 0, &lvc);
            lvc.pszText = L"NIC";
            ListView_InsertColumn(hList, 1, &lvc);
            lvc.pszText = L"State";
            ListView_InsertColumn(hList, 2, &lvc);
            lvc.pszText = L"Bandwidth";
            ListView_InsertColumn(hList, 3, &lvc);
            lvc.pszText = L"Latency";
            ListView_InsertColumn(hList, 4, &lvc);
            lvc.pszText = L"Packets";
            ListView_InsertColumn(hList, 5, &lvc);

            /* Set up timer for periodic updates */
            SetTimer(hDlg, IDT_BONDING_STATUS_TIMER, 1000, NULL);

            /* Initial update */
            UpdateStatusDisplay(hDlg);

            SetForegroundWindow(hDlg);
            return TRUE;
        }

        case WM_TIMER:
            if (wParam == IDT_BONDING_STATUS_TIMER)
            {
                UpdateStatusDisplay(hDlg);
            }
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case IDRETRY: /* Refresh button */
                    UpdateStatusDisplay(hDlg);
                    return TRUE;

                case IDCANCEL: /* Close button */
                    KillTimer(hDlg, IDT_BONDING_STATUS_TIMER);
                    ShowWindow(hDlg, SW_HIDE);
                    return TRUE;
            }
            break;

        case WM_CLOSE:
            KillTimer(hDlg, IDT_BONDING_STATUS_TIMER);
            ShowWindow(hDlg, SW_HIDE);
            return TRUE;

        case WM_DESTROY:
            KillTimer(hDlg, IDT_BONDING_STATUS_TIMER);
            data = (bonding_status_data_t *)GetWindowLongPtr(hDlg, DWLP_USER);
            if (data)
            {
                free(data);
            }
            break;
    }

    return FALSE;
}

/* Update status display */
static void
UpdateStatusDisplay(HWND hDlg)
{
    UpdateTunnelList(hDlg);
    UpdateSummaryInfo(hDlg);
}

/* Update tunnel list */
static void
UpdateTunnelList(HWND hDlg)
{
    bonding_status_data_t *data = (bonding_status_data_t *)GetWindowLongPtr(hDlg, DWLP_USER);
    if (!data || !data->connection || !data->connection->bonding_profile)
    {
        return;
    }

    bonding_profile_t *profile = (bonding_profile_t *)data->connection->bonding_profile;
    HWND hList = GetDlgItem(hDlg, ID_LST_TUNNEL_STATUS);
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    ipc_message_t response;
    ipc_status_response_t *status_resp = NULL;

    /* Connect to service via IPC */
    hPipe = service_ipc_connect();
    if (hPipe == INVALID_HANDLE_VALUE)
    {
        /* Service not available - show disconnected state */
        ListView_DeleteAllItems(hList);
        return;
    }

    /* Send GET_STATUS command */
    if (service_ipc_send_command(hPipe, IPC_CMD_GET_STATUS, NULL, 0) != 0)
    {
        service_ipc_disconnect(hPipe);
        ListView_DeleteAllItems(hList);
        return;
    }

    /* Receive response */
    if (service_ipc_receive_response(hPipe, &response, 5000) != 0)
    {
        service_ipc_disconnect(hPipe);
        ListView_DeleteAllItems(hList);
        return;
    }

    /* Disconnect */
    service_ipc_disconnect(hPipe);

    /* Parse response */
    if (response.message_type == IPC_RESP_STATUS && response.payload_length >= sizeof(ipc_status_response_t))
    {
        status_resp = (ipc_status_response_t *)response.payload;
    }
    else
    {
        /* Error response or invalid data */
        ListView_DeleteAllItems(hList);
        return;
    }

    /* Clear existing items */
    ListView_DeleteAllItems(hList);

    /* Update each tunnel */
    int tunnel_count = (status_resp->tunnel_count < profile->tunnel_count) ? 
                       status_resp->tunnel_count : profile->tunnel_count;
    
    for (int i = 0; i < tunnel_count && i < 32; i++)
    {
        tunnel_state_t state = status_resp->tunnel_states[i];

        WCHAR tunnel_name[32];
        swprintf_s(tunnel_name, 32, L"Tunnel %d", i + 1);

        LVITEM lvi = {0};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = i;
        lvi.pszText = tunnel_name;
        int idx = ListView_InsertItem(hList, &lvi);

        /* Set NIC name */
        if (i < profile->tunnel_count && profile->tunnels[i].nic_name)
        {
            WCHAR nic_name[256];
            MultiByteToWideChar(CP_UTF8, 0, profile->tunnels[i].nic_name, -1, nic_name, 256);
            ListView_SetItemText(hList, idx, 1, nic_name);
        }

        /* Set state */
        const WCHAR *state_text = L"Disconnected";
        switch (state)
        {
            case TUNNEL_STATE_CONNECTING:
                state_text = L"Connecting";
                break;
            case TUNNEL_STATE_CONNECTED:
                state_text = L"Connected";
                break;
            case TUNNEL_STATE_FAILED:
                state_text = L"Failed";
                break;
            default:
                state_text = L"Disconnected";
                break;
        }
        ListView_SetItemText(hList, idx, 2, (LPWSTR)state_text);

        /* Statistics are not available via IPC yet - leave empty or show N/A */
        ListView_SetItemText(hList, idx, 3, L"N/A");
        ListView_SetItemText(hList, idx, 4, L"N/A");
        ListView_SetItemText(hList, idx, 5, L"N/A");

        /* Update progress bar */
        HWND hProgress = GetDlgItem(hDlg, ID_PROGRESS_TUNNEL_1 + i);
        if (hProgress && state == TUNNEL_STATE_CONNECTED)
        {
            SendMessage(hProgress, PBM_SETPOS, 50, 0); /* Example: 50% */
        }
    }
}

/* Update summary information */
static void
UpdateSummaryInfo(HWND hDlg)
{
    bonding_status_data_t *data = (bonding_status_data_t *)GetWindowLongPtr(hDlg, DWLP_USER);
    if (!data || !data->connection || !data->connection->bonding_profile)
    {
        return;
    }

    bonding_profile_t *profile = (bonding_profile_t *)data->connection->bonding_profile;
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    ipc_message_t response;
    ipc_status_response_t *status_resp = NULL;
    int active_count = 0;

    /* Connect to service via IPC */
    hPipe = service_ipc_connect();
    if (hPipe == INVALID_HANDLE_VALUE)
    {
        /* Service not available */
        SetDlgItemText(hDlg, ID_TXT_TOTAL_BANDWIDTH, L"N/A");
        WCHAR tunnels_text[64];
        swprintf_s(tunnels_text, 64, L"0 / %d", profile->tunnel_count);
        SetDlgItemText(hDlg, ID_TXT_ACTIVE_TUNNELS, tunnels_text);
        return;
    }

    /* Send GET_STATUS command */
    if (service_ipc_send_command(hPipe, IPC_CMD_GET_STATUS, NULL, 0) != 0)
    {
        service_ipc_disconnect(hPipe);
        SetDlgItemText(hDlg, ID_TXT_TOTAL_BANDWIDTH, L"N/A");
        WCHAR tunnels_text[64];
        swprintf_s(tunnels_text, 64, L"0 / %d", profile->tunnel_count);
        SetDlgItemText(hDlg, ID_TXT_ACTIVE_TUNNELS, tunnels_text);
        return;
    }

    /* Receive response */
    if (service_ipc_receive_response(hPipe, &response, 5000) != 0)
    {
        service_ipc_disconnect(hPipe);
        SetDlgItemText(hDlg, ID_TXT_TOTAL_BANDWIDTH, L"N/A");
        WCHAR tunnels_text[64];
        swprintf_s(tunnels_text, 64, L"0 / %d", profile->tunnel_count);
        SetDlgItemText(hDlg, ID_TXT_ACTIVE_TUNNELS, tunnels_text);
        return;
    }

    /* Disconnect */
    service_ipc_disconnect(hPipe);

    /* Parse response */
    if (response.message_type == IPC_RESP_STATUS && response.payload_length >= sizeof(ipc_status_response_t))
    {
        status_resp = (ipc_status_response_t *)response.payload;
    }
    else
    {
        SetDlgItemText(hDlg, ID_TXT_TOTAL_BANDWIDTH, L"N/A");
        WCHAR tunnels_text[64];
        swprintf_s(tunnels_text, 64, L"0 / %d", profile->tunnel_count);
        SetDlgItemText(hDlg, ID_TXT_ACTIVE_TUNNELS, tunnels_text);
        return;
    }

    /* Count active tunnels */
    int tunnel_count = (status_resp->tunnel_count < profile->tunnel_count) ? 
                       status_resp->tunnel_count : profile->tunnel_count;
    
    for (int i = 0; i < tunnel_count && i < 32; i++)
    {
        if (status_resp->tunnel_states[i] == TUNNEL_STATE_CONNECTED)
        {
            active_count++;
        }
    }

    /* Update total bandwidth (not available via IPC yet) */
    SetDlgItemText(hDlg, ID_TXT_TOTAL_BANDWIDTH, L"N/A");

    /* Update active tunnels count */
    WCHAR tunnels_text[64];
    swprintf_s(tunnels_text, 64, L"%d / %d", active_count, profile->tunnel_count);
    SetDlgItemText(hDlg, ID_TXT_ACTIVE_TUNNELS, tunnels_text);
}

/* Update bonding status */
void
bonding_status_update(tunnel_state_t *states, int tunnel_count)
{
    /* This function can be called from the bonding manager to update status */
    /* Implementation depends on how status updates are triggered */
}

/* Display bonding status window */
void
bonding_status_display(HWND hWnd, connection_t *connection)
{
    if (!connection)
    {
        return;
    }

    /* Create or show status window */
    if (connection->hwndBondingStatus && IsWindow(connection->hwndBondingStatus))
    {
        ShowWindow(connection->hwndBondingStatus, SW_SHOW);
        SetForegroundWindow(connection->hwndBondingStatus);
    }
    else
    {
        HWND hStatus = CreateLocalizedDialogParam(ID_DLG_BONDING_STATUS, BondingStatusDialogProc, (LPARAM)connection);
        if (hStatus)
        {
            connection->hwndBondingStatus = hStatus;
            ShowWindow(hStatus, SW_SHOW);
        }
    }
}
