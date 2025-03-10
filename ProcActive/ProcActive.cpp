
#include <Windows.h>
#include <iostream>
#include <strsafe.h>

#include "..\common\data.h"

struct ApcData 
{
    OVERLAPPED overlapped;
    HANDLE hEvent;
    BOOL invalid;
    DWORD errorcode;
    ProcData* proc_data;
    ULONG64 record_count;
};

#define SERVICE_NAME L"ProcActiveDrv"
#define SERVICE_DISPLAY_NAME L"ProcActiveDrv"


BOOL InstallDrvService();
BOOL StartDrvService();
BOOL DeleteDrvService();
BOOL IsServiceRunning(SC_HANDLE schService);

BOOL InstallDrvService() {
    SC_HANDLE schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (schSCManager == NULL) {
        printf("OpenSCManager failed (%d)\n", GetLastError());
        return FALSE;
    }
    WCHAR sys_name[MAX_PATH] = { 0 };
    GetModuleFileName(NULL, sys_name, ARRAYSIZE(sys_name));
    *(wcsrchr(sys_name, L'\\') + 1) = L'\0';
    wcsncat_s(sys_name, ARRAYSIZE(sys_name), L"drv.sys", wcslen(L"drv.sys"));
    SC_HANDLE schService = CreateService(
        schSCManager,              // SCManager database
        SERVICE_NAME,              // Name of service
        SERVICE_DISPLAY_NAME,      // Display name
        SERVICE_ALL_ACCESS,        // Desired access
        SERVICE_KERNEL_DRIVER,     // Service type
        SERVICE_DEMAND_START,      // Start type (manual)
        SERVICE_ERROR_NORMAL,      // Error control type
        sys_name,              // Path to service binary
        NULL,                      // Load ordering group
        NULL,                      // Tag identifier
        NULL,                      // Dependencies
        NULL,                      // Service start name
        NULL                       // Password
    );

    if (schService == NULL) {
        printf("CreateService failed (%d)\n", GetLastError());
        CloseServiceHandle(schSCManager);
        return FALSE;
    }

    printf("Service installed successfully.\n");

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
    return TRUE;
}

BOOL StartDrvService() {
    SC_HANDLE schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (schSCManager == NULL) {
        printf("OpenSCManager failed (%d)\n", GetLastError());
        return FALSE;
    }

    SC_HANDLE schService = OpenService(schSCManager, SERVICE_NAME, SERVICE_ALL_ACCESS);
    if (schService == NULL) {
        printf("OpenService failed (%d)\n", GetLastError());
        CloseServiceHandle(schSCManager);
        return FALSE;
    }
    if (IsServiceRunning(schService))
    {
        return TRUE;
    }    

    if (!::StartService(schService, 0, NULL)) {
        printf("StartService failed (%d)\n", GetLastError());
        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
        return FALSE;
    }

    printf("Service started successfully.\n");

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
    return TRUE;
}


BOOL IsServiceRunning(SC_HANDLE schService) {
    SERVICE_STATUS_PROCESS ssStatus;
    DWORD dwBytesNeeded;

    if (!QueryServiceStatusEx(
        schService,
        SC_STATUS_PROCESS_INFO,
        (LPBYTE)&ssStatus,
        sizeof(SERVICE_STATUS_PROCESS),
        &dwBytesNeeded)) {
        printf("QueryServiceStatusEx failed (%d)\n", GetLastError());
        return FALSE;
    }

    
    if (ssStatus.dwCurrentState == SERVICE_RUNNING) {
        return TRUE; 
    }

    return FALSE; 
}


BOOL DeleteDrvService() {
    SC_HANDLE schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (schSCManager == NULL) {
        printf("OpenSCManager failed (%d)\n", GetLastError());
        return FALSE;
    }

    SC_HANDLE schService = OpenService(schSCManager, SERVICE_NAME, SERVICE_ALL_ACCESS);
    if (schService == NULL) {
        printf("OpenService failed (%d)\n", GetLastError());
        CloseServiceHandle(schSCManager);
        return FALSE;
    }
    if (IsServiceRunning(schService)) {
        SERVICE_STATUS ssStatus;
        if (!ControlService(schService, SERVICE_CONTROL_STOP, &ssStatus)) {
            printf("ControlService failed (%d)\n", GetLastError());
            CloseServiceHandle(schService);
            CloseServiceHandle(schSCManager);
            return FALSE;
        }

        
        while (IsServiceRunning(schService)) {
            Sleep(100); 
        }

        printf("Service stopped successfully.\n");
    }

    if (!DeleteService(schService)) {
        printf("DeleteService failed (%d)\n", GetLastError());
        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
        return FALSE;
    }

    printf("Service deleted successfully.\n");

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
    return TRUE;
}

VOID WINAPI completion_routine(DWORD errcode, DWORD nread, LPOVERLAPPED lpOverlapped)
{
    ApcData* apc_data = (ApcData*)lpOverlapped->hEvent;
    apc_data->errorcode = errcode;
    if (errcode == 0) {        
        if (nread == 0)
            apc_data->invalid = TRUE;
    }
    else if (errcode == ERROR_DEVICE_NOT_CONNECTED) {
        apc_data->invalid = TRUE;
    }
    SetEvent(apc_data->hEvent);
}

char* WCHAR2UTF8(const WCHAR* value)
{
    if (NULL == value || L'\0' == *value)
    {
        return NULL;
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, NULL, 0, NULL, NULL);    
    std::unique_ptr<CHAR[]> temp((CHAR*)malloc(size * sizeof(char)));

    WideCharToMultiByte(CP_UTF8, 0, value, -1, temp.get(), size, NULL, NULL);
    return temp.release();
}

int monitor()
{
    if (!StartDrvService())
    {
        return 0;
    }
    SetConsoleOutputCP(65001);
    HANDLE device = CreateFile(SYMBOLIC_PROCACTIVE, GENERIC_READ, 0, 0, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (INVALID_HANDLE_VALUE == device)
    {
        printf("CreateFile Failed error:%d\n", GetLastError());
        return 1;
    }
    ApcData apc_data = { 0 };

    ULONG len = sizeof(ProcData) + 32 * sizeof(WCHAR); // 32 default  exe full path len, 32 should is small, next will add len
    ProcData* data = (ProcData*)malloc(len);
    char utf8_name[1024] = { 0 };
    if (NULL == data)
    {
        return 1;
    }

    HANDLE hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (NULL == hEvent)
    {
        return 1;
    }
    apc_data.hEvent = hEvent;
    apc_data.overlapped.hEvent = &apc_data;

    while (1)
    {
        memset(data, 0, len);
        apc_data.proc_data = data;
        if (!ReadFileEx(device, data, len, &apc_data.overlapped, completion_routine))
        {
            break;
        }
        if (apc_data.invalid)
        {
            break;
        }
        WaitForSingleObjectEx(hEvent, INFINITE, TRUE);
        ResetEvent(hEvent);
        if (ERROR_INSUFFICIENT_BUFFER == apc_data.errorcode)
        {
            free(data);
            len += 256 * sizeof(WCHAR);
            data = (ProcData*)malloc(len);
            if (NULL == data)
            {
                return 1;
            }
        }
        else if (!apc_data.invalid)
        {
            FILETIME time = { 0 };
            time.dwLowDateTime = data->current_time.LowPart;
            time.dwHighDateTime = data->current_time.HighPart;
            SYSTEMTIME sys_time;
            FileTimeToSystemTime(&time, &sys_time);
            CHAR time_buff[256] = { 0 };
            StringCbPrintfA(time_buff, ARRAYSIZE(time_buff), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                sys_time.wYear, sys_time.wMonth, sys_time.wDay,
                sys_time.wHour, sys_time.wMinute, sys_time.wSecond, sys_time.wMilliseconds);            
            //wprintf(L"action:%s time:%s pid:%d %ls \n", data->createorexit ? L"create" : L"exit", time_buff, (DWORD)data->pid, *GetName(data) != L'\0' ? GetName(data) : L"NULL");
            WideCharToMultiByte(CP_UTF8, 0, GetName(data), -1, utf8_name, sizeof(utf8_name), NULL, NULL); 
            apc_data.record_count++;
            printf("record_count:%llu action:%s time:%s pid:%d %s \n", apc_data.record_count, data->createorexit ? "create" : "exit", time_buff, (DWORD)data->pid, *GetName(data) != L'\0' ? utf8_name : "NULL");
        }
    }

    printf("apc_data errorcode:%d invalid:%d\n", apc_data.errorcode, apc_data.invalid);    
    return 1;
}

int wmain(int argc, WCHAR* argv[])
{
    bool valid = false;
    if (2 == argc)
    {
        if (0 == _wcsicmp(argv[1], L"-install"))
        {
            return InstallDrvService()?0:1;
        }
        else if (0 == _wcsicmp(argv[1], L"-uninstall"))
        {
            return DeleteDrvService()?0:1;
        }
        else if (0 == _wcsicmp(argv[1], L"-monitor"))
        {
            return monitor();
        }
    }
    printf("usage: \nProcActive.exe -install\n\
ProcActive.exe -uninstall\n\
ProcActive.exe -monitor");
    
}


