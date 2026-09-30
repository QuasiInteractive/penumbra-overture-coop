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
#include "TechDemoPaths.h"

#include <stdio.h>
#include <string.h>
#include <vector>

#ifdef WIN32
	#include <windows.h>
#else
	#include <unistd.h>
	#include <sys/stat.h>
#endif

static std::string Slashes(std::string s)
{
	for (size_t i = 0; i < s.size(); ++i)
		if (s[i] == '\\') s[i] = '/';
	while (s.size() > 3 && s[s.size() - 1] == '/')
		s.erase(s.size() - 1);
	return s;
}

std::string TdParentFolder(const std::string &asFolder)
{
	const std::string s = Slashes(asFolder);
	const size_t pos = s.find_last_of('/');
	if (pos == std::string::npos || pos == 0)
		return "";
	return s.substr(0, pos);
}

std::string TdExeFolder()
{
#ifdef WIN32
	char aPath[MAX_PATH] = "";
	GetModuleFileNameA(NULL, aPath, MAX_PATH);
	return TdParentFolder(aPath);
#else
	char aPath[4096] = "";
	ssize_t n = readlink("/proc/self/exe", aPath, sizeof(aPath) - 1);
	if (n <= 0) return ".";
	aPath[n] = 0;
	return TdParentFolder(aPath);
#endif
}

bool TdFileExists(const std::string &asPath)
{
#ifdef WIN32
	const DWORD a = GetFileAttributesA(asPath.c_str());
	return a != INVALID_FILE_ATTRIBUTES;
#else
	struct stat st;
	return stat(asPath.c_str(), &st) == 0;
#endif
}

static bool IsOvertureFolder(const std::string &asFolder)
{
	return !asFolder.empty() &&
		   TdFileExists(asFolder + "/resources.cfg") &&
		   TdFileExists(asFolder + "/config/game.cfg");
}

#ifdef WIN32
static std::string RegString(HKEY aRoot, const char *asKey, const char *asValue)
{
	char aBuf[1024] = "";
	DWORD lSize = sizeof(aBuf);
	if (RegGetValueA(aRoot, asKey, asValue, RRF_RT_REG_SZ, NULL, aBuf, &lSize) == ERROR_SUCCESS)
		return Slashes(aBuf);
	return "";
}

/* the "path" entries of steamapps/libraryfolders.vdf (plus Steam itself) */
static std::vector<std::string> SteamLibraries()
{
	std::vector<std::string> vLibs;
	std::string sSteam = RegString(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath");
	if (sSteam.empty())
		sSteam = RegString(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath");
	if (sSteam.empty())
		return vLibs;
	vLibs.push_back(sSteam);

	FILE *pF = fopen((sSteam + "/steamapps/libraryfolders.vdf").c_str(), "r");
	if (pF == NULL)
		return vLibs;
	char aLine[1024];
	while (fgets(aLine, sizeof(aLine), pF))
	{
		/* <tabs>"path"<tabs>"D:\\SteamLibrary" */
		const char *p = strstr(aLine, "\"path\"");
		if (p == NULL) continue;
		p = strchr(p + 6, '"');
		if (p == NULL) continue;
		const char *pEnd = strrchr(p + 1, '"');
		if (pEnd == NULL || pEnd <= p + 1) continue;
		std::string sPath(p + 1, pEnd);
		/* vdf escapes backslashes as a pair */
		std::string sClean;
		for (size_t i = 0; i < sPath.size(); ++i)
		{
			if (sPath[i] == '\\' && i + 1 < sPath.size() && sPath[i + 1] == '\\') ++i;
			sClean += sPath[i];
		}
		vLibs.push_back(Slashes(sClean));
	}
	fclose(pF);
	return vLibs;
}
#endif

std::string TdFindOvertureFolder()
{
#ifdef WIN32
	std::vector<std::string> vLibs = SteamLibraries();
	for (size_t i = 0; i < vLibs.size(); ++i)
	{
		const std::string s = vLibs[i] + "/steamapps/common/Penumbra Overture/redist";
		if (IsOvertureFolder(s)) return s;
	}
	const char *vOthers[] = {
		"C:/GOG Games/Penumbra Overture",
		"C:/Program Files (x86)/GOG Galaxy/Games/Penumbra Overture",
		"C:/Program Files (x86)/Penumbra Overture/redist",
		"C:/Program Files/Penumbra Overture/redist",
		"C:/Program Files (x86)/Steam/steamapps/common/Penumbra Overture/redist",
	};
	for (size_t i = 0; i < sizeof(vOthers) / sizeof(vOthers[0]); ++i)
		if (IsOvertureFolder(vOthers[i])) return vOthers[i];
#endif
	return "";
}

bool TdSetWorkingFolder(const std::string &asPath)
{
#ifdef WIN32
	return SetCurrentDirectoryA(asPath.c_str()) != 0;
#else
	return chdir(asPath.c_str()) == 0;
#endif
}

bool TdCopyFileIfMissing(const std::string &asFrom, const std::string &asTo)
{
	if (TdFileExists(asTo)) return true;
#ifdef WIN32
	return CopyFileA(asFrom.c_str(), asTo.c_str(), TRUE) != 0;
#else
	FILE *pIn = fopen(asFrom.c_str(), "rb");
	if (!pIn) return false;
	FILE *pOut = fopen(asTo.c_str(), "wb");
	if (!pOut) { fclose(pIn); return false; }
	char aBuf[4096];
	size_t n;
	while ((n = fread(aBuf, 1, sizeof(aBuf), pIn)) > 0) fwrite(aBuf, 1, n, pOut);
	fclose(pIn); fclose(pOut);
	return true;
#endif
}

#ifdef WIN32
static BOOL CALLBACK TdSetIconProc(HWND ahWnd, LPARAM)
{
	HINSTANCE hInst = GetModuleHandleA(NULL);
	HICON hBig = (HICON)LoadImageA(hInst, MAKEINTRESOURCEA(1), IMAGE_ICON,
								   GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
	HICON hSmall = (HICON)LoadImageA(hInst, MAKEINTRESOURCEA(1), IMAGE_ICON,
									 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
	if (hBig) SendMessageA(ahWnd, WM_SETICON, ICON_BIG, (LPARAM)hBig);
	if (hSmall) SendMessageA(ahWnd, WM_SETICON, ICON_SMALL, (LPARAM)hSmall);
	return TRUE;
}
#endif

void TdApplyExeIconToWindows()
{
#ifdef WIN32
	EnumThreadWindows(GetCurrentThreadId(), TdSetIconProc, 0);
#endif
}
