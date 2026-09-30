/*
 * Copyright (C) 2006-2010 - Frictional Games
 *
 * This file is part of Penumbra Overture.
 *
 * Penumbra Overture is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Penumbra Overture is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Penumbra Overture.  If not, see <http://www.gnu.org/licenses/>.
 */
//#include <vld.h>

#include "Init.h"

#include "SDL/SDL.h"

#ifdef WIN32
	#include <windows.h>
	#include <dbghelp.h>
	#include <signal.h>
	#include <stdlib.h>
	#include <string.h>
	#include <exception>
#endif

#ifdef WIN32
//-----------------------------------------------------------------------
// Crash reporter: any crash, abort(), uncaught C++ exception or pure
// virtual call writes what happened and the call stack (function, file,
// line from overture.pdb) into hpl.log before the process dies. Without
// this a player's crash left nothing to go on.
//-----------------------------------------------------------------------

static bool gbCrashLogged = false;

static void CrashWalkStack(CONTEXT aCtx)
{
	HANDLE hProc = GetCurrentProcess();
	SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
	SymInitialize(hProc, NULL, TRUE);

	STACKFRAME64 sf;
	memset(&sf, 0, sizeof(sf));
	DWORD lMachine;
#if defined(_M_IX86)
	lMachine = IMAGE_FILE_MACHINE_I386;
	sf.AddrPC.Offset = aCtx.Eip;
	sf.AddrFrame.Offset = aCtx.Ebp;
	sf.AddrStack.Offset = aCtx.Esp;
#else
	lMachine = IMAGE_FILE_MACHINE_AMD64;
	sf.AddrPC.Offset = aCtx.Rip;
	sf.AddrFrame.Offset = aCtx.Rbp;
	sf.AddrStack.Offset = aCtx.Rsp;
#endif
	sf.AddrPC.Mode = AddrModeFlat;
	sf.AddrFrame.Mode = AddrModeFlat;
	sf.AddrStack.Mode = AddrModeFlat;

	char aSymBuf[sizeof(SYMBOL_INFO) + 256];
	for (int i = 0; i < 40; ++i)
	{
		if (!StackWalk64(lMachine, hProc, GetCurrentThread(), &sf, &aCtx, NULL,
						 SymFunctionTableAccess64, SymGetModuleBase64, NULL))
			break;
		const DWORD64 lAddr = sf.AddrPC.Offset;
		if (lAddr == 0)
			break;

		char aMod[MAX_PATH] = "?";
		HMODULE hMod = NULL;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
							   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
							   (LPCSTR)(uintptr_t)lAddr, &hMod))
			GetModuleFileNameA(hMod, aMod, MAX_PATH);
		const char *sMod = strrchr(aMod, '\\') ? strrchr(aMod, '\\') + 1 : aMod;

		SYMBOL_INFO *pSym = (SYMBOL_INFO *)aSymBuf;
		memset(aSymBuf, 0, sizeof(aSymBuf));
		pSym->SizeOfStruct = sizeof(SYMBOL_INFO);
		pSym->MaxNameLen = 255;
		DWORD64 lDisp = 0;
		if (SymFromAddr(hProc, lAddr, &lDisp, pSym))
		{
			IMAGEHLP_LINE64 line;
			memset(&line, 0, sizeof(line));
			line.SizeOfStruct = sizeof(line);
			DWORD lLineDisp = 0;
			if (SymGetLineFromAddr64(hProc, lAddr, &lLineDisp, &line))
				Log("  #%02d %s!%s  (%s:%u)\n", i, sMod, pSym->Name, line.FileName, (unsigned)line.LineNumber);
			else
				Log("  #%02d %s!%s+0x%X\n", i, sMod, pSym->Name, (unsigned)lDisp);
		}
		else
			Log("  #%02d %s+0x%X\n", i, sMod, (unsigned)(lAddr - (DWORD64)(uintptr_t)hMod));
	}
	SymCleanup(hProc);
}

static void CrashReport(const char *asWhat, CONTEXT *apCtx)
{
	if (gbCrashLogged)
		return; /* abort() after a logged terminate: report once */
	gbCrashLogged = true;
	Log("\n==================== CRASH: %s ====================\n", asWhat);
	CONTEXT ctx;
	if (apCtx)
		ctx = *apCtx;
	else
		RtlCaptureContext(&ctx);
	CrashWalkStack(ctx);
	Log("==== send this hpl.log with the bug report ====\n");
}

static LONG WINAPI CrashSehFilter(EXCEPTION_POINTERS *apInfo)
{
	char aWhat[96];
	sprintf(aWhat, "exception 0x%08lX at 0x%p",
			apInfo->ExceptionRecord->ExceptionCode, apInfo->ExceptionRecord->ExceptionAddress);
	CrashReport(aWhat, apInfo->ContextRecord);
	return EXCEPTION_CONTINUE_SEARCH;
}

static void CrashOnFatalSignal(int alSig)
{
	/* the C runtime turned a hardware fault into a signal: it keeps the
	   real faulting context for us while the handler runs */
	EXCEPTION_POINTERS *pInfo = (EXCEPTION_POINTERS *)_pxcptinfoptrs;
	if (pInfo && pInfo->ExceptionRecord && pInfo->ContextRecord)
		CrashSehFilter(pInfo);
	else
		CrashReport(alSig == SIGSEGV ? "SIGSEGV" : alSig == SIGILL ? "SIGILL" : "SIGFPE", NULL);
}

static void CrashOnAbort(int)
{
	CrashReport("abort() (a library or the runtime gave up)", NULL);
}

static void CrashOnTerminate()
{
	CrashReport("uncaught C++ exception (std::terminate)", NULL);
	abort();
}

static void CrashOnPureCall()
{
	CrashReport("pure virtual function call", NULL);
	abort();
}

static bool gbNormalExit = false;

static void CrashOnExit()
{
	/* exit() while the game was still running: something decided to quit
	   without telling us (a library error path) */
	if (!gbNormalExit)
		CrashReport("exit() called while the game was running", NULL);
}

/* SDL_image, libpng, jpeg, zlib (msvcrt.dll) and the old Frictional DLLs
   (msvcr71.dll) bring their own C runtime with its own signal table: an
   abort() in them skips ours and just ends the process with code 3. */
typedef void (__cdecl *tCrtSignalHandler)(int);
typedef tCrtSignalHandler (__cdecl *tCrtSignalFunc)(int, tCrtSignalHandler);

static void HookOtherCrtAbort(const char *asDll)
{
	HMODULE hCrt = LoadLibraryA(asDll);
	if (hCrt == NULL)
		return;
	tCrtSignalFunc pSignal = (tCrtSignalFunc)GetProcAddress(hCrt, "signal");
	if (pSignal)
		pSignal(SIGABRT, CrashOnAbort);
}

static void InstallCrashReporter()
{
	SetUnhandledExceptionFilter(CrashSehFilter);
	signal(SIGABRT, CrashOnAbort);
	signal(SIGSEGV, CrashOnFatalSignal);
	signal(SIGILL, CrashOnFatalSignal);
	signal(SIGFPE, CrashOnFatalSignal);
	std::set_terminate(CrashOnTerminate);
	_set_purecall_handler(CrashOnPureCall);
	static bool bAtExit = false;
	if (!bAtExit)
	{
		bAtExit = true;
		atexit(CrashOnExit);
	}
	HookOtherCrtAbort("msvcrt.dll");
	HookOtherCrtAbort("msvcr71.dll");
}
#endif

int hplMain(const tString& asCommandLine)
{
#ifdef WIN32
	InstallCrashReporter();
#endif
	cInit *pInit = hplNew( cInit, () );

	bool bRet = pInit->Init(asCommandLine);
	
	if(bRet==false){
		hplDelete( pInit->mpGame );
		CreateMessageBoxW(_W("Error!"),pInit->msErrorMessage.c_str());
		OpenBrowserWindow(_W("http://support.frictionalgames.com"));
#ifdef WIN32
		gbNormalExit = true;
#endif
		return 1;
	}

#ifdef WIN32
	/* drivers and libraries loaded during Init may have put in their own
	   crash filter; ours goes back on top */
	InstallCrashReporter();
#endif
	pInit->Run();

	pInit->Exit();

	hplDelete( pInit );
	
	cMemoryManager::LogResults();

#ifdef WIN32
	gbNormalExit = true;
#endif
	return 0;
}
