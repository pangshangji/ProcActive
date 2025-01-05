
//#include <Windows.h>
//#include <ntddk.h>
//#include <ntdef.h>

//#define NTSTRSAFE_LIB

#ifdef DRIVER
#include <ntstrsafe.h>
#endif

#define DEVICE_NAME                    L"SJPProcActive"
#define PROCACTIVE_DEVICE_NAME         L"\\Device\\" DEVICE_NAME
#define SYMBOLIC_PROCACTIVE            L"\\??\\" DEVICE_NAME


typedef struct _ProcData
{	
	ULONG64 pid;
	DWORD session_id;	
	BOOLEAN createorexit;
	LARGE_INTEGER current_time;
	ULONG max_name_len; //bytes count, include L'\0'	
}ProcData, * PProcData;

#define GetName(data) (WCHAR*)((PCHAR)data+sizeof(ProcData))