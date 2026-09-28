// syntax-check stub: minimal Win32 surface, only used in --win32 mode
#pragma once
#include <cstddef>
#include <cstdio>
#include <cstdarg>
#define WINAPI
#define APIENTRY
#define CALLBACK
#define WINBASEAPI
typedef int BOOL; typedef unsigned char BYTE; typedef unsigned short WORD; typedef unsigned long DWORD; typedef unsigned long ULONG; typedef long LONG; typedef unsigned int UINT; typedef int INT;
typedef void* HANDLE; typedef void* HWND; typedef void* HINSTANCE; typedef void* HGLOBAL; typedef void* HMODULE; typedef void* LPVOID; typedef const void* LPCVOID; typedef char* LPSTR; typedef const char* LPCSTR; typedef wchar_t* LPWSTR; typedef const wchar_t* LPCWSTR; typedef DWORD* LPDWORD; typedef ULONG* PULONG; typedef unsigned long long ULONGLONG; typedef long long LONGLONG; typedef size_t SIZE_T; typedef unsigned short USHORT; typedef unsigned char UCHAR;
#define TRUE 1
#define FALSE 0
#define NO_ERROR 0L
#define ERROR_SUCCESS 0L
#define ERROR_BUFFER_OVERFLOW 111L
#define ERROR_INSUFFICIENT_BUFFER 122L
#define MAX_PATH 260
#define CF_TEXT 1
#define INFINITE 0xFFFFFFFF
extern "C" {
DWORD GetLastError(void); DWORD GetTickCount(void); void Sleep(DWORD); BOOL OpenClipboard(HWND); BOOL CloseClipboard(void); HANDLE GetClipboardData(UINT); LPVOID GlobalLock(HGLOBAL); BOOL GlobalUnlock(HGLOBAL); HMODULE GetModuleHandleA(LPCSTR); void OutputDebugStringA(LPCSTR); int MessageBoxA(HWND, LPCSTR, LPCSTR, UINT); DWORD GetCurrentThreadId(void); BOOL QueryPerformanceCounter(LONGLONG*); BOOL QueryPerformanceFrequency(LONGLONG*);
int _snprintf(char*, size_t, const char*, ...); int _vsnprintf(char*, size_t, const char*, va_list); int _stricmp(const char*, const char*); int _strnicmp(const char*, const char*, size_t);
}
#ifndef WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#endif
