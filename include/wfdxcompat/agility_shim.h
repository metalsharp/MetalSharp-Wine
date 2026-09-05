#ifndef WFDXCOMPAT_AGILITY_SHIM_H
#define WFDXCOMPAT_AGILITY_SHIM_H

#include <windows.h>

HRESULT wfdx_agility_get_interface(REFCLSID clsid, REFIID iid, void **object);
void wfdx_agility_initialize_from_application(void);

#endif
