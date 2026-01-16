/**
 * @file bonding_dialog.h
 * @brief Header for bonding configuration dialog
 */

#ifndef BONDING_DIALOG_H
#define BONDING_DIALOG_H

#include <windows.h>
#include "../include/bonding_config.h"

INT_PTR CALLBACK BondingDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);

int bonding_dialog_show(HWND parent, connection_t *connection);

#endif /* BONDING_DIALOG_H */
