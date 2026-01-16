/**
 * @file bonding_status.h
 * @brief Header for bonding status display
 */

#ifndef BONDING_STATUS_H
#define BONDING_STATUS_H

#include <windows.h>
#include "../include/bonding_types.h"

/* Forward declaration */
typedef struct connection connection_t;

void bonding_status_update(tunnel_state_t *states, int tunnel_count);

void bonding_status_display(HWND hWnd, connection_t *connection);

INT_PTR CALLBACK BondingStatusDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);

#endif /* BONDING_STATUS_H */
