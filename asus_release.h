/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"
EFI_STATUS GuAsusPreflight(EFI_SYSTEM_TABLE *,EFI_HANDLE,EFI_HANDLE);
EFI_STATUS GuAsusRelease(VOID);
BOOLEAN GuAsusReleased(VOID);
EFI_STATUS GuAsusFinish(VOID);
BOOLEAN GuAsusReadyForWindows(VOID);
BOOLEAN GuAsusMutated(VOID);
