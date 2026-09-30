/*
 * Copyright (C) 2006-2010 - Frictional Games
 *
 * This file is part of HPL1 Engine.
 *
 * HPL1 Engine is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HPL1 Engine is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HPL1 Engine.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "impl/LowLevelInputSDL.h"

#include "impl/MouseSDL.h"
#include "impl/KeyboardSDL.h"

#include "system/LowLevelSystem.h"
#include "system/String.h"

#include <stdlib.h>
#include <string.h>

namespace hpl {

	//-----------------------------------------------------------------------

	/* Dev aid: HPL_AUTOINPUT="<sec>:<what>|..." feeds timed input so a test
	   run needs nobody at the keyboard. <what>: click, rclick, key=<k>
	   (press + release), down=<k>, up=<k>, move=<x>,<y>. <k>: esc, enter,
	   space, tab, shift, ctrl, or one letter/digit. */
	static SDLKey AutoInputKey(const tString &asName)
	{
		const tString s = cString::ToLowerCase(asName);
		if (s == "esc") return SDLK_ESCAPE;
		if (s == "enter") return SDLK_RETURN;
		if (s == "space") return SDLK_SPACE;
		if (s == "tab") return SDLK_TAB;
		if (s == "shift") return SDLK_LSHIFT;
		if (s == "ctrl") return SDLK_LCTRL;
		if (s.size() == 1) return (SDLKey)s[0];
		return SDLK_UNKNOWN;
	}

	static void AutoInputKeyEvent(SDLKey aKey, bool abDown)
	{
		SDL_Event ev;
		memset(&ev, 0, sizeof(ev));
		ev.type = abDown ? SDL_KEYDOWN : SDL_KEYUP;
		ev.key.state = abDown ? SDL_PRESSED : SDL_RELEASED;
		ev.key.keysym.sym = aKey;
		ev.key.keysym.unicode = (aKey < 128) ? (Uint16)aKey : 0;
		SDL_PushEvent(&ev);
	}

	static int glAutoMouseX = 0, glAutoMouseY = 0; /* fake pointer: no focus needed */

	static void AutoInputMouseButton(int alButton, bool abDown)
	{
		const int x = glAutoMouseX, y = glAutoMouseY;
		SDL_Event ev;
		memset(&ev, 0, sizeof(ev));
		ev.type = abDown ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
		ev.button.button = (Uint8)alButton;
		ev.button.state = abDown ? SDL_PRESSED : SDL_RELEASED;
		ev.button.x = (Uint16)x;
		ev.button.y = (Uint16)y;
		SDL_PushEvent(&ev);
	}

	static void AutoInputPump()
	{
		struct tStep { unsigned long mlTime; tString msWhat; };
		static std::vector<tStep> vSteps;
		static bool bRead = false;
		static unsigned long lStart = 0;
		if (!bRead)
		{
			bRead = true;
			lStart = SDL_GetTicks();
			const char *pEnv = getenv("HPL_AUTOINPUT");
			if (pEnv == NULL) return;
			tStringVec vParts;
			tString sSep = "|";
			cString::GetStringVec(pEnv, vParts, &sSep);
			for (size_t i = 0; i < vParts.size(); ++i)
			{
				const size_t lColon = vParts[i].find(':');
				if (lColon == tString::npos) continue;
				tStep step;
				step.mlTime = (unsigned long)(cString::ToFloat(vParts[i].substr(0, lColon).c_str(), 0) * 1000);
				step.msWhat = vParts[i].substr(lColon + 1);
				const tString sW = cString::ToLowerCase(step.msWhat);
				/* a press is a down now and an up 100 ms later */
				if (sW == "click" || sW == "rclick" || sW.find("key=") == 0)
				{
					tStep up = step;
					up.mlTime += 100;
					up.msWhat = sW == "click" ? "clickup" : sW == "rclick" ? "rclickup" : "up=" + step.msWhat.substr(4);
					step.msWhat = sW.find("key=") == 0 ? "down=" + step.msWhat.substr(4) : sW;
					vSteps.push_back(step);
					vSteps.push_back(up);
				}
				else
					vSteps.push_back(step);
			}
		}
		const unsigned long lNow = SDL_GetTicks() - lStart;
		for (size_t i = 0; i < vSteps.size();)
		{
			if (vSteps[i].mlTime > lNow) { ++i; continue; }
			const tString sW = cString::ToLowerCase(vSteps[i].msWhat);
			Log(" autoinput: %s\n", sW.c_str());
			if (sW == "click") AutoInputMouseButton(SDL_BUTTON_LEFT, true);
			else if (sW == "clickup") AutoInputMouseButton(SDL_BUTTON_LEFT, false);
			else if (sW == "rclick") AutoInputMouseButton(SDL_BUTTON_RIGHT, true);
			else if (sW == "rclickup") AutoInputMouseButton(SDL_BUTTON_RIGHT, false);
			else if (sW.find("down=") == 0) AutoInputKeyEvent(AutoInputKey(sW.substr(5)), true);
			else if (sW.find("up=") == 0) AutoInputKeyEvent(AutoInputKey(sW.substr(3)), false);
			else if (sW.find("move=") == 0)
			{
				const tString sXY = sW.substr(5);
				const size_t lComma = sXY.find(',');
				if (lComma != tString::npos)
				{
					const int x = atoi(sXY.substr(0, lComma).c_str()), y = atoi(sXY.substr(lComma + 1).c_str());
					SDL_Event ev;
					memset(&ev, 0, sizeof(ev));
					ev.type = SDL_MOUSEMOTION;
					ev.motion.x = (Uint16)x;
					ev.motion.y = (Uint16)y;
					ev.motion.xrel = (Sint16)(x - glAutoMouseX);
					ev.motion.yrel = (Sint16)(y - glAutoMouseY);
					SDL_PushEvent(&ev);
					glAutoMouseX = x;
					glAutoMouseY = y;
				}
			}
			vSteps.erase(vSteps.begin() + i);
		}
	}

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cLowLevelInputSDL::cLowLevelInputSDL(iLowLevelGraphics *apLowLevelGraphics)
	{
		mpLowLevelGraphics = apLowLevelGraphics;
		LockInput(true);
	}

	//-----------------------------------------------------------------------

	cLowLevelInputSDL::~cLowLevelInputSDL()
	{
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHOD
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	void cLowLevelInputSDL::LockInput(bool abX)
	{
		SDL_WM_GrabInput(abX ? SDL_GRAB_ON : SDL_GRAB_OFF);
	}

	//-----------------------------------------------------------------------

	void cLowLevelInputSDL::BeginInputUpdate()
	{
		//SDL_PumpEvents();
		AutoInputPump();

		SDL_Event sdlEvent;

		while(SDL_PollEvent(&sdlEvent)!=0)
		{
			mlstEvents.push_back(sdlEvent);
		}
	}

	//-----------------------------------------------------------------------

	void cLowLevelInputSDL::EndInputUpdate()
	{
		mlstEvents.clear();
	}

	//-----------------------------------------------------------------------

	iMouse* cLowLevelInputSDL::CreateMouse()
	{
		return hplNew( cMouseSDL,(this,mpLowLevelGraphics));
	}

	//-----------------------------------------------------------------------

	iKeyboard* cLowLevelInputSDL::CreateKeyboard()
	{
		return hplNew( cKeyboardSDL,(this) );
	}

	//-----------------------------------------------------------------------

}
