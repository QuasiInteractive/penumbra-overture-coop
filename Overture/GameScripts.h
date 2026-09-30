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
#ifndef GAME_SCRIPTS_H
#define GAME_SCRIPTS_H

#include "StdAfx.h"

#include "GameTypes.h"

using namespace hpl;

//------------------------------------
class cGameScripts
{
public:
	static void Init();
};


/** Penumbra co-op (protocol v9): apply a replicated script mutation coming
    off the wire. Implemented in GameScripts.cpp so it can reuse the exact
    script-function bodies with re-broadcast suppressed. */
void NetApplyScriptEvent(int alOp, const hpl::tString &asName, int alVal);

/** Penumbra co-op (v14). gbNetScriptApplying: true while a REPLICATED
    mutation is applied (script hooks stay silent). gbNetScriptPlayerContext:
    true while a script runs because of something only THIS player did
    (pick/interact/examine, player collide, inventory use/pickup/combine, a
    message-box callback, a numerical panel, a lamp lit-change) — the Add*Var
    replication forwards a guest's Add to the host only from such a context;
    symmetric scripts (OnStart/OnLoad/OnUpdate/timers/entity collide) run on
    every machine and only the HOST's result is broadcast (as an absolute
    Set), so a counter is never incremented twice. Both defined in
    GameScripts.cpp; neither has any effect without a live session. */
extern bool gbNetScriptApplying;
extern bool gbNetScriptPlayerContext;

/** RAII: marks the enclosed RunScriptCommand as player-driven (nests). */
struct cNetScriptPlayerScope
{
	bool mbPrev;
	cNetScriptPlayerScope() : mbPrev(gbNetScriptPlayerContext) { gbNetScriptPlayerContext = true; }
	~cNetScriptPlayerScope() { gbNetScriptPlayerContext = mbPrev; }
private:
	cNetScriptPlayerScope(const cNetScriptPlayerScope &);
	cNetScriptPlayerScope &operator=(const cNetScriptPlayerScope &);
};

#endif // GAME_SCRIPTS_H
