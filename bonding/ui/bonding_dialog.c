/**
 * @file bonding_dialog.c
 * @brief GUI dialog for bonding configuration
 */

#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bonding_config.h"
#include "bonding_types.h"
#include "nic_detector.h"
#include "../../openvpn-gui-res.h"
#include "../../localization.h"
#include "../../options.h"
#include "../../misc.h"
#include "../../registry.h"

extern options_t o;

/* Dialog data structure */
typedef struct {
    bonding_profile_t *profile;
    connection_t *connection;
    nic_info_t *available_nics;
    int available_nic_count;
    nic_info_t *selected_nics;
    int selected_nic_count;
    BOOL config_changed;
} bonding_dialog_data_t;

/* Forward declarations */
static void PopulateNICList(HWND hDlg);
static void PopulateBondingModeCombo(HWND hDlg);
static void LoadBondingProfile(HWND hDlg, bonding_profile_t *profile);
static BOOL SaveBondingProfile(HWND hDlg, bonding_profile_t **profile);
static BOOL ValidateBondingConfig(HWND hDlg);
static BOOL ApplyBondingConfig(HWND hDlg);
static void EnableBondingControls(HWND hDlg, BOOL enable);
static void MoveNICBetweenLists(HWND hDlg, int from_list, int to_list);
static void PopulateProfileList(HWND hDlg);

/* Dialog procedure */
INT_PTR CALLBACK
BondingDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    bonding_dialog_data_t *data = NULL;
    NMHDR *nmhdr = NULL;

    switch (message)
    {
        case WM_INITDIALOG:
        {
            data = (bonding_dialog_data_t *)calloc(1, sizeof(bonding_dialog_data_t));
            if (!data)
            {
                EndDialog(hDlg, IDCANCEL);
                return FALSE;
            }

            SetWindowLongPtr(hDlg, DWLP_USER, (LONG_PTR)data);
            data->connection = (connection_t *)lParam;
            data->profile = data->connection ? data->connection->bonding_profile : NULL;
            data->config_changed = FALSE;

            /* Initialize list view columns */
            HWND hListAvailable = GetDlgItem(hDlg, ID_LST_AVAILABLE_NICS);
            HWND hListSelected = GetDlgItem(hDlg, ID_LST_SELECTED_NICS);

            LVCOLUMN lvc = {0};
            lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            lvc.cx = 100;

            /* Available NICs list */
            lvc.pszText = L"Name";
            ListView_InsertColumn(hListAvailable, 0, &lvc);
            lvc.pszText = L"Type";
            ListView_InsertColumn(hListAvailable, 1, &lvc);
            lvc.pszText = L"Speed";
            ListView_InsertColumn(hListAvailable, 2, &lvc);
            lvc.pszText = L"Status";
            ListView_InsertColumn(hListAvailable, 3, &lvc);

            /* Selected NICs list */
            lvc.pszText = L"Name";
            ListView_InsertColumn(hListSelected, 0, &lvc);
            lvc.pszText = L"Type";
            ListView_InsertColumn(hListSelected, 1, &lvc);
            lvc.pszText = L"Speed";
            ListView_InsertColumn(hListSelected, 2, &lvc);
            lvc.pszText = L"Status";
            ListView_InsertColumn(hListSelected, 3, &lvc);

            /* Populate controls */
            PopulateBondingModeCombo(hDlg);
            PopulateNICList(hDlg);
            PopulateProfileList(hDlg);

            /* Load existing profile if available */
            if (data->profile)
            {
                LoadBondingProfile(hDlg, data->profile);
            }

            /* Set initial checkbox state */
            if (data->connection)
            {
                CheckDlgButton(hDlg, ID_CHK_ENABLE_BONDING, data->connection->bonding_enabled ? BST_CHECKED : BST_UNCHECKED);
            }
            else
            {
                CheckDlgButton(hDlg, ID_CHK_ENABLE_BONDING, BST_UNCHECKED);
            }

            EnableBondingControls(hDlg, IsDlgButtonChecked(hDlg, ID_CHK_ENABLE_BONDING) == BST_CHECKED);

            SetForegroundWindow(hDlg);
            return TRUE;
        }

        case WM_COMMAND:
            data = (bonding_dialog_data_t *)GetWindowLongPtr(hDlg, DWLP_USER);
            if (!data)
            {
                return FALSE;
            }

            switch (LOWORD(wParam))
            {
                case ID_CHK_ENABLE_BONDING:
                    if (HIWORD(wParam) == BN_CLICKED)
                    {
                        BOOL enabled = IsDlgButtonChecked(hDlg, ID_CHK_ENABLE_BONDING) == BST_CHECKED;
                        EnableBondingControls(hDlg, enabled);
                        data->config_changed = TRUE;
                    }
                    return TRUE;

                case ID_BTN_ADD_NIC:
                    MoveNICBetweenLists(hDlg, ID_LST_AVAILABLE_NICS, ID_LST_SELECTED_NICS);
                    data->config_changed = TRUE;
                    return TRUE;

                case ID_BTN_REMOVE_NIC:
                    MoveNICBetweenLists(hDlg, ID_LST_SELECTED_NICS, ID_LST_AVAILABLE_NICS);
                    data->config_changed = TRUE;
                    return TRUE;

                case ID_BTN_REFRESH_NICS:
                    PopulateNICList(hDlg);
                    data->config_changed = TRUE;
                    return TRUE;

                case ID_CMB_PROFILE_LIST:
                    if (HIWORD(wParam) == CBN_SELCHANGE)
                    {
                        int sel = ComboBox_GetCurSel(GetDlgItem(hDlg, ID_CMB_PROFILE_LIST));
                        if (sel >= 0)
                        {
                            WCHAR profile_name[MAX_PATH];
                            ComboBox_GetLBText(GetDlgItem(hDlg, ID_CMB_PROFILE_LIST), sel, profile_name);
                            /* Build full path by joining config_dir with filename */
                            WCHAR profile_path[MAX_PATH];
                            if (PathCombine(profile_path, o.config_dir, profile_name) != NULL)
                            {
                                /* Load profile from file */
                                char profile_path_utf8[MAX_PATH];
                                WideCharToMultiByte(CP_UTF8, 0, profile_path, -1, profile_path_utf8, MAX_PATH, NULL, NULL);
                                bonding_profile_t *loaded = bonding_config_load(profile_path_utf8);
                                if (loaded)
                                {
                                    if (data->profile)
                                    {
                                        bonding_config_free(data->profile);
                                    }
                                    data->profile = loaded;
                                    LoadBondingProfile(hDlg, loaded);
                                    data->config_changed = TRUE;
                                }
                            }
                        }
                    }
                    return TRUE;

                case ID_BTN_LOAD_PROFILE:
                {
                    WCHAR profile_name[MAX_PATH];
                    GetDlgItemText(hDlg, ID_CMB_PROFILE_LIST, profile_name, MAX_PATH);
                    if (wcslen(profile_name) > 0)
                    {
                        /* Build full path by joining config_dir with filename */
                        WCHAR profile_path[MAX_PATH];
                        if (PathCombine(profile_path, o.config_dir, profile_name) != NULL)
                        {
                            char profile_path_utf8[MAX_PATH];
                            WideCharToMultiByte(CP_UTF8, 0, profile_path, -1, profile_path_utf8, MAX_PATH, NULL, NULL);
                            bonding_profile_t *loaded = bonding_config_load(profile_path_utf8);
                            if (loaded)
                            {
                                if (data->profile)
                                {
                                    bonding_config_free(data->profile);
                                }
                                data->profile = loaded;
                                LoadBondingProfile(hDlg, loaded);
                                data->config_changed = TRUE;
                            }
                            else
                            {
                                ShowLocalizedMsg(IDS_BONDING_ERR_INVALID_CONFIG);
                            }
                        }
                    }
                    return TRUE;
                }

                case ID_BTN_SAVE_PROFILE:
                {
                    WCHAR profile_name[MAX_PATH];
                    GetDlgItemText(hDlg, ID_EDT_PROFILE_NAME, profile_name, MAX_PATH);
                    if (wcslen(profile_name) == 0)
                    {
                        MessageBox(hDlg, L"Please enter a profile name", L"Error", MB_OK | MB_ICONERROR);
                        return TRUE;
                    }

                    if (SaveBondingProfile(hDlg, &data->profile))
                    {
                        /* Ensure filename ends with .ovpn-bond */
                        WCHAR filename[MAX_PATH];
                        wcscpy_s(filename, MAX_PATH, profile_name);
                        if (wcslen(filename) < 10 || wcscmp(filename + wcslen(filename) - 10, L".ovpn-bond") != 0)
                        {
                            wcscat_s(filename, MAX_PATH, L".ovpn-bond");
                        }
                        
                        /* Build full path by joining config_dir with filename */
                        WCHAR profile_path[MAX_PATH];
                        if (PathCombine(profile_path, o.config_dir, filename) != NULL)
                        {
                            char profile_path_utf8[MAX_PATH];
                            WideCharToMultiByte(CP_UTF8, 0, profile_path, -1, profile_path_utf8, MAX_PATH, NULL, NULL);
                            if (bonding_config_save(data->profile, profile_path_utf8) == 0)
                            {
                                /* Store the full path in the profile */
                                if (data->profile->config_path)
                                {
                                    free(data->profile->config_path);
                                }
                                data->profile->config_path = (char *)malloc(strlen(profile_path_utf8) + 1);
                                if (data->profile->config_path)
                                {
                                    strcpy(data->profile->config_path, profile_path_utf8);
                                }
                                
                                PopulateProfileList(hDlg);
                                MessageBox(hDlg, L"Profile saved successfully", L"Success", MB_OK | MB_ICONINFORMATION);
                                data->config_changed = FALSE;
                            }
                            else
                            {
                                MessageBox(hDlg, L"Failed to save profile", L"Error", MB_OK | MB_ICONERROR);
                            }
                        }
                    }
                    return TRUE;
                }

                case IDOK:
                    if (!ValidateBondingConfig(hDlg))
                    {
                        return TRUE;
                    }
                    if (ApplyBondingConfig(hDlg))
                    {
                        EndDialog(hDlg, IDOK);
                    }
                    return TRUE;

                case IDCANCEL:
                    EndDialog(hDlg, IDCANCEL);
                    return TRUE;

                case IDAPPLY:
                    if (!ValidateBondingConfig(hDlg))
                    {
                        return TRUE;
                    }
                    if (ApplyBondingConfig(hDlg))
                    {
                        data->config_changed = FALSE;
                        MessageBox(hDlg, L"Configuration applied", L"Success", MB_OK | MB_ICONINFORMATION);
                    }
                    return TRUE;
            }
            break;

        case WM_NOTIFY:
            nmhdr = (NMHDR *)lParam;
            if (nmhdr->idFrom == ID_LST_AVAILABLE_NICS || nmhdr->idFrom == ID_LST_SELECTED_NICS)
            {
                if (nmhdr->code == NM_DBLCLK)
                {
                    int from_list = (nmhdr->idFrom == ID_LST_AVAILABLE_NICS) ? ID_LST_AVAILABLE_NICS : ID_LST_SELECTED_NICS;
                    int to_list = (nmhdr->idFrom == ID_LST_AVAILABLE_NICS) ? ID_LST_SELECTED_NICS : ID_LST_AVAILABLE_NICS;
                    MoveNICBetweenLists(hDlg, from_list, to_list);
                    data = (bonding_dialog_data_t *)GetWindowLongPtr(hDlg, DWLP_USER);
                    if (data)
                    {
                        data->config_changed = TRUE;
                    }
                }
            }
            break;

        case WM_CLOSE:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;

        case WM_DESTROY:
            data = (bonding_dialog_data_t *)GetWindowLongPtr(hDlg, DWLP_USER);
            if (data)
            {
                if (data->available_nics)
                {
                    for (int i = 0; i < data->available_nic_count; i++)
                    {
                        nic_detector_free_info(&data->available_nics[i]);
                    }
                    free(data->available_nics);
                }
                if (data->selected_nics)
                {
                    for (int i = 0; i < data->selected_nic_count; i++)
                    {
                        nic_detector_free_info(&data->selected_nics[i]);
                    }
                    free(data->selected_nics);
                }
                free(data);
            }
            break;
    }

    return FALSE;
}

/* Show bonding dialog */
int
bonding_dialog_show(HWND parent, connection_t *connection)
{
    INT_PTR result = LocalizedDialogBoxParamEx(ID_DLG_BONDING, parent, BondingDialogProc, (LPARAM)connection);
    return (result == IDOK) ? 0 : -1;
}

/* Populate NIC list */
static void
PopulateNICList(HWND hDlg)
{
    bonding_dialog_data_t *data = (bonding_dialog_data_t *)GetWindowLongPtr(hDlg, DWLP_USER);
    if (!data)
    {
        return;
    }

    HWND hListAvailable = GetDlgItem(hDlg, ID_LST_AVAILABLE_NICS);
    HWND hListSelected = GetDlgItem(hDlg, ID_LST_SELECTED_NICS);

    ListView_DeleteAllItems(hListAvailable);
    ListView_DeleteAllItems(hListSelected);

    /* Free existing NIC lists */
    if (data->available_nics)
    {
        for (int i = 0; i < data->available_nic_count; i++)
        {
            nic_detector_free_info(&data->available_nics[i]);
        }
        free(data->available_nics);
        data->available_nics = NULL;
        data->available_nic_count = 0;
    }
    if (data->selected_nics)
    {
        for (int i = 0; i < data->selected_nic_count; i++)
        {
            nic_detector_free_info(&data->selected_nics[i]);
        }
        free(data->selected_nics);
        data->selected_nics = NULL;
        data->selected_nic_count = 0;
    }

    /* Enumerate NICs */
    nic_info_t nics[32];
    int nic_count = nic_detector_enumerate(nics, 32);
    if (nic_count <= 0)
    {
        return;
    }

    /* Allocate memory for NIC lists */
    data->available_nics = (nic_info_t *)calloc(nic_count, sizeof(nic_info_t));
    data->selected_nics = (nic_info_t *)calloc(nic_count, sizeof(nic_info_t));
    if (!data->available_nics || !data->selected_nics)
    {
        /* Free enumerated NICs */
        for (int i = 0; i < nic_count; i++)
        {
            nic_detector_free_info(&nics[i]);
        }
        return;
    }

    /* Populate available NICs */
    for (int i = 0; i < nic_count; i++)
    {
        nic_info_t *dst_nic = &data->available_nics[data->available_nic_count];
        nic_info_t *src_nic = &nics[i];
        
        /* Initialize destination */
        memset(dst_nic, 0, sizeof(nic_info_t));
        
        /* Duplicate strings */
        if (src_nic->name)
        {
            dst_nic->name = (char*)malloc(strlen(src_nic->name) + 1);
            if (dst_nic->name)
                strcpy(dst_nic->name, src_nic->name);
        }
        if (src_nic->description)
        {
            dst_nic->description = (char*)malloc(strlen(src_nic->description) + 1);
            if (dst_nic->description)
                strcpy(dst_nic->description, src_nic->description);
        }
        if (src_nic->guid)
        {
            dst_nic->guid = (char*)malloc(strlen(src_nic->guid) + 1);
            if (dst_nic->guid)
                strcpy(dst_nic->guid, src_nic->guid);
        }
        
        /* Copy other fields */
        dst_nic->type = src_nic->type;
        dst_nic->status = src_nic->status;
        dst_nic->speed = src_nic->speed;
        dst_nic->index = src_nic->index;
        
        /* Convert name to wide string for display */
        WCHAR wname[256] = {0};
        WCHAR wdesc[256] = {0};
        if (src_nic->description)
        {
            MultiByteToWideChar(CP_ACP, 0, src_nic->description, -1, wdesc, 256);
        }
        else if (src_nic->name)
        {
            MultiByteToWideChar(CP_ACP, 0, src_nic->name, -1, wname, 256);
        }
        
        LVITEM lvi = {0};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = data->available_nic_count;
        lvi.pszText = wdesc[0] ? wdesc : wname;
        int idx = ListView_InsertItem(hListAvailable, &lvi);
        
        /* Set type */
        const WCHAR *type_str = L"Unknown";
        if (src_nic->type == NIC_TYPE_ETHERNET)
            type_str = L"Ethernet";
        else if (src_nic->type == NIC_TYPE_WIFI)
            type_str = L"WiFi";
        else if (src_nic->type == NIC_TYPE_LTE)
            type_str = L"LTE";
        ListView_SetItemText(hListAvailable, idx, 1, (LPWSTR)type_str);
        
        /* Set speed */
        WCHAR speed[32];
        swprintf_s(speed, 32, L"%lu Mbps", src_nic->speed);
        ListView_SetItemText(hListAvailable, idx, 2, speed);
        
        /* Set status */
        const WCHAR *status_str = L"Disconnected";
        if (src_nic->status == NIC_STATUS_CONNECTED)
            status_str = L"Connected";
        ListView_SetItemText(hListAvailable, idx, 3, (LPWSTR)status_str);
        
        data->available_nic_count++;
    }
    
    /* Free source NICs */
    for (int i = 0; i < nic_count; i++)
    {
        nic_detector_free_info(&nics[i]);
    }
}

/* Populate bonding mode combo box */
static void
PopulateBondingModeCombo(HWND hDlg)
{
    HWND hCombo = GetDlgItem(hDlg, ID_CMB_BONDING_MODE);
    ComboBox_ResetContent(hCombo);

    ComboBox_AddString(hCombo, LoadLocalizedString(IDS_BONDING_MODE_ROUND_ROBIN));
    ComboBox_AddString(hCombo, LoadLocalizedString(IDS_BONDING_MODE_WEIGHTED));
    ComboBox_AddString(hCombo, LoadLocalizedString(IDS_BONDING_MODE_ACTIVE_BACKUP));
    ComboBox_AddString(hCombo, LoadLocalizedString(IDS_BONDING_MODE_ADAPTIVE));

    ComboBox_SetCurSel(hCombo, 0); /* Default to Round-Robin */
}

/* Load bonding profile into dialog */
static void
LoadBondingProfile(HWND hDlg, bonding_profile_t *profile)
{
    if (!profile)
    {
        return;
    }

    /* Set bonding mode */
    HWND hCombo = GetDlgItem(hDlg, ID_CMB_BONDING_MODE);
    int mode_index = (int)profile->mode;
    if (mode_index >= 0 && mode_index < 4)
    {
        ComboBox_SetCurSel(hCombo, mode_index);
    }

    /* Set profile name */
    if (profile->profile_name)
    {
        WCHAR wname[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, profile->profile_name, -1, wname, MAX_PATH);
        SetDlgItemText(hDlg, ID_EDT_PROFILE_NAME, wname);
    }
}

/* Save bonding profile from dialog */
static BOOL
SaveBondingProfile(HWND hDlg, bonding_profile_t **profile)
{
    bonding_dialog_data_t *data = (bonding_dialog_data_t *)GetWindowLongPtr(hDlg, DWLP_USER);
    if (!data)
    {
        return FALSE;
    }

    /* Get selected NICs count */
    HWND hListSelected = GetDlgItem(hDlg, ID_LST_SELECTED_NICS);
    int selected_count = ListView_GetItemCount(hListSelected);
    if (selected_count < 2)
    {
        MessageBox(hDlg, LoadLocalizedString(IDS_BONDING_ERR_MIN_NICS), L"Error", MB_OK | MB_ICONERROR);
        return FALSE;
    }

    /* Create or update profile */
    if (!*profile)
    {
        *profile = (bonding_profile_t *)calloc(1, sizeof(bonding_profile_t));
        if (!*profile)
        {
            return FALSE;
        }
        /* Initialize defaults */
        (*profile)->packet_queue_size = 1000;
        (*profile)->packet_timeout = 100;
        (*profile)->sequencing_enabled = 1;
        (*profile)->flow_control_enabled = 1;
    }

    /* Get bonding mode */
    HWND hCombo = GetDlgItem(hDlg, ID_CMB_BONDING_MODE);
    int mode_index = ComboBox_GetCurSel(hCombo);
    (*profile)->mode = (bonding_mode_t)mode_index;

    /* Get profile name */
    WCHAR wname[MAX_PATH];
    GetDlgItemText(hDlg, ID_EDT_PROFILE_NAME, wname, MAX_PATH);
    if (wcslen(wname) > 0)
    {
        if ((*profile)->profile_name)
        {
            free((*profile)->profile_name);
        }
        int len = WideCharToMultiByte(CP_UTF8, 0, wname, -1, NULL, 0, NULL, NULL);
        (*profile)->profile_name = (char *)malloc(len);
        WideCharToMultiByte(CP_UTF8, 0, wname, -1, (*profile)->profile_name, len, NULL, NULL);
    }

    /* Set tunnel count */
    (*profile)->tunnel_count = selected_count;

    /* Allocate tunnels array if needed */
    if (!(*profile)->tunnels || (*profile)->tunnel_count != selected_count)
    {
        /* Free existing tunnels if any */
        if ((*profile)->tunnels)
        {
            for (int i = 0; i < (*profile)->tunnel_count; i++)
            {
                if ((*profile)->tunnels[i].nic_name)
                    free((*profile)->tunnels[i].nic_name);
                if ((*profile)->tunnels[i].server_host)
                    free((*profile)->tunnels[i].server_host);
                if ((*profile)->tunnels[i].config_file)
                    free((*profile)->tunnels[i].config_file);
            }
            free((*profile)->tunnels);
        }
        
        (*profile)->tunnels = (tunnel_config_t *)calloc(selected_count, sizeof(tunnel_config_t));
        if (!(*profile)->tunnels)
        {
            return FALSE;
        }
    }

    /* Get server info from connection or existing profile */
    const char *server_host = NULL;
    int server_port = 1194;
    char config_file_utf8[MAX_PATH] = {0};
    const char *config_file = NULL;
    
    if (data->connection && data->connection->config_file[0])
    {
        /* Use connection's config file */
        WideCharToMultiByte(CP_UTF8, 0, data->connection->config_file, -1, config_file_utf8, MAX_PATH, NULL, NULL);
        config_file = config_file_utf8;
    }
    else if (*profile && (*profile)->tunnels && (*profile)->tunnels[0].config_file)
    {
        /* Reuse from existing profile */
        config_file = (*profile)->tunnels[0].config_file;
    }
    
    if (*profile && (*profile)->tunnels && (*profile)->tunnels[0].server_host)
    {
        server_host = (*profile)->tunnels[0].server_host;
        server_port = (*profile)->tunnels[0].server_port;
    }

    /* Populate each tunnel with NIC name and defaults */
    for (int i = 0; i < selected_count; i++)
    {
        tunnel_config_t *tunnel = &(*profile)->tunnels[i];
        
        /* Get NIC name from list view */
        WCHAR nic_name_w[256];
        ListView_GetItemText(hListSelected, i, 0, nic_name_w, 256);
        
        /* Convert to UTF-8 and store */
        if (tunnel->nic_name)
        {
            free(tunnel->nic_name);
        }
        int nic_name_len = WideCharToMultiByte(CP_UTF8, 0, nic_name_w, -1, NULL, 0, NULL, NULL);
        tunnel->nic_name = (char *)malloc(nic_name_len);
        if (tunnel->nic_name)
        {
            WideCharToMultiByte(CP_UTF8, 0, nic_name_w, -1, tunnel->nic_name, nic_name_len, NULL, NULL);
        }
        
        /* Set defaults */
        tunnel->weight = 1;
        tunnel->server_port = server_port + i; /* Use different ports for each tunnel */
        tunnel->state = TUNNEL_STATE_DISCONNECTED;
        
        /* Set server_host if available */
        if (server_host)
        {
            if (tunnel->server_host)
            {
                free(tunnel->server_host);
            }
            tunnel->server_host = (char *)malloc(strlen(server_host) + 1);
            if (tunnel->server_host)
            {
                strcpy(tunnel->server_host, server_host);
            }
        }
        
        /* Set config_file if available */
        if (config_file)
        {
            if (tunnel->config_file)
            {
                free(tunnel->config_file);
            }
            tunnel->config_file = (char *)malloc(strlen(config_file) + 1);
            if (tunnel->config_file)
            {
                strcpy(tunnel->config_file, config_file);
            }
        }
    }

    return TRUE;
}

/* Validate bonding configuration */
static BOOL
ValidateBondingConfig(HWND hDlg)
{
    HWND hListSelected = GetDlgItem(hDlg, ID_LST_SELECTED_NICS);
    int selected_count = ListView_GetItemCount(hListSelected);

    if (IsDlgButtonChecked(hDlg, ID_CHK_ENABLE_BONDING) == BST_CHECKED)
    {
        if (selected_count < 2)
        {
            MessageBox(hDlg, LoadLocalizedString(IDS_BONDING_ERR_MIN_NICS), L"Error", MB_OK | MB_ICONERROR);
            return FALSE;
        }

        HWND hCombo = GetDlgItem(hDlg, ID_CMB_BONDING_MODE);
        if (ComboBox_GetCurSel(hCombo) < 0)
        {
            MessageBox(hDlg, LoadLocalizedString(IDS_BONDING_ERR_INVALID_CONFIG), L"Error", MB_OK | MB_ICONERROR);
            return FALSE;
        }
    }

    return TRUE;
}

/* Apply bonding configuration */
static BOOL
ApplyBondingConfig(HWND hDlg)
{
    bonding_dialog_data_t *data = (bonding_dialog_data_t *)GetWindowLongPtr(hDlg, DWLP_USER);
    if (!data)
    {
        return FALSE;
    }
    
    /* Connection is always valid since dialog is opened with a connection */
    if (!data->connection)
    {
        return FALSE;
    }

    BOOL enabled = IsDlgButtonChecked(hDlg, ID_CHK_ENABLE_BONDING) == BST_CHECKED;
    data->connection->bonding_enabled = enabled;

    if (enabled)
    {
        if (!SaveBondingProfile(hDlg, &data->profile))
        {
            return FALSE;
        }

        /* Validate profile */
        validation_result_t validation = bonding_config_validate(data->profile);
        if (validation.error_code != BONDING_ERROR_SUCCESS)
        {
            WCHAR msg[512];
            MultiByteToWideChar(CP_UTF8, 0, validation.error_message, -1, msg, 512);
            MessageBox(hDlg, msg, L"Validation Error", MB_OK | MB_ICONERROR);
            return FALSE;
        }

        data->connection->bonding_profile = data->profile;
        data->connection->flags |= FLAG_BONDING_ACTIVE;
        
        /* Save bonding settings to registry */
        WCHAR profile_path[MAX_PATH] = {0};
        if (data->profile && data->profile->config_path)
        {
            MultiByteToWideChar(CP_UTF8, 0, data->profile->config_path, -1, profile_path, MAX_PATH);
        }
        SaveBondingSettings(data->connection->config_name, TRUE, profile_path);
    }
    else
    {
        data->connection->flags &= ~FLAG_BONDING_ACTIVE;
        
        /* Save bonding settings to registry */
        SaveBondingSettings(data->connection->config_name, FALSE, NULL);
    }

    return TRUE;
}

/* Enable/disable bonding controls */
static void
EnableBondingControls(HWND hDlg, BOOL enable)
{
    EnableWindow(GetDlgItem(hDlg, ID_CMB_BONDING_MODE), enable);
    EnableWindow(GetDlgItem(hDlg, ID_LST_AVAILABLE_NICS), enable);
    EnableWindow(GetDlgItem(hDlg, ID_LST_SELECTED_NICS), enable);
    EnableWindow(GetDlgItem(hDlg, ID_BTN_ADD_NIC), enable);
    EnableWindow(GetDlgItem(hDlg, ID_BTN_REMOVE_NIC), enable);
    EnableWindow(GetDlgItem(hDlg, ID_EDT_PROFILE_NAME), enable);
    EnableWindow(GetDlgItem(hDlg, ID_BTN_SAVE_PROFILE), enable);
    EnableWindow(GetDlgItem(hDlg, ID_BTN_LOAD_PROFILE), enable);
    EnableWindow(GetDlgItem(hDlg, ID_CMB_PROFILE_LIST), enable);
}

/* Move NIC between lists */
static void
MoveNICBetweenLists(HWND hDlg, int from_list, int to_list)
{
    HWND hFrom = GetDlgItem(hDlg, from_list);
    HWND hTo = GetDlgItem(hDlg, to_list);

    int sel = ListView_GetNextItem(hFrom, -1, LVNI_SELECTED);
    if (sel < 0)
    {
        return;
    }

    WCHAR name[256];
    LVITEM lvi = {0};
    lvi.mask = LVIF_TEXT;
    lvi.iItem = sel;
    lvi.pszText = name;
    lvi.cchTextMax = 256;
    ListView_GetItem(hFrom, &lvi);

    /* Get all column texts */
    WCHAR type[64], speed[64], status[64];
    ListView_GetItemText(hFrom, sel, 1, type, 64);
    ListView_GetItemText(hFrom, sel, 2, speed, 64);
    ListView_GetItemText(hFrom, sel, 3, status, 64);

    /* Remove from source list */
    ListView_DeleteItem(hFrom, sel);

    /* Add to destination list */
    lvi.iItem = ListView_GetItemCount(hTo);
    int new_idx = ListView_InsertItem(hTo, &lvi);
    ListView_SetItemText(hTo, new_idx, 1, type);
    ListView_SetItemText(hTo, new_idx, 2, speed);
    ListView_SetItemText(hTo, new_idx, 3, status);
}

/* Populate profile list */
static void
PopulateProfileList(HWND hDlg)
{
    HWND hCombo = GetDlgItem(hDlg, ID_CMB_PROFILE_LIST);
    ComboBox_ResetContent(hCombo);

    /* Search for .ovpn-bond files in config directory */
    WCHAR search_path[MAX_PATH];
    wcscpy_s(search_path, MAX_PATH, o.config_dir);
    wcscat_s(search_path, MAX_PATH, L"\\*.ovpn-bond");

    WIN32_FIND_DATA find_data;
    HANDLE hFind = FindFirstFile(search_path, &find_data);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                ComboBox_AddString(hCombo, find_data.cFileName);
            }
        } while (FindNextFile(hFind, &find_data));
        FindClose(hFind);
    }
}
