/*
 * Copyright 2009 Maarten Lankhorst
 * Copyright 2011 Andrew Eikum for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "wine/port.h"

#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "wine/library.h"

#include "ole2.h"
#include "olectl.h"
#include "rpcproxy.h"
#include "propsys.h"
#include "initguid.h"
#include "propkeydef.h"
#include "mmdeviceapi.h"
#include "dshow.h"
#include "dsound.h"
#include "audioclient.h"
#include "endpointvolume.h"
#include "audiopolicy.h"
#include "devpkey.h"
#include "winreg.h"

#include "mmdevapi.h"
#include "wine/debug.h"
#include "wine/unicode.h"

WINE_DEFAULT_DEBUG_CHANNEL(mmdevapi);

static HINSTANCE instance;

DriverFuncs drvs;

static BOOL load_driver(const WCHAR *name)
{
    WCHAR driver_module[264];
    static const WCHAR wineW[] = {'w','i','n','e',0};
    static const WCHAR dotdrvW[] = {'.','d','r','v',0};

    lstrcpyW(driver_module, wineW);
    lstrcatW(driver_module, name);
    lstrcatW(driver_module, dotdrvW);

    TRACE("Attempting to load %s\n", wine_dbgstr_w(driver_module));

    drvs.module = LoadLibraryW(driver_module);
    if(!drvs.module){
        TRACE("Unable to load %s: %u\n", wine_dbgstr_w(driver_module),
                GetLastError());
        return FALSE;
    }

#define LDFC(n) do { drvs.p##n = (void*)GetProcAddress(drvs.module, #n);\
        if(!drvs.p##n) { FreeLibrary(drvs.module); return FALSE; } } while(0)
    LDFC(GetEndpointIDs);
    LDFC(GetAudioEndpoint);
    LDFC(GetAudioSessionManager);
#undef LDFC

    lstrcpyW(drvs.module_name, driver_module);
    TRACE("Successfully loaded %s\n", wine_dbgstr_w(driver_module));

    return TRUE;
}

static BOOL init_driver(void)
{
    static const WCHAR alsaW[] = {'a','l','s','a',0};
    static const WCHAR ossW[] = {'o','s','s',0};
    static const WCHAR coreaudioW[] = {'c','o','r','e','a','u','d','i','o',0};
    static const WCHAR *default_drivers[] = { alsaW, coreaudioW, ossW };
    static const WCHAR drv_key[] = {'S','o','f','t','w','a','r','e','\\',
        'W','i','n','e','\\','D','r','i','v','e','r','s',0};
    static const WCHAR drv_value[] = {'A','u','d','i','o',0};
    HKEY key;
    UINT i;

    if(drvs.module)
        return TRUE;

    if(RegOpenKeyW(HKEY_CURRENT_USER, drv_key, &key) == ERROR_SUCCESS){
        WCHAR driver_name[256], *p, *next;
        DWORD size = sizeof(driver_name);

        if(RegQueryValueExW(key, drv_value, 0, NULL, (BYTE*)driver_name,
                    &size) == ERROR_SUCCESS){
            RegCloseKey(key);

            if(driver_name[0] == '\0')
                return TRUE;

            for(next = p = driver_name; next; p = next + 1){
                next = strchrW(p, ',');
                if(next)
                    *next = '\0';

                if(load_driver(p))
                    return TRUE;

                TRACE("Failed to load driver: %s\n", wine_dbgstr_w(driver_name));
            }

            ERR("No drivers in the registry loaded successfully!\n");
            return FALSE;
        }

        RegCloseKey(key);
    }

    for(i = 0; i < sizeof(default_drivers)/sizeof(*default_drivers); ++i)
        if(load_driver(default_drivers[i]))
            return TRUE;

    return FALSE;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    TRACE("(0x%p, %d, %p)\n", hinstDLL, fdwReason, lpvReserved);

    switch (fdwReason)
    {
        case DLL_PROCESS_ATTACH:
            instance = hinstDLL;
            DisableThreadLibraryCalls(hinstDLL);
            break;
        case DLL_PROCESS_DETACH:
            MMDevEnum_Free();
            break;
    }

    return TRUE;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}

typedef HRESULT (*FnCreateInstance)(REFIID riid, LPVOID *ppobj);

typedef struct {
    IClassFactory IClassFactory_iface;
    REFCLSID rclsid;
    FnCreateInstance pfnCreateInstance;
} IClassFactoryImpl;

static inline IClassFactoryImpl *impl_from_IClassFactory(IClassFactory *iface)
{
    return CONTAINING_RECORD(iface, IClassFactoryImpl, IClassFactory_iface);
}

static HRESULT WINAPI
MMCF_QueryInterface(LPCLASSFACTORY iface, REFIID riid, LPVOID *ppobj)
{
    IClassFactoryImpl *This = impl_from_IClassFactory(iface);
    TRACE("(%p, %s, %p)\n", This, debugstr_guid(riid), ppobj);
    if (ppobj == NULL)
        return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_IClassFactory))
    {
        *ppobj = iface;
        IUnknown_AddRef(iface);
        return S_OK;
    }
    *ppobj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI MMCF_AddRef(LPCLASSFACTORY iface)
{
    return 2;
}

static ULONG WINAPI MMCF_Release(LPCLASSFACTORY iface)
{
    /* static class, won't be freed */
    return 1;
}

static HRESULT WINAPI MMCF_CreateInstance(
    LPCLASSFACTORY iface,
    LPUNKNOWN pOuter,
    REFIID riid,
    LPVOID *ppobj)
{
    IClassFactoryImpl *This = impl_from_IClassFactory(iface);
    TRACE("(%p, %p, %s, %p)\n", This, pOuter, debugstr_guid(riid), ppobj);

    if (pOuter)
        return CLASS_E_NOAGGREGATION;

    if (ppobj == NULL) {
        WARN("invalid parameter\n");
        return E_POINTER;
    }
    *ppobj = NULL;
    return This->pfnCreateInstance(riid, ppobj);
}

static HRESULT WINAPI MMCF_LockServer(LPCLASSFACTORY iface, BOOL dolock)
{
    IClassFactoryImpl *This = impl_from_IClassFactory(iface);
    FIXME("(%p, %d) stub!\n", This, dolock);
    return S_OK;
}

static const IClassFactoryVtbl MMCF_Vtbl = {
    MMCF_QueryInterface,
    MMCF_AddRef,
    MMCF_Release,
    MMCF_CreateInstance,
    MMCF_LockServer
};

static IClassFactoryImpl MMDEVAPI_CF[] = {
    { { &MMCF_Vtbl }, &CLSID_MMDeviceEnumerator, (FnCreateInstance)MMDevEnum_Create }
};

HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv)
{
    int i = 0;
    TRACE("(%s, %s, %p)\n", debugstr_guid(rclsid), debugstr_guid(riid), ppv);

    if(!init_driver()){
        ERR("Driver initialization failed\n");
        return E_FAIL;
    }

    if (ppv == NULL) {
        WARN("invalid parameter\n");
        return E_INVALIDARG;
    }

    *ppv = NULL;

    if (!IsEqualIID(riid, &IID_IClassFactory) &&
        !IsEqualIID(riid, &IID_IUnknown)) {
        WARN("no interface for %s\n", debugstr_guid(riid));
        return E_NOINTERFACE;
    }

    for (i = 0; i < sizeof(MMDEVAPI_CF)/sizeof(MMDEVAPI_CF[0]); ++i)
    {
        if (IsEqualGUID(rclsid, MMDEVAPI_CF[i].rclsid)) {
            IUnknown_AddRef(&MMDEVAPI_CF[i].IClassFactory_iface);
            *ppv = &MMDEVAPI_CF[i];
            return S_OK;
        }
        i++;
    }

    WARN("(%s, %s, %p): no class found.\n", debugstr_guid(rclsid),
         debugstr_guid(riid), ppv);
    return CLASS_E_CLASSNOTAVAILABLE;
}

/***********************************************************************
 *		DllRegisterServer (MMDEVAPI.@)
 */
HRESULT WINAPI DllRegisterServer(void)
{
    return __wine_register_resources( instance );
}

/***********************************************************************
 *		DllUnregisterServer (MMDEVAPI.@)
 */
HRESULT WINAPI DllUnregisterServer(void)
{
    return __wine_unregister_resources( instance );
}
