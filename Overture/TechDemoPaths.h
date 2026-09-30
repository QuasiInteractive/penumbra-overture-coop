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
#ifndef GAME_TECHDEMO_PATHS_H
#define GAME_TECHDEMO_PATHS_H

#include <string>

/* Folder helpers for the tech demo co-op install (the exe in the tech
   demo's redist/coop folder). Kept apart from the engine headers: they need
   <windows.h>. Paths use '/', no trailing '/'. */

/** The folder the running exe is in. */
std::string TdExeFolder();

/** The folder above asFolder ("" when there is none). */
std::string TdParentFolder(const std::string &asFolder);

bool TdFileExists(const std::string &asPath);

/** Penumbra Overture's game folder (the one holding resources.cfg and
    config/game.cfg), looked up in every Steam library, then the usual
    GOG and retail folders. "" when none is found. */
std::string TdFindOvertureFolder();

/** Makes asPath the working folder (relative file reads go there). */
bool TdSetWorkingFolder(const std::string &asPath);

/** Gives this thread's windows the exe's own icon (SDL 1.2 shows a
    generic one in the title bar and taskbar). */
void TdApplyExeIconToWindows();

/** Copies a file, keeping an existing destination. */
bool TdCopyFileIfMissing(const std::string &asFrom, const std::string &asTo);

#endif // GAME_TECHDEMO_PATHS_H
