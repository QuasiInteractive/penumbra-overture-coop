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
#include "MainMenu.h"

#include "Init.h"
#include "GraphicsHelper.h"
#include "ButtonHandler.h"
#include "MapHandler.h"
#include "SaveHandler.h"
#include "IntroStory.h"
#include "Player.h"
#include "PlayerHelper.h"
#include "EffectHandler.h"
#include "HapticGameCamera.h"

#ifdef PENUMBRA_MULTIPLAYER
#include "multiplayer/NetworkManager.h"
#include "input/Keyboard.h"
#include <cctype>
#include <cstring>
#endif

#include "OALWrapper/OAL_Init.h"

float gfMenuFadeAmount;
bool gbMustRestart=false;

static eMainMenuState gvMenuBackStates[] = {
		eMainMenuState_Exit,//eMainMenuState_Start,
		eMainMenuState_Start,//eMainMenuState_NewGame,
		eMainMenuState_Start,//eMainMenuState_Exit,
		eMainMenuState_Start,//eMainMenuState_Continue,
		eMainMenuState_Start,//eMainMenuState_Resume,

		eMainMenuState_Start,//eMainMenuState_Multiplayer,
		eMainMenuState_Multiplayer,//eMainMenuState_MultiplayerHostLobby,
		eMainMenuState_Multiplayer,//eMainMenuState_MultiplayerJoin,
		eMainMenuState_Multiplayer,//eMainMenuState_MultiplayerName (v13)
		eMainMenuState_Multiplayer,//eMainMenuState_MultiplayerBrowser,
		eMainMenuState_MultiplayerBrowser,//eMainMenuState_MultiplayerPassword,

		eMainMenuState_Start,//eMainMenuState_LoadGameSpot,
		eMainMenuState_Start,//eMainMenuState_LoadGameAuto,
		eMainMenuState_Start,//eMainMenuState_LoadGameFavorite,

		eMainMenuState_Start,//eMainMenuState_Options,
		eMainMenuState_Options,//eMainMenuState_OptionsGraphics,
		eMainMenuState_OptionsGraphics,//eMainMenuState_OptionsGraphicsAdvanced,
		eMainMenuState_Options,//eMainMenuState_OptionsControls,
		eMainMenuState_Options,//eMainMenuState_OptionsGame,
		eMainMenuState_Options,//eMainMenuState_OptionsSound,
		eMainMenuState_OptionsControls,//eMainMenuState_OptionsKeySetupMove,
		eMainMenuState_OptionsControls,//eMainMenuState_OptionsKeySetupAction,
		eMainMenuState_OptionsControls,//eMainMenuState_OptionsKeySetupMisc,

		eMainMenuState_Options,//eMainMenuState_GraphicsRestart,

		eMainMenuState_Start,	//eMainMenuState_FirstStart,
};

#ifdef PENUMBRA_MULTIPLAYER
class cMainMenuWidget_MultiIpLine;
class cMainMenuWidget_MultiServerRow;
class cMainMenuWidget_MultiPublicToggle;

namespace {

static cMainMenuWidget_Text *gpMulJoinFoot = NULL;
static cMainMenuWidget_Text *gpMulHostFoot = NULL;

static cMainMenuWidget_MultiIpLine *gpMulTypedIp = NULL;
static bool gMulJoinAwaitHandshake = false;

/* v13 username screen: the typing line (name mode), its status line and the
   "Playing as ..." line on the Multiplayer screen. */
static cMainMenuWidget_MultiIpLine *gpMulTypedName = NULL;
static cMainMenuWidget_Text *gpMulNameFoot = NULL;
static cMainMenuWidget_Text *gpMulNameShown = NULL;
/* Server browser screen: Internet tab = master server list, LAN tab = the
   broadcast scan. Rows are fixed slots showing one page of the network
   manager's result vector, re-synced every frame (cheap: string copies);
   the status line / selection / page live further down (browser section). */
static const int kMulBrowserRows = 8;
static std::vector<cMainMenuWidget_MultiServerRow *> gvMulRows;
static bool gMulBrowserInternet = true;
/* Password prompt: the locked row that was chosen, joined once typed (its
   name / address are drawn by the prompt's panel). */
static cMainMenuWidget_MultiIpLine *gpMulTypedPw = NULL;
static cDiscoveredServer gMulPendingServer;
/* Host lobby: 'Public (list on master)' toggle + the port-forward note. */
static cMainMenuWidget_MultiPublicToggle *gpMulPublicToggle = NULL;
static cMainMenuWidget_Text *gpMulHostPublicNote = NULL;

static tString MulTrimAscii(const tString &s)
{
	size_t a = 0, b = s.size();
	while (a < b && (s[a] == ' ' || s[a] == '\t'))
		++a;
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t'))
		--b;
	if (a >= b)
		return "";
	return s.substr(a, b - a);
}

static bool MulIpv4LikeHostPort(const tString &s)
{
	size_t colons = 0;
	for (size_t i = 0; i < s.size(); ++i)
		if (s[i] == ':')
			++colons;
	return colons == 1;
}

static tString MulNormalizeJoinAddr(const tString &in, uint16_t defPort)
{
	tString s = MulTrimAscii(in);
	if (s.empty())
		s = "127.0.0.1";
	if (MulIpv4LikeHostPort(s))
		return s;
	return s + ":" + cString::ToString((int)defPort);
}

static int KeypadDigit(eKey mk)
{
	if (mk >= eKey_KP0 && mk <= eKey_KP9)
		return static_cast<int>(mk - eKey_KP0);
	return -1;
}

} 

//-----------------------------------------------------------------------
// Drawing kit for the multiplayer screens (server browser, password
// prompt, character picker). Only primitives the menu already uses:
// solid quads = effect_white.jpg through the diffalpha2d material tinted
// with a colour (what cMainMenuWidget_List draws its back and selection
// with), text = font_menu_small.fnt through iFontData::Draw. Everything is
// drawn in the 800x600 menu space; z: panel 28-32, boxes/rows 33-36,
// text 40 (the widgets' own z), mouse 100.
//-----------------------------------------------------------------------

namespace {

/** One shared white quad, created on first use, released by ~cMainMenu. */
static cGfxObject *gpMulWhiteGfx = NULL;
static cGraphicsDrawer *gpMulWhiteDrawer = NULL;

static void MulFillRect(cGraphicsDrawer *apDrawer, float afX, float afY, float afW, float afH,
						float afZ, const cColor &aCol)
{
	if (apDrawer == NULL || afW <= 0 || afH <= 0)
		return;
	if (gpMulWhiteGfx == NULL)
	{
		gpMulWhiteGfx = apDrawer->CreateGfxObject("effect_white.jpg", "diffalpha2d");
		gpMulWhiteDrawer = apDrawer;
	}
	if (gpMulWhiteGfx == NULL)
		return;
	apDrawer->DrawGfxObject(gpMulWhiteGfx, cVector3f(afX, afY, afZ), cVector2f(afW, afH), aCol);
}

/** 1-px (afT) outline just inside the rectangle. */
static void MulFrameRect(cGraphicsDrawer *apDrawer, float afX, float afY, float afW, float afH,
						 float afZ, const cColor &aCol, float afT = 1.0f)
{
	MulFillRect(apDrawer, afX, afY, afW, afT, afZ, aCol);
	MulFillRect(apDrawer, afX, afY + afH - afT, afW, afT, afZ, aCol);
	MulFillRect(apDrawer, afX, afY + afT, afT, afH - 2 * afT, afZ, aCol);
	MulFillRect(apDrawer, afX + afW - afT, afY + afT, afT, afH - 2 * afT, afZ, aCol);
}

/** A 12x12 padlock (shackle + body + keyhole) with its top-left at x,y. */
static void MulDrawLock(cGraphicsDrawer *apDrawer, float afX, float afY, float afZ, const cColor &aCol)
{
	MulFillRect(apDrawer, afX + 2.5f, afY, 7.0f, 1.5f, afZ, aCol);         /* shackle top */
	MulFillRect(apDrawer, afX + 2.5f, afY, 1.5f, 5.5f, afZ, aCol);         /* shackle left */
	MulFillRect(apDrawer, afX + 8.0f, afY, 1.5f, 5.5f, afZ, aCol);         /* shackle right */
	MulFillRect(apDrawer, afX, afY + 5.0f, 12.0f, 7.0f, afZ, aCol);        /* body */
	MulFillRect(apDrawer, afX + 5.25f, afY + 7.0f, 1.5f, 3.0f, afZ + 0.5f, /* keyhole */
				cColor(0.05f, 0.05f, 0.08f, 1.0f));
}

static void MulReleaseGfx()
{
	if (gpMulWhiteGfx && gpMulWhiteDrawer)
		gpMulWhiteDrawer->DestroyGfxObject(gpMulWhiteGfx);
	gpMulWhiteGfx = NULL;
	gpMulWhiteDrawer = NULL;
}

/** Text is ALWAYS drawn through "%ls": server names, maps and player names
    come off the network and a '%' in one must not reach the formatter. */
static void MulText(iFontData *apFont, float afX, float afY, float afZ, float afSize,
					const cColor &aCol, eFontAlign aAlign, const tWString &asText)
{
	if (apFont == NULL || asText.empty())
		return;
	apFont->Draw(cVector3f(afX, afY, afZ), cVector2f(afSize, afSize), aCol, aAlign, _W("%ls"),
				 asText.c_str());
}

static float MulTextWidth(iFontData *apFont, float afSize, const tWString &asText)
{
	if (apFont == NULL || asText.empty())
		return 0;
	return apFont->GetLength(cVector2f(afSize, afSize), asText.c_str());
}

/** asText, cut with "..." so it is at most afMaxW wide. */
static tWString MulFitText(iFontData *apFont, float afSize, const tWString &asText, float afMaxW)
{
	if (afMaxW <= 0 || MulTextWidth(apFont, afSize, asText) <= afMaxW)
		return asText;
	const tWString kDots = _W("...");
	tWString sCut = asText;
	while (!sCut.empty())
	{
		sCut.erase(sCut.size() - 1);
		while (!sCut.empty() && sCut[sCut.size() - 1] == L' ')
			sCut.erase(sCut.size() - 1);
		const tWString sTry = sCut + kDots;
		if (MulTextWidth(apFont, afSize, sTry) <= afMaxW)
			return sTry;
	}
	return kDots;
}

static bool MulMouseIn(cInit *apInit, const cRect2f &aRect)
{
	if (apInit == NULL || apInit->mpMainMenu == NULL || aRect.w <= 0 || aRect.h <= 0)
		return false;
	return cMath::PointBoxCollision(apInit->mpMainMenu->GetMousePos(), aRect);
}

static iFontData *MulMenuFont(cInit *apInit)
{
	return apInit->mpGame->GetResources()->GetFontManager()->CreateFontData("font_menu_small.fnt", 30);
}

static void MulClickSound(cInit *apInit)
{
	apInit->mpGame->GetSound()->GetSoundHandler()->PlayGui("gui_menu_click", false, 1);
}

/* Palette: the menu's own blues (cMainMenuWidget_Button's glow 0.1/0.32/1.0,
   cMainMenuWidget_List's back 0.05/0.05/0.1 and selection 0/0/0.73). */
static cColor MulColPanel() { return cColor(0.02f, 0.025f, 0.05f, 0.86f); }
static cColor MulColTitleBar() { return cColor(0.05f, 0.09f, 0.22f, 0.92f); }
static cColor MulColBorder() { return cColor(0.28f, 0.34f, 0.52f, 0.9f); }
static cColor MulColAccent() { return cColor(0.25f, 0.5f, 1.0f, 0.95f); }
static cColor MulColBox() { return cColor(0.07f, 0.08f, 0.13f, 0.88f); }
static cColor MulColBoxHover() { return cColor(0.11f, 0.17f, 0.38f, 0.92f); }
static cColor MulColSelected() { return cColor(0.0f, 0.08f, 0.6f, 0.85f); }
static cColor MulColText() { return cColor(0.78f, 1.0f); }
static cColor MulColTextBright() { return cColor(1.0f, 1.0f); }
static cColor MulColTextDim() { return cColor(0.45f, 1.0f); }
static cColor MulColWarn() { return cColor(0.95f, 0.5f, 0.4f, 1.0f); }
static cColor MulColGold() { return cColor(0.9f, 0.78f, 0.42f, 1.0f); }

} // namespace

/** ASCII typing line. Address mode (default): IPv4/host:port characters,
    220 max. Name mode (v13, abNameMode): any printable ASCII incl. spaces,
    kNetPlayerNameMaxChars max, re-seeded from the current player name on
    every activation; Enter is reported through TakeEnter(). */
class cMainMenuWidget_MultiIpLine : public cMainMenuWidget_Text
{
public:
	cMainMenuWidget_MultiIpLine(cInit *apInit, const cVector3f &avPos, const tString &asciiSeed,
							   cVector2f avFontSize, eFontAlign aAlignment, bool abNameMode = false);

	void OnMouseDown(eMButton aButton);

	void PollTyping();

	void BlurTyping() { mbTypingFocus = false; }

	/** Focus arms select-all: the first keypress replaces the seeded address
	    wholesale instead of appending to it. */
	void FocusTyping() { mbTypingFocus = true; mbSelectAll = true; }

	bool IsTypingFocused() const { return mbTypingFocus; }

	const tString &GetAscii() const { return msAscii; }
	void SetAscii(const tString &asAscii)
	{
		msAscii = asAscii;
		mbSelectAll = false;
		FlushToWide();
	}

	/** Password prompt reuse: any printable ASCII is accepted (not just
	    address characters) and the field draws asterisks. */
	void SetPasswordMode(bool abX) { mbPasswordMode = abX; }

	/** Draw as a boxed input field afWidth wide (no [ ] brackets); the
	    whole box is the click target. 0 = the classic '[ text_ ]' line. */
	void SetFieldBox(float afWidth) { mfBoxW = afWidth; UpdateHitBox(); }

	/** v13: Enter was pressed while focused since the last call (one-shot). */
	bool TakeEnter()
	{
		const bool b = mbEnterPressed;
		mbEnterPressed = false;
		return b;
	}

	virtual void OnActivate()
	{
		mbTypingFocus = false;
		mbEnterPressed = false;
		if (mbNameMode && mpInit && mpInit->mpNetworkManager)
		{
			/* the screen always opens on the CURRENT name (renames included) */
			msAscii = mpInit->mpNetworkManager->GetLocalPlayerName();
			FlushToWide();
		}
		cMainMenuWidget::OnActivate();
	}

	virtual void OnDraw();

private:
	tString msAscii;
	bool mbTypingFocus;
	bool mbSelectAll;
	bool mbNameMode;     /**< v13: username instead of an address */
	bool mbEnterPressed; /**< v13: see TakeEnter */
	bool mbPasswordMode; /**< server-browser password prompt: any printable ASCII, drawn as asterisks */
	float mfBoxW;        /**< SetFieldBox width, 0 = bracket line */

	void FlushToWide();
	/** The shown text ('[ ' ... ' ]' or the boxed text, no cursor). */
	tWString ShownText(bool abBrackets) const;
	/** Hit box = what is drawn (centred text grows both ways; the plain
	    Text::UpdateSize only widened it to the right). */
	void UpdateHitBox();

	size_t MaxLen() const { return mbNameMode ? kNetPlayerNameMaxChars : 220; }

	bool CharOk(char c) const
	{
		if (mbNameMode || mbPasswordMode)
			return c >= 32 && c < 127; /* printable ASCII, spaces included */
		if (std::isalnum((unsigned char)c))
			return true;
		return c == '.' || c == ':' || c == '-' || c == '[' || c == ']';
	}
};

//-----------------------------------------------------------------------

/** v13: the Start screen's "Multiplayer" button — asks for a username
    first when multiplayer.cfg has none. */
class cMainMenuWidget_MultiEnter : public cMainMenuWidget_MainButton
{
public:
	cMainMenuWidget_MultiEnter(cInit *apInit, const cVector3f &avPos, const tWString &asText)
		: cMainMenuWidget_MainButton(apInit, avPos, asText, eMainMenuState_Multiplayer)
	{
	}

	virtual void OnMouseDown(eMButton aButton);
};

/** v13: the username screen's save button (Enter does the same). */
class cMainMenuWidget_MultiNameSave : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_MultiNameSave(cInit *apInit, const cVector3f &avPos, const tWString &lbl)
		: cMainMenuWidget_Button(apInit, avPos, lbl, eMainMenuState_LastEnum, 24, eFontAlign_Center)
	{
	}

	virtual void OnMouseDown(eMButton aButton);
};

//-----------------------------------------------------------------------

class cMainMenuWidget_MultiLobbyBack : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_MultiLobbyBack(cInit *apInit, const cVector3f &avPos, const tWString &asLabel);

	virtual void OnMouseDown(eMButton aButton);
};

//-----------------------------------------------------------------------

class cMainMenuWidget_MultiHostStartListen : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_MultiHostStartListen(cInit *apInit, const cVector3f &avPos,
										 const tWString &lbl)
		: cMainMenuWidget_Button(apInit, avPos, lbl, eMainMenuState_MultiplayerHostLobby, 24,
								 eFontAlign_Center)
	{
	}

	virtual void OnMouseDown(eMButton aButton);
};

//-----------------------------------------------------------------------

class cMainMenuWidget_MultiJoinTry : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_MultiJoinTry(cInit *apInit, const cVector3f &avPos, const tWString &lbl)
		: cMainMenuWidget_Button(apInit, avPos, lbl, eMainMenuState_LastEnum, 24, eFontAlign_Center)
	{
	}

	virtual void OnMouseDown(eMButton aButton);
};

//-----------------------------------------------------------------------

class cMainMenuWidget_MultiLaunchPlaying : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_MultiLaunchPlaying(cInit *apInit, const cVector3f &avPos, const tWString &lbl,
									   bool requireHostListen, bool requireJoinAck,
									   eGameDifficulty aDiff)
		: cMainMenuWidget_Button(apInit, avPos, lbl, eMainMenuState_LastEnum, 23, eFontAlign_Center)
		, mbListenHost(requireHostListen)
		, mbGuestAck(requireJoinAck)
		, mDifficulty(aDiff)
	{
	}

	virtual void OnMouseDown(eMButton aButton);

private:
	bool mbListenHost;
	bool mbGuestAck;
	eGameDifficulty mDifficulty;
};

//-----------------------------------------------------------------------

cMainMenuWidget_MultiIpLine::cMainMenuWidget_MultiIpLine(cInit *apInit, const cVector3f &avPos,
														 const tString &asciiSeed,
														 cVector2f avFontSize, eFontAlign aAlignment,
														 bool abNameMode)
	: cMainMenuWidget_Text(apInit, avPos, _W(""), avFontSize, aAlignment), mbTypingFocus(false), mbSelectAll(false)
	, mbNameMode(abNameMode), mbEnterPressed(false), mbPasswordMode(false), mfBoxW(0)
{
	msAscii = asciiSeed;
	FlushToWide();
	UpdateSize();
}

void cMainMenuWidget_MultiIpLine::FlushToWide()
{
	if (mbPasswordMode)
		msText = tWString(msAscii.size(), L'*');
	else if (msAscii.empty())
		msText = mbNameMode ? _W("(type name)") : _W("(type host)");
	else
		msText = cString::To16Char(msAscii);
	UpdateSize();
	UpdateHitBox();
}

tWString cMainMenuWidget_MultiIpLine::ShownText(bool abBrackets) const
{
	tWString sShow = abBrackets ? _W("[ ") : _W("");
	if (msAscii.empty())
		sShow += mbNameMode ? _W("type your name here") :
				 mbPasswordMode ? _W("type password") : _W("type address here");
	else if (mbPasswordMode)
		sShow += tWString(msAscii.size(), L'*');
	else
		sShow += cString::To16Char(msAscii);
	if (abBrackets)
		sShow += _W(" ]");
	return sShow;
}

void cMainMenuWidget_MultiIpLine::UpdateHitBox()
{
	if (mfBoxW > 0)
	{
		mRect.w = mfBoxW;
		mRect.h = mvFontSize.y + 12;
		mRect.y = mvPositon.y - 5;
	}
	else
	{
		mRect.w = mpFont->GetLength(mvFontSize, ShownText(true).c_str()) + mvFontSize.x; /* + cursor */
	}
	if (mAlignment == eFontAlign_Center)
		mRect.x = mvPositon.x - mRect.w / 2;
	else if (mAlignment == eFontAlign_Right)
		mRect.x = mvPositon.x - mRect.w;
	else
		mRect.x = mvPositon.x;
}

void cMainMenuWidget_MultiIpLine::OnDraw()
{
	/* Drawn as an obvious input field: [ address_ ] with a blinking cursor
	   while it has typing focus (it grabs focus when the screen opens). */
	static int sBlink = 0;
	++sBlink;
	const bool bCursor = mbTypingFocus && ((sBlink / 25) % 2) == 0;

	if (mfBoxW > 0)
	{
		/* boxed field (password prompt): dark box, frame lit while typing,
		   dim placeholder, text clipped from the left so the end shows */
		const float fBoxH = mvFontSize.y + 12;
		const float fX = mRect.x, fY = mvPositon.y - 5;
		MulFillRect(mpDrawer, fX, fY, mfBoxW, fBoxH, 33, cColor(0.01f, 0.01f, 0.03f, 0.9f));
		MulFrameRect(mpDrawer, fX, fY, mfBoxW, fBoxH, 34,
						mbTypingFocus ? cColor(0.25f, 0.5f, 1.0f, 0.95f) : cColor(0.3f, 0.34f, 0.5f, 0.8f));
		tWString sShow = ShownText(false);
		const float fMaxW = mfBoxW - 16 - mvFontSize.x;
		while (!msAscii.empty() && sShow.size() > 1 && mpFont->GetLength(mvFontSize, sShow.c_str()) > fMaxW)
			sShow.erase(0, 1);
		if (bCursor)
			sShow += _W("_");
		const cColor col = msAscii.empty() ? cColor(0.45f, 1.0f) : cColor(1.0f, 1.0f);
		mpFont->Draw(mvPositon, mvFontSize, col, mAlignment, _W("%ls"), sShow.c_str());
		return;
	}

	tWString sShow = _W("[ ");
	if (msAscii.empty())
		sShow += mbNameMode ? _W("type your name here") :
				 mbPasswordMode ? _W("type password") : _W("type address here");
	else if (mbPasswordMode)
		sShow += tWString(msAscii.size(), L'*');
	else
		sShow += cString::To16Char(msAscii);
	if (bCursor)
		sShow += _W("_");
	sShow += _W(" ]");

	const cColor col = mbTypingFocus ? cColor(1.0f, 1.0f) : cColor(0.7f, 1.0f);
	mpFont->Draw(mvPositon, mvFontSize, col, mAlignment, _W("%ls"), sShow.c_str());
}

void cMainMenuWidget_MultiIpLine::OnMouseDown(eMButton aButton)
{
	(void)aButton;
	FocusTyping();
}

void cMainMenuWidget_MultiIpLine::PollTyping()
{
	if (!mbTypingFocus || !IsActive())
		return;

	iKeyboard *kb = mpInit->mpGame->GetInput()->GetKeyboard();
	const int drainMax = 96;
	for (int n = 0; n < drainMax && kb->KeyIsPressed(); ++n)
	{
		cKeyPress kp = kb->GetKey();

		if (kp.mKey == eKey_ESCAPE)
		{
			mbTypingFocus = false;
			continue;
		}

		if (kp.mKey == eKey_RETURN || kp.mKey == eKey_KP_ENTER)
		{
			/* v13: BEFORE the select-all clear — Enter on the seeded name
			   keeps it (the username screen saves it). */
			mbEnterPressed = true;
			continue;
		}

		/* First edit after focus replaces the seeded address wholesale —
		   nobody wants to backspace 127.0.0.1 nine times. */
		if (mbSelectAll)
		{
			msAscii.clear();
			FlushToWide();
			mbSelectAll = false;
		}

		if (kp.mKey == eKey_BACKSPACE)
		{
			if (!msAscii.empty())
				msAscii.erase(msAscii.size() - 1);
			FlushToWide();
			continue;
		}

		int kpd = KeypadDigit(kp.mKey);
		if (kpd >= 0 && msAscii.size() < MaxLen())
		{
			msAscii += (char)('0' + kpd);
			FlushToWide();
			continue;
		}

		if ((kp.mlModifier & eKeyModifier_CTRL) && kp.mKey == eKey_v)
		{
			/* Ctrl+V replaces the whole field — the clipboard holds the
			   address a friend just sent, not a fragment to splice in. */
			tString sClip = cNetworkManager::GetClipboardTextAscii();
			tString sNew;
			for (size_t ci = 0; ci < sClip.size() && sNew.size() < MaxLen(); ++ci)
			{
				if (CharOk(sClip[ci]))
					sNew += sClip[ci];
			}
			if (!sNew.empty())
			{
				msAscii = sNew;
				mbSelectAll = false;
				FlushToWide();
			}
			continue;
		}

		if ((kp.mlModifier & eKeyModifier_CTRL))
			continue;

		int uch = kp.mlUnicode;
		if (uch >= 32 && uch < 127 && CharOk((char)uch) && msAscii.size() < MaxLen())
		{
			msAscii += (char)uch;
			FlushToWide();
		}
		else if (kp.mKey == eKey_KP_PERIOD || kp.mKey == eKey_PERIOD)
		{
			if (msAscii.size() < MaxLen())
			{
				msAscii += '.';
				FlushToWide();
			}
		}
		else if (kp.mKey == eKey_KP_MINUS || kp.mKey == eKey_MINUS)
		{
			if (msAscii.size() < MaxLen())
			{
				msAscii += '-';
				FlushToWide();
			}
		}

		else if (kp.mKey == eKey_COLON && msAscii.size() < MaxLen())
		{
			msAscii += ':';
			FlushToWide();
		}
	}
}

//-----------------------------------------------------------------------

namespace {

/* v13: the username screen's commit — the save button and Enter both land
   here. Empty (after sanitising) = stay on the screen and say so. */
static void MulSaveTypedName(cInit *apInit)
{
	if (!apInit || !apInit->mpNetworkManager || gpMulTypedName == NULL)
		return;
	const tString sName = cNetworkManager::SanitizePlayerName(gpMulTypedName->GetAscii());
	if (sName.empty())
	{
		if (gpMulNameFoot)
		{
			gpMulNameFoot->msText = _W("Type a name first (letters, digits, spaces; 24 characters max).");
			gpMulNameFoot->UpdateSize();
		}
		return;
	}
	apInit->mpNetworkManager->SetLocalPlayerName(sName); /* writes multiplayer.cfg */
	apInit->mpGame->GetSound()->GetSoundHandler()->PlayGui("gui_menu_click", false, 1);
	apInit->mpMainMenu->SetState(eMainMenuState_Multiplayer);
}

}

void cMainMenuWidget_MultiEnter::OnMouseDown(eMButton aButton)
{
	(void)aButton;
	const bool bNoName = mpInit->mpNetworkManager == NULL ||
		mpInit->mpNetworkManager->GetLocalPlayerName().empty();
	mpInit->mpMainMenu->SetState(bNoName ? eMainMenuState_MultiplayerName : eMainMenuState_Multiplayer);
	mpInit->mpGame->GetSound()->GetSoundHandler()->PlayGui("gui_menu_click", false, 1);
}

void cMainMenuWidget_MultiNameSave::OnMouseDown(eMButton aButton)
{
	(void)aButton;
	MulSaveTypedName(mpInit);
}

//-----------------------------------------------------------------------

cMainMenuWidget_MultiLobbyBack::cMainMenuWidget_MultiLobbyBack(cInit *apInit, const cVector3f &avPos,
																 const tWString &asLabel)
	: cMainMenuWidget_Button(apInit, avPos, asLabel, eMainMenuState_LastEnum, 22, eFontAlign_Center)
{
}

void cMainMenuWidget_MultiLobbyBack::OnMouseDown(eMButton aButton)
{
	gMulJoinAwaitHandshake = false;
	if (gpMulJoinFoot)
	{
		gpMulJoinFoot->msText = _W("(not connected yet)");
		gpMulJoinFoot->UpdateSize();
	}
	if (mpInit->mpNetworkManager)
		mpInit->mpNetworkManager->Disconnect();
	mpInit->mpGame->GetSound()->GetSoundHandler()->PlayGui("gui_menu_click", false, 1);
	mpInit->mpMainMenu->SetState(eMainMenuState_Multiplayer);
	(void)aButton;
}

void cMainMenuWidget_MultiHostStartListen::OnMouseDown(eMButton aButton)
{
	if (!mpInit->mpNetworkManager)
		return;
	mpInit->mpNetworkManager->HostGame(mpInit->mpNetworkManager->GetDefaultPort());
	mpInit->mpMainMenu->SetState(eMainMenuState_MultiplayerHostLobby);
	mpInit->mpGame->GetSound()->GetSoundHandler()->PlayGui("gui_menu_click", false, 1);
	(void)aButton;
}

void cMainMenuWidget_MultiJoinTry::OnMouseDown(eMButton aButton)
{
	if (!mpInit->mpNetworkManager || gpMulTypedIp == NULL)
		return;
	tString raw = gpMulTypedIp->GetAscii();
	tString trimmed = MulTrimAscii(raw);
	tString sj = MulNormalizeJoinAddr(trimmed, mpInit->mpNetworkManager->GetDefaultPort());

	mpInit->mpConfig->SetString("Multiplayer", "LastJoinHost",
								trimmed.empty() ? tString("127.0.0.1") : trimmed);
	mpInit->mpNetworkManager->JoinGame(sj.c_str());
	gMulJoinAwaitHandshake = true;

	if (gpMulJoinFoot)
	{
		const tWString kConnecting = _W("Connecting... (waiting for handshake)");
		gpMulJoinFoot->msText = kConnecting;
		gpMulJoinFoot->UpdateSize();
	}
	mpInit->mpGame->GetSound()->GetSoundHandler()->PlayGui("gui_menu_click", false, 1);
	(void)aButton;
}

void cMainMenuWidget_MultiLaunchPlaying::OnMouseDown(eMButton aButton)
{
	if (!mpInit->mpNetworkManager)
		return;
	if (mbListenHost && !mpInit->mpNetworkManager->IsHosting())
		return;
	if (mbGuestAck && !mpInit->mpNetworkManager->IsClientSynced())
		return;

	mpInit->mpGraphicsHelper->DrawLoadingScreen("");
	mpInit->mpMainMenu->SetActive(false);
	mpInit->ResetGame(true);
	mpInit->mDifficulty = mDifficulty;

	if (mpInit->mbShowIntro)
	{
		mpInit->mpIntroStory->SetActive(true);
	}
	else
	{
		mpInit->mpGame->GetUpdater()->SetContainer("Default");
		mpInit->mpGame->GetScene()->SetDrawScene(true);
		mpInit->mpMapHandler->Load(mpInit->msStartMap, mpInit->msStartLink);
	}
	mpInit->mpGame->GetSound()->GetSoundHandler()->PlayGui("gui_menu_click", false, 1);
	(void)aButton;
}


//-----------------------------------------------------------------------
// Server browser (Internet = master server list, LAN = broadcast scan),
// password prompt, host lobby 'Public' toggle.
//
// Browser layout (800x600 menu space), one panel at x 16..576, y 186..590:
//   title bar      y 186..212   "Server browser" + "Playing as: <name>"
//   tabs           y 220..244   [Internet] [LAN] ............ [Refresh]
//   column header  y 250..270   lock | Name | Map | Players | Seen
//   8 rows         y 272..448   22 px, striped; hover / selected fill
//   status + pager y 454..474   "3 servers found ..."   [<] 1/2 [>]
//   divider        y 479
//   character      y 486..535   Character: [<] The Fisherman [>] + chips
//   divider        y 541
//   buttons        y 548..574   [Join] [Direct connect] ........ [Back]
//-----------------------------------------------------------------------

namespace {

static const float kMulBrPanelX = 16, kMulBrPanelY = 186, kMulBrPanelW = 560, kMulBrPanelH = 404;
static const float kMulBrListX = 24, kMulBrListW = 532;   /* rows: x 24..556 */
static const float kMulBrHeaderY = 250, kMulBrHeaderH = 20;
static const float kMulBrRowsY = 272, kMulBrRowH = 22;
static const float kMulBrStatusY = 454;
/* column anchors (text x; Players/Seen are centred) */
static const float kMulColLockX = 30, kMulColNameX = 50, kMulColNameW = 200;
static const float kMulColMapX = 262, kMulColMapW = 146;
static const float kMulColPlayersX = 446, kMulColSeenX = 516;

/* A click on a row that cannot be joined explains itself on the status
   line for a few seconds; the live status takes over again afterwards. */
static tString gMulBrowserNotice;
static float gMulBrowserNoticeLeft = 0;
/* selection = the server's address (survives refreshes and paging) */
static tString gMulBrowserSelAddr;
static int gMulBrowserFirst = 0;        /* first list index shown (page start) */
static float gMulBrowserClock = 0;      /* browser-screen time, for the double click */
static float gMulBrowserLastClick = -10;
/* status line, rebuilt every frame by cMainMenu::Update */
static tWString gMulBrowserStatus;
static int gMulBrowserStatusKind = 0;   /* 0 info, 1 busy, 2 error, 3 notice */

static void MulBrowserNotice(const tString &asText)
{
	gMulBrowserNotice = asText;
	gMulBrowserNoticeLeft = 4.0f;
}

/** Ask the network manager for the list the current tab shows. */
static void MulRefreshBrowser(cInit *apInit)
{
	if (!apInit || !apInit->mpNetworkManager)
		return;
	if (gMulBrowserInternet)
		apInit->mpNetworkManager->RefreshInternetServers();
	else
		apInit->mpNetworkManager->StartDiscovery();
}

static const std::vector<cDiscoveredServer> *MulBrowserList(cInit *apInit)
{
	if (!apInit || !apInit->mpNetworkManager)
		return NULL;
	return gMulBrowserInternet ? &apInit->mpNetworkManager->GetInternetServers()
							   : &apInit->mpNetworkManager->GetDiscoveredServers();
}

/** Index of the selected server in the current tab's list, -1 = none. */
static int MulBrowserSelectedIndex(cInit *apInit)
{
	const std::vector<cDiscoveredServer> *pList = MulBrowserList(apInit);
	if (pList == NULL || gMulBrowserSelAddr.empty())
		return -1;
	for (size_t i = 0; i < pList->size(); ++i)
		if ((*pList)[i].msAddress == gMulBrowserSelAddr)
			return (int)i;
	return -1;
}

/** Join a browser row through the SAME path as the direct-connect screen,
    then show that screen so its handshake status line does the talking.
    abSetPassword=false leaves whatever SetJoinPassword / cfg join_password=
    holds untouched (a LAN pong cannot tell us the host wants one). */
static void MulJoinServer(cInit *apInit, const cDiscoveredServer &aServer,
						  bool abSetPassword, const tString &asPassword)
{
	if (!apInit || !apInit->mpNetworkManager || !apInit->mpMainMenu || aServer.msAddress.empty())
		return;
	cNetworkManager *nm = apInit->mpNetworkManager;
	if (abSetPassword)
		nm->SetJoinPassword(asPassword);
	apInit->mpConfig->SetString("Multiplayer", "LastJoinHost", aServer.msAddress);
	if (gpMulTypedIp)
		gpMulTypedIp->SetAscii(aServer.msAddress);
	nm->JoinGame(aServer.msAddress.c_str());
	gMulJoinAwaitHandshake = true;
	if (gpMulJoinFoot)
	{
		gpMulJoinFoot->msText = _W("Connecting... (waiting for handshake)");
		gpMulJoinFoot->UpdateSize();
	}
	apInit->mpMainMenu->SetState(eMainMenuState_MultiplayerJoin);
	MulClickSound(apInit);
}

/** Double click / Join button / Enter on a row: other version = explain;
    password = the prompt; otherwise join now. */
static void MulActivateServer(cInit *apInit, const cDiscoveredServer &aServer)
{
	if (!apInit || !apInit->mpNetworkManager || !apInit->mpMainMenu)
		return;
	if (!aServer.mbVersionMatch)
	{
		MulBrowserNotice("That server runs another version of the mod - both machines need the same zip.");
		return;
	}
	if (aServer.mbPassword)
	{
		gMulPendingServer = aServer;
		if (gpMulTypedPw)
			gpMulTypedPw->SetAscii("");
		apInit->mpMainMenu->SetState(eMainMenuState_MultiplayerPassword);
		MulClickSound(apInit);
		return;
	}
	MulJoinServer(apInit, aServer, false, "");
}

/** Keeps the page start on a page boundary inside the list. */
static void MulClampBrowserPage(size_t alCount)
{
	if (gMulBrowserFirst < 0 || alCount == 0)
		gMulBrowserFirst = 0;
	else if ((size_t)gMulBrowserFirst >= alCount)
		gMulBrowserFirst = (int)(((alCount - 1) / kMulBrowserRows) * kMulBrowserRows);
	gMulBrowserFirst -= gMulBrowserFirst % kMulBrowserRows;
}

/** "12s" / "4m" / "2h" — how long ago the master last heard the host. */
static tWString MulAgeText(uint16_t alSeconds)
{
	if (alSeconds < 60)
		return cString::To16Char(cString::ToString((int)alSeconds) + "s");
	if (alSeconds < 3600)
		return cString::To16Char(cString::ToString((int)(alSeconds / 60)) + "m");
	return cString::To16Char(cString::ToString((int)(alSeconds / 3600)) + "h");
}

}

//-----------------------------------------------------------------------

/** Decoration only: a dark translucent panel with a title bar, drop shadow
    and a thin frame (its hit box is off-screen, it never takes a click).
    abPlayingAs adds "Playing as: <name>" on the right of the title bar;
    abPendingServer adds the password prompt's server name + address. */
class cMainMenuWidget_MultiPanel : public cMainMenuWidget
{
public:
	cMainMenuWidget_MultiPanel(cInit *apInit, const cRect2f &aBox, const tWString &asTitle,
							   bool abPlayingAs = false, bool abPendingServer = false)
		: cMainMenuWidget(apInit, cVector3f(aBox.x, aBox.y, 40), cVector2f(0, 0))
		, mBox(aBox), msTitle(asTitle), mbPlayingAs(abPlayingAs), mbPendingServer(abPendingServer)
	{
		mpFont = MulMenuFont(apInit);
		mRect = cRect2f(-1000, -1000, 0, 0);
	}

	void OnDraw()
	{
		const float x = mBox.x, y = mBox.y, w = mBox.w, h = mBox.h;
		MulFillRect(mpDrawer, x + 5, y + 5, w, h, 28, cColor(0, 0, 0, 0.45f)); /* shadow */
		MulFillRect(mpDrawer, x, y, w, h, 30, MulColPanel());
		MulFillRect(mpDrawer, x, y, w, 26, 31, MulColTitleBar());
		MulFillRect(mpDrawer, x, y + 26, w, 1, 32, MulColAccent());
		MulFrameRect(mpDrawer, x, y, w, h, 32, MulColBorder());
		MulText(mpFont, x + 12, y + 4, 40, 19, MulColTextBright(), eFontAlign_Left, msTitle);

		if (mbPlayingAs && mpInit->mpNetworkManager)
		{
			const tString sName = mpInit->mpNetworkManager->GetLocalPlayerName();
			const tWString wsName = sName.empty() ? tWString(_W("(no name)")) : cString::To16Char(sName);
			const tWString wsLine = MulFitText(mpFont, 13, _W("Playing as: ") + wsName, w * 0.45f);
			MulText(mpFont, x + w - 12, y + 7, 40, 13, MulColText(), eFontAlign_Right, wsLine);
		}

		if (mbPendingServer)
		{
			const cDiscoveredServer &sv = gMulPendingServer;
			const tWString wsName = cString::To16Char(sv.msName.empty() ? tString("(unnamed)") : sv.msName);
			MulText(mpFont, x + w / 2, y + 38, 40, 18, MulColTextBright(), eFontAlign_Center,
					MulFitText(mpFont, 18, wsName, w - 40));
			tString sSub = sv.msAddress;
			if (!sv.msMap.empty())
				sSub += "   -   " + sv.msMap;
			if (sv.mlMaxPlayers > 0)
				sSub += "   -   " + cString::ToString((int)sv.mlPlayerCount) + "/" +
						cString::ToString((int)sv.mlMaxPlayers) + " players";
			MulText(mpFont, x + w / 2, y + 62, 40, 13, MulColTextDim(), eFontAlign_Center,
					MulFitText(mpFont, 13, cString::To16Char(sSub), w - 40));
		}
	}

private:
	iFontData *mpFont;
	cRect2f mBox;
	tWString msTitle;
	bool mbPlayingAs;
	bool mbPendingServer;
};

//-----------------------------------------------------------------------

/** A boxed button: dark fill + frame, brighter fill on hover (fading like
    the menu's text buttons), accent fill when IsSelected() (tabs), greyed
    and inert when !IsEnabled(), not drawn at all when !IsVisible(). The
    hit box is the whole box. With aNextState != LastEnum a click goes to
    that state like cMainMenuWidget_Button; subclasses override OnPress. */
class cMainMenuWidget_MultiBoxButton : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_MultiBoxButton(cInit *apInit, const cRect2f &aBox, const tWString &asText,
								   eMainMenuState aNextState = eMainMenuState_LastEnum,
								   float afFontSize = 17)
		: cMainMenuWidget_Button(apInit, cVector3f(aBox.x + aBox.w / 2, aBox.y + (aBox.h - afFontSize) / 2, 40),
								 asText, aNextState, afFontSize, eFontAlign_Center)
		, mBox(aBox)
	{
		mRect = aBox;
	}

	virtual bool IsEnabled() { return true; }
	virtual bool IsSelected() { return false; }
	virtual bool IsVisible() { return true; }

	virtual void OnPress()
	{
		if (mNextState != eMainMenuState_LastEnum)
			cMainMenuWidget_Button::OnMouseDown(eMButton_Left); /* SetState + click */
	}

	virtual void OnMouseDown(eMButton aButton)
	{
		(void)aButton;
		if (!IsVisible() || !IsEnabled())
			return;
		OnPress();
	}

	virtual void OnMouseOver(bool abOver)
	{
		cMainMenuWidget_Button::OnMouseOver(abOver && IsVisible() && IsEnabled());
	}

	virtual void OnDraw()
	{
		if (!IsVisible())
			return;
		const bool bEnabled = IsEnabled();
		const bool bSel = IsSelected();
		const float x = mBox.x, y = mBox.y, w = mBox.w, h = mBox.h;
		const float fA = bEnabled ? mfAlpha : 0.0f;

		cColor colFill = bSel ? cColor(0.08f, 0.2f, 0.52f, 0.92f) : MulColBox();
		if (!bEnabled)
			colFill = cColor(0.05f, 0.05f, 0.07f, 0.6f);
		MulFillRect(mpDrawer, x, y, w, h, 33, colFill);
		if (fA > 0)
		{
			const cColor colHover = MulColBoxHover();
			MulFillRect(mpDrawer, x, y, w, h, 33.5f,
						cColor(colHover.r, colHover.g, colHover.b, colHover.a * fA * 0.8f));
		}
		const cColor colFrame = bSel ? MulColAccent() :
			(bEnabled ? cColor(0.3f + 0.25f * fA, 0.35f + 0.25f * fA, 0.5f + 0.4f * fA, 0.75f + 0.25f * fA)
					  : cColor(0.2f, 0.2f, 0.25f, 0.5f));
		MulFrameRect(mpDrawer, x, y, w, h, 34, colFrame);
		if (bSel)
			MulFillRect(mpDrawer, x, y, w, 2, 34.5f, MulColAccent()); /* tab accent on top */

		const cColor colText = !bEnabled ? cColor(0.35f, 1.0f) :
			(bSel ? MulColTextBright() : cColor(0.7f + 0.3f * fA, 1.0f));
		if (fA > 0)
			MulText(mpFont, mvPositon.x + 1, mvPositon.y + 1, 39, mvFontSize.y,
					cColor(0.1f, 0.32f, 1.0f, fA * 0.8f), eFontAlign_Center, msText);
		MulText(mpFont, mvPositon.x, mvPositon.y, 40, mvFontSize.y, colText, eFontAlign_Center, msText);
	}

protected:
	cRect2f mBox;
};

//-----------------------------------------------------------------------

/** 'Internet' / 'LAN' tab: selects the list and refreshes it. */
class cMainMenuWidget_MultiBrowserTab : public cMainMenuWidget_MultiBoxButton
{
public:
	cMainMenuWidget_MultiBrowserTab(cInit *apInit, const cRect2f &aBox, const tWString &lbl,
									bool abInternet)
		: cMainMenuWidget_MultiBoxButton(apInit, aBox, lbl)
		, mbInternet(abInternet)
	{
	}

	virtual bool IsSelected() { return gMulBrowserInternet == mbInternet; }

	virtual void OnPress()
	{
		if (gMulBrowserInternet != mbInternet)
		{
			gMulBrowserSelAddr = "";
			gMulBrowserFirst = 0;
		}
		gMulBrowserInternet = mbInternet;
		gMulBrowserNoticeLeft = 0;
		MulRefreshBrowser(mpInit);
		MulClickSound(mpInit);
	}

private:
	bool mbInternet;
};

class cMainMenuWidget_MultiBrowserRefresh : public cMainMenuWidget_MultiBoxButton
{
public:
	cMainMenuWidget_MultiBrowserRefresh(cInit *apInit, const cRect2f &aBox, const tWString &lbl)
		: cMainMenuWidget_MultiBoxButton(apInit, aBox, lbl)
	{
	}

	virtual void OnPress()
	{
		gMulBrowserNoticeLeft = 0;
		MulRefreshBrowser(mpInit);
		MulClickSound(mpInit);
	}
};

/** 'Join': the selected row (disabled while nothing is selected). */
class cMainMenuWidget_MultiBrowserJoin : public cMainMenuWidget_MultiBoxButton
{
public:
	cMainMenuWidget_MultiBrowserJoin(cInit *apInit, const cRect2f &aBox, const tWString &lbl)
		: cMainMenuWidget_MultiBoxButton(apInit, aBox, lbl)
	{
	}

	virtual bool IsEnabled() { return MulBrowserSelectedIndex(mpInit) >= 0; }

	virtual void OnPress()
	{
		const int lSel = MulBrowserSelectedIndex(mpInit);
		const std::vector<cDiscoveredServer> *pList = MulBrowserList(mpInit);
		if (lSel >= 0 && pList)
			MulActivateServer(mpInit, (*pList)[(size_t)lSel]);
	}
};

/** Pager arrows under the list; only there when the list has more rows
    than fit. */
class cMainMenuWidget_MultiBrowserPage : public cMainMenuWidget_MultiBoxButton
{
public:
	cMainMenuWidget_MultiBrowserPage(cInit *apInit, const cRect2f &aBox, int alDir)
		: cMainMenuWidget_MultiBoxButton(apInit, aBox, alDir < 0 ? _W("<") : _W(">"),
										 eMainMenuState_LastEnum, 15)
		, mlDir(alDir)
	{
	}

	virtual bool IsVisible()
	{
		const std::vector<cDiscoveredServer> *pList = MulBrowserList(mpInit);
		return pList && (int)pList->size() > kMulBrowserRows;
	}

	virtual bool IsEnabled()
	{
		const std::vector<cDiscoveredServer> *pList = MulBrowserList(mpInit);
		if (pList == NULL)
			return false;
		if (mlDir < 0)
			return gMulBrowserFirst > 0;
		return gMulBrowserFirst + kMulBrowserRows < (int)pList->size();
	}

	virtual void OnPress()
	{
		gMulBrowserFirst += mlDir * kMulBrowserRows;
		const std::vector<cDiscoveredServer> *pList = MulBrowserList(mpInit);
		MulClampBrowserPage(pList ? pList->size() : 0);
		MulClickSound(mpInit);
	}

private:
	int mlDir;
};

/** Multiplayer menu -> browser, refreshing on the way so it is never blank. */
class cMainMenuWidget_MultiOpenBrowser : public cMainMenuWidget_MainButton
{
public:
	cMainMenuWidget_MultiOpenBrowser(cInit *apInit, const cVector3f &avPos, const tWString &lbl)
		: cMainMenuWidget_MainButton(apInit, avPos, lbl, eMainMenuState_MultiplayerBrowser)
	{
	}

	virtual void OnMouseDown(eMButton aButton)
	{
		cMainMenuWidget_MainButton::OnMouseDown(aButton);
		gMulBrowserNoticeLeft = 0;
		MulRefreshBrowser(mpInit);
	}
};

//-----------------------------------------------------------------------

/** The list frame (decoration, no clicks): column header strip, the list
    background, a scroll thumb on the right when there is more than one
    page, the centred 'Searching...' / 'No servers' message on an empty
    list, and the status line + 'n/m' page label under it. */
class cMainMenuWidget_MultiBrowserTable : public cMainMenuWidget
{
public:
	cMainMenuWidget_MultiBrowserTable(cInit *apInit)
		: cMainMenuWidget(apInit, cVector3f(kMulBrListX, kMulBrHeaderY, 40), cVector2f(0, 0))
		, mfTime(0)
	{
		mpFont = MulMenuFont(apInit);
		mRect = cRect2f(-1000, -1000, 0, 0);
	}

	void OnUpdate(float afTimeStep) { mfTime += afTimeStep; }

	void OnDraw()
	{
		const std::vector<cDiscoveredServer> *pList = MulBrowserList(mpInit);
		const size_t lCount = pList ? pList->size() : 0;
		const float fRowsH = kMulBrRowH * kMulBrowserRows;

		/* header strip + labels */
		MulFillRect(mpDrawer, kMulBrListX, kMulBrHeaderY, kMulBrListW, kMulBrHeaderH, 33,
					cColor(0.09f, 0.11f, 0.2f, 0.95f));
		MulFillRect(mpDrawer, kMulBrListX, kMulBrHeaderY + kMulBrHeaderH - 1, kMulBrListW, 1, 34,
					MulColBorder());
		const float fHy = kMulBrHeaderY + 3;
		const cColor colHead(0.66f, 0.74f, 0.95f, 1.0f);
		MulDrawLock(mpDrawer, kMulColLockX - 1, kMulBrHeaderY + 4, 35, cColor(0.5f, 0.56f, 0.75f, 0.9f));
		MulText(mpFont, kMulColNameX, fHy, 40, 13, colHead, eFontAlign_Left, _W("Name"));
		MulText(mpFont, kMulColMapX, fHy, 40, 13, colHead, eFontAlign_Left, _W("Map"));
		MulText(mpFont, kMulColPlayersX, fHy, 40, 13, colHead, eFontAlign_Center, _W("Players"));
		MulText(mpFont, kMulColSeenX, fHy, 40, 13, colHead, eFontAlign_Center,
				gMulBrowserInternet ? _W("Seen") : _W("Where"));

		/* list background + frame */
		MulFillRect(mpDrawer, kMulBrListX, kMulBrRowsY, kMulBrListW, fRowsH, 31,
					cColor(0.01f, 0.01f, 0.03f, 0.55f));
		MulFrameRect(mpDrawer, kMulBrListX - 1, kMulBrHeaderY - 1, kMulBrListW + 2,
					 kMulBrRowsY + fRowsH - kMulBrHeaderY + 2, 34, cColor(0.2f, 0.24f, 0.38f, 0.8f));

		/* scroll indicator: track + thumb, only with more than one page */
		if ((int)lCount > kMulBrowserRows)
		{
			const float fTrackX = kMulBrListX + kMulBrListW + 5;
			MulFillRect(mpDrawer, fTrackX, kMulBrRowsY, 5, fRowsH, 33, cColor(0.1f, 0.12f, 0.2f, 0.9f));
			const float fThumbH = fRowsH * (float)kMulBrowserRows / (float)lCount;
			const float fThumbY = kMulBrRowsY + fRowsH * (float)gMulBrowserFirst / (float)lCount;
			MulFillRect(mpDrawer, fTrackX, fThumbY, 5, fThumbH, 34, MulColAccent());
		}

		/* empty list: say why in the middle of it */
		if (lCount == 0 && mpInit->mpNetworkManager)
		{
			cNetworkManager *nm = mpInit->mpNetworkManager;
			const bool bBusy = gMulBrowserInternet ? nm->IsInternetRefreshActive() : nm->IsDiscoveryActive();
			const float fMidY = kMulBrRowsY + fRowsH / 2 - 16;
			const float fMidX = kMulBrListX + kMulBrListW / 2;
			if (bBusy)
			{
				tWString sDots = _W("Searching");
				const int lDots = ((int)(mfTime * 3.0f)) % 4;
				for (int d = 0; d < lDots; ++d)
					sDots += _W(".");
				MulText(mpFont, fMidX, fMidY + 6, 40, 18, MulColText(), eFontAlign_Center, sDots);
			}
			else
			{
				MulText(mpFont, fMidX, fMidY, 40, 18, MulColText(), eFontAlign_Center, _W("No servers found"));
				MulText(mpFont, fMidX, fMidY + 24, 40, 13, MulColTextDim(), eFontAlign_Center,
						gMulBrowserInternet ? _W("Try the LAN tab, Refresh, or Direct connect.")
											: _W("Try the Internet tab, Refresh, or Direct connect."));
			}
		}

		/* dividers: above the character picker and above the buttons */
		MulFillRect(mpDrawer, kMulBrListX, 479, kMulBrListW, 1, 32, cColor(0.2f, 0.24f, 0.38f, 0.7f));
		MulFillRect(mpDrawer, kMulBrListX, 541, kMulBrListW, 1, 32, cColor(0.2f, 0.24f, 0.38f, 0.7f));

		/* status line (left) + page label (between the pager arrows) */
		const bool bPaged = (int)lCount > kMulBrowserRows;
		cColor colStatus = MulColText();
		if (gMulBrowserStatusKind == 1)
			colStatus = cColor(0.75f, 0.82f, 1.0f, 0.65f + 0.35f * (float)fabs(sin(mfTime * 3.0f)));
		else if (gMulBrowserStatusKind == 2 || gMulBrowserStatusKind == 3)
			colStatus = MulColWarn();
		MulText(mpFont, kMulBrListX + 2, kMulBrStatusY + 3, 40, 13, colStatus, eFontAlign_Left,
				MulFitText(mpFont, 13, gMulBrowserStatus, bPaged ? 420.0f : kMulBrListW - 4));
		if (bPaged)
		{
			const int lPages = (int)((lCount + kMulBrowserRows - 1) / kMulBrowserRows);
			const int lPage = gMulBrowserFirst / kMulBrowserRows + 1;
			MulText(mpFont, 519, kMulBrStatusY + 3, 40, 13, MulColText(), eFontAlign_Center,
					cString::To16Char(cString::ToString(lPage) + "/" + cString::ToString(lPages)));
		}
	}

private:
	iFontData *mpFont;
	float mfTime;
};

//-----------------------------------------------------------------------

/** One list slot (index gMulBrowserFirst + slot of the current tab's
    list): lock | name | map | players/max | seen. Click selects, a second
    click on the selected row within 0.45 s (or the engine's double click,
    or Join / Enter) joins. Other-version servers draw grey and explain
    themselves instead of joining. Every slot draws its stripe, so the
    table keeps its shape while the list is short. */
class cMainMenuWidget_MultiServerRow : public cMainMenuWidget
{
public:
	cMainMenuWidget_MultiServerRow(cInit *apInit, const cRect2f &aRect, int alSlot)
		: cMainMenuWidget(apInit, cVector3f(aRect.x, aRect.y, 40), cVector2f(0, 0))
		, mlSlot(alSlot)
		, mbHasServer(false)
		, mServer()
	{
		mpFont = MulMenuFont(apInit);
		mRect = aRect;
		mbOver = false;
	}

	/** Mirror slot mlSlot of the current page of the list. */
	void SyncFromList(const std::vector<cDiscoveredServer> &avList)
	{
		const int lIdx = gMulBrowserFirst + mlSlot;
		mbHasServer = lIdx >= 0 && (size_t)lIdx < avList.size();
		if (mbHasServer)
			mServer = avList[(size_t)lIdx];
	}

	virtual void OnMouseOver(bool abOver) { mbOver = abOver; }

	virtual void OnMouseDown(eMButton aButton)
	{
		(void)aButton;
		if (!mbHasServer || !mpInit->mpNetworkManager || !mpInit->mpMainMenu)
			return;
		const bool bSame = gMulBrowserSelAddr == mServer.msAddress;
		if (bSame && gMulBrowserClock - gMulBrowserLastClick < 0.45f)
		{
			gMulBrowserLastClick = -10;
			MulActivateServer(mpInit, mServer);
			return;
		}
		gMulBrowserSelAddr = mServer.msAddress;
		gMulBrowserLastClick = gMulBrowserClock;
		gMulBrowserNoticeLeft = 0;
		MulClickSound(mpInit);
	}

	virtual void OnDoubleClick(eMButton aButton)
	{
		(void)aButton;
		if (mbHasServer && gMulBrowserSelAddr == mServer.msAddress)
		{
			gMulBrowserLastClick = -10;
			MulActivateServer(mpInit, mServer);
		}
	}

	virtual void OnDraw()
	{
		const float x = mRect.x, y = mRect.y, w = mRect.w, h = mRect.h;
		if (mlSlot % 2 == 1)
			MulFillRect(mpDrawer, x, y, w, h, 32, cColor(0.6f, 0.7f, 1.0f, 0.05f));
		if (!mbHasServer)
			return;

		const bool bSel = gMulBrowserSelAddr == mServer.msAddress;
		const bool bMatch = mServer.mbVersionMatch;
		if (bSel)
		{
			MulFillRect(mpDrawer, x, y, w, h, 33, MulColSelected());
			MulFillRect(mpDrawer, x, y, 3, h, 34, MulColAccent());
		}
		else if (mbOver)
		{
			MulFillRect(mpDrawer, x, y, w, h, 33, cColor(0.12f, 0.18f, 0.42f, 0.55f));
		}

		const cColor colBase = !bMatch ? cColor(0.4f, 1.0f) :
			(bSel ? MulColTextBright() : (mbOver ? cColor(0.92f, 1.0f) : cColor(0.74f, 1.0f)));
		const float fTy = y + 4;

		if (mServer.mbPassword)
			MulDrawLock(mpDrawer, kMulColLockX - 1, y + 5, 35, bMatch ? MulColGold() : cColor(0.45f, 1.0f));

		const tWString wsName = cString::To16Char(mServer.msName.empty() ? tString("(unnamed)") : mServer.msName);
		MulText(mpFont, kMulColNameX, fTy, 40, 14, colBase, eFontAlign_Left,
				MulFitText(mpFont, 14, wsName, kMulColNameW));

		if (!bMatch)
			MulText(mpFont, kMulColMapX, fTy + 1, 40, 13, MulColWarn(), eFontAlign_Left,
					_W("other mod version"));
		else
			MulText(mpFont, kMulColMapX, fTy, 40, 14, colBase, eFontAlign_Left,
					MulFitText(mpFont, 14, cString::To16Char(mServer.msMap.empty() ? tString("-") : mServer.msMap),
							   kMulColMapW));

		const bool bFull = mServer.mlMaxPlayers > 0 && mServer.mlPlayerCount >= mServer.mlMaxPlayers;
		const tWString wsPlayers = cString::To16Char(cString::ToString((int)mServer.mlPlayerCount) + "/" +
													 cString::ToString((int)mServer.mlMaxPlayers));
		MulText(mpFont, kMulColPlayersX, fTy, 40, 14, (bFull && bMatch) ? MulColWarn() : colBase,
				eFontAlign_Center, wsPlayers);

		MulText(mpFont, kMulColSeenX, fTy + 1, 40, 13, bSel ? colBase : MulColTextDim(), eFontAlign_Center,
				mServer.mbInternet ? MulAgeText(mServer.mlAgeSeconds) : tWString(_W("LAN")));
	}

private:
	iFontData *mpFont;
	int mlSlot;
	bool mbHasServer;
	cDiscoveredServer mServer;
};

/** Password screen 'Join': stores the typed password, joins the pending row. */
class cMainMenuWidget_MultiPwJoin : public cMainMenuWidget_MultiBoxButton
{
public:
	cMainMenuWidget_MultiPwJoin(cInit *apInit, const cRect2f &aBox, const tWString &lbl)
		: cMainMenuWidget_MultiBoxButton(apInit, aBox, lbl)
	{
	}

	virtual void OnPress()
	{
		if (!gpMulTypedPw)
			return;
		MulJoinServer(mpInit, gMulPendingServer, true, gpMulTypedPw->GetAscii());
	}
};

/** Host lobby: 'Public (list on master): ON/OFF'. Bound to the cfg `public=`
    setting (cNetworkManager::SetPublic); the choice is also remembered in
    the game config unless multiplayer.cfg pins it. */
class cMainMenuWidget_MultiPublicToggle : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_MultiPublicToggle(cInit *apInit, const cVector3f &avPos)
		: cMainMenuWidget_Button(apInit, avPos, _W(""), eMainMenuState_LastEnum, 18, eFontAlign_Center)
	{
		RefreshLabel();
	}

	void RefreshLabel()
	{
		const bool bOn = mpInit->mpNetworkManager && mpInit->mpNetworkManager->IsPublic();
		msText = bOn ? _W("[x] Public (list on master server)") : _W("[ ] Public (list on master server)");
		mRect.w = mpFont->GetLength(mvFontSize, msText.c_str());
		mRect.x = mvPositon.x - mRect.w / 2;
	}

	virtual void OnMouseDown(eMButton aButton)
	{
		(void)aButton;
		cNetworkManager *nm = mpInit->mpNetworkManager;
		if (!nm)
			return;
		nm->SetPublic(!nm->IsPublic()); /* while hosting: registers / unregisters at once */
		mpInit->mpConfig->SetBool("Multiplayer", "Public", nm->IsPublic());
		RefreshLabel();
		mpInit->mpGame->GetSound()->GetSoundHandler()->PlayGui("gui_menu_click", false, 1);
	}
};

//-----------------------------------------------------------------------
// v18: character picker (Multiplayer screen, Direct-connect / join status
// screen, server browser, host lobby).
//-----------------------------------------------------------------------

namespace {

/** Philip is the host's character (cNetworkManager::IsCharacterSelectable
    rule since v19: by NAME, so a list without him has no host-only entry).
    Only used to LABEL a chip '(host)'; what may be picked is always
    IsCharacterSelectable's answer. */
static bool MulIsHostCharacter(const tString &asBase)
{
	tString s = asBase;
	for (size_t i = 0; i < s.size(); ++i)
		s[i] = (char)std::tolower((unsigned char)s[i]);
	return s == "phillip";
}

/** The host's own character: what it plays, else Philip, else entry 0. */
static tString MulHostCharacterName(cNetworkManager *nm)
{
	tString sHost = nm->GetLocalCharacterName();
	if (!sHost.empty())
		return sHost;
	for (size_t i = 0; i < nm->GetCharacterListSize(); ++i)
		if (MulIsHostCharacter(nm->GetCharacterBaseName(i)))
			return nm->GetCharacterBaseName(i);
	return nm->GetCharacterBaseName(0);
}

/** The next (alDir=+1) / previous (-1) selectable character after
    asFrom, wrapping; "" or unknown asFrom starts at the list's edge. Its
    own entry when it is the only selectable one, "" when none is. */
static tString MulStepCharacter(cNetworkManager *nm, const tString &asFrom, int alDir)
{
	const int n = (int)nm->GetCharacterListSize();
	if (n <= 0)
		return "";
	const int lCur = nm->FindCharacterIndex(asFrom);
	const int lStart = lCur >= 0 ? lCur : (alDir > 0 ? n - 1 : 0);
	for (int s = 1; s <= n; ++s)
	{
		const int i = ((lStart + alDir * s) % n + n) % n;
		if (nm->IsCharacterSelectable((size_t)i))
			return nm->GetCharacterBaseName((size_t)i);
	}
	return "";
}

}

/** v18: 'Character:  [<]  Red  [>]'. The arrows step to the previous /
    next selectable character (wrapping; never the host's Philip; while
    connected as a guest never one another player holds; offline every
    other character), a click on the name steps forward, and with chips on
    a row of every character is drawn under it ('Philip (host)', 'Red
    (taken)', the current one lit) - a click on a free one picks it. Every
    pick saves character= in multiplayer.cfg (SetCharacterPreference) and,
    when connected, asks the host at once - also from the in-game Esc
    menu, where the host swaps us live. While hosting it only reads 'You
    play Philip' (no hit box). */
class cMainMenuWidget_MultiCharacter : public cMainMenuWidget
{
public:
	cMainMenuWidget_MultiCharacter(cInit *apInit, const cVector3f &avPos, bool abChips = false,
								   float afMaxWidth = 520)
		: cMainMenuWidget(apInit, avPos, cVector2f(0, 0))
		, mbChips(abChips), mfMaxWidth(afMaxWidth), mbHostLine(false), mbPicker(false)
	{
		mpFont = MulMenuFont(apInit);
		mbOver = false;
		Layout();
	}

	virtual void OnUpdate(float afTimeStep)
	{
		(void)afTimeStep;
		Layout(); /* the name table can change under us at any time */
	}

	virtual void OnActivate() { Layout(); }

	virtual void OnMouseOver(bool abOver) { mbOver = abOver; }

	virtual void OnMouseDown(eMButton aButton)
	{
		(void)aButton;
		cNetworkManager *nm = mpInit->mpNetworkManager;
		if (nm == NULL || nm->IsHosting() || !mbPicker)
			return; /* the host is always Philip */
		Layout();
		tString sPick;
		if (MulMouseIn(mpInit, mLeftBox))
			sPick = MulStepCharacter(nm, CurrentFrom(nm), -1);
		else if (MulMouseIn(mpInit, mRightBox) || MulMouseIn(mpInit, mNameBox))
			sPick = MulStepCharacter(nm, CurrentFrom(nm), +1);
		else
		{
			for (size_t c = 0; c < mvChips.size(); ++c)
				if (mvChips[c].mbSelectable && MulMouseIn(mpInit, mvChips[c].mBox))
					sPick = mvChips[c].msBase;
		}
		if (sPick.empty())
			return; /* nothing selectable (one character, or every other one taken) */
		nm->SetCharacterPreference(sPick); /* cfg character= + request when connected */
		Layout();
		MulClickSound(mpInit);
	}

	virtual void OnDraw()
	{
		const float y = mvPositon.y;
		if (!mbPicker)
		{
			/* host line / no characters: plain centred text */
			MulText(mpFont, mvPositon.x, y + 2, 40, 18, mbHostLine ? MulColText() : MulColTextDim(),
					eFontAlign_Center, msLine);
		}
		else
		{
			MulText(mpFont, mfLabelX, y + 2, 40, 18, MulColText(), eFontAlign_Left, _W("Character:"));
			DrawArrow(mLeftBox, _W("<"));
			DrawArrow(mRightBox, _W(">"));
			const bool bNameHover = mbCanStep && MulMouseIn(mpInit, mNameBox);
			MulFillRect(mpDrawer, mNameBox.x, mNameBox.y, mNameBox.w, mNameBox.h, 33,
						bNameHover ? cColor(0.06f, 0.09f, 0.2f, 0.85f) : cColor(0.03f, 0.04f, 0.08f, 0.78f));
			MulFrameRect(mpDrawer, mNameBox.x, mNameBox.y, mNameBox.w, mNameBox.h, 34,
						 cColor(0.25f, 0.3f, 0.46f, 0.8f));
			MulText(mpFont, mNameBox.x + mNameBox.w / 2, y + 2, 40, 18,
					mbShownBad ? MulColWarn() : MulColTextBright(), eFontAlign_Center, msShown);
			if (!msStatus.empty())
				MulText(mpFont, mfStatusX, y + 5, 40, 13, mbShownBad ? MulColWarn() : MulColTextDim(),
						eFontAlign_Left, msStatus);
		}

		for (size_t c = 0; c < mvChips.size(); ++c)
		{
			const cChip &chip = mvChips[c];
			const cRect2f &b = chip.mBox;
			const bool bHover = chip.mbSelectable && MulMouseIn(mpInit, b);
			cColor colText = MulColText();
			if (chip.mbCurrent)
			{
				MulFillRect(mpDrawer, b.x, b.y, b.w, b.h, 33, cColor(0.08f, 0.2f, 0.52f, 0.92f));
				MulFrameRect(mpDrawer, b.x, b.y, b.w, b.h, 34, MulColAccent());
				colText = MulColTextBright();
			}
			else if (chip.mbSelectable)
			{
				MulFillRect(mpDrawer, b.x, b.y, b.w, b.h, 33, bHover ? MulColBoxHover() : MulColBox());
				MulFrameRect(mpDrawer, b.x, b.y, b.w, b.h, 34,
							 bHover ? cColor(0.55f, 0.65f, 1.0f, 1.0f) : cColor(0.3f, 0.35f, 0.5f, 0.75f));
				colText = bHover ? MulColTextBright() : MulColText();
			}
			else
			{
				MulFrameRect(mpDrawer, b.x, b.y, b.w, b.h, 34, cColor(0.2f, 0.2f, 0.26f, 0.55f));
				colText = chip.mbHost ? MulColGold() : cColor(0.4f, 1.0f);
			}
			MulText(mpFont, b.x + b.w / 2, b.y + 3, 40, 13, colText, eFontAlign_Center, chip.msText);
		}
	}

private:
	struct cChip
	{
		tString msBase;
		tWString msText;
		cRect2f mBox;
		bool mbSelectable;
		bool mbCurrent;
		bool mbHost;
	};

	iFontData *mpFont;
	bool mbChips;
	float mfMaxWidth;
	bool mbHostLine;   /* hosting: 'You play Philip' */
	bool mbPicker;     /* guest / offline with characters: the arrows line */
	bool mbCanStep;    /* some other character is selectable */
	bool mbShownBad;   /* shown pick is not what we get (taken / host only / missing) */
	tWString msLine;   /* !mbPicker text */
	tWString msShown;  /* the name in the field */
	tWString msStatus; /* '(taken)' etc. right of the arrows */
	float mfLabelX;
	float mfStatusX;
	cRect2f mLeftBox, mNameBox, mRightBox;
	std::vector<cChip> mvChips;

	/** What the arrows step from: the preference, else (connected) what
	    the host gave us. */
	tString CurrentFrom(cNetworkManager *nm)
	{
		tString sFrom = nm->GetCharacterPreference();
		if (sFrom.empty() && nm->IsClientSynced())
			sFrom = nm->GetLocalCharacterName();
		return sFrom;
	}

	void DrawArrow(const cRect2f &b, const wchar_t *asText)
	{
		const bool bHover = mbCanStep && MulMouseIn(mpInit, b);
		MulFillRect(mpDrawer, b.x, b.y, b.w, b.h, 33,
					!mbCanStep ? cColor(0.05f, 0.05f, 0.07f, 0.6f) : (bHover ? MulColBoxHover() : MulColBox()));
		MulFrameRect(mpDrawer, b.x, b.y, b.w, b.h, 34,
					 !mbCanStep ? cColor(0.2f, 0.2f, 0.25f, 0.5f) :
					 (bHover ? cColor(0.55f, 0.65f, 1.0f, 1.0f) : cColor(0.3f, 0.35f, 0.5f, 0.8f)));
		MulText(mpFont, b.x + b.w / 2, b.y + 2, 40, 17,
				!mbCanStep ? cColor(0.35f, 1.0f) : (bHover ? MulColTextBright() : MulColText()),
				eFontAlign_Center, asText);
	}

	/** Rebuilds texts, boxes and chips from the network manager (cheap:
	    a handful of GetLength calls) and sets the hit box around them. */
	void Layout()
	{
		cNetworkManager *nm = mpInit->mpNetworkManager;
		const float cx = mvPositon.x, y = mvPositon.y;
		const float kSize = 18, kArrowW = 24, kBoxH = 24, kGap = 8;
		mvChips.clear();
		mbHostLine = false;
		mbPicker = false;
		mbCanStep = false;
		mbShownBad = false;
		msStatus = _W("");
		float fLeft = cx, fRight = cx;

		if (nm == NULL || nm->GetCharacterListSize() == 0)
		{
			msLine = nm == NULL ? tWString(_W("Character: -")) : tWString(_W("Character: (no characters installed)"));
			const float w = MulTextWidth(mpFont, kSize, msLine);
			fLeft = cx - w / 2;
			fRight = cx + w / 2;
		}
		else if (nm->IsHosting())
		{
			mbHostLine = true;
			msLine = cString::To16Char("You play " +
				cNetworkManager::GetCharacterDisplayName(MulHostCharacterName(nm)) + " (the host's character)");
			const float w = MulTextWidth(mpFont, kSize, msLine);
			fLeft = cx - w / 2;
			fRight = cx + w / 2;
		}
		else
		{
			mbPicker = true;
			const bool bConnected = nm->IsClientSynced();
			const tString sPref = nm->GetCharacterPreference();
			const tString sActual = bConnected ? nm->GetLocalCharacterName() : tString("");
			const tString sShown = sPref.empty() ? sActual : sPref;
			msShown = cString::To16Char(sShown.empty() ? tString("Any") :
				cNetworkManager::GetCharacterDisplayName(sShown));
			tString sStatus;
			if (!sPref.empty())
			{
				const int lIdx = nm->FindCharacterIndex(sPref);
				if (lIdx < 0)
					sStatus = "(not installed)";
				else if (MulIsHostCharacter(sPref))
					sStatus = "(host only)";
				else if (bConnected && nm->IsCharacterTakenByOther(sPref))
					sStatus = "(taken)";
				else if (bConnected && !sActual.empty() && nm->FindCharacterIndex(sActual) != lIdx)
					sStatus = "(playing " + cNetworkManager::GetCharacterDisplayName(sActual) + ")";
				mbShownBad = !sStatus.empty();
			}
			else if (!bConnected)
				sStatus = "(the host picks)";
			msStatus = cString::To16Char(sStatus);

			/* a step goes somewhere new, or fixes a pick we cannot have */
			const tString sNext = MulStepCharacter(nm, CurrentFrom(nm), +1);
			mbCanStep = !sNext.empty() &&
				(nm->FindCharacterIndex(sNext) != nm->FindCharacterIndex(CurrentFrom(nm)) || mbShownBad ||
				 sPref.empty());

			/* the name field fits the longest name, so the arrows stay put */
			float fNameW = MulTextWidth(mpFont, kSize, msShown);
			for (size_t i = 0; i < nm->GetCharacterListSize(); ++i)
			{
				const float w = MulTextWidth(mpFont, kSize, cString::To16Char(
					cNetworkManager::GetCharacterDisplayName(nm->GetCharacterBaseName(i))));
				if (w > fNameW)
					fNameW = w;
			}
			fNameW += 24;
			if (fNameW < 120)
				fNameW = 120;
			const float fLabelW = MulTextWidth(mpFont, kSize, _W("Character:"));
			const float fStatusW = msStatus.empty() ? 0 : MulTextWidth(mpFont, 13, msStatus) + kGap;
			const float fTotal = fLabelW + kGap + kArrowW + 4 + fNameW + 4 + kArrowW + fStatusW;
			float x = cx - fTotal / 2;
			fLeft = x;
			mfLabelX = x;
			x += fLabelW + kGap;
			mLeftBox = cRect2f(x, y, kArrowW, kBoxH);
			x += kArrowW + 4;
			mNameBox = cRect2f(x, y, fNameW, kBoxH);
			x += fNameW + 4;
			mRightBox = cRect2f(x, y, kArrowW, kBoxH);
			x += kArrowW;
			mfStatusX = x + kGap;
			fRight = x + fStatusW;
		}

		float fBottom = y + kBoxH;
		if (mbChips && nm != NULL && nm->GetCharacterListSize() > 0)
		{
			/* one chip per character, wrapped into centred rows */
			/* lit chip = what we get: the host's own; a guest's usable
			   preference, else (connected) what the host gave us - never a
			   hand-edited host-only / taken pick */
			int lCurIdx = -1;
			if (mbHostLine)
				lCurIdx = nm->FindCharacterIndex(MulHostCharacterName(nm));
			else if (!nm->GetCharacterPreference().empty() && !mbShownBad)
				lCurIdx = nm->FindCharacterIndex(nm->GetCharacterPreference());
			else if (nm->IsClientSynced())
				lCurIdx = nm->FindCharacterIndex(nm->GetLocalCharacterName());
			std::vector<cChip> vAll;
			for (size_t i = 0; i < nm->GetCharacterListSize(); ++i)
			{
				cChip chip;
				chip.msBase = nm->GetCharacterBaseName(i);
				chip.mbHost = MulIsHostCharacter(chip.msBase);
				chip.mbCurrent = (int)i == lCurIdx;
				chip.mbSelectable = !mbHostLine && nm->IsCharacterSelectable(i);
				tString sText = cNetworkManager::GetCharacterDisplayName(chip.msBase);
				if (chip.mbHost)
					sText += mbHostLine ? " (you)" : " (host)";
				else if (nm->IsCharacterTakenByOther(chip.msBase))
					sText += " (taken)";
				chip.msText = cString::To16Char(sText);
				chip.mBox = cRect2f(0, 0, MulTextWidth(mpFont, 13, chip.msText) + 16, 19);
				vAll.push_back(chip);
			}
			const float kChipGap = 6;
			float fRowY = y + kBoxH + 6;
			size_t lRowStart = 0;
			while (lRowStart < vAll.size())
			{
				float fRowW = vAll[lRowStart].mBox.w;
				size_t lRowEnd = lRowStart + 1;
				while (lRowEnd < vAll.size() && fRowW + kChipGap + vAll[lRowEnd].mBox.w <= mfMaxWidth)
				{
					fRowW += kChipGap + vAll[lRowEnd].mBox.w;
					++lRowEnd;
				}
				float x = cx - fRowW / 2;
				if (x < fLeft)
					fLeft = x;
				if (x + fRowW > fRight)
					fRight = x + fRowW;
				for (size_t c = lRowStart; c < lRowEnd; ++c)
				{
					vAll[c].mBox.x = x;
					vAll[c].mBox.y = fRowY;
					x += vAll[c].mBox.w + kChipGap;
					mvChips.push_back(vAll[c]);
				}
				fRowY += 19 + 4;
				lRowStart = lRowEnd;
			}
			fBottom = fRowY - 4;
		}

		/* only a guest's picker is clickable; the host's line is plain text */
		if (mbPicker)
			mRect = cRect2f(fLeft, y, fRight - fLeft, fBottom - y);
		else
			mRect = cRect2f(-1000, -1000, 0, 0);
	}
};

#endif /* PENUMBRA_MULTIPLAYER */

//////////////////////////////////////////////////////////////////////////
// WIDGET
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cMainMenuWidget::cMainMenuWidget(cInit *apInit, const cVector3f &avPos, const cVector2f &avSize)
{
	mpInit = apInit;
	mpDrawer = mpInit->mpGame->GetGraphics()->GetDrawer();

	mvPositon = cVector3f(avPos.x, avPos.y, 40);
	
	mRect.w = avSize.x;
	mRect.h = avSize.y;
	mRect.x = avPos.x - mRect.w/2;
	mRect.y = avPos.y;

	mbActive = true;
}

cMainMenuWidget::~cMainMenuWidget()
{
}

			  
//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// MAIN BUTTON
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cMainMenuWidget_MainButton::cMainMenuWidget_MainButton(cInit *apInit, const cVector3f &avPos, 
											  const tWString& asText, eMainMenuState aNextState)
											: cMainMenuWidget(apInit,avPos,cVector2f(1,1))
{
	mpFont = mpInit->mpGame->GetResources()->GetFontManager()->CreateFontData("font_menu_small.fnt",30);

	msText = asText;
	mvFontSize = 35;
	mbOver = false;

	mfOverTimer = 0;
	mfAlpha =0;

	mRect.w = mpFont->GetLength(mvFontSize,msText.c_str());
	mRect.h = mvFontSize.y +8;
	mRect.x = avPos.x - mRect.w/2;
	mRect.y = avPos.y+3;

	mNextState = aNextState;

	msTip = _W("");
}

cMainMenuWidget_MainButton::~cMainMenuWidget_MainButton()
{
	
}

void cMainMenuWidget_MainButton::OnUpdate(float afTimeStep)
{
	mfOverTimer += afTimeStep*1.3f;

	if(mbOver)
	{
		mfAlpha += 1.8f*afTimeStep;
		if(mfAlpha >1) mfAlpha =1;
	}
	else
	{
		mfAlpha -= 1.3f*afTimeStep;
		if(mfAlpha <0) mfAlpha =0;
	}
}

//-----------------------------------------------------------------------

void cMainMenuWidget_MainButton::OnMouseOver(bool abOver)
{
	mbOver = abOver;

	if(abOver){
		mpInit->mpMainMenu->SetButtonTip(msTip);
	}
}

//-----------------------------------------------------------------------

void cMainMenuWidget_MainButton::OnMouseDown(eMButton aButton)
{
	mpInit->mpMainMenu->SetState(mNextState);
	mpInit->mpGame->GetSound()->GetSoundHandler()->PlayGui("gui_menu_click",false,1);
}

//-----------------------------------------------------------------------

void cMainMenuWidget_MainButton::OnDraw()
{
	mpFont->Draw(mvPositon,mvFontSize,cColor(0.62f + mfAlpha*0.3f,1),eFontAlign_Center,msText.c_str());

	float fAdd = sin(mfOverTimer) * 16.0f;

	if(mfAlpha >0)
	{
		mpFont->Draw(mvPositon + cVector3f(fAdd,0,-1),mvFontSize,cColor(0.56f,0.35f * mfAlpha),eFontAlign_Center,msText.c_str());
		mpFont->Draw(mvPositon + cVector3f(-fAdd,0,-1),mvFontSize,cColor(0.56f,0.35f * mfAlpha),eFontAlign_Center,msText.c_str());
	}
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// BUTTON
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cMainMenuWidget_Button::cMainMenuWidget_Button(cInit *apInit, const cVector3f &avPos, 
													   const tWString& asText, eMainMenuState aNextState,
													   cVector2f avFontSize, eFontAlign aAlignment)
													   : cMainMenuWidget(apInit,avPos,cVector2f(1,1))
{
	mpFont = mpInit->mpGame->GetResources()->GetFontManager()->CreateFontData("font_menu_small.fnt",30);

	msText = asText;
	mvFontSize = avFontSize;
	mbOver = false;

	mAlignment = aAlignment;

	mfAlpha =0;
	mfOverTimer =0;

	mRect.w = mpFont->GetLength(mvFontSize,msText.c_str());
	mRect.h = mvFontSize.y +3;
	mRect.y = avPos.y+3;

	if(mAlignment == eFontAlign_Center)
	{
		mRect.x = avPos.x - mRect.w/2;
	}
	else if(mAlignment == eFontAlign_Left)
	{
		mRect.x = avPos.x;
	}
	else if(mAlignment == eFontAlign_Right)
	{
		mRect.x = avPos.x  - mRect.w;
	}

	mNextState = aNextState;
}

cMainMenuWidget_Button::~cMainMenuWidget_Button()
{

}

void cMainMenuWidget_Button::OnUpdate(float afTimeStep)
{
	if(mbOver)
	{
		mfAlpha += 1.8f*afTimeStep;
		if(mfAlpha >1) mfAlpha =1;
	}
	else
	{
		mfAlpha -= 1.3f*afTimeStep;
		if(mfAlpha <0) mfAlpha =0;
	}

	mfOverTimer += afTimeStep*0.4f;
}

//-----------------------------------------------------------------------

void cMainMenuWidget_Button::OnMouseOver(bool abOver)
{
	mbOver = abOver;

	if(mbOver){
		mpInit->mpMainMenu->SetButtonTip(msTip);
	}
}

//-----------------------------------------------------------------------

void cMainMenuWidget_Button::OnMouseDown(eMButton aButton)
{
	mpInit->mpMainMenu->SetState(mNextState);
	mpInit->mpGame->GetSound()->GetSoundHandler()->PlayGui("gui_menu_click",false,1);
}

//-----------------------------------------------------------------------

void cMainMenuWidget_Button::OnDraw()
{
	mpFont->Draw(mvPositon,mvFontSize,cColor(0.62f ,1),mAlignment,msText.c_str());

	if(mfAlpha > 0)
	{
		float fX = 0.8f + sin(mfOverTimer)*0.2f;
		
		mpFont->Draw(mvPositon+cVector3f(0,0,1),mvFontSize,cColor(0.9f,0.95f,1.0f,mfAlpha*fX),mAlignment,msText.c_str());
		mpFont->Draw(mvPositon+cVector3f(2,2,-1),mvFontSize,cColor(0.1f,0.32f,1.0f,mfAlpha*fX),mAlignment,msText.c_str());
		mpFont->Draw(mvPositon+cVector3f(-2,-2,-1),mvFontSize,cColor(0.1f,0.32f,1.0f,mfAlpha*fX),mAlignment,msText.c_str());
		mpFont->Draw(mvPositon+cVector3f(3,3,-2),mvFontSize,cColor(0.1f,0.32f,1.0f,mfAlpha*0.5f*fX),mAlignment,msText.c_str());
		mpFont->Draw(mvPositon+cVector3f(-3,-3,-2),mvFontSize,cColor(0.1f,0.32f,1.0f,mfAlpha*0.5f*fX),mAlignment,msText.c_str());
	}

}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// TEXT
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cMainMenuWidget_Text::cMainMenuWidget_Text(cInit *apInit, const cVector3f &avPos, const tWString& asText,
											cVector2f avFontSize, eFontAlign aAlignment,
											cMainMenuWidget *apExtra, float afMaxWidth)
: cMainMenuWidget(apInit,avPos,cVector2f(1,1))
{
	mpFont = mpInit->mpGame->GetResources()->GetFontManager()->CreateFontData("font_menu_small.fnt",30);

	mfMaxWidth = afMaxWidth;

	msText = asText;
	mvFontSize = avFontSize;

	mAlignment = aAlignment;
	
	mRect.w = mpFont->GetLength(mvFontSize,msText.c_str());
	mRect.h = mvFontSize.y +3;
	mRect.y = avPos.y+3;

	if(mAlignment == eFontAlign_Center)
	{
		mRect.x = avPos.x - mRect.w/2;
	}
	else if(mAlignment == eFontAlign_Left)
	{
		mRect.x = avPos.x;
	}
	else if(mAlignment == eFontAlign_Right)
	{
		mRect.x = avPos.x  - mRect.w;
	}

	mpExtra = apExtra;

	mbOver = false;
}

cMainMenuWidget_Text::~cMainMenuWidget_Text()
{
	
}

//-----------------------------------------------------------------------

void cMainMenuWidget_Text::UpdateSize()
{
	mRect.w = mpFont->GetLength(mvFontSize,msText.c_str());
}

void cMainMenuWidget_Text::OnDraw()
{
	if(mfMaxWidth <=0)
		mpFont->Draw(mvPositon,mvFontSize,cColor(0.9f,1),mAlignment,_W("%ls"),msText.c_str());
	else
		mpFont->DrawWordWrap(	mvPositon,mfMaxWidth,mvFontSize.y+1,
								mvFontSize,cColor(0.9f,1),mAlignment,msText.c_str());
}

//-----------------------------------------------------------------------

void cMainMenuWidget_Text::OnMouseDown(eMButton aButton)
{
	if(mpExtra)
	{
		mpExtra->OnMouseDown(aButton);
	}
}

void cMainMenuWidget_Text::OnMouseOver(bool abOver)
{
	//if(abOver == mbOver) return;

	mbOver = abOver;

	if(mpExtra)
	{
		if(mpExtra->mbOver==false)
			mpExtra->OnMouseOver(abOver);
	}
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// IMAGE
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cMainMenuWidget_Image::cMainMenuWidget_Image(cInit *apInit, const cVector3f &avPos,const cVector2f& avSize,
											 const tString& asImageFile, const tString& asImageMat,
											 const cColor& aColor)
										   : cMainMenuWidget(apInit,avPos,avSize)
{
	mpImage = mpDrawer->CreateGfxObject(asImageFile,asImageMat);

	mColor = aColor;

	mvSize = avSize;
}

cMainMenuWidget_Image::~cMainMenuWidget_Image()
{

}

//-----------------------------------------------------------------------

void cMainMenuWidget_Image::OnDraw()
{
	mpDrawer->DrawGfxObject(mpImage,mvPositon,mvSize,mColor);
}

//////////////////////////////////////////////////////////////////////////
// LIST
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cMainMenuWidget_List::cMainMenuWidget_List(cInit *apInit, const cVector3f &avPos,const cVector2f &avSize,
					 cVector2f avFontSize)
 : cMainMenuWidget(apInit,avPos,avSize)
{
	mpDrawer = mpInit->mpGame->GetGraphics()->GetDrawer();

	mpFont = mpInit->mpGame->GetResources()->GetFontManager()->CreateFontData("font_menu_small.fnt",30);

	mpBackGfx = mpDrawer->CreateGfxObject("effect_white.jpg","diffalpha2d");

	mpDownGfx = mpDrawer->CreateGfxObject("menu_list_down.bmp","diffalpha2d");
	mpUpGfx = mpDrawer->CreateGfxObject("menu_list_up.bmp","diffalpha2d");
	mpSlideGfx = mpDrawer->CreateGfxObject("menu_list_slide.bmp","diffalpha2d");
	mpBorderLeftGfx = mpDrawer->CreateGfxObject("menu_list_border_left.bmp","diffalpha2d");
	mpBorderTopGfx = mpDrawer->CreateGfxObject("menu_list_border_top.bmp","diffalpha2d");
	mpBorderBottomGfx = mpDrawer->CreateGfxObject("menu_list_border_bottom.bmp","diffalpha2d");
	mpSlideButtonGfx = mpDrawer->CreateGfxObject("menu_list_slider_button.bmp","diffalpha2d");
	
	mvFontSize = avFontSize;

	mRect.x = avPos.x;
	mRect.y = avPos.y;

	mRect.w = avSize.x;
	mRect.h = avSize.y;

	mlMaxRows = (int)floor((avSize.y-6) / (avFontSize.y +2));

	mvPosition = avPos;
	mvSize = avSize;

	mlSelected = -1;

	mlFirstRow =0;

	mbSlideButtonPressed = false;
	mfSlideButtonMove =0;

	mvLastMousePos = mpInit->mpMainMenu->GetMousePos();
}
cMainMenuWidget_List::~cMainMenuWidget_List()
{

}

//-----------------------------------------------------------------------

void cMainMenuWidget_List::OnUpdate(float afTimeStep)
{
	////////////////////////////////
	//Slide button

	//Size
	float fT = (float)mlMaxRows / (float)mvEntries.size();
	if(fT > 1) fT = 1;
	mfSlideButtonSize = (mvSize.y - 28) * fT;

	//Pos
	mfSlideButtonPos =0;
	if((int)mvEntries.size() > mlMaxRows)
	{
		mfSlideButtonPos = ((mvSize.y - 28) - mfSlideButtonSize) *
			(float) mlFirstRow / (float)(mvEntries.size() - mlMaxRows);
	}

	/////////////////////////////////
	//Slide button pressed
	if(mbSlideButtonPressed && (int)mvEntries.size() > mlMaxRows)
	{
		float fMinStep = ((mvSize.y - 28) - mfSlideButtonSize) / (float)mvEntries.size();
		cVector2f vRelMouse = mpInit->mpMainMenu->GetMousePos() - mvLastMousePos;

		mfSlideButtonMove += vRelMouse.y;
		
		
		while(mfSlideButtonMove <= -fMinStep && mlFirstRow >0)
		{
			mlFirstRow--;
			mfSlideButtonMove += fMinStep;
		}
		while(mfSlideButtonMove >= fMinStep && mlFirstRow < (int)mvEntries.size() - mlMaxRows)
		{
			mlFirstRow++;
			mfSlideButtonMove -= fMinStep;
		}
	}

	mvLastMousePos = mpInit->mpMainMenu->GetMousePos();
}

//-----------------------------------------------------------------------

void cMainMenuWidget_List::OnMouseOver(bool abOver)
{
	if(abOver==false){
		mbSlideButtonPressed = false;
		mfSlideButtonMove =0;
	}
}

//-----------------------------------------------------------------------

void cMainMenuWidget_List::OnDraw()
{
	mpDrawer->DrawGfxObject(mpBackGfx,mvPositon-cVector3f(0,0,1),mvSize,cColor(0.05f,0.05f,0.1f,1));
	
	//Up
	mpDrawer->DrawGfxObject(mpUpGfx,
							cVector3f(mvPositon.x + mvSize.x - 14,mvPositon.y,mvPositon.z+1),
							cVector2f(14,14), cColor(1,1));
	//Down
	mpDrawer->DrawGfxObject(mpDownGfx,
							cVector3f(mvPositon.x + mvSize.x - 14,mvPositon.y + mvSize.y - 14,mvPositon.z+1),
							cVector2f(14,14), cColor(1,1));
	//Slide
	mpDrawer->DrawGfxObject(mpSlideGfx,
							cVector3f(mvPositon.x + mvSize.x - 14,mvPositon.y + 14,mvPositon.z+1),
							cVector2f(14,mvSize.y-28), cColor(1,1));

	//Border Top
	mpDrawer->DrawGfxObject(mpBorderTopGfx,
							cVector3f(mvPositon.x + 3,mvPositon.y,mvPositon.z+1),
							cVector2f(mvSize.x-17,3), cColor(1,1));
	//Border Bottom
	mpDrawer->DrawGfxObject(mpBorderBottomGfx,
							cVector3f(mvPositon.x + 3,mvPositon.y+mvSize.y - 3,mvPositon.z+1),
							cVector2f(mvSize.x-17,3), cColor(1,1));

	//Border Left
	mpDrawer->DrawGfxObject(mpBorderLeftGfx,
							cVector3f(mvPositon.x,mvPositon.y,mvPositon.z+1),
							cVector2f(3,mvSize.y), cColor(1,1));

	//Slider Button
	cVector3f vButtonStart = cVector3f(mvPositon.x + mvSize.x - 14,mvPositon.y+14,mvPositon.z+2);
	mpDrawer->DrawGfxObject(mpSlideButtonGfx,vButtonStart + cVector3f(0,mfSlideButtonPos,0),
								cVector2f(14,mfSlideButtonSize), cColor(1,1));
	
	
	cVector3f vPos = mvPositon + cVector3f(5,3,0);

	for(size_t i=mlFirstRow; i< mvEntries.size(); ++i)
	{
		if((int)i-mlFirstRow >= mlMaxRows) break;

		if(mlSelected == i)
		{
			mpFont->Draw(vPos,mvFontSize,cColor(0.95f,1),eFontAlign_Left,mvEntries[i].c_str());
			mpDrawer->DrawGfxObject(mpBackGfx,vPos+cVector3f(0,2,-1),
									cVector2f(mvSize.x-5, mvFontSize.y), 
									cColor(0.0f,0.0f,0.73f,1));
		}
		else
			mpFont->Draw(vPos,mvFontSize,cColor(0.7f,1),eFontAlign_Left,mvEntries[i].c_str());

		vPos.y += mvFontSize.y+2;
	}
}

//-----------------------------------------------------------------------

void cMainMenuWidget_List::OnMouseDown(eMButton aButton)
{
	cVector2f vLocalMouse = mpInit->mpMainMenu->GetMousePos() - 
							cVector2f(mvPositon.x,mvPositon.y);

	//Scrollbar
	if(vLocalMouse.x > mvSize.x - 14)
	{
		//Up Arrow
		if(vLocalMouse.y <= 14)
		{
			if(mlFirstRow>0) mlFirstRow--;
		}
		//Down Arrow
		else if(vLocalMouse.y >= mvSize.y - 14)
		{
			if(mlFirstRow < (int)mvEntries.size() - mlMaxRows) mlFirstRow++;
		}
		//Press slide button
		else if(vLocalMouse.y >= mfSlideButtonPos && 
				vLocalMouse.y <= mfSlideButtonPos + mfSlideButtonSize) 
		{
			mbSlideButtonPressed = true;
		}
	}
	//Entries
	else
	{
		int lSelected = mlFirstRow + (int)floor((vLocalMouse.y-3) / (mvFontSize.y+2));
		if(lSelected < (int)mvEntries.size()) mlSelected = lSelected;
	}
}

void cMainMenuWidget_List::OnMouseUp(eMButton aButton)
{
	mbSlideButtonPressed = false;
	mfSlideButtonMove =0;
}

//-----------------------------------------------------------------------

void cMainMenuWidget_List::AddEntry(const tWString &asText)
{
	mvEntries.push_back(asText);
	if(mlSelected==-1 && mvEntries.size()==1) mlSelected =0;
}

//-----------------------------------------------------------------------

const tWString& cMainMenuWidget_List::GetSelectedEntry()
{
	return mvEntries[0];
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// NEW GAME
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cMainMenuWidget_NewGame::cMainMenuWidget_NewGame(cInit *apInit, const cVector3f &avPos, 
											   const tWString& asText,
											   cVector2f avFontSize, eFontAlign aAlignment,
											   eGameDifficulty aDiffuculty)
					: cMainMenuWidget_Button(apInit,avPos,asText,
											eMainMenuState_LastEnum,avFontSize,aAlignment)
{
	mDiffuculty = aDiffuculty;

	switch(mDiffuculty)
	{
	case eGameDifficulty_Easy:	msTip = kTranslate("MainMenu", "TipDifficultyEasy");
								break;
	case eGameDifficulty_Normal:msTip = kTranslate("MainMenu", "TipDifficultyNormal");
								break;
	case eGameDifficulty_Hard:	msTip = kTranslate("MainMenu", "TipDifficultyHard");
								break;
	}
}
//-----------------------------------------------------------------------

void cMainMenuWidget_NewGame::OnMouseDown(eMButton aButton)
{
	mpInit->mpGraphicsHelper->DrawLoadingScreen("");
	
	mpInit->mpMainMenu->SetActive(false);
	mpInit->ResetGame(true);

	mpInit->mDifficulty = mDiffuculty;
	
	if(mpInit->mbShowIntro)
	{
		mpInit->mpIntroStory->SetActive(true);
	}
	else
	{
		mpInit->mpGame->GetUpdater()->SetContainer("Default");
		mpInit->mpGame->GetScene()->SetDrawScene(true);
		
		mpInit->mpMapHandler->Load(	mpInit->msStartMap,mpInit->msStartLink);
	}
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// CONTINUE
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cMainMenuWidget_Continue::cMainMenuWidget_Continue(cInit *apInit, const cVector3f &avPos, 
												 const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
												 : cMainMenuWidget_Button(apInit,avPos,asText,
												 eMainMenuState_LastEnum,avFontSize,aAlignment)
{

}
//-----------------------------------------------------------------------

void cMainMenuWidget_Continue::OnMouseDown(eMButton aButton)
{
	//mpInit->mpGraphicsHelper->DrawLoadingScreen("other_loading.jpg");
	
	mpInit->mpMainMenu->SetActive(false);

	tWString sAuto = _W("save/auto/") + mpInit->mpSaveHandler->GetLatest(_W("save/auto/"),_W("*.sav"));
	tWString sSpot = _W("save/spot/") + mpInit->mpSaveHandler->GetLatest(_W("save/spot/"),_W("*.sav"));
	
	tWString sFile = _W("");

	if(sAuto == _W("save/auto/"))
	{
		sFile = sSpot;
	}
	else if(sSpot == _W("save/spot/"))
	{
		sFile = sAuto;
	}
	else
	{
		tWString sSaveDir = mpInit->mpSaveHandler->GetSaveDir();
		cDate dateAuto = FileModifiedDate(sSaveDir + sAuto);
		cDate dateSpot = FileModifiedDate(sSaveDir + sSpot);
		
		if(dateAuto > dateSpot) 
		{
			sFile = sAuto;
		}
		else
		{
			sFile = sSpot;
		}
	}

	if(sFile != _W(""))
		mpInit->mpSaveHandler->LoadGameFromFile(sFile);
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// QUIT
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cMainMenuWidget_Quit::cMainMenuWidget_Quit(cInit *apInit, const cVector3f &avPos, 
												 const tWString& asText,
												 cVector2f avFontSize, eFontAlign aAlignment)
												 : cMainMenuWidget_Button(apInit,avPos,
												 asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
{

}
//-----------------------------------------------------------------------

void cMainMenuWidget_Quit::OnMouseDown(eMButton aButton)
{
	mpInit->mpGame->Exit();
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// RESUME
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cMainMenuWidget_Resume::cMainMenuWidget_Resume(cInit *apInit, const cVector3f &avPos, 
										   const tWString& asText)
										   : cMainMenuWidget_MainButton(apInit,avPos,asText,eMainMenuState_LastEnum)
{

}
//-----------------------------------------------------------------------

void cMainMenuWidget_Resume::OnMouseDown(eMButton aButton)
{
	mpInit->mpGame->GetSound()->GetSoundHandler()->PlayGui("gui_menu_click",false,1);

	mpInit->mpMainMenu->SetActive(false);
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// LOADGAME
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

tWStringVec gvSaveGameFileVec[3];

class cMainMenuWidget_SaveGameList : public cMainMenuWidget_List
{
public:
	cMainMenuWidget_SaveGameList(cInit *apInit, const cVector3f &avPos,const cVector2f &avSize,
									cVector2f avFontSize,tWString asDir, int alNum)
									: cMainMenuWidget_List(apInit,avPos, avSize, avFontSize)
	{
		msDir = asDir;
		mlNum = alNum;
	}
	
	void OnDoubleClick(eMButton aButton)
	{
		if(mlSelected <0) return;
		
		tWString sFile = msDir + _W("/") + gvSaveGameFileVec[mlNum][mlSelected];

		mpInit->mpMainMenu->SetActive(false);
		mpInit->ResetGame(true);

		mpInit->mpSaveHandler->LoadGameFromFile(sFile);
	}

private:
	tWString msDir;
	int mlNum;
};

cMainMenuWidget_SaveGameList *gpSaveGameList[3]= {NULL, NULL, NULL};

class cMainMenuWidget_LoadSaveGame : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_LoadSaveGame(cInit *apInit, const cVector3f &avPos, const tWString& asText,
								cVector2f avFontSize, eFontAlign aAlignment, 
								tWString asDir,int alNum)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		msDir = asDir;
		mlNum = alNum;
	}

	void OnMouseDown(eMButton aButton)
	{
		int lSelected = gpSaveGameList[mlNum]->GetSelectedIndex();
		if(lSelected <0) return;

		tWString sFile = msDir + _W("/") + gvSaveGameFileVec[mlNum][lSelected];

		mpInit->mpMainMenu->SetActive(false);
		mpInit->ResetGame(true);

		mpInit->mpSaveHandler->LoadGameFromFile(sFile);
	}

	tWString msDir;
	int mlNum;
};

class cMainMenuWidget_RemoveSaveGame : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_RemoveSaveGame(cInit *apInit, const cVector3f &avPos, const tWString& asText,
		cVector2f avFontSize, eFontAlign aAlignment, 
		tWString asDir,int alNum)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		msDir = asDir;
		mlNum = alNum;
	}

	void OnMouseDown(eMButton aButton)
	{
		int lSelected = gpSaveGameList[mlNum]->GetSelectedIndex();
		if(lSelected <0) return;

		tWString sFile =	mpInit->mpSaveHandler->GetSaveDir() + msDir + 
							_W("/") + gvSaveGameFileVec[mlNum][lSelected];

		RemoveFile(sFile);
		mpInit->mpMainMenu->UpdateWidgets();
	}

	tWString msDir;
	int mlNum;
};

class cMainMenuWidget_FavoriteSaveGame : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_FavoriteSaveGame(cInit *apInit, const cVector3f &avPos, const tWString& asText,
		cVector2f avFontSize, eFontAlign aAlignment, 
		tWString asDir,int alNum)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		msDir = asDir;
		mlNum = alNum;
	}

	void OnMouseDown(eMButton aButton)
	{
		int lSelected = gpSaveGameList[mlNum]->GetSelectedIndex();
		if(lSelected <0) return;

		tWString sFile =	mpInit->mpSaveHandler->GetSaveDir() + msDir + 
							_W("/") + gvSaveGameFileVec[mlNum][lSelected];

		tWString sDest =	mpInit->mpSaveHandler->GetSaveDir() + _W("save/favorite/") + 
							gvSaveGameFileVec[mlNum][lSelected];

		CloneFile(sFile,sDest,true);
		mpInit->mpMainMenu->UpdateWidgets();
	}

	tWString msDir;
	int mlNum;
};

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// OPTIONS CONTROLS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------


cMainMenuWidget_Text *gpInvertMouseYText=NULL;
cMainMenuWidget_Text *gpMouseSensitivityText=NULL;
cMainMenuWidget_Text *gpToggleCrouchText=NULL;
cMainMenuWidget_Text *gpUseHapticsText=NULL;
cMainMenuWidget_Text *gpWidgetInteractModeCameraSpeedText = NULL;
cMainMenuWidget_Text *gpWidgetActionModeCameraSpeedText = NULL;
cMainMenuWidget_Text *gpWidgetWeightForceScaleText = NULL;


//-----------------------------------------------------------------------

class cMainMenuWidget_UseHaptics : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_UseHaptics(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment){msTip = _W("");}

	void OnMouseDown(eMButton aButton)
	{
		mpInit->mbHasHapticsOnRestart = !mpInit->mbHasHapticsOnRestart;

		gpUseHapticsText->msText = mpInit->mbHasHapticsOnRestart ?
			kTranslate("MainMenu","On") : kTranslate("MainMenu","Off");
		gbMustRestart = true;
	}
};

//-----------------------------------------------------------------------

class cMainMenuWidget_WeightForceScale : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_WeightForceScale(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment){msTip = _W("");}

		void OnMouseDown(eMButton aButton)
		{
			float afX = mpInit->mfHapticForceMul;
			if(aButton == eMButton_Left)
			{
				afX += 0.1f;
				if(afX>3.0f) afX = 3.0f;
			}
			else if(aButton == eMButton_Right)
			{
				afX -= 0.1f;
				if(afX<0.0f) afX = 0.0f;
			}

			char sTempVec[256];
			sprintf(sTempVec,"%.1f",afX);
			gpWidgetWeightForceScaleText->msText = cString::To16Char(sTempVec);

			mpInit->mfHapticForceMul = afX;
		}
};

//-----------------------------------------------------------------------

class cMainMenuWidget_InteractModeCameraSpeed : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_InteractModeCameraSpeed(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment){msTip = _W("");}

		void OnMouseDown(eMButton aButton)
		{
			float afX = mpInit->mpPlayer->GetHapticCamera()->GetInteractModeCameraSpeed();
			if(aButton == eMButton_Left)
			{
				afX += 0.1f;
				if(afX>3.0f) afX = 3.0f;
			}
			else if(aButton == eMButton_Right)
			{
				afX -= 0.1f;
				if(afX<0.1f) afX = 0.1f;
			}

			char sTempVec[256];
			sprintf(sTempVec,"%.1f",afX);
			gpWidgetInteractModeCameraSpeedText->msText = cString::To16Char(sTempVec);

			mpInit->mpPlayer->GetHapticCamera()->SetInteractModeCameraSpeed(afX);
		}
};

//-----------------------------------------------------------------------

class cMainMenuWidget_ActionModeCameraSpeed : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_ActionModeCameraSpeed(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment){msTip = _W("");}

		void OnMouseDown(eMButton aButton)
		{
			float afX = mpInit->mpPlayer->GetHapticCamera()->GetActionModeCameraSpeed();
			if(aButton == eMButton_Left)
			{
				afX += 0.1f;
				if(afX>3.0f) afX = 3.0f;
			}
			else if(aButton == eMButton_Right)
			{
				afX -= 0.1f;
				if(afX<0.1f) afX = 0.1f;
			}

			char sTempVec[256];
			sprintf(sTempVec,"%.1f",afX);
			gpWidgetActionModeCameraSpeedText->msText = cString::To16Char(sTempVec);

			mpInit->mpPlayer->GetHapticCamera()->SetActionModeCameraSpeed(afX);
		}
};


//-----------------------------------------------------------------------

class cMainMenuWidget_InvertMouseY : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_InvertMouseY(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
								: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		msTip = kTranslate("MainMenu", "TipControlsInvertMouseY");
	}
	
	void OnMouseDown(eMButton aButton)
	{
		mpInit->mpButtonHandler->mbInvertMouseY = !mpInit->mpButtonHandler->mbInvertMouseY;
		gpInvertMouseYText->msText = mpInit->mpButtonHandler->mbInvertMouseY ? kTranslate("MainMenu","On") : 
																				kTranslate("MainMenu","Off");
	}
};

//------------------------------------------------------------

class cMainMenuWidget_MouseSensitivity : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_MouseSensitivity(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		msTip = kTranslate("MainMenu", "TipControlsMouseSensitivity");
	}

	void OnMouseDown(eMButton aButton)
	{
		if(aButton == eMButton_Left)
		{
			mpInit->mpButtonHandler->mfMouseSensitivity += 0.2f;
			if(mpInit->mpButtonHandler->mfMouseSensitivity>5.0f) 
				mpInit->mpButtonHandler->mfMouseSensitivity= 5.0f;
		}
		else if(aButton == eMButton_Right)
		{
			mpInit->mpButtonHandler->mfMouseSensitivity -= 0.2f;
			if(mpInit->mpButtonHandler->mfMouseSensitivity<0.2f) 
				mpInit->mpButtonHandler->mfMouseSensitivity= 0.2f;
		}

		char sTempVec[256];
		sprintf(sTempVec,"%.1f",mpInit->mpButtonHandler->mfMouseSensitivity);
		gpMouseSensitivityText->msText = cString::To16Char(sTempVec);
	}
};

//------------------------------------------------------------

class cMainMenuWidget_ToggleCrouch : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_ToggleCrouch(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		msTip = kTranslate("MainMenu", "TipControlsToggleCrouch");
	}

	void OnMouseDown(eMButton aButton)
	{
		mpInit->mpButtonHandler->mbToggleCrouch = !mpInit->mpButtonHandler->mbToggleCrouch;
		gpToggleCrouchText->msText = mpInit->mpButtonHandler->mbToggleCrouch ? kTranslate("MainMenu","On")
																			: kTranslate("MainMenu","Off");
	}
};

//////////////////////////////////////////////////////////////////////////
// OPTIONS SOUND
//////////////////////////////////////////////////////////////////////////

cMainMenuWidget_Text *gpSoundVolumeText=NULL;
cMainMenuWidget_Text *gpSoundHardwareText=NULL;
cMainMenuWidget_Text *gpSoundOutputDevice=NULL;

class cMainMenuWidget_SoundVolume : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_SoundVolume(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		msTip = kTranslate("MainMenu", "TipSoundVolume");
	}

	void OnMouseDown(eMButton aButton)
	{
		float fVolume = mpInit->mpGame->GetSound()->GetLowLevel()->GetVolume();

		if(aButton  == eMButton_Left)
		{
			fVolume += 0.1f;
			if(fVolume > 1.0f) fVolume = 1.0f;
		}
		else if(aButton  == eMButton_Right)
		{
			fVolume -= 0.1f;
			if(fVolume < 0.0f) fVolume = 0.0f;
		}

		mpInit->mpGame->GetSound()->GetLowLevel()->SetVolume(fVolume);

		char sTempVec[256];
		sprintf(sTempVec,"%.0f",mpInit->mpGame->GetSound()->GetLowLevel()->GetVolume()*100);
		gpSoundVolumeText->msText = cString::To16Char(sTempVec);
	}
};

class cMainMenuWidget_SoundHardware : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_SoundHardware(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		msTip = kTranslate("MainMenu", "TipSoundHardware");
	}

	void OnMouseDown(eMButton aButton)
	{
		mpInit->mbUseSoundHardware = !mpInit->mbUseSoundHardware;

		gpSoundHardwareText->msText = mpInit->mbUseSoundHardware ?
			kTranslate("MainMenu","On") : kTranslate("MainMenu","Off");

		gbMustRestart = true;
	}
};

class cMainMenuWidget_SoundOutputDevice : public cMainMenuWidget_Button
{
public:
	tStringVec mlDevices;

	cMainMenuWidget_SoundOutputDevice(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		msTip = kTranslate("MainMenu", "TipSoundOutputDevice");
		mlDevices = OAL_Info_GetOutputDevices();
	}

	void OnMouseDown(eMButton aButton)
	{
		int lCurrentNum = 0;

		//get current num
        for(int i=0; i<mlDevices.size(); ++i)
		{
			if(mlDevices[i] == mpInit->msDeviceName)
			{
				lCurrentNum = i;
				break;
			}
		}

		if(aButton == eMButton_Left)
		{
			lCurrentNum++;
			if(lCurrentNum >= mlDevices.size()) lCurrentNum =0;
		}
		else if(aButton == eMButton_Right)
		{
			lCurrentNum--;
			if(lCurrentNum < 0) lCurrentNum =mlDevices.size()-1;
		}

		mpInit->msDeviceName = mlDevices[lCurrentNum];

		gpSoundOutputDevice->msText = cString::To16Char(mpInit->msDeviceName);

		gbMustRestart = true;
	}
};

//////////////////////////////////////////////////////////////////////////
// OPTIONS GAME
//////////////////////////////////////////////////////////////////////////

cMainMenuWidget_Text *gpLanguageText=NULL;
cMainMenuWidget_Text *gpSubtitlesText=NULL;

cMainMenuWidget_Text *gpSimpleSwingText=NULL;
cMainMenuWidget_Text *gpAllowQuickSaveText=NULL;
cMainMenuWidget_Text *gpDisablePersonalText=NULL;
cMainMenuWidget_Text *gpDifficultyText=NULL;
cMainMenuWidget_Text *gpFlashItemsText=NULL;
cMainMenuWidget_Text *gpShowCrossHairText=NULL;


class cMainMenuWidget_ShowCrossHair : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_ShowCrossHair(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		msTip = _W("");
	}

	void OnMouseDown(eMButton aButton)
	{
		mpInit->mbShowCrossHair = !mpInit->mbShowCrossHair;

		gpShowCrossHairText->msText = mpInit->mbShowCrossHair ?
			kTranslate("MainMenu","On") : kTranslate("MainMenu","Off");
	}
};

class cMainMenuWidget_FlashItems : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_FlashItems(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		msTip = kTranslate("MainMenu", "TipGameFlashText");
	}

	void OnMouseDown(eMButton aButton)
	{
		mpInit->mbFlashItems = !mpInit->mbFlashItems;

		gpFlashItemsText->msText = mpInit->mbFlashItems ?
			kTranslate("MainMenu","On") : kTranslate("MainMenu","Off");
	}
};


tString gvDifficultyLevel[] = {"Easy","Normal","Hard"};
int glDifficultyLevelNum = 3;

class cMainMenuWidget_Difficulty : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_Difficulty(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		msTip = kTranslate("MainMenu", "TipGameDifficulty");
	}

	void OnMouseDown(eMButton aButton)
	{
		int lCurrent = (int)mpInit->mDifficulty;
		if(aButton == eMButton_Left)
		{
			lCurrent++;
			if(lCurrent >= glDifficultyLevelNum) lCurrent =0;
		}
		else if(aButton == eMButton_Right)
		{
			lCurrent--;
			if(lCurrent < 0) lCurrent =glDifficultyLevelNum-1;
		}

		gpDifficultyText->msText = kTranslate("MainMenu",gvDifficultyLevel[lCurrent]);
		mpInit->mDifficulty = (eGameDifficulty)lCurrent;
	}
};

class cMainMenuWidget_SimpleSwing : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_SimpleSwing(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
	}

	void OnMouseDown(eMButton aButton)
	{
		mpInit->mbSimpleWeaponSwing = !mpInit->mbSimpleWeaponSwing;
		
		gpSimpleSwingText->msText = mpInit->mbSimpleWeaponSwing ?
									kTranslate("MainMenu","On") : kTranslate("MainMenu","Off");
	}
};

class cMainMenuWidget_AllowQuickSave : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_AllowQuickSave(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
	}

	void OnMouseDown(eMButton aButton)
	{
		mpInit->mbAllowQuickSave = !mpInit->mbAllowQuickSave;

		gpAllowQuickSaveText->msText = mpInit->mbAllowQuickSave ?
			kTranslate("MainMenu","On") : kTranslate("MainMenu","Off");
	}
};

class cMainMenuWidget_DisablePersonal : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_DisablePersonal(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		msTip = kTranslate("MainMenu", "TipGameDisablePersonal");
	}

	void OnMouseDown(eMButton aButton)
	{
		mpInit->mbDisablePersonalNotes = !mpInit->mbDisablePersonalNotes;

		gpDisablePersonalText->msText = mpInit->mbDisablePersonalNotes ?
			kTranslate("MainMenu","Off") : kTranslate("MainMenu","On");
	}
};

class cMainMenuWidget_Subtitles : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_Subtitles(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		msTip = kTranslate("MainMenu", "TipGameSubtitles");
	}

	void OnMouseDown(eMButton aButton)
	{
		mpInit->mbSubtitles = !mpInit->mbSubtitles;

		gpSubtitlesText->msText = mpInit->mbSubtitles ?
			kTranslate("MainMenu","On") : kTranslate("MainMenu","Off");
	}
};

class cMainMenuWidget_Language : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_Language(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		tWStringList lstStrings;
		apInit->mpGame->GetResources()->GetLowLevel()->FindFilesInDir(lstStrings,_W("config/"),_W("*.lang"));

		mlCurrentFile =0;
		int lIdx=0;
		for(tWStringListIt it = lstStrings.begin(); it != lstStrings.end();++it)
		{
			mvFiles.push_back(*it);
			if(	cString::To16Char(cString::ToLowerCase(apInit->msLanguageFile)) == 
				cString::ToLowerCaseW(*it))
			{
				mlCurrentFile = lIdx;
			}

			++lIdx;
		}

		msTip = kTranslate("MainMenu", "TipGameLanguage");
	}

	void OnMouseDown(eMButton aButton)
	{
		if(aButton  == eMButton_Left)
		{
			mlCurrentFile++;
			if(mlCurrentFile >= (int)mvFiles.size()) mlCurrentFile=0;
		}
		else if(aButton  == eMButton_Right)
		{
			mlCurrentFile--;
			if(mlCurrentFile < 0) mlCurrentFile= (int)mvFiles.size()-1;
		}

		gpLanguageText->msText = cString::SetFileExtW(mvFiles[mlCurrentFile],_W(""));
		mpInit->msLanguageFile = cString::To8Char(mvFiles[mlCurrentFile]);

		if(mpInit->mpMapHandler->GetCurrentMapName() != "")
		{
			gbMustRestart = true;
		}
		else
		{
			mpInit->mpGame->GetResources()->ClearResourceDirs();
			mpInit->mpGame->GetResources()->AddResourceDir("core/programs");
			mpInit->mpGame->GetResources()->AddResourceDir("core/textures");
			mpInit->mpGame->GetResources()->LoadResourceDirsFile("resources.cfg");
			
			mpInit->mpGame->GetResources()->SetLanguageFile(mpInit->msLanguageFile);

			mpInit->mpMainMenu->UpdateWidgets();
		}
	}

	std::vector<tWString> mvFiles;
	int mlCurrentFile;
};

//------------------------------------------------------------






//------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// OPTIONS GRAPHICS
//////////////////////////////////////////////////////////////////////////

cMainMenuWidget_Text *gpResolutionText=NULL;
cMainMenuWidget_Text *gpPostEffectsText=NULL;
cMainMenuWidget_Text *gpBloomText=NULL;
cMainMenuWidget_Text *gpMotionBlurText=NULL;
cMainMenuWidget_Text *gpVSyncText=NULL;
cMainMenuWidget_Text *gpTextureQualityText=NULL;
cMainMenuWidget_Text *gpShaderQualityText=NULL;
cMainMenuWidget_Text *gpShadowsText=NULL;
cMainMenuWidget_Text *gpTextureFilterText=NULL;
cMainMenuWidget_Text *gpTextureAnisotropyText=NULL;
cMainMenuWidget_Text *gpGammaText=NULL;
cMainMenuWidget_Text *gpGammaText2=NULL;
cMainMenuWidget_Text *gpFSAAText=NULL;
cMainMenuWidget_Text *gpDoFText=NULL;

cVector2l gvResolutions[] = {cVector2l(640,480), cVector2l(800,600), cVector2l(1024, 768),
							cVector2l(1152,864),cVector2l(1280,720),cVector2l(1280,768),
							cVector2l(1280,800),cVector2l(1280,960),cVector2l(1280,1024),
							cVector2l(1360,768),cVector2l(1360,1024),cVector2l(1400,1050),
							cVector2l(1440,900),cVector2l(1680,1050),cVector2l(1600,1200), 
							cVector2l(1920,1080),cVector2l(1920,1200)
						};
int glResolutionNum = 17;

tString gvTextureQuality[] = {"High","Medium","Low"};
int glTextureQualityNum = 3;
tString gvShaderQuality[] = {"Very Low","Low","Medium","High"};
int glShaderQualityNum = 4;


cMainMenuWidget_Text *gpNoiseFilterText=NULL;

//------------------------------------------------------------

class cMainMenuWidget_Gamma : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_Gamma(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment,
							int alGNum)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		mfMax = 3.0f;
		mfMin = 0.1f;
		mfStep = 0.1f;
		
		mlGNum = alGNum;

		msTip = kTranslate("MainMenu", "TipGraphicsGamma");
	}

	void OnMouseDown(eMButton aButton)
	{
		mfGamma = mpInit->mpGame->GetGraphics()->GetLowLevel()->GetGammaCorrection();

		if(aButton == eMButton_Left)
		{
			mfGamma += mfStep;
			if(mfGamma >mfMax) mfGamma = mfMax;
		}
		else if(aButton == eMButton_Right)
		{
			mfGamma -= mfStep;
			if(mfGamma < mfMin) mfGamma = mfMin;
		}

		mpInit->mpGame->GetGraphics()->GetLowLevel()->SetGammaCorrection(mfGamma);
		
		char sTempVec[256];
		sprintf(sTempVec,"%.1f",mfGamma);
		
		gpGammaText->msText = cString::To16Char(sTempVec);
		if(mlGNum == 1)
			gpGammaText2->msText = cString::To16Char(sTempVec);
	}

	float mfGamma;
	float mfMax;
	float mfMin;
	float mfStep;
	int mlGNum;
};

//------------------------------------------------------------


class cMainMenuWidget_NoiseFilter : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_NoiseFilter(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		msTip = kTranslate("MainMenu", "TipGraphicsNoiseFilter");
	}

	void OnMouseDown(eMButton aButton)
	{
		bool bX = mpInit->mpPlayer->GetNoiseFilter()->IsActive();
		mpInit->mpPlayer->GetNoiseFilter()->SetActive(!bX);

		gpNoiseFilterText->msText = mpInit->mpPlayer->GetNoiseFilter()->IsActive() ? 
							kTranslate("MainMenu","On") : kTranslate("MainMenu","Off");
	}
};

//------------------------------------------------------------

tString gvShadowTypes[] = {"On","Only Static","Off"};

class cMainMenuWidget_Shadows : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_Shadows(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		mlCurrent = apInit->mpGame->GetGraphics()->GetRenderer3D()->GetShowShadows();
		msTip = kTranslate("MainMenu", "TipGraphicsShadows");
	}

	void OnMouseDown(eMButton aButton)
	{
		if(aButton == eMButton_Left)
		{
			mlCurrent++;
			if(mlCurrent >= 3) mlCurrent =0;
		}
		else if(aButton == eMButton_Right)
		{
			mlCurrent--;
			if(mlCurrent < 0) mlCurrent =2;
		}

		gpShadowsText->msText = kTranslate("MainMenu",gvShadowTypes[mlCurrent]);
		mpInit->mpGame->GetGraphics()->GetRenderer3D()->SetShowShadows((eRendererShowShadows)mlCurrent);
	}

	int mlCurrent;
};

//------------------------------------------------------------

tString gvTextureFilter[] = {"Bilinear","Trilinear"};

class cMainMenuWidget_TextureFilter : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_TextureFilter(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		mlCurrent = apInit->mpGame->GetResources()->GetMaterialManager()->GetTextureFilter();
		msTip = kTranslate("MainMenu", "TipGraphicsTextureFilter");
	}

	void OnMouseDown(eMButton aButton)
	{
		if(aButton == eMButton_Left)
		{
			mlCurrent--;
			if(mlCurrent<0)mlCurrent=1;
		}
		else if(aButton == eMButton_Right)
		{
			mlCurrent++;
			if(mlCurrent>1)mlCurrent=0;
		}

		gpTextureFilterText->msText = kTranslate("MainMenu",gvTextureFilter[mlCurrent]);
		mpInit->mpGame->GetResources()->GetMaterialManager()->SetTextureFilter((eTextureFilter)mlCurrent);
	}

	int mlCurrent;
};

//------------------------------------------------------------

class cMainMenuWidget_TextureAnisotropy : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_TextureAnisotropy(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		mlMax = mpInit->mpGame->GetGraphics()->GetLowLevel()->GetCaps(eGraphicCaps_MaxAnisotropicFiltering);
		msTip = kTranslate("MainMenu", "TipGraphicsTextureAnisotropy");
	}

	void OnMouseDown(eMButton aButton)
	{
		int lX = (int)mpInit->mpGame->GetResources()->GetMaterialManager()->GetTextureAnisotropy();

		if(aButton == eMButton_Left)
		{
			lX *= 2;
			if(lX > mlMax) lX = 1;
		}
		else if(aButton == eMButton_Right)
		{
			lX /= 2;
			if(lX < 1) lX = mlMax;
		}
		
		if(lX!=1)
			gpTextureAnisotropyText->msText = cString::To16Char(cString::ToString(lX)+"x");
		else
			gpTextureAnisotropyText->msText = kTranslate("MainMenu","Off");

		mpInit->mpGame->GetResources()->GetMaterialManager()->SetTextureAnisotropy((float)lX);
	}

	int mlMax;
};

//------------------------------------------------------------

class cMainMenuWidget_FSAA : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_FSAA(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		mlMax = 4;
		msTip = kTranslate("MainMenu", "TipGraphicsFSAA");
	}

	void OnMouseDown(eMButton aButton)
	{
		int lX = mpInit->mlFSAA;

		if(aButton == eMButton_Left)
		{
			if(lX ==0)	lX = 2;
			else		lX *= 2;

			if(lX > mlMax) lX = 0;
		}
		else if(aButton == eMButton_Right)
		{
			if(lX == 2)			lX = 0;
            else if(lX == 0)	lX = -1;
			else				lX /= 2;
			
			if(lX < 0) lX = mlMax;
		}

		if(lX!=0)
			gpFSAAText->msText = cString::To16Char(cString::ToString(lX)+"x");
		else
			gpFSAAText->msText = kTranslate("MainMenu","Off");

		mpInit->mlFSAA = lX;

		gbMustRestart = true;
	}

	int mlMax;
	int mlCurrent;
};

class cMainMenuWidget_DOF : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_DOF(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		msTip = kTranslate("MainMenu", "TipGraphicsDOF");	
	}

	void OnMouseDown(eMButton aButton)
	{
		bool bX = mpInit->mpEffectHandler->GetDepthOfField()->IsDisabled();
		mpInit->mpEffectHandler->GetDepthOfField()->SetDisabled(!bX);

		gpDoFText->msText = bX ? kTranslate("MainMenu","On") : kTranslate("MainMenu","Off");
	}
};


//------------------------------------------------------------

class cMainMenuWidget_ShaderQuality : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_ShaderQuality(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		msTip = kTranslate("MainMenu", "TipGraphicsShaderQuality");
	}

	void OnMouseDown(eMButton aButton)
	{
		int lCurrent = iMaterial::GetQuality();
		if(aButton == eMButton_Left)
		{
			lCurrent++;
			if(lCurrent >= glShaderQualityNum) lCurrent =0;
		}
		else if(aButton == eMButton_Right)
		{
			lCurrent--;
			if(lCurrent < 0) lCurrent =glShaderQualityNum-1;
		}

		gpShaderQualityText->msText = kTranslate("MainMenu",gvShaderQuality[lCurrent]);
		iMaterial::SetQuality((eMaterialQuality) lCurrent);

		if(mpInit->mpMapHandler->GetCurrentMapName() != "") gbMustRestart = true;
	}
};

//------------------------------------------------------------

class cMainMenuWidget_TextureQuality : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_TextureQuality(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		mlCurrent = apInit->mpGame->GetResources()->GetMaterialManager()->GetTextureSizeLevel();
		msTip = kTranslate("MainMenu", "TipGraphicsTextureQuality");
	}

	void OnMouseDown(eMButton aButton)
	{
		if(aButton == eMButton_Right)
		{
			mlCurrent++;
			if(mlCurrent >= glTextureQualityNum) mlCurrent =0;
		}
		else if(aButton == eMButton_Left)
		{
			mlCurrent--;
			if(mlCurrent < 0) mlCurrent =glTextureQualityNum-1;
		}

		gpTextureQualityText->msText = kTranslate("MainMenu",gvTextureQuality[mlCurrent]);
		mpInit->mpGame->GetResources()->GetMaterialManager()->SetTextureSizeLevel(mlCurrent);

		gbMustRestart = true;
	}

	int mlCurrent;
};

//------------------------------------------------------------


class cMainMenuWidget_Resolution : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_Resolution(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		msTip = kTranslate("MainMenu", "TipGraphicsResolution");
	}

	void OnMouseDown(eMButton aButton)
	{
		int lCurrentNum =0;

		//get current num
        for(int i=0; i<glResolutionNum; ++i)
		{
			if(gvResolutions[i] == mpInit->mvScreenSize)
			{
				lCurrentNum = i;
				break;
			}
		}

		if(aButton == eMButton_Left)
		{
			lCurrentNum++;
			if(lCurrentNum >= glResolutionNum) lCurrentNum =0;
		}
		else if(aButton == eMButton_Right)
		{
			lCurrentNum--;
			if(lCurrentNum < 0) lCurrentNum =glResolutionNum-1;
		}

		mpInit->mvScreenSize = gvResolutions[lCurrentNum];

		char sTempVec[256];
		sprintf(sTempVec,"%d x %d",mpInit->mvScreenSize.x, mpInit->mvScreenSize.y);
		gpResolutionText->msText = cString::To16Char(sTempVec);
		
		gbMustRestart = true;
	}
};

//-----------------------------------------------------------

class cMainMenuWidget_PostEffects : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_PostEffects(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		msTip = kTranslate("MainMenu", "TipGraphicsPostEffects");
	}

	void OnMouseDown(eMButton aButton)
	{
		bool bX = mpInit->mpGame->GetGraphics()->GetRendererPostEffects()->GetActive();
		mpInit->mpGame->GetGraphics()->GetRendererPostEffects()->SetActive(!bX);
		mpInit->mbPostEffects = !bX;

		gpPostEffectsText->msText = mpInit->mpGame->GetGraphics()->GetRendererPostEffects()->GetActive() ? 
			kTranslate("MainMenu","On") : kTranslate("MainMenu","Off");
	}
};

//-----------------------------------------------------------

class cMainMenuWidget_Bloom : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_Bloom(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		msTip = kTranslate("MainMenu", "TipGraphicsBloom");
	}

	void OnMouseDown(eMButton aButton)
	{
		bool bX = mpInit->mpGame->GetGraphics()->GetRendererPostEffects()->GetBloomActive();
		mpInit->mpGame->GetGraphics()->GetRendererPostEffects()->SetBloomActive(!bX);

		gpBloomText->msText = mpInit->mpGame->GetGraphics()->GetRendererPostEffects()->GetBloomActive() ? 
								kTranslate("MainMenu","On") : kTranslate("MainMenu","Off");
	}
};

//------------------------------------------------------------


class cMainMenuWidget_MotionBlur : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_MotionBlur(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		msTip = kTranslate("MainMenu", "TipGraphicsMotionBlur");
	}

	void OnMouseDown(eMButton aButton)
	{
		bool bX = mpInit->mpGame->GetGraphics()->GetRendererPostEffects()->GetMotionBlurActive();
		mpInit->mpGame->GetGraphics()->GetRendererPostEffects()->SetMotionBlurActive(!bX);

		gpMotionBlurText->msText = mpInit->mpGame->GetGraphics()->GetRendererPostEffects()->GetMotionBlurActive() ? 
			kTranslate("MainMenu","On") : kTranslate("MainMenu","Off");
	}
};

//------------------------------------------------------------

class cMainMenuWidget_VSync : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_VSync(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		msTip = kTranslate("MainMenu", "TipGraphicsVSync");
	}

	void OnMouseDown(eMButton aButton)
	{
		mpInit->mbVsync = !mpInit->mbVsync;
		mpInit->mpGame->GetGraphics()->GetLowLevel()->SetVsyncActive(mpInit->mbVsync);

		gpVSyncText->msText = mpInit->mbVsync ? kTranslate("MainMenu","On") : kTranslate("MainMenu","Off");
	}
};

//------------------------------------------------------------

class cMainMenuWidget_GfxBack : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_GfxBack(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
	}

	void OnMouseDown(eMButton aButton)
	{
		mpInit->mpGame->GetSound()->GetSoundHandler()->PlayGui("gui_menu_click",false,1);
		if(gbMustRestart)
		{
			mpInit->mpMainMenu->SetState(eMainMenuState_GraphicsRestart);
			gbMustRestart = false;
		}
		else
		{
			mpInit->mpMainMenu->SetState(eMainMenuState_Options);
		}
	}
};

//////////////////////////////////////////////////////////////////////////
// OPTIONS KEY CONFIG
//////////////////////////////////////////////////////////////////////////

class cMainMenuWidget_KeyButton : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_KeyButton(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, 
								eFontAlign aAlignment, cMainMenuWidget_Text *apKeyWiget,
								const tString &asActionName)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
		mpKeyWidget = apKeyWiget;
		msActionName = asActionName;

		iAction *pAction = mpInit->mpGame->GetInput()->GetAction(asActionName);
		if(pAction)
		{
			tString sKeyName = pAction->GetInputName();
			mpKeyWidget->msText =kTranslate("ButtonNames",sKeyName);
			
			//If translation is missing, set to default
			if(mpKeyWidget->msText == _W(""))
				mpKeyWidget->msText = cString::To16Char(sKeyName);

		}
		else
		{
			mpKeyWidget->msText = kTranslate("MainMenu","Empty");
			//FatalError("Action for %s button does not exist!\n",msText.c_str());
		}

        mpKeyWidget->SetExtraWidget(this);		
		mpKeyWidget->UpdateSize();
	}

	void OnMouseDown(eMButton aButton)
	{
		mpKeyWidget->msText = _W(".....");
		mpInit->mpMainMenu->SetInputToAction(msActionName,mpKeyWidget);
	}

	void Reset()
	{
		iAction *pAction = mpInit->mpGame->GetInput()->GetAction(msActionName);
		
		if(pAction)
		{
			tString sKeyName = pAction->GetInputName();
			mpKeyWidget->msText =kTranslate("ButtonNames",sKeyName);

			//If translation is missing, set to default
			if(mpKeyWidget->msText == _W(""))
				mpKeyWidget->msText = cString::To16Char(sKeyName);

            mpKeyWidget->UpdateSize();
		}
		else
		{
			mpKeyWidget->msText = kTranslate("MainMenu","Empty");
		}
	}

private:
	cMainMenuWidget_Text *mpKeyWidget;
	tString msActionName;
};

//------------------------------------------------------------


class cMainMenuWidget_KeyReset : public cMainMenuWidget_Button
{
public:
	cMainMenuWidget_KeyReset(cInit *apInit, const cVector3f &avPos, const tWString& asText,cVector2f avFontSize, 
							eFontAlign aAlignment)
		: cMainMenuWidget_Button(apInit,avPos,asText,eMainMenuState_LastEnum,avFontSize,aAlignment)
	{
	}

	void OnMouseDown(eMButton aButton)
	{
		//Log("Setting deafult keys!\n");
		mpInit->mpButtonHandler->SetDefaultKeys();
		mpInit->mpMainMenu->ResetWidgets(eMainMenuState_OptionsKeySetupMove);
		mpInit->mpMainMenu->ResetWidgets(eMainMenuState_OptionsKeySetupAction);
		mpInit->mpMainMenu->ResetWidgets(eMainMenuState_OptionsKeySetupMisc);
	}
private:
	cMainMenuWidget_Text *mpKeyWidget;
	tString msActionName;
};

//------------------------------------------------------------


//////////////////////////////////////////////////////////////////////////
// CONSTRUCTORS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cMainMenu::cMainMenu(cInit *apInit)  : iUpdateable("MainMenu")
{
	mState = eMainMenuState_Start;

	mpLogo = NULL;
	mpBackground = NULL;

	mpInit = apInit;
	mpDrawer = mpInit->mpGame->GetGraphics()->GetDrawer();

	//Load graphics
	mpGfxBlackQuad = mpDrawer->CreateGfxObject("effect_black.bmp","diffalpha2d");
	mpGfxMouse = mpDrawer->CreateGfxObject("player_crosshair_pointer.bmp","diffalpha2d");

	mpGfxRainDrop = mpDrawer->CreateGfxObject("menu_rain_drop.jpg","diffadditive2d");
	mpGfxRainSplash = mpDrawer->CreateGfxObject("menu_rain_splash.jpg","diffadditive2d");
	mpGfxSnowFlake = mpDrawer->CreateGfxObject("menu_snow_flake.jpg","diffadditive2d");
	
	//Init effects
	mvRainDrops.resize(70);
	mvRainSplashes.resize(180);
	for(size_t i=0; i < mvRainSplashes.size(); ++i){
		mvRainSplashes[i].mCol = cColor(1,0);
		mvRainSplashes[i].mpGfx = mpGfxRainSplash;
	}
	for(size_t i=0; i < mvRainDrops.size(); ++i){
		mvRainDrops[i].mCol = cColor(1,0);
		mvRainDrops[i].mpGfx = mpGfxRainDrop;
	}

	mvSnowFlakes.resize(80);
	for(size_t i=0; i< mvSnowFlakes.size(); ++i){
		mvSnowFlakes[i].mvPos = cVector3f(cMath::RandRectf(350,800),cMath::RandRectf(200,550),20);
		mvSnowFlakes[i].mvVel = cVector3f(0,cMath::RandRectf(15,40),0);
		mvSnowFlakes[i].mvSize = cMath::RandRectf(2,10);
		mvSnowFlakes[i].mpGfx = mpGfxSnowFlake;
	}
	
	//load fonts
	mpFont = mpInit->mpGame->GetResources()->GetFontManager()->CreateFontData("font_menu_small.fnt",20,32,255);
	mpTipFont  = mpInit->mpGame->GetResources()->GetFontManager()->CreateFontData("verdana.fnt");

	//////////////////////////////////
	//Init widgets
	
	mvState.resize(eMainMenuState_LastEnum);
	
	Reset();
}

//-----------------------------------------------------------------------

cMainMenu::~cMainMenu(void)
{
	STLDeleteAll(mlstWidgets);
#ifdef PENUMBRA_MULTIPLAYER
	MulReleaseGfx(); /* the multiplayer screens' shared white quad */
#endif

    mpDrawer->DestroyGfxObject(mpGfxBlackQuad);
	mpDrawer->DestroyGfxObject(mpGfxMouse);
	mpDrawer->DestroyGfxObject(mpGfxRainDrop);
	mpDrawer->DestroyGfxObject(mpGfxRainSplash);
	mpDrawer->DestroyGfxObject(mpGfxSnowFlake);

	if(mpLogo) mpInit->mpGame->GetResources()->GetTextureManager()->Destroy(mpLogo);
	if(mpBackground) mpInit->mpGame->GetResources()->GetTextureManager()->Destroy(mpBackground);
	
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// PUBLIC METHODS
//////////////////////////////////////////////////////////////////////////


//-----------------------------------------------------------------------

void cMainMenu::Reset()
{
	mbActive = false;
	mbFadeIn = false;
	mfAlpha =0;
	mfFadeAmount = 0;

	mbMouseIsDown = false;

	mbUpdateWidgets = false;

	mpCurrentActionText = NULL;

	mbGameActive = false;

	//Effects:
	mfRainDropCount = 0;

}

//-----------------------------------------------------------------------

void cMainMenu::OnPostSceneDraw()
{
	mpInit->mpGraphicsHelper->ClearScreen(cColor(0,0));
	
	mpInit->mpGraphicsHelper->DrawTexture(mpLogo,0,cVector3f(800,180,30),cColor(1,1));
	mpInit->mpGraphicsHelper->DrawTexture(mpBackground,cVector3f(0,180,0),cVector3f(800,420,0),cColor(1,1));

	////////////////////////////////
	// Fade in
	if (mbFadeIn)
	{
		mpDrawer->DrawGfxObject(mpGfxBlackQuad,cVector3f(0,0,120),cVector2f(800,600), cColor(1,1-mfFadeAmount));
	}
}

//-----------------------------------------------------------------------


void cMainMenu::OnDraw()
{
	////////////////////////////////
	// Draw widgets
	tMainMenuWidgetListIt it = mlstWidgets.begin();
	for(; it != mlstWidgets.end(); ++it)
	{
		cMainMenuWidget* pWidget = *it;
		
		if(pWidget->IsActive()) pWidget->OnDraw();
	}

	DrawBackground();

	////////////////////////////
	//Draw tip
	if(msButtonTip != _W(""))
	{
		mpTipFont->DrawWordWrap(cVector3f(10,570,150),780,13,12,cColor(1,1),
								eFontAlign_Left,msButtonTip.c_str());
	}

	////////////////////////////////
	// Draw mouse
	if(mpCurrentActionText) return;
	cResourceImage *pImage = mpGfxMouse->GetMaterial()->GetImage(eMaterialTexture_Diffuse);
	cVector2l vSize = pImage->GetSize();
	cVector2f vPosAdd(((float)vSize.x) / 2.0f, ((float)vSize.y) / 2.0f);
	mpDrawer->DrawGfxObject(mpGfxMouse,cVector3f(0,0,100)+(mvMousePos - vPosAdd));
}

//-----------------------------------------------------------------------

static void DrawParticle(cGraphicsDrawer *apDrawer, cMainMenuParticle *apParticle)
{
	apDrawer->DrawGfxObject(apParticle->mpGfx,
							apParticle->mvPos- apParticle->mvSize/2,
							apParticle->mvSize,
							apParticle->mCol);
}

void cMainMenu::DrawBackground()
{
	if(mbGameActive)
	{
		for(size_t i=0; i < mvSnowFlakes.size(); ++i) DrawParticle(mpDrawer,&mvSnowFlakes[i]);
	}
	else
	{
		for(size_t i=0; i < mvRainDrops.size(); ++i) DrawParticle(mpDrawer,&mvRainDrops[i]);
		for(size_t i=0; i < mvRainSplashes.size(); ++i) DrawParticle(mpDrawer,&mvRainSplashes[i]);
	}
}

//-----------------------------------------------------------------------

void cMainMenu::Update(float afTimeStep)
{
	/* Network pump now runs from cNetworkUpdater in the GLOBAL updater state
	   (covers PreMenu/MainMenu/MapLoadText/Default) — pumping here as well
	   would tick the session twice per menu frame. */
	if (mbFadeIn)
	{
		if (mfFadeAmount < 1)
			mfFadeAmount += 0.5f * afTimeStep;
		else
		{
			mbFadeIn = false;
			mfFadeAmount = 0;
		}
	}
	
	if(mbUpdateWidgets)
	{
		mbUpdateWidgets = false;
		CreateWidgets();
		SetState(mState);
	}


	if(mpCurrentActionText)
	{
		cInput *pInput = mpInit->mpGame->GetInput();

		if(CheckForInput())
		{
			//Log("Creating action '%s'\n",msCurrentActionName.c_str());
			iAction *pAction = pInput->InputToAction(msCurrentActionName);
			
			mpCurrentActionText->msText = kTranslate("ButtonNames",pAction->GetInputName());
			
			//If translation is missing, set to default
			if(mpCurrentActionText->msText == _W(""))
				mpCurrentActionText->msText = cString::To16Char(pAction->GetInputName());

			mpCurrentActionText->UpdateSize();

			
			tString sAction = mpInit->mpButtonHandler->GetActionName(pAction->GetInputName(),msCurrentActionName);
			if(sAction != "")
			{
				pInput->DestroyAction(sAction);
				
				mpInit->mpMainMenu->ResetWidgets(eMainMenuState_OptionsKeySetupMove);
				mpInit->mpMainMenu->ResetWidgets(eMainMenuState_OptionsKeySetupAction);
				mpInit->mpMainMenu->ResetWidgets(eMainMenuState_OptionsKeySetupMisc);
			}

			mpCurrentActionText = NULL;
			Log("Reset check for input!\n");
		}
		
	}
	else
	{
#ifdef PENUMBRA_MULTIPLAYER
		{
			static eMainMenuState sMulPrevState = eMainMenuState_LastEnum;
			if (gpMulTypedIp != NULL && mState == eMainMenuState_MultiplayerJoin)
			{
				/* Focus on arrival: just TYPE the address, no click hunt.
				   (OnActivate cleared the focus when the screen opened.) */
				if (sMulPrevState != eMainMenuState_MultiplayerJoin)
					gpMulTypedIp->FocusTyping();
				gpMulTypedIp->PollTyping();
			}
			if (gpMulTypedName != NULL && mState == eMainMenuState_MultiplayerName)
			{
				/* v13 username screen: same focus-on-arrival; Enter saves */
				if (sMulPrevState != eMainMenuState_MultiplayerName)
					gpMulTypedName->FocusTyping();
				gpMulTypedName->PollTyping();
				if (gpMulTypedName->TakeEnter())
					MulSaveTypedName(mpInit);
			}
			if (gpMulTypedPw != NULL && mState == eMainMenuState_MultiplayerPassword)
			{
				/* password prompt: same focus-on-arrival; Enter joins */
				if (sMulPrevState != eMainMenuState_MultiplayerPassword)
					gpMulTypedPw->FocusTyping();
				gpMulTypedPw->PollTyping();
				if (gpMulTypedPw->TakeEnter())
					MulJoinServer(mpInit, gMulPendingServer, true, gpMulTypedPw->GetAscii());
			}
			sMulPrevState = mState;
		}

		if (gpMulNameShown != NULL && mpInit->mpNetworkManager != NULL &&
			mState == eMainMenuState_Multiplayer)
		{
			/* v13: reflects a rename the moment we come back from that screen */
			const tString sName = mpInit->mpNetworkManager->GetLocalPlayerName();
			const tWString wl = sName.empty()
				? tWString(_W("No username yet - friends see you as 'Player <id>'. Pick one under 'Change name'."))
				: (_W("Playing as: ") + cString::To16Char(sName));
			if (gpMulNameShown->msText != wl)
			{
				gpMulNameShown->msText = wl;
				gpMulNameShown->UpdateSize();
			}
		}

		/* Server browser: keep the page inside the list, mirror it into the
		   row slots, keyboard (Up/Down select, PgUp/PgDn page, Enter join,
		   F5 refresh) and the status line under the table. */
		if (mState == eMainMenuState_MultiplayerBrowser && mpInit->mpNetworkManager != NULL)
		{
			cNetworkManager *nm = mpInit->mpNetworkManager;
			const std::vector<cDiscoveredServer> &vList =
				gMulBrowserInternet ? nm->GetInternetServers() : nm->GetDiscoveredServers();
			gMulBrowserClock += afTimeStep;

			{
				iKeyboard *kb = mpInit->mpGame->GetInput()->GetKeyboard();
				for (int n = 0; n < 32 && kb->KeyIsPressed(); ++n)
				{
					const cKeyPress kp = kb->GetKey();
					const int lCount = (int)vList.size();
					int lSel = MulBrowserSelectedIndex(mpInit);
					if ((kp.mKey == eKey_UP || kp.mKey == eKey_DOWN) && lCount > 0)
					{
						if (lSel < 0)
							lSel = kp.mKey == eKey_DOWN ? gMulBrowserFirst : gMulBrowserFirst + kMulBrowserRows - 1;
						else
							lSel += kp.mKey == eKey_DOWN ? 1 : -1;
						if (lSel < 0)
							lSel = 0;
						if (lSel >= lCount)
							lSel = lCount - 1;
						gMulBrowserSelAddr = vList[(size_t)lSel].msAddress;
						gMulBrowserFirst = lSel - lSel % kMulBrowserRows; /* follow it onto its page */
					}
					else if (kp.mKey == eKey_PAGEUP || kp.mKey == eKey_PAGEDOWN)
					{
						gMulBrowserFirst += kp.mKey == eKey_PAGEDOWN ? kMulBrowserRows : -kMulBrowserRows;
						if (gMulBrowserFirst < 0)
							gMulBrowserFirst = 0;
					}
					else if ((kp.mKey == eKey_RETURN || kp.mKey == eKey_KP_ENTER) && lSel >= 0)
					{
						MulActivateServer(mpInit, vList[(size_t)lSel]);
						break; /* the screen may have changed */
					}
					else if (kp.mKey == eKey_F5)
					{
						gMulBrowserNoticeLeft = 0;
						MulRefreshBrowser(mpInit);
					}
				}
			}

			MulClampBrowserPage(vList.size());
			for (size_t r = 0; r < gvMulRows.size(); ++r)
				if (gvMulRows[r])
					gvMulRows[r]->SyncFromList(vList);

			const int lCount = (int)vList.size();
			tString sStatus;
			int lKind = 0;
			if (gMulBrowserNoticeLeft > 0)
			{
				gMulBrowserNoticeLeft -= afTimeStep;
				sStatus = gMulBrowserNotice;
				lKind = 3;
			}
			else if (gMulBrowserInternet && nm->IsInternetRefreshActive())
			{
				sStatus = "Searching the internet (master server " + nm->GetMasterServer() + ") ...";
				lKind = 1;
			}
			else if (!gMulBrowserInternet && nm->IsDiscoveryActive())
			{
				sStatus = "Searching the LAN (and Hamachi/Radmin) ...";
				lKind = 1;
			}
			else if (gMulBrowserInternet && lCount == 0 && !nm->GetInternetFailReason().empty())
			{
				sStatus = nm->GetInternetFailReason();
				lKind = 2;
			}
			else if (lCount == 0)
				sStatus = gMulBrowserInternet ? "No servers found - try LAN or Direct connect."
											  : "No servers found on the LAN - try Internet or Direct connect.";
			else
				sStatus = cString::ToString(lCount) + (lCount == 1 ? " server found" : " servers found") +
						  (gMulBrowserInternet ? tString("") : tString(" on the LAN")) +
						  (MulBrowserSelectedIndex(mpInit) >= 0 ? " - press Join (or double-click / Enter)."
																: " - click one, then Join (or double-click).");
			gMulBrowserStatus = cString::To16Char(sStatus);
			gMulBrowserStatusKind = lKind;
		}

		/* Host lobby: what the 'Public' toggle means right now. */
		if (gpMulHostPublicNote != NULL && mpInit->mpNetworkManager != NULL &&
			mState == eMainMenuState_MultiplayerHostLobby)
		{
			cNetworkManager *nm = mpInit->mpNetworkManager;
			const tString sPort = cString::ToString((int)nm->GetDefaultPort());
			const tString sMaster = nm->GetMasterServer();
			tString sNote;
			if (nm->IsPublic() && sMaster == kNetMasterDefaultHost)
				sNote = "public=1 but no master_server= in multiplayer.cfg - nobody can list this server.";
			else if (nm->IsPublic())
				sNote = "Listed on " + sMaster + ".  Forward UDP " + sPort +
						" on your router so internet players can connect.";
			else
				sNote = "Not listed. Friends join by IP; forward UDP " + sPort +
						" on your router for internet friends.";
			const tWString wsNote = cString::To16Char(sNote);
			if (gpMulHostPublicNote->msText != wsNote)
			{
				gpMulHostPublicNote->msText = wsNote;
				gpMulHostPublicNote->UpdateSize();
			}
		}

		if (gpMulHostFoot != NULL && mpInit->mpNetworkManager != NULL &&
			mState == eMainMenuState_MultiplayerHostLobby &&
			mpInit->mpNetworkManager->IsHosting())
		{
			tString lh = "Listening on UDP port ";
			lh += cString::ToString((int)mpInit->mpNetworkManager->GetDefaultPort());
			const int lFriends = mpInit->mpNetworkManager->GetConnectedGuestCount();
			/* "Players n/max" — the same n/max the server browser shows */
			lh += ".   Players " + cString::ToString(lFriends + 1) + "/" +
				cString::ToString((int)mpInit->mpNetworkManager->GetMaxPlayers());
			lh += lFriends == 1 ? " (1 friend connected)" :
				(" (" + cString::ToString(lFriends) + " friends connected)");
			const tWString wlh = cString::To16Char(lh);
			if (gpMulHostFoot->msText != wlh)
			{
				gpMulHostFoot->msText = wlh;
				gpMulHostFoot->UpdateSize();
			}
		}

		if (gpMulJoinFoot != NULL && mpInit->mpNetworkManager != NULL &&
			mState == eMainMenuState_MultiplayerJoin)
		{
			cNetworkManager *nm = mpInit->mpNetworkManager;
			const tWString kConnected = _W("Connected - you will enter the game when the host launches.");
			const tWString kConnecting = _W("Connecting... (waiting for handshake)");
			if (!nm->GetJoinFailReason().empty())
			{
				/* a refused join beats every other status on this screen */
				const tWString sFail = cString::To16Char(nm->GetJoinFailReason());
				if (gpMulJoinFoot->msText != sFail)
				{
					gpMulJoinFoot->msText = sFail;
					gpMulJoinFoot->UpdateSize();
				}
			}
			else if (nm->IsClientSynced())
			{
				if (gpMulJoinFoot->msText != kConnected)
				{
					gpMulJoinFoot->msText = kConnected;
					gpMulJoinFoot->UpdateSize();
				}
			}
			else if (gMulJoinAwaitHandshake)
			{
				if (gpMulJoinFoot->msText != kConnecting)
				{
					gpMulJoinFoot->msText = kConnecting;
					gpMulJoinFoot->UpdateSize();
				}
			}
		}
#endif
	}

	////////////////////////////////
	// Update effect
	if(mbGameActive)
	{
		for(size_t i=0; i< mvSnowFlakes.size(); ++i)
		{
			cMainMenuParticle &flake = mvSnowFlakes[i];
			
			if(flake.mvPos.y >= 600 - (150 - mvSnowFlakes[i].mvSize.x*15))
			{
				mvSnowFlakes[i].mvPos = cVector3f(cMath::RandRectf(350,800),200,20);
				mvSnowFlakes[i].mvVel = cVector3f(0,cMath::RandRectf(15,40),0);
				mvSnowFlakes[i].mvSize = cMath::RandRectf(2,10);
			}

			flake.mvPos += flake.mvVel*afTimeStep;
			flake.mvVel.x += cMath::RandRectf(-2, 2);
			if(flake.mvVel.x <	-25) flake.mvVel.x = -25;
			if(flake.mvVel.x >	25) flake.mvVel.x = 25;

			float fDist = cMath::Vector3Dist(flake.mvPos,cVector3f(550,550,20));
			float fAlpha = 1 - fDist/380;
			if(fAlpha <0)fAlpha =0;
			flake.mCol = cColor(1,fAlpha);
		}
	}
	else
	{
		//Rainsplashes
		for(size_t i=0; i < mvRainSplashes.size(); ++i)
		{
			cMainMenuParticle &splash = mvRainSplashes[i];
			if(splash.mCol.a <= 0)
			{
				splash.mCol.a =1;
				
				float fX,fY;

				if(i<120)
				{
					fX = cMath::RandRectf(400, 700);
					fY = cMath::RandRectf(500, 550);
				}
				else if(i<140)
				{
					fX = cMath::RandRectf(480, 605);
					fY = cMath::RandRectf(278, 320);
				}
				else if(i<160)
				{
					fX = cMath::RandRectf(360, 460);
					fY = cMath::RandRectf(288, 330);
				}
				else
				{
					fX = cMath::RandRectf(615, 760);
					fY = cMath::RandRectf(258, 310);
				}
				
				splash.mvPos = cVector3f(fX,fY,20);
				splash.mvSize = cMath::RandRectf(3,16);
				
				splash.mpGfx = mpGfxRainSplash;

				splash.mvVel.x = cMath::RandRectf(1,5);
			}
			else
			{
				splash.mCol.a -= afTimeStep * splash.mvVel.x;
				if(splash.mCol.a <0)splash.mCol.a =0;
			}
		}

		//Rain drops
		if(mfRainDropCount<=0)
		{
			for(size_t i=0; i < mvRainDrops.size(); ++i)
			{
				float fX = cMath::RandRectf(150, 800);
				float fY = cMath::RandRectf(180, 600);
				mvRainDrops[i].mvPos = cVector3f(fX,fY,20);
				mvRainDrops[i].mvSize = cMath::RandRectf(28,34);
				
				float fDist = cMath::Vector3Dist(mvRainDrops[i].mvPos,cVector3f(550,400,20));
				float fAlpha = 1 - fDist/380;
				if(fAlpha <0)fAlpha =0;
				mvRainDrops[i].mCol = cColor(1,fAlpha);

				mvRainDrops[i].mpGfx = mpGfxRainDrop;
			}

			mfRainDropCount = 1.0f/37.0f;
		}
		else
		{
			mfRainDropCount -= afTimeStep;
		}
	}


	////////////////////////////////
	// Update buttons
	msButtonTip = _W("");
	tMainMenuWidgetListIt it = mlstWidgets.begin();
	for(; it != mlstWidgets.end(); ++it)
	{
		cMainMenuWidget *pWidget = *it;

		if(pWidget->IsActive())pWidget->OnUpdate(afTimeStep);

		if(cMath::PointBoxCollision(mvMousePos,pWidget->GetRect()))
		{
			if(pWidget->IsActive())pWidget->OnMouseOver(true);
		}
		else
		{
			if(pWidget->IsActive())pWidget->OnMouseOver(false);
		}
	}
}

//-----------------------------------------------------------------------

void cMainMenu::AddMousePos(const cVector2f &avRel)
{
	if(mpCurrentActionText) return;

	mvMousePos += avRel;

	if(mvMousePos.x < 0) mvMousePos.x =0;
	if(mvMousePos.x >= 800) mvMousePos.x =800;
	if(mvMousePos.y < 0) mvMousePos.y =0;
	if(mvMousePos.y >= 600) mvMousePos.y =600;
}

void cMainMenu::SetMousePos(const cVector2f &avPos)
{
	if(mpCurrentActionText) return;

	mvMousePos = avPos;

}

//-----------------------------------------------------------------------

void cMainMenu::OnMouseDown(eMButton aButton)
{
#ifdef PENUMBRA_MULTIPLAYER
	if (gpMulTypedIp && mState == eMainMenuState_MultiplayerJoin &&
		gpMulTypedIp->IsTypingFocused() &&
		!cMath::PointBoxCollision(mvMousePos, gpMulTypedIp->GetRect()))
	{
		gpMulTypedIp->BlurTyping();
	}
	if (gpMulTypedName && mState == eMainMenuState_MultiplayerName &&
		gpMulTypedName->IsTypingFocused() &&
		!cMath::PointBoxCollision(mvMousePos, gpMulTypedName->GetRect()))
	{
		gpMulTypedName->BlurTyping();
	}
	if (gpMulTypedPw && mState == eMainMenuState_MultiplayerPassword &&
		gpMulTypedPw->IsTypingFocused() &&
		!cMath::PointBoxCollision(mvMousePos, gpMulTypedPw->GetRect()))
	{
		gpMulTypedPw->BlurTyping();
	}
#endif

	if(mpCurrentActionText) return;

	////////////////////////////////
	// Update buttons
	tMainMenuWidgetListIt it = mlstWidgets.begin();
	for(; it != mlstWidgets.end(); ++it)
	{
		cMainMenuWidget *pWidget = *it;

		if(cMath::PointBoxCollision(mvMousePos,pWidget->GetRect()))
		{
			if(pWidget->IsActive())
			{
				pWidget->OnMouseDown(aButton);
				break;
			}
		}
	}

	mbMouseIsDown = true;
}

void cMainMenu::OnMouseUp(eMButton aButton)
{
	if(mpCurrentActionText) return;

	////////////////////////////////
	// Update buttons
	tMainMenuWidgetListIt it = mlstWidgets.begin();
	for(; it != mlstWidgets.end(); ++it)
	{
		cMainMenuWidget *pWidget = *it;

		if(cMath::PointBoxCollision(mvMousePos,pWidget->GetRect()))
		{
			if(pWidget->IsActive()) pWidget->OnMouseUp(aButton);
		}
	}

	mbMouseIsDown = false;
}

//-----------------------------------------------------------------------

void cMainMenu::OnMouseDoubleClick(eMButton aButton)
{
	if(mpCurrentActionText) return;

	////////////////////////////////
	// Update buttons
	tMainMenuWidgetListIt it = mlstWidgets.begin();
	for(; it != mlstWidgets.end(); ++it)
	{
		cMainMenuWidget *pWidget = *it;

		if(cMath::PointBoxCollision(mvMousePos,pWidget->GetRect()))
		{
			if(pWidget->IsActive()) pWidget->OnDoubleClick(aButton);
		}
	}

	mbMouseIsDown = false;
}

//-----------------------------------------------------------------------

void cMainMenu::SetActive(bool abX)
{
	if(mbActive == abX) return;

	
	mbActive = abX;

	if(mbActive)
	{
		if(mpInit->mbHasHaptics)
			mpInit->mpPlayer->GetHapticCamera()->SetActive(false);
		
		if (!mpInit->mbFullScreen) {
			mpInit->mpGame->GetInput()->GetLowLevel()->LockInput(false);
		}

		mpInit->mpGame->GetUpdater()->SetContainer("MainMenu");
		mpInit->mpGame->GetScene()->SetDrawScene(false);
		mpInit->mpGame->GetScene()->SetUpdateMap(false);
		if(mpInit->mbHasHaptics)
		{
			mpInit->mpGame->GetHaptic()->GetLowLevel()->StopAllForces();
			mpInit->mpGame->GetHaptic()->GetLowLevel()->SetUpdateShapes(false);
		}
		
		mpInit->mpButtonHandler->ChangeState(eButtonHandlerState_MainMenu);
				
		///////////////////////////////
		//Init menu
		CreateWidgets();
		cSoundHandler *pSoundHandler =mpInit->mpGame->GetSound()->GetSoundHandler();
		
		if(mpInit->mpMapHandler->GetCurrentMapName() != "")
		{
			mpInit->mpGame->GetSound()->GetSoundHandler()->PauseAll(eSoundDest_World | eSoundDest_Gui);
			mpInit->mpGame->GetSound()->GetMusicHandler()->Pause();

			mbGameActive = true;
			pSoundHandler->PlayGui("gui_wind1",true,1);
		}
		else
		{
			mpInit->mpGame->GetSound()->GetMusicHandler()->Play("music_theme.ogg",1,5.0f,false);
			
			if(pSoundHandler->IsPlaying("gui_rain1")==false)
				pSoundHandler->PlayGui("gui_rain1",true,1);
			
			mbGameActive = false;
			mbFadeIn = true;
		}
		
		bool bFirstStart = mpInit->mpConfig->GetBool("Game","FirstStart",true);
		if(bFirstStart)
		{
			SetState(eMainMenuState_FirstStart);
			mLastState = eMainMenuState_FirstStart;

			mpInit->mpConfig->SetBool("Game","FirstStart",false);
		}
		else
		{
			SetState(eMainMenuState_Start);
			mLastState = eMainMenuState_Start;
		}

		gbMustRestart=false;

		mpCurrentActionText = NULL;

		mpLogo = mpInit->mpGame->GetResources()->GetTextureManager()->Create2D("menu_logo.jpg",false);
		
		if(mbGameActive)
			mpBackground = mpInit->mpGame->GetResources()->GetTextureManager()->Create2D("menu_background_ingame.jpg",false);
		else
			mpBackground = mpInit->mpGame->GetResources()->GetTextureManager()->Create2D("menu_background.jpg",false);

	}
	else
	{
		if (!mpInit->mbFullScreen) {
			mpInit->mpGame->GetInput()->GetLowLevel()->LockInput(true);
		}

		if(mpInit->mbHasHaptics)
			mpInit->mpPlayer->GetHapticCamera()->SetActive(true);

		cSoundHandler *pSoundHandler =mpInit->mpGame->GetSound()->GetSoundHandler();
		
		if(mpInit->mpMapHandler->GetCurrentMapName() != "")
		{
			if (pSoundHandler->IsPlaying("gui_wind1"))
				pSoundHandler->Stop("gui_wind1");

			pSoundHandler->ResumeAll(eSoundDest_World | eSoundDest_Gui);
			mpInit->mpGame->GetSound()->GetMusicHandler()->Resume();
		}
		else
		{
			if (pSoundHandler->IsPlaying("gui_rain1"))
				pSoundHandler->Stop("gui_rain1");
			mpInit->mpGame->GetSound()->GetMusicHandler()->Stop(0.3f);
		}

		mpInit->mpGame->GetUpdater()->SetContainer("Default");
		mpInit->mpGame->GetScene()->SetDrawScene(true);
		mpInit->mpGame->GetScene()->SetUpdateMap(true);
		if(mpInit->mbHasHaptics)
			mpInit->mpGame->GetHaptic()->GetLowLevel()->SetUpdateShapes(true);
		mpInit->mpButtonHandler->ChangeState(eButtonHandlerState_Game);

		if(mpLogo) mpInit->mpGame->GetResources()->GetTextureManager()->Destroy(mpLogo);
		mpLogo = NULL;
		if(mpBackground) mpInit->mpGame->GetResources()->GetTextureManager()->Destroy(mpBackground);
		mpBackground = NULL;
	}
}
//-----------------------------------------------------------------------

void cMainMenu::Exit()
{
	if(mpCurrentActionText)
	{
		mpCurrentActionText = NULL;
	}
	else if(mState==eMainMenuState_Start && mpInit->mpMapHandler->GetCurrentMapName() != "")
	{
		SetActive(false);
	}
	else if((	mState==eMainMenuState_OptionsGraphics || 
				mState==eMainMenuState_OptionsSound ||
				mState==eMainMenuState_OptionsGame)
			&& gbMustRestart)
	{
		SetState(eMainMenuState_GraphicsRestart);

		mpInit->mpGame->GetSound()->GetSoundHandler()->PlayGui("gui_menu_click",false,1);

		gbMustRestart =false;
	}
	else
	{
		SetState(gvMenuBackStates[mState]);
		
		mpInit->mpGame->GetSound()->GetSoundHandler()->PlayGui("gui_menu_click",false,1);
	}
}

//-----------------------------------------------------------------------

void cMainMenu::OnExit()
{
	SetActive(false);
}

//-----------------------------------------------------------------------

void cMainMenu::SetState(eMainMenuState aState)
{
	mLastState = mState;

	mState = aState;
		
	//Set all widgets as not active.
	tMainMenuWidgetListIt it = mlstWidgets.begin();
	for(; it != mlstWidgets.end(); ++it)
	{
		cMainMenuWidget *pWidget = *it;
		pWidget->SetActive(false);
	}
		
	//Set all widgets for current state as active.
	it = mvState[aState].begin();
	for(; it != mvState[aState].end(); ++it)
	{
		cMainMenuWidget *pWidget = *it;
		pWidget->SetActive(true);
	}
}

//-----------------------------------------------------------------------

void cMainMenu::SetInputToAction(const tString &asActionName,cMainMenuWidget_Text *apText)
{
	msCurrentActionName = asActionName;
	mpCurrentActionText = apText;
	InitCheckInput();
}

//-----------------------------------------------------------------------

void cMainMenu::InitCheckInput()
{
	cInput *pInput = mpInit->mpGame->GetInput();

	for(int i=0; i<eKey_LastEnum;++i) 
	{
		mvKeyPressed[i] = pInput->GetKeyboard()->KeyIsDown((eKey)i);
	}

	for(int i=0; i<eMButton_LastEnum; ++i)
	{
		mvMousePressed[i] = pInput->GetMouse()->ButtonIsDown((eMButton)i);
	}
}

//-----------------------------------------------------------------------

bool cMainMenu::CheckForInput()
{
	cInput *pInput = mpInit->mpGame->GetInput();

	////////////////////
	//Keyboard
	for(int i=0; i<eKey_LastEnum;++i) 
	{
		if(pInput->GetKeyboard()->KeyIsDown((eKey)i))
		{
			if(mvKeyPressed[i]==false) return true;
		}
		else
		{
			mvKeyPressed[i] = false;
		}
	}
	
	////////////////////
	//Mouse
	for(int i=0; i<eMButton_LastEnum; ++i)
	{
		if(pInput->GetMouse()->ButtonIsDown((eMButton)i))
		{
			if(mvMousePressed[i]==false) return true;
		}
		else
		{
			mvMousePressed[i] = false;
		}
	}

	return false;
}

//-----------------------------------------------------------------------

void cMainMenu::ResetWidgets(eMainMenuState aState)
{
	tMainMenuWidgetListIt it = mvState[aState].begin();
	for(; it != mvState[aState].end(); ++it)
	{
		cMainMenuWidget *pWidget = *it;
		pWidget->Reset();
	}
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// PRIVATE METHODS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

class cTempFileAndData
{
public:
	cTempFileAndData(const tWString& asFile, const cDate& aDate)
	{
		msFile = asFile;
		mDate = aDate;
	}
	
	bool operator<(const cTempFileAndData &aA) const
	{
		return mDate < aA.mDate;
	}
	bool operator>(const cTempFileAndData &aA) const
	{
		return mDate > aA.mDate;
	}
	bool operator==(const cTempFileAndData &aA) const
	{
		return mDate == aA.mDate;
	}

	tWString msFile;
	cDate mDate;
};

typedef std::multiset<cTempFileAndData,std::greater<cTempFileAndData> > tTempFileAndDataSet;
typedef tTempFileAndDataSet::iterator tTempFileAndDataSetIt;


//-----------------------------------------------------------------------

void cMainMenu::CreateWidgets()
{
	cVector3f vPos;
	tWString sText;
	cMainMenuWidget_Text *pTempTextWidget;
	char sTempVec[256];


	cVector3f vTextStart(220, 230, 40);


	///////////////////////////////
	//Erase all previous
#ifdef PENUMBRA_MULTIPLAYER
	gpMulJoinFoot = NULL;
	gpMulHostFoot = NULL;
	gpMulTypedIp = NULL;
	gMulJoinAwaitHandshake = false;
	gpMulTypedName = NULL;
	gpMulNameFoot = NULL;
	gpMulNameShown = NULL;
	gvMulRows.clear();
	gpMulTypedPw = NULL;
	gpMulPublicToggle = NULL;
	gpMulHostPublicNote = NULL;
	gMulBrowserNoticeLeft = 0;
	gMulBrowserFirst = 0;
	gMulBrowserLastClick = -10;
#endif
	STLDeleteAll(mlstWidgets);
	for(size_t i=0; i< eMainMenuState_LastEnum; ++i) mvState[i].clear();

	///////////////////////////////
	// First start
	//////////////////////////////
	bool bFirstStart = mpInit->mpConfig->GetBool("Game","FirstStart",true);
	if(bFirstStart)
	{
		vPos = cVector3f(40, 190, 40);
		AddWidgetToState(eMainMenuState_FirstStart,hplNew( cMainMenuWidget_Text,(mpInit,vPos,kTranslate("MainMenu", "Welcome"),15,eFontAlign_Left)) ); 
		vPos.y += 18;
		AddWidgetToState(eMainMenuState_FirstStart,hplNew( cMainMenuWidget_Text,(mpInit,vPos,kTranslate("MainMenu", "Too Improve"),15,eFontAlign_Left)) ); 
		vPos.y += 28;
		AddWidgetToState(eMainMenuState_FirstStart,hplNew( cMainMenuWidget_Text,(mpInit,vPos,kTranslate("MainMenu", "StartTip1"),15,eFontAlign_Left)) ); 
		vPos.y += 18;
		AddWidgetToState(eMainMenuState_FirstStart,hplNew( cMainMenuWidget_Text,(mpInit,vPos,kTranslate("MainMenu", "StartTip2"),15,eFontAlign_Left)) ); 
		vPos.y += 18;
		AddWidgetToState(eMainMenuState_FirstStart,hplNew( cMainMenuWidget_Text,(mpInit,vPos,kTranslate("MainMenu", "StartTip3"),15,eFontAlign_Left)) ); 
		vPos.y += 28;
		vPos.x = 395;
		cMainMenuWidget *pGammaFirstButton = hplNew( cMainMenuWidget_Gamma,(mpInit,vPos,kTranslate("MainMenu","Gamma:"),20,eFontAlign_Right,1) );
		AddWidgetToState(eMainMenuState_FirstStart,pGammaFirstButton); 
		vPos.x = 405;
		sprintf(sTempVec,"%.1f",mpInit->mpGame->GetGraphics()->GetLowLevel()->GetGammaCorrection());
		sText = cString::To16Char(sTempVec);
		gpGammaText2 = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left) );
		AddWidgetToState(eMainMenuState_FirstStart,gpGammaText2); 
		gpGammaText2->SetExtraWidget(pGammaFirstButton);

		vPos.y += 31;
		AddWidgetToState(eMainMenuState_FirstStart,hplNew( cMainMenuWidget_Image,(mpInit,
																			cVector3f(250,vPos.y,30),
																			cVector2f(300,200),
																			"menu_gamma.bmp",
																			"diffalpha2d",
																			cColor(1,1))) );
		vPos.y+=205;
		//AddWidgetToState(eMainMenuState_FirstStart,hplNew( cMainMenuWidget_Text(mpInit,vPos,kTranslate("MainMenu", "StartTip4"),15,eFontAlign_Left)); 
		//vPos.y += 28;
		AddWidgetToState(eMainMenuState_FirstStart,hplNew( cMainMenuWidget_Button,(mpInit,vPos,kTranslate("MainMenu","OK"),eMainMenuState_Start,20,eFontAlign_Center)) );
	}
	


	///////////////////////////////
	//Start menu:
	//////////////////////////////
	vPos = vTextStart;//cVector3f(400, 260, 40);

	if(mpInit->mpMapHandler->GetCurrentMapName() != "")
	{
		AddWidgetToState(eMainMenuState_Start,hplNew( cMainMenuWidget_Resume,(mpInit,vPos,kTranslate("MainMenu","Resume"))) );
		vPos.y += 60;
	}
	else
	{
		tWString sAuto = mpInit->mpSaveHandler->GetLatest(_W("save/auto/"),_W("*.sav"));
		tWString sSpot = mpInit->mpSaveHandler->GetLatest(_W("save/spot/"),_W("*.sav"));

		if(sAuto != _W("") || sSpot != _W(""))
		{
			AddWidgetToState(eMainMenuState_Start,hplNew( cMainMenuWidget_MainButton,(mpInit,vPos,kTranslate("MainMenu","Continue"),eMainMenuState_Continue)) ); 
			vPos.y += 51;
		}
	}

	AddWidgetToState(eMainMenuState_Start,hplNew( cMainMenuWidget_MainButton,(mpInit,vPos,kTranslate("MainMenu","New Game"),eMainMenuState_NewGame)) ); 
	vPos.y += 51;
	AddWidgetToState(eMainMenuState_Start,hplNew( cMainMenuWidget_MainButton,(mpInit,vPos,kTranslate("MainMenu","Load Game"),eMainMenuState_LoadGameSpot)) ); 
	vPos.y += 51;
#ifdef PENUMBRA_MULTIPLAYER
	/* v13: asks for a username first when multiplayer.cfg has none */
	AddWidgetToState(eMainMenuState_Start,hplNew(
		cMainMenuWidget_MultiEnter,(mpInit, vPos, _W("Multiplayer"))) );
	vPos.y += 51;
#endif
	AddWidgetToState(eMainMenuState_Start,hplNew( cMainMenuWidget_MainButton,(mpInit,vPos,kTranslate("MainMenu","Options"),eMainMenuState_Options)) ); 
	vPos.y += 51;
	AddWidgetToState(eMainMenuState_Start,hplNew( cMainMenuWidget_MainButton,(mpInit,vPos,kTranslate("MainMenu","Exit"),eMainMenuState_Exit)) );
	
	
	///////////////////////////////////
	// New Game
	///////////////////////////////////

	vPos = vTextStart;//cVector3f(400, 260, 40);
	//AddWidgetToState(eMainMenuState_NewGame,hplNew( cMainMenuWidget_Text(mpInit,vPos,kTranslate("MainMenu","StartNewGame"),24,eFontAlign_Center)); 
	//vPos.y += 34;
	AddWidgetToState(eMainMenuState_NewGame,hplNew( cMainMenuWidget_NewGame,(mpInit,vPos,kTranslate("MainMenu","Easy"),24,eFontAlign_Center,eGameDifficulty_Easy)) ); 
	vPos.y += 30;
	AddWidgetToState(eMainMenuState_NewGame,hplNew( cMainMenuWidget_Text,(mpInit,vPos,kTranslate("MainMenu","EasyDesc"),16,eFontAlign_Center)) ); 
	vPos.y += 42;
	AddWidgetToState(eMainMenuState_NewGame,hplNew( cMainMenuWidget_NewGame,(mpInit,vPos,kTranslate("MainMenu","Normal"),24,eFontAlign_Center,eGameDifficulty_Normal)) ); 
	vPos.y += 30;
	AddWidgetToState(eMainMenuState_NewGame,hplNew( cMainMenuWidget_Text,(mpInit,vPos,kTranslate("MainMenu","NormalDesc"),16,eFontAlign_Center)) ); 
	vPos.y += 42;
	AddWidgetToState(eMainMenuState_NewGame,hplNew( cMainMenuWidget_NewGame,(mpInit,vPos,kTranslate("MainMenu","Hard"),24,eFontAlign_Center,eGameDifficulty_Hard)) ); 
	vPos.y += 30;
	AddWidgetToState(eMainMenuState_NewGame,hplNew( cMainMenuWidget_Text,(mpInit,vPos,kTranslate("MainMenu","HardDesc"),16,eFontAlign_Center)) ); 
	vPos.y +=46;
	AddWidgetToState(eMainMenuState_NewGame,hplNew( cMainMenuWidget_Button,(mpInit,vPos,kTranslate("MainMenu","Back"),eMainMenuState_Start,22,eFontAlign_Center)) );

	///////////////////////////////////
	// Continue
	///////////////////////////////////
	vPos = vTextStart;//cVector3f(400, 260, 40);
	AddWidgetToState(eMainMenuState_Continue,hplNew( cMainMenuWidget_Text,(mpInit,vPos,kTranslate("MainMenu","ContinueLastSave"),20,eFontAlign_Center)) ); 
	vPos.y += 34;
	AddWidgetToState(eMainMenuState_Continue,hplNew( cMainMenuWidget_Continue,(mpInit,vPos,kTranslate("MainMenu","Yes"),20,eFontAlign_Center)) ); 
	vPos.y += 29;
	AddWidgetToState(eMainMenuState_Continue,hplNew( cMainMenuWidget_Button,(mpInit,vPos,kTranslate("MainMenu","No"),eMainMenuState_Start,20,eFontAlign_Center)) );

#ifdef PENUMBRA_MULTIPLAYER
	{
		const uint16_t defPort = mpInit->mpNetworkManager->GetDefaultPort();
		char portBuf[24];
		sprintf(portBuf, "%u", (unsigned)defPort);
		const tString lastHost = MulTrimAscii(mpInit->mpConfig->GetString("Multiplayer", "LastJoinHost", "127.0.0.1"));

		///////////////////////////////////
		vPos = vTextStart;
		AddWidgetToState(eMainMenuState_Multiplayer,
						 hplNew(cMainMenuWidget_Text,
								(mpInit, vPos,
								 _W("Multiplayer - co-op mod (listen server)."), 22,
								 eFontAlign_Center)));
		vPos.y += 36;
		AddWidgetToState(eMainMenuState_Multiplayer,
						 hplNew(cMainMenuWidget_Text,
								(mpInit, vPos,
								 _W(
									 "Hosts listen on UDP; friends join via your IP/host and the same port. Windows Firewall may prompt once."),
								 13, eFontAlign_Center)));
		vPos.y += 20;
		AddWidgetToState(eMainMenuState_Multiplayer,
						 hplNew(cMainMenuWidget_Text,
								(mpInit, vPos,
								 _W("A Quasi Interactive mod  -  quasi-interactive.com"),
								 13, eFontAlign_Center)));

		/* five buttons + name line + character line + hint + Back must fit
		   above y=600: 38 px pitch */
		vPos.y += 30;
		AddWidgetToState(eMainMenuState_Multiplayer,
						 hplNew(cMainMenuWidget_MultiHostStartListen,
								(mpInit, vPos, _W("Host / listen"))));
		vPos.y += 38;
		AddWidgetToState(
			eMainMenuState_Multiplayer,
			hplNew(cMainMenuWidget_MultiOpenBrowser,(mpInit, vPos, _W("Server browser"))));
		vPos.y += 38;
		AddWidgetToState(
			eMainMenuState_Multiplayer,
			hplNew(cMainMenuWidget_MainButton,(mpInit, vPos, _W("Direct connect"), eMainMenuState_MultiplayerJoin)));
		vPos.y += 38;
		/* v13: username (multiplayer.cfg player_name) — shown to the party */
		AddWidgetToState(
			eMainMenuState_Multiplayer,
			hplNew(cMainMenuWidget_MainButton,(mpInit, vPos, _W("Change name"), eMainMenuState_MultiplayerName)));
		vPos.y += 38;
		gpMulNameShown = hplNew(cMainMenuWidget_Text,(mpInit, vPos, _W(""), 13, eFontAlign_Center));
		AddWidgetToState(eMainMenuState_Multiplayer, gpMulNameShown); /* text set in Update */
		vPos.y += 22;
		/* v18: 'Character: [<] Red [>]' (multiplayer.cfg character=) */
		AddWidgetToState(eMainMenuState_Multiplayer, hplNew(cMainMenuWidget_MultiCharacter,(mpInit, vPos, false)));
		vPos.y += 24;

		sprintf(sTempVec, "F11 toggles hosting | F10 joins 127.0.0.1:%s | Port %s", portBuf, portBuf);
		AddWidgetToState(eMainMenuState_Multiplayer,
						 hplNew(cMainMenuWidget_Text,
								(mpInit, vPos, cString::To16Char(sTempVec), 13, eFontAlign_Center)));
		vPos.y += 36;
		AddWidgetToState(
			eMainMenuState_Multiplayer,
			hplNew(cMainMenuWidget_MainButton,(mpInit, vPos, kTranslate("MainMenu", "Back"),
												eMainMenuState_Start)));

		///////////////////////////////////
		// Host lobby
		///////////////////////////////////
		vPos = vTextStart;
		AddWidgetToState(eMainMenuState_MultiplayerHostLobby,
						 hplNew(cMainMenuWidget_Text,
								(mpInit, vPos, _W("Hosting session"), 24, eFontAlign_Center)));
		vPos.y += 42;
		{
			tString lh = "Listening on UDP port ";
			lh += cString::ToString((int)defPort);
			lh += ". Tell your friends HOST:";
			lh += portBuf;
			gpMulHostFoot = hplNew(cMainMenuWidget_Text,(mpInit, vPos, cString::To16Char(lh), 14,
														 eFontAlign_Center));
			AddWidgetToState(eMainMenuState_MultiplayerHostLobby, gpMulHostFoot);
		}
		vPos.y += 26;
		{
			/* The address the friend must type — read it off this screen.
			   Radmin/Hamachi addresses are the ones that work across the
			   internet; plain LAN ones only on the same network. */
			std::vector<tString> vAddrLines;
			mpInit->mpNetworkManager->GetLocalAddressLines(vAddrLines);
			tString sAddrs = "Your addresses:   ";
			if (vAddrLines.empty())
				sAddrs += "(no network adapter found)";
			for (size_t a = 0; a < vAddrLines.size(); ++a)
			{
				if (a > 0)
					sAddrs += "    ";
				sAddrs += vAddrLines[a];
			}
			AddWidgetToState(eMainMenuState_MultiplayerHostLobby,
							 hplNew(cMainMenuWidget_Text,
									(mpInit, vPos, cString::To16Char(sAddrs), 14,
									 eFontAlign_Center)));
			vPos.y += 24;
			AddWidgetToState(eMainMenuState_MultiplayerHostLobby,
							 hplNew(cMainMenuWidget_Text,
									(mpInit, vPos,
									 _W("Give a friend the Radmin/Hamachi one; they type it under 'Direct connect'."),
									 13, eFontAlign_Center)));
			vPos.y += 30;
			/* 'Public' toggle: multiplayer.cfg `public=` pins it; otherwise
			   the last menu choice is remembered in the game config. */
			if (!mpInit->mpNetworkManager->IsPublicFromCfg())
				mpInit->mpNetworkManager->SetPublic(
					mpInit->mpConfig->GetBool("Multiplayer", "Public", mpInit->mpNetworkManager->IsPublic()));
			gpMulPublicToggle = hplNew(cMainMenuWidget_MultiPublicToggle,(mpInit, vPos));
			AddWidgetToState(eMainMenuState_MultiplayerHostLobby, gpMulPublicToggle);
			vPos.y += 26;
			gpMulHostPublicNote = hplNew(cMainMenuWidget_Text,(mpInit, vPos, _W(""), 13, eFontAlign_Center));
			AddWidgetToState(eMainMenuState_MultiplayerHostLobby, gpMulHostPublicNote);
		}
		vPos.y += 26;
		/* v18: 'You play Philip' — the host is slot 0, no picker */
		AddWidgetToState(eMainMenuState_MultiplayerHostLobby,
						 hplNew(cMainMenuWidget_MultiCharacter,(mpInit, vPos, false)));
		vPos.y += 30;
		AddWidgetToState(
			eMainMenuState_MultiplayerHostLobby,
			hplNew(cMainMenuWidget_MultiLaunchPlaying,(mpInit, vPos,
													   _W("Launch new game"),
													   true, false, eGameDifficulty_Normal)));
		vPos.y += 40;
		/* Same screens the single-player Load Game button opens; hosting stays
		   live behind them, and when the save's world comes up every connected
		   friend is launched into that map automatically (the map beacon). */
		AddWidgetToState(
			eMainMenuState_MultiplayerHostLobby,
			hplNew(cMainMenuWidget_Button,(mpInit, vPos,
										   _W("Load a save (friends follow you into it)"),
										   eMainMenuState_LoadGameSpot, 24, eFontAlign_Center)));
		vPos.y += 52;
		AddWidgetToState(
			eMainMenuState_MultiplayerHostLobby,
			hplNew(cMainMenuWidget_MultiLobbyBack,(mpInit, vPos, _W("Stop hosting / back"))));

		///////////////////////////////////
		// Join
		///////////////////////////////////
		vPos = vTextStart;
		AddWidgetToState(eMainMenuState_MultiplayerJoin,
						 hplNew(cMainMenuWidget_Text,
								(mpInit, vPos, _W("Join host (direct UDP)"), 24, eFontAlign_Center)));
		vPos.y += 42;
		AddWidgetToState(eMainMenuState_MultiplayerJoin,
						 hplNew(cMainMenuWidget_Text,
								(mpInit, vPos,
								 _W("Type the host's IP (their Radmin/Hamachi address works), then press Connect. Add :PORT if not 7777."),
								 13, eFontAlign_Center)));
		vPos.y += 52;
		gpMulTypedIp =
			hplNew(cMainMenuWidget_MultiIpLine,(mpInit, vPos, lastHost, 20, eFontAlign_Center));
		AddWidgetToState(eMainMenuState_MultiplayerJoin, gpMulTypedIp);
		vPos.y += 46;
		gpMulJoinFoot = hplNew(cMainMenuWidget_Text,(mpInit, vPos, _W("(not connected yet)"), 14,
													  eFontAlign_Center));
		AddWidgetToState(eMainMenuState_MultiplayerJoin, gpMulJoinFoot);
		vPos.y += 32;
		/* v18: pick while waiting for the host to launch (asks at once when
		   connected, else only saves character= for the next join); the
		   chips under the arrows show every character and which are taken */
		AddWidgetToState(eMainMenuState_MultiplayerJoin,
						 hplNew(cMainMenuWidget_MultiCharacter,(mpInit, vPos, true, 420.0f)));
		vPos.y += 62;
		AddWidgetToState(
			eMainMenuState_MultiplayerJoin,
			hplNew(cMainMenuWidget_MultiJoinTry,(mpInit, vPos, _W("Connect"))));
		vPos.y += 40;
		AddWidgetToState(
			eMainMenuState_MultiplayerJoin,
			hplNew(cMainMenuWidget_MultiLaunchPlaying,(mpInit, vPos, _W("Enter game manually (fallback)"),
													   false, true, eGameDifficulty_Normal)));
		vPos.y += 44;
		AddWidgetToState(
			eMainMenuState_MultiplayerJoin,
			hplNew(cMainMenuWidget_MultiLobbyBack,(mpInit, vPos, _W("Cancel"))));

		///////////////////////////////////
		// Username (v13) — reached from the Start screen's Multiplayer
		// button while no name is set, and from "Change name".
		///////////////////////////////////
		vPos = vTextStart;
		AddWidgetToState(eMainMenuState_MultiplayerName,
						 hplNew(cMainMenuWidget_Text,
								(mpInit, vPos, _W("What's your username?"), 24, eFontAlign_Center)));
		vPos.y += 42;
		AddWidgetToState(eMainMenuState_MultiplayerName,
						 hplNew(cMainMenuWidget_Text,
								(mpInit, vPos,
								 _W("Shown to the other players. Letters, digits and spaces, up to 24 characters. Saved to multiplayer.cfg."),
								 13, eFontAlign_Center)));
		vPos.y += 50;
		gpMulTypedName = hplNew(cMainMenuWidget_MultiIpLine,
								(mpInit, vPos, mpInit->mpNetworkManager->GetLocalPlayerName(), 20,
								 eFontAlign_Center, true));
		AddWidgetToState(eMainMenuState_MultiplayerName, gpMulTypedName);
		vPos.y += 50;
		gpMulNameFoot = hplNew(cMainMenuWidget_Text,(mpInit, vPos, _W("Press Enter or click Save."), 14,
													  eFontAlign_Center));
		AddWidgetToState(eMainMenuState_MultiplayerName, gpMulNameFoot);
		vPos.y += 50;
		AddWidgetToState(
			eMainMenuState_MultiplayerName,
			hplNew(cMainMenuWidget_MultiNameSave,(mpInit, vPos, _W("Save"))));
		vPos.y += 40;
		AddWidgetToState(
			eMainMenuState_MultiplayerName,
			hplNew(cMainMenuWidget_MainButton,(mpInit, vPos, kTranslate("MainMenu", "Back"),
												eMainMenuState_Multiplayer)));

		///////////////////////////////////
		// Server browser (Internet / LAN) - one panel, see the layout
		// comment above cMainMenuWidget_MultiPanel.
		///////////////////////////////////
		{
			const eMainMenuState st = eMainMenuState_MultiplayerBrowser;
			AddWidgetToState(st, hplNew(cMainMenuWidget_MultiPanel,
				(mpInit, cRect2f(kMulBrPanelX, kMulBrPanelY, kMulBrPanelW, kMulBrPanelH),
				 _W("Server browser"), true, false)));
			AddWidgetToState(st, hplNew(cMainMenuWidget_MultiBrowserTab,
				(mpInit, cRect2f(kMulBrListX, 220, 110, 24), _W("Internet"), true)));
			AddWidgetToState(st, hplNew(cMainMenuWidget_MultiBrowserTab,
				(mpInit, cRect2f(kMulBrListX + 116, 220, 110, 24), _W("LAN"), false)));
			AddWidgetToState(st, hplNew(cMainMenuWidget_MultiBrowserRefresh,
				(mpInit, cRect2f(kMulBrListX + kMulBrListW - 96, 220, 96, 24), _W("Refresh"))));
			AddWidgetToState(st, hplNew(cMainMenuWidget_MultiBrowserTable,(mpInit)));
			gvMulRows.clear();
			for (int r = 0; r < kMulBrowserRows; ++r)
			{
				cMainMenuWidget_MultiServerRow *pRow = hplNew(cMainMenuWidget_MultiServerRow,
					(mpInit, cRect2f(kMulBrListX, kMulBrRowsY + r * kMulBrRowH, kMulBrListW, kMulBrRowH), r));
				gvMulRows.push_back(pRow);
				AddWidgetToState(st, pRow);
			}
			AddWidgetToState(st, hplNew(cMainMenuWidget_MultiBrowserPage,
				(mpInit, cRect2f(482, kMulBrStatusY, 22, 20), -1)));
			AddWidgetToState(st, hplNew(cMainMenuWidget_MultiBrowserPage,
				(mpInit, cRect2f(534, kMulBrStatusY, 22, 20), +1)));
			/* divider, then the guest's character (chips: who is taken) */
			AddWidgetToState(st, hplNew(cMainMenuWidget_MultiCharacter,
				(mpInit, cVector3f(kMulBrPanelX + kMulBrPanelW / 2, 486, 40), true, kMulBrListW)));
			AddWidgetToState(st, hplNew(cMainMenuWidget_MultiBrowserJoin,
				(mpInit, cRect2f(kMulBrListX, 548, 120, 26), _W("Join"))));
			AddWidgetToState(st, hplNew(cMainMenuWidget_MultiBoxButton,
				(mpInit, cRect2f(kMulBrListX + 128, 548, 170, 26), _W("Direct connect"),
				 eMainMenuState_MultiplayerJoin)));
			AddWidgetToState(st, hplNew(cMainMenuWidget_MultiBoxButton,
				(mpInit, cRect2f(kMulBrListX + kMulBrListW - 96, 548, 96, 26), kTranslate("MainMenu", "Back"),
				 eMainMenuState_Multiplayer)));
		}

		///////////////////////////////////
		// Password prompt for a locked row: panel x 96..496, y 214..450
		// (title, server name + address/map/players, hint, boxed field,
		// Enter/Esc hint, Join / Cancel).
		///////////////////////////////////
		{
			const eMainMenuState st = eMainMenuState_MultiplayerPassword;
			const float fPx = kMulBrPanelX + kMulBrPanelW / 2 - 200, fPy = 214, fPw = 400;
			const float fCx = fPx + fPw / 2;
			AddWidgetToState(st, hplNew(cMainMenuWidget_MultiPanel,
				(mpInit, cRect2f(fPx, fPy, fPw, 236), _W("Password required"), false, true)));
			AddWidgetToState(st, hplNew(cMainMenuWidget_Text,
				(mpInit, cVector3f(fCx, fPy + 90, 40),
				 _W("The host set a join password. Type the one they gave you."), 13, eFontAlign_Center)));
			gpMulTypedPw = hplNew(cMainMenuWidget_MultiIpLine,
				(mpInit, cVector3f(fCx, fPy + 124, 40), tString(""), 20, eFontAlign_Center));
			gpMulTypedPw->SetPasswordMode(true);
			gpMulTypedPw->SetAscii("");
			gpMulTypedPw->SetFieldBox(fPw - 60);
			AddWidgetToState(st, gpMulTypedPw);
			AddWidgetToState(st, hplNew(cMainMenuWidget_Text,
				(mpInit, cVector3f(fCx, fPy + 162, 40),
				 _W("Enter = join     Esc = back to the list"), 12, eFontAlign_Center)));
			AddWidgetToState(st, hplNew(cMainMenuWidget_MultiPwJoin,
				(mpInit, cRect2f(fCx - 150, fPy + 190, 140, 28), _W("Join"))));
			AddWidgetToState(st, hplNew(cMainMenuWidget_MultiBoxButton,
				(mpInit, cRect2f(fCx + 10, fPy + 190, 140, 28), _W("Cancel"),
				 eMainMenuState_MultiplayerBrowser)));
		}
	}
#endif

	///////////////////////////////////
	// Load Game
	///////////////////////////////////

	for(size_t i=0; i< 3; ++i)
	{
		vPos = vTextStart;//cVector3f(400, 260, 40);
		
		eMainMenuState state = (eMainMenuState)(eMainMenuState_LoadGameSpot + i);
		
		///////////////////////////
		//Head
		AddWidgetToState(state,hplNew( cMainMenuWidget_Text,(mpInit,vPos,kTranslate("MainMenu","Load Game"),27,eFontAlign_Center)) ); 
		vPos.y += 42;
		vPos.x -= 110;
		
		///////////////////////////
		//Buttons
		AddWidgetToState(state,hplNew( cMainMenuWidget_Button,(mpInit,vPos,kTranslate("MainMenu","Saved Games"),eMainMenuState_LoadGameSpot,25,eFontAlign_Center)) ); 
		vPos.y += 32;
		AddWidgetToState(state,hplNew( cMainMenuWidget_Button,(mpInit,vPos,kTranslate("MainMenu","Auto Saves"),eMainMenuState_LoadGameAuto,25,eFontAlign_Center)) ); 
		vPos.y += 32;
		AddWidgetToState(state,hplNew( cMainMenuWidget_Button,(mpInit,vPos,kTranslate("MainMenu","Favorites"),eMainMenuState_LoadGameFavorite,25,eFontAlign_Center)) ); 
		
		///////////////////////////
		//Back
		vPos.y += 150;
		vPos.x += 130;
		vPos.y += 32;
		AddWidgetToState(state,hplNew( cMainMenuWidget_Button,(mpInit,vPos,kTranslate("MainMenu","Back"),eMainMenuState_Start,23,eFontAlign_Center)) );
        
		///////////////////////////
		//Load type
		vPos = vTextStart;//cVector3f(400, 260, 40);
		vPos.y += 42;
		vPos.x += 185;
		tString sLoadType = "Saved Games";
		if(i==1)sLoadType = "Auto Saves";
		if(i==2)sLoadType = "Favorites";
		AddWidgetToState(state,hplNew( cMainMenuWidget_Text,(mpInit,vPos,kTranslate("MainMenu",sLoadType)+_W(":"),21,eFontAlign_Center)) ); 
		
		///////////////////////////
		//Saved games list
		
		//Set up
		vPos = vTextStart;//cVector3f(400, 260 , 40);
		vPos.y += 46 + 30;
		vPos.x += 15;
		
		tWString sDir = _W("save/spot");
		if(i == 1)		sDir = _W("save/auto");
		else if(i == 2) sDir = _W("save/favorite");
		
		gpSaveGameList[i] = hplNew( cMainMenuWidget_SaveGameList,(
														mpInit,vPos,cVector2f(355,170),15,sDir,(int)i) );
		AddWidgetToState(state,gpSaveGameList[i]);
			
		iLowLevelResources *pLowLevelResources = mpInit->mpGame->GetResources()->GetLowLevel();
		iLowLevelSystem *pLowLevelSystem = mpInit->mpGame->GetSystem()->GetLowLevel();

		tWStringList lstFiles;
		tTempFileAndDataSet setTempFiles;
		
		tWString sFullPath = mpInit->mpSaveHandler->GetSaveDir() + sDir;
		pLowLevelResources->FindFilesInDir(lstFiles,sFullPath,_W("*.sav"));

		tWStringListIt fileIt = lstFiles.begin();
		for(; fileIt != lstFiles.end(); ++fileIt)
		{
			tWString sFile = *fileIt;
			cDate date = FileModifiedDate(sFullPath+_W("/")+sFile);
            
			setTempFiles.insert(cTempFileAndData(sFile,date));
		}

		//Go through the sorted array and add to File vector and as list entries
		gvSaveGameFileVec[i].clear();
		tTempFileAndDataSetIt dateIt = setTempFiles.begin();
		for(; dateIt != setTempFiles.end(); ++dateIt)
		{
			const cTempFileAndData &temp = *dateIt;

			tWString sFile = temp.msFile;

			gvSaveGameFileVec[i].push_back(sFile);

			sFile = cString::SetFileExtW(sFile,_W(""));
			sFile = cString::SubW(sFile,0, (int)sFile.length() - 3);
			sFile = cString::ReplaceCharToW(sFile,_W("_"),_W(" "));
			sFile = cString::ReplaceCharToW(sFile,_W("."),_W(":"));

			//TODO: PROBLEM!!!
			gpSaveGameList[i]->AddEntry(sFile);
			//gpSaveGameList[i]->AddEntry(sFile);
		}

		///////////////////////////
		//Save game buttons
		vPos.y += 170;
		vPos.x = vTextStart.x + 20;
		
		AddWidgetToState(state,hplNew( cMainMenuWidget_LoadSaveGame,(mpInit,vPos,kTranslate("MainMenu","Load"),17,eFontAlign_Left,sDir,(int)i)) ); 
		
		vPos.x += 70;
		if(i!=2)
			AddWidgetToState(state,hplNew( cMainMenuWidget_FavoriteSaveGame,(mpInit,vPos,kTranslate("MainMenu","Add To Favorites"),17,eFontAlign_Left,sDir,(int)i)) ); 
		
		vPos.x += 205;
		AddWidgetToState(state,hplNew( cMainMenuWidget_RemoveSaveGame,(mpInit,vPos,kTranslate("MainMenu","Remove"),17,eFontAlign_Left,sDir,(int)i)) ); 
		
		
        
	}

	///////////////////////////////////
	// Quit
	///////////////////////////////////
	vPos = vTextStart;//cVector3f(400, 260, 40);
	AddWidgetToState(eMainMenuState_Exit,hplNew( cMainMenuWidget_Text,(mpInit,vPos,kTranslate("MainMenu","SureQuit"),20,eFontAlign_Center)) ); 
	vPos.y += 34;
	AddWidgetToState(eMainMenuState_Exit,hplNew( cMainMenuWidget_Quit,(mpInit,vPos,kTranslate("MainMenu","Yes"),20,eFontAlign_Center)) ); 
	vPos.y += 29;
	AddWidgetToState(eMainMenuState_Exit,hplNew( cMainMenuWidget_Button,(mpInit,vPos,kTranslate("MainMenu","No"),eMainMenuState_Start,20,eFontAlign_Center)) );

	///////////////////////////////////
	// Options
	///////////////////////////////////
	vPos = vTextStart;//cVector3f(400, 260, 40);
	AddWidgetToState(eMainMenuState_Options,hplNew( cMainMenuWidget_Button,(mpInit,vPos,kTranslate("MainMenu","Controls"),eMainMenuState_OptionsControls,25,eFontAlign_Center)) ); 
	vPos.y += 37;
	AddWidgetToState(eMainMenuState_Options,hplNew( cMainMenuWidget_Button,(mpInit,vPos,kTranslate("MainMenu","Game"),eMainMenuState_OptionsGame,25,eFontAlign_Center)) );
	vPos.y += 37;
	AddWidgetToState(eMainMenuState_Options,hplNew( cMainMenuWidget_Button,(mpInit,vPos,kTranslate("MainMenu","Sound"),eMainMenuState_OptionsSound,25,eFontAlign_Center)) );
	vPos.y += 37;
	AddWidgetToState(eMainMenuState_Options,hplNew( cMainMenuWidget_Button,(mpInit,vPos,kTranslate("MainMenu","Graphics"),eMainMenuState_OptionsGraphics,25,eFontAlign_Center)) );
	vPos.y += 37;
	AddWidgetToState(eMainMenuState_Options,hplNew( cMainMenuWidget_Button,(mpInit,vPos,kTranslate("MainMenu","Back"),eMainMenuState_Start,25,eFontAlign_Center)) );

	///////////////////////////////////
	// Options Controls
	///////////////////////////////////
	vPos = vTextStart;//cVector3f(400, 260, 40);
	//Head
	AddWidgetToState(eMainMenuState_OptionsControls,hplNew( cMainMenuWidget_Text,(mpInit,vPos,kTranslate("MainMenu","Controls"),25,eFontAlign_Center)) ); 
	vPos.y += 37;
	//Buttons
	cMainMenuWidget *pWidgetInvertMouseY = hplNew( cMainMenuWidget_InvertMouseY,(mpInit,vPos,kTranslate("MainMenu","Invert Mouse Y:"),20,eFontAlign_Right) );
	AddWidgetToState(eMainMenuState_OptionsControls,pWidgetInvertMouseY); 
	vPos.y += 29;
	cMainMenuWidget *pWidgetMouseSensitivity = hplNew( cMainMenuWidget_MouseSensitivity,(mpInit,vPos,kTranslate("MainMenu","Mouse Sensitivity:"),20,eFontAlign_Right) );
	AddWidgetToState(eMainMenuState_OptionsControls,pWidgetMouseSensitivity); 
	vPos.y += 29;
	cMainMenuWidget *pWidgetToggleCrouch = hplNew( cMainMenuWidget_ToggleCrouch,(mpInit,vPos,kTranslate("MainMenu","Toggle Crouch:"),20,eFontAlign_Right) );
	AddWidgetToState(eMainMenuState_OptionsControls,pWidgetToggleCrouch); 
	vPos.y += 29;
	cMainMenuWidget *pWidgetUseHaptics = NULL;
	cMainMenuWidget *pWidgetInteractModeCameraSpeed = NULL;
	cMainMenuWidget *pWidgetActionModeCameraSpeed = NULL;
	cMainMenuWidget *pWidgetWeightForceScale = NULL;
	if(mpInit->mbHapticsAvailable)
	{
		vPos.y+= 5;
		//Use haptics
		tWString sText = kTranslate("MainMenu","Use Haptics:");
		if(sText == _W("")) sText = _W("Use Haptics:");
		pWidgetUseHaptics = hplNew( cMainMenuWidget_UseHaptics,(mpInit,vPos,sText,20,eFontAlign_Right) );
		AddWidgetToState(eMainMenuState_OptionsControls,pWidgetUseHaptics); 
		vPos.y += 29;
		
		//Weight Force Scale
		sText = kTranslate("MainMenu","Weight Force Scale:");
		if(sText == _W("")) sText = _W("Weight Force Scale:");
		pWidgetWeightForceScale = hplNew( cMainMenuWidget_WeightForceScale,(mpInit,vPos,sText,20,eFontAlign_Right) );
		AddWidgetToState(eMainMenuState_OptionsControls,pWidgetWeightForceScale); 
		vPos.y += 29;

		//InteractMode camera speed
		sText = kTranslate("MainMenu","InteractMode Camera Speed:");
		if(sText == _W("")) sText = _W("InteractMode Camera Speed:");
		pWidgetInteractModeCameraSpeed = hplNew( cMainMenuWidget_InteractModeCameraSpeed,(mpInit,vPos,sText,20,eFontAlign_Right) );
		AddWidgetToState(eMainMenuState_OptionsControls,pWidgetInteractModeCameraSpeed); 
		vPos.y += 29;

		//ActionMode camera speed
		sText = kTranslate("MainMenu","ActionMode Camera Speed:");
		if(sText == _W("")) sText = _W("ActionMode Camera Speed:");
		pWidgetActionModeCameraSpeed = hplNew( cMainMenuWidget_ActionModeCameraSpeed,(mpInit,vPos,sText,20,eFontAlign_Right) );
		AddWidgetToState(eMainMenuState_OptionsControls,pWidgetActionModeCameraSpeed); 
		vPos.y += 29;
		
		vPos.y+= 5;
	}
	cMainMenuWidget *pWidgetChangeKeyConf = hplNew( cMainMenuWidget_Button,(mpInit,vPos,kTranslate("MainMenu","Change Key Mapping"),eMainMenuState_OptionsKeySetupMove,20,eFontAlign_Center) );
	AddWidgetToState(eMainMenuState_OptionsControls,pWidgetChangeKeyConf); 
	vPos.y += 35;
	AddWidgetToState(eMainMenuState_OptionsControls,hplNew( cMainMenuWidget_Button,(mpInit,vPos,kTranslate("MainMenu","Back"),eMainMenuState_Options,23,eFontAlign_Center) )); 

	//Text
	vPos = cVector3f(vTextStart.x+12, vTextStart.y+37, vTextStart.z);

	sText = mpInit->mpButtonHandler->mbInvertMouseY ? kTranslate("MainMenu","On") : kTranslate("MainMenu","Off");
	gpInvertMouseYText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left,pWidgetInvertMouseY) );
	AddWidgetToState(eMainMenuState_OptionsControls,gpInvertMouseYText); 

	vPos.y += 29;
	sprintf(sTempVec,"%.1f",mpInit->mpButtonHandler->mfMouseSensitivity);
	sText = cString::To16Char(sTempVec);
	gpMouseSensitivityText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left,pWidgetMouseSensitivity) );
	AddWidgetToState(eMainMenuState_OptionsControls,gpMouseSensitivityText); 

	vPos.y += 29;
	sText = mpInit->mpButtonHandler->mbToggleCrouch ? kTranslate("MainMenu","On"): kTranslate("MainMenu","Off");
	gpToggleCrouchText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left,pWidgetToggleCrouch) );
	AddWidgetToState(eMainMenuState_OptionsControls,gpToggleCrouchText); 

	if(mpInit->mbHapticsAvailable)
	{
		vPos.y+= 5;
		vPos.y += 29;
		sText = mpInit->mbHasHapticsOnRestart ? kTranslate("MainMenu","On") : kTranslate("MainMenu","Off");
		gpUseHapticsText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left) );
		AddWidgetToState(eMainMenuState_OptionsControls,gpUseHapticsText);
		gpUseHapticsText->SetExtraWidget(pWidgetUseHaptics);
		
		vPos.y += 29;
		sprintf(sTempVec,"%.1f",mpInit->mfHapticForceMul);
		sText = cString::To16Char(sTempVec);
		gpWidgetWeightForceScaleText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left,pWidgetWeightForceScale) );
		AddWidgetToState(eMainMenuState_OptionsControls,gpWidgetWeightForceScaleText); 

		vPos.y += 29;
		sprintf(sTempVec,"%.1f",mpInit->mpPlayer->GetHapticCamera()->GetInteractModeCameraSpeed());
		sText = cString::To16Char(sTempVec);
		gpWidgetInteractModeCameraSpeedText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left,pWidgetInteractModeCameraSpeed) );
		AddWidgetToState(eMainMenuState_OptionsControls,gpWidgetInteractModeCameraSpeedText); 

		vPos.y += 29;
		sprintf(sTempVec,"%.1f",mpInit->mpPlayer->GetHapticCamera()->GetActionModeCameraSpeed());
		sText = cString::To16Char(sTempVec);
		gpWidgetActionModeCameraSpeedText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left,pWidgetActionModeCameraSpeed) );
		AddWidgetToState(eMainMenuState_OptionsControls,gpWidgetActionModeCameraSpeedText); 
	}

	///////////////////////////////////
	// Options Key Setup General stuff
	///////////////////////////////////
	for(int i=0; i<3; ++i)
	{
		eMainMenuState state = (eMainMenuState)(i+ eMainMenuState_OptionsKeySetupMove);
		cVector3f vPos = vTextStart;//cVector3f(400, 260, 40);
		//Head
		AddWidgetToState(state,hplNew( cMainMenuWidget_Text,(mpInit,vPos,kTranslate("MainMenu","Configure Keys"),25,eFontAlign_Center)) ); 
		vPos.y += 42;
		vPos.x -= 110;
		//Buttons
		AddWidgetToState(state,hplNew( cMainMenuWidget_Button,(mpInit,vPos,kTranslate("MainMenu","Movement"),eMainMenuState_OptionsKeySetupMove,25,eFontAlign_Center)) ); 
		vPos.y += 32;
		AddWidgetToState(state,hplNew( cMainMenuWidget_Button,(mpInit,vPos,kTranslate("MainMenu","Actions"),eMainMenuState_OptionsKeySetupAction,25,eFontAlign_Center)) ); 
		vPos.y += 32;
		AddWidgetToState(state,hplNew( cMainMenuWidget_Button,(mpInit,vPos,kTranslate("MainMenu","Misc"),eMainMenuState_OptionsKeySetupMisc,25,eFontAlign_Center)) ); 
		//Back
		vPos.y += 150;
		vPos.x += 130;
		AddWidgetToState(state,hplNew( cMainMenuWidget_KeyReset,(mpInit,vPos,kTranslate("MainMenu","Reset to defaults"),23,eFontAlign_Center)) );
		vPos.y += 32;
		AddWidgetToState(state,hplNew( cMainMenuWidget_Button,(mpInit,vPos,kTranslate("MainMenu","Back"),eMainMenuState_OptionsControls,23,eFontAlign_Center)) );
	}

	///////////////////////////////////
	// Options Key Setup Move
	///////////////////////////////////

	cMainMenuWidget *pWidgetKeyButton;
	float fKeyTextXAdd = 195;

	//Key buttons
	cInput *pInput = mpInit->mpGame->GetInput();
	vPos = vTextStart;//cVector3f(400, 260, 40);
	vPos.y += 46;
	vPos.x += 15;

	pTempTextWidget = hplNew( cMainMenuWidget_Text,(mpInit,vPos+cVector3f(fKeyTextXAdd,0,0),_W(""),18,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsKeySetupMove,pTempTextWidget); 
	pWidgetKeyButton = hplNew( cMainMenuWidget_KeyButton,(mpInit,vPos,kTranslate("MainMenu","Forward:"),
													18,eFontAlign_Left,pTempTextWidget,"Forward") );
	AddWidgetToState(eMainMenuState_OptionsKeySetupMove,pWidgetKeyButton); 
	
	vPos.y += 23;
	pTempTextWidget = hplNew( cMainMenuWidget_Text,(mpInit,vPos+cVector3f(fKeyTextXAdd,0,0),_W(""),18,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsKeySetupMove,pTempTextWidget); 
	pWidgetKeyButton = hplNew( cMainMenuWidget_KeyButton,(mpInit,vPos,kTranslate("MainMenu","Backward:"),
												18,eFontAlign_Left,pTempTextWidget,"Backward") );
	AddWidgetToState(eMainMenuState_OptionsKeySetupMove,pWidgetKeyButton); 
	
	vPos.y += 23;
	pTempTextWidget = hplNew( cMainMenuWidget_Text,(mpInit,vPos+cVector3f(fKeyTextXAdd,0,0),_W(""),18,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsKeySetupMove,pTempTextWidget); 
	pWidgetKeyButton = hplNew( cMainMenuWidget_KeyButton,(mpInit,vPos,kTranslate("MainMenu","Strafe Left:"),
					18,eFontAlign_Left,pTempTextWidget,"Left") );
	AddWidgetToState(eMainMenuState_OptionsKeySetupMove,pWidgetKeyButton); 
	
	vPos.y += 23;
	pTempTextWidget = hplNew( cMainMenuWidget_Text,(mpInit,vPos+cVector3f(fKeyTextXAdd,0,0),_W(""),18,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsKeySetupMove,pTempTextWidget); 
	pWidgetKeyButton = hplNew( cMainMenuWidget_KeyButton,(mpInit,vPos,kTranslate("MainMenu","Strafe Right:"),
					18,eFontAlign_Left,pTempTextWidget,"Right") );
	AddWidgetToState(eMainMenuState_OptionsKeySetupMove,pWidgetKeyButton); 
	
	vPos.y += 23;
	pTempTextWidget = hplNew( cMainMenuWidget_Text,(mpInit,vPos+cVector3f(fKeyTextXAdd,0,0),_W(""),18,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsKeySetupMove,pTempTextWidget); 
	pWidgetKeyButton = hplNew( cMainMenuWidget_KeyButton,(mpInit,vPos,kTranslate("MainMenu","Run:"),
					18,eFontAlign_Left,pTempTextWidget,"Run") );
	AddWidgetToState(eMainMenuState_OptionsKeySetupMove,pWidgetKeyButton); 
	
	vPos.y += 23;
	pTempTextWidget = hplNew( cMainMenuWidget_Text,(mpInit,vPos+cVector3f(fKeyTextXAdd,0,0),_W(""),18,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsKeySetupMove,pTempTextWidget); 
	pWidgetKeyButton = hplNew( cMainMenuWidget_KeyButton,(mpInit,vPos,kTranslate("MainMenu","Crouch:"),
					18,eFontAlign_Left,pTempTextWidget,"Crouch") );
	AddWidgetToState(eMainMenuState_OptionsKeySetupMove,pWidgetKeyButton); 
	
	vPos.y += 23;
	pTempTextWidget = hplNew( cMainMenuWidget_Text,(mpInit,vPos+cVector3f(fKeyTextXAdd,0,0),_W(""),18,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsKeySetupMove,pTempTextWidget); 
	pWidgetKeyButton = hplNew( cMainMenuWidget_KeyButton,(mpInit,vPos,kTranslate("MainMenu","Jump:"),
					18,eFontAlign_Left,pTempTextWidget,"Jump") );
	AddWidgetToState(eMainMenuState_OptionsKeySetupMove,pWidgetKeyButton); 
	
	vPos.y += 23;
	pTempTextWidget = hplNew( cMainMenuWidget_Text,(mpInit,vPos+cVector3f(fKeyTextXAdd,0,0),_W(""),18,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsKeySetupMove,pTempTextWidget); 
	pWidgetKeyButton = hplNew( cMainMenuWidget_KeyButton,(mpInit,vPos,kTranslate("MainMenu","Lean Left:"),
					18,eFontAlign_Left,pTempTextWidget,"LeanLeft") );
	AddWidgetToState(eMainMenuState_OptionsKeySetupMove,pWidgetKeyButton); 
	
	vPos.y += 23;
	pTempTextWidget = hplNew( cMainMenuWidget_Text,(mpInit,vPos+cVector3f(fKeyTextXAdd,0,0),_W(""),18,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsKeySetupMove,pTempTextWidget); 
	pWidgetKeyButton = hplNew( cMainMenuWidget_KeyButton,(mpInit,vPos,kTranslate("MainMenu","Lean Right:"),
					18,eFontAlign_Left,pTempTextWidget,"LeanRight") );
	AddWidgetToState(eMainMenuState_OptionsKeySetupMove,pWidgetKeyButton); 
	

	///////////////////////////////////
	// Options Key Setup Action
	///////////////////////////////////
	//Key buttons
	vPos = vTextStart;//cVector3f(400, 260, 40);
	vPos.y += 46;
	vPos.x += 15;

	pTempTextWidget = hplNew( cMainMenuWidget_Text,(mpInit,vPos+cVector3f(fKeyTextXAdd,0,0),_W(""),18,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsKeySetupAction,pTempTextWidget); 
	AddWidgetToState(eMainMenuState_OptionsKeySetupAction,hplNew( cMainMenuWidget_KeyButton,(mpInit,vPos,kTranslate("MainMenu","Interact:"),
		18,eFontAlign_Left,pTempTextWidget,"Interact")) ); 

	vPos.y += 23;
	pTempTextWidget = hplNew( cMainMenuWidget_Text,(mpInit,vPos+cVector3f(fKeyTextXAdd,0,0),_W(""),18,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsKeySetupAction,pTempTextWidget); 
	AddWidgetToState(eMainMenuState_OptionsKeySetupAction,hplNew( cMainMenuWidget_KeyButton,(mpInit,vPos,kTranslate("MainMenu","Examine:"),
		18,eFontAlign_Left,pTempTextWidget,"Examine")) ); 

	vPos.y += 23;
	pTempTextWidget = hplNew( cMainMenuWidget_Text,(mpInit,vPos+cVector3f(fKeyTextXAdd,0,0),_W(""),18,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsKeySetupAction,pTempTextWidget); 
	AddWidgetToState(eMainMenuState_OptionsKeySetupAction,hplNew( cMainMenuWidget_KeyButton,(mpInit,vPos,kTranslate("MainMenu","InteractMode:"),
		18,eFontAlign_Left,pTempTextWidget,"InteractMode")) ); 

	vPos.y += 23;
	pTempTextWidget = hplNew( cMainMenuWidget_Text,(mpInit,vPos+cVector3f(fKeyTextXAdd,0,0),_W(""),18,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsKeySetupAction,pTempTextWidget); 
	AddWidgetToState(eMainMenuState_OptionsKeySetupAction,hplNew( cMainMenuWidget_KeyButton,(mpInit,vPos,kTranslate("MainMenu","Holster:"),
		18,eFontAlign_Left,pTempTextWidget,"Holster")) ); 

	vPos.y += 23;
	pTempTextWidget = hplNew( cMainMenuWidget_Text,(mpInit,vPos+cVector3f(fKeyTextXAdd,0,0),_W(""),18,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsKeySetupAction,pTempTextWidget); 
	AddWidgetToState(eMainMenuState_OptionsKeySetupAction,hplNew( cMainMenuWidget_KeyButton,(mpInit,vPos,kTranslate("MainMenu","LookMode:"),
		18,eFontAlign_Left,pTempTextWidget,"LookMode")) ); 


	///////////////////////////////////
	// Options Key Setup Misc
	///////////////////////////////////
	//Key buttons
	vPos = vTextStart;//cVector3f(400, 260, 40);
	vPos.y += 46;
	vPos.x += 15;

	pTempTextWidget = hplNew( cMainMenuWidget_Text,(mpInit,vPos+cVector3f(fKeyTextXAdd,0,0),_W(""),18,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsKeySetupMisc,pTempTextWidget); 
	AddWidgetToState(eMainMenuState_OptionsKeySetupMisc,hplNew( cMainMenuWidget_KeyButton,(mpInit,vPos,kTranslate("MainMenu","Inventory:"),
		18,eFontAlign_Left,pTempTextWidget,"Inventory")) ); 

	vPos.y += 23;
	pTempTextWidget = hplNew( cMainMenuWidget_Text,(mpInit,vPos+cVector3f(fKeyTextXAdd,0,0),_W(""),18,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsKeySetupMisc,pTempTextWidget); 
	AddWidgetToState(eMainMenuState_OptionsKeySetupMisc,hplNew( cMainMenuWidget_KeyButton,(mpInit,vPos,kTranslate("MainMenu","Notebook:"),
		18,eFontAlign_Left,pTempTextWidget,"NoteBook")) ); 

	vPos.y += 23;
	pTempTextWidget = hplNew( cMainMenuWidget_Text,(mpInit,vPos+cVector3f(fKeyTextXAdd,0,0),_W(""),18,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsKeySetupMisc,pTempTextWidget); 
	AddWidgetToState(eMainMenuState_OptionsKeySetupMisc,hplNew( cMainMenuWidget_KeyButton,(mpInit,vPos,kTranslate("MainMenu","Pers. Notes:"),
		18,eFontAlign_Left,pTempTextWidget,"PersonalNotes")) ); 

	vPos.y += 23;
	pTempTextWidget = hplNew( cMainMenuWidget_Text,(mpInit,vPos+cVector3f(fKeyTextXAdd,0,0),_W(""),18,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsKeySetupMisc,pTempTextWidget); 
	AddWidgetToState(eMainMenuState_OptionsKeySetupMisc,hplNew( cMainMenuWidget_KeyButton,(mpInit,vPos,kTranslate("MainMenu","Flashlight:"),
		18,eFontAlign_Left,pTempTextWidget,"Flashlight")) ); 

	vPos.y += 23;
	pTempTextWidget = hplNew( cMainMenuWidget_Text,(mpInit,vPos+cVector3f(fKeyTextXAdd,0,0),_W(""),18,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsKeySetupMisc,pTempTextWidget); 
	AddWidgetToState(eMainMenuState_OptionsKeySetupMisc,hplNew( cMainMenuWidget_KeyButton,(mpInit,vPos,kTranslate("MainMenu","Glowstick:"),
		18,eFontAlign_Left,pTempTextWidget,"GlowStick")) ); 

	///////////////////////////////////
	// Options Game
	///////////////////////////////////
	vPos = vTextStart;//cVector3f(400, 260, 40);
	//Head
	AddWidgetToState(eMainMenuState_OptionsGame,hplNew( cMainMenuWidget_Text,(mpInit,vPos,kTranslate("MainMenu","Game"),25,eFontAlign_Center)) ); 
	vPos.y += 37;
	//Buttons
	cMainMenuWidget *pWidgetLanguage = hplNew( cMainMenuWidget_Language,(mpInit,vPos,kTranslate("MainMenu","Language:"),20,eFontAlign_Right) );
	AddWidgetToState(eMainMenuState_OptionsGame,pWidgetLanguage); 
	vPos.y += 29;
	cMainMenuWidget *pWidgetSubtitles = hplNew( cMainMenuWidget_Subtitles,(mpInit,vPos,kTranslate("MainMenu","Subtitle:"),20,eFontAlign_Right) );
	AddWidgetToState(eMainMenuState_OptionsGame,pWidgetSubtitles); 
	vPos.y += 50;
	cMainMenuWidget *pWidgetDifficulty = hplNew( cMainMenuWidget_Difficulty,(mpInit,vPos,kTranslate("MainMenu","Difficulty:"),20,eFontAlign_Right) );
	AddWidgetToState(eMainMenuState_OptionsGame,pWidgetDifficulty); 
	vPos.y += 29;
	if(mpInit->mbSimpleSwingInOptions)
	{
		AddWidgetToState(eMainMenuState_OptionsGame,hplNew( cMainMenuWidget_SimpleSwing,(mpInit,vPos,kTranslate("MainMenu","SimpleSwing:"),20,eFontAlign_Right)) ); 
		vPos.y += 29;
	}
	//AddWidgetToState(eMainMenuState_OptionsGame,hplNew( cMainMenuWidget_AllowQuickSave(mpInit,vPos,kTranslate("MainMenu","AllowQuickSave:"),20,eFontAlign_Right)); 
	//vPos.y += 29;
	tWString sCrosshairText = kTranslate("MainMenu","Show Crosshair:");
	if(sCrosshairText == _W("")) sCrosshairText = _W("Show Crosshair:");
	cMainMenuWidget *pWidgetShowCrossHair = hplNew( cMainMenuWidget_ShowCrossHair,(mpInit,vPos,sCrosshairText,20,eFontAlign_Right) );
	AddWidgetToState(eMainMenuState_OptionsGame,pWidgetShowCrossHair); 
	vPos.y += 29;
		
	cMainMenuWidget *pWidgetFlashItems = hplNew( cMainMenuWidget_FlashItems,(mpInit,vPos,kTranslate("MainMenu","FlashItems:"),20,eFontAlign_Right) );
	AddWidgetToState(eMainMenuState_OptionsGame,pWidgetFlashItems); 
	vPos.y += 29;
	
	cMainMenuWidget *pWidgetDisablePersonal = hplNew( cMainMenuWidget_DisablePersonal,(mpInit,vPos,kTranslate("MainMenu","DisablePersonal:"),20,eFontAlign_Right) );
	AddWidgetToState(eMainMenuState_OptionsGame,pWidgetDisablePersonal); 
	vPos.y += 35;
	AddWidgetToState(eMainMenuState_OptionsGame,hplNew( cMainMenuWidget_GfxBack,(mpInit,vPos,kTranslate("MainMenu","Back"),23,eFontAlign_Center)) ); 

	//Text
	vPos = cVector3f(vTextStart.x+12, vTextStart.y+37, vTextStart.z);

	sText = cString::To16Char(cString::SetFileExt(mpInit->msLanguageFile,""));
	gpLanguageText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsGame,gpLanguageText); 
	gpLanguageText->SetExtraWidget(pWidgetLanguage);

	vPos.y += 29;
	sText = mpInit->mbSubtitles ? kTranslate("MainMenu","On") : kTranslate("MainMenu","Off");
	gpSubtitlesText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsGame,gpSubtitlesText); 
	gpSubtitlesText->SetExtraWidget(pWidgetSubtitles);
	
	vPos.y +=25;
	AddWidgetToState(eMainMenuState_OptionsGame,hplNew( cMainMenuWidget_Text,(mpInit,cVector3f(vPos-cVector3f(12,0,0)),
										kTranslate("MainMenu","VoiceLanguange:"),12,eFontAlign_Right)) );
	AddWidgetToState(eMainMenuState_OptionsGame,hplNew( cMainMenuWidget_Text,(mpInit,cVector3f(vPos),
										kTranslate("MainMenu","SetThisToLanguageOfVoice"),12,eFontAlign_Left)) );
	vPos.y += 25;
	
	sText = kTranslate("MainMenu",gvDifficultyLevel[mpInit->mDifficulty]);
	gpDifficultyText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsGame,gpDifficultyText); 
	gpDifficultyText->SetExtraWidget(pWidgetDifficulty);
	vPos.y += 29;

	if(mpInit->mbSimpleSwingInOptions)
	{
		sText = mpInit->mbSimpleWeaponSwing ? kTranslate("MainMenu","On") : kTranslate("MainMenu","Off");
		gpSimpleSwingText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left) );
		AddWidgetToState(eMainMenuState_OptionsGame,gpSimpleSwingText); 
		vPos.y += 29;
	}
	
	//sText = mpInit->mbAllowQuickSave ? kTranslate("MainMenu","On") : kTranslate("MainMenu","Off");
	//gpAllowQuickSaveText = hplNew( cMainMenuWidget_Text, (mpInit,vPos,sText,20,eFontAlign_Left) );
	//AddWidgetToState(eMainMenuState_OptionsGame,gpAllowQuickSaveText); 
	//vPos.y += 29;
	sText = mpInit->mbShowCrossHair ? kTranslate("MainMenu","On") : kTranslate("MainMenu","Off");
	gpShowCrossHairText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsGame,gpShowCrossHairText);
	gpShowCrossHairText->SetExtraWidget(pWidgetShowCrossHair);
	vPos.y += 29;
	
	sText = mpInit->mbFlashItems ? kTranslate("MainMenu","On") : kTranslate("MainMenu","Off");
	gpFlashItemsText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsGame,gpFlashItemsText);
	gpFlashItemsText->SetExtraWidget(pWidgetFlashItems);
	
	vPos.y += 29;
	sText = mpInit->mbDisablePersonalNotes ? kTranslate("MainMenu","Off") : kTranslate("MainMenu","On");
	gpDisablePersonalText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsGame,gpDisablePersonalText); 
	gpDisablePersonalText->SetExtraWidget(pWidgetDisablePersonal);

	///////////////////////////////////
	// Options Sound
	///////////////////////////////////
	vPos = vTextStart;//cVector3f(400, 230, 40);
	//Head
	AddWidgetToState(eMainMenuState_OptionsSound,hplNew( cMainMenuWidget_Text, (mpInit,vPos,kTranslate("MainMenu","Sound"),25,eFontAlign_Center)) ); 
	vPos.y += 37;
	
	//Buttons
	cMainMenuWidget *pWidgetSoundVolume = hplNew( cMainMenuWidget_SoundVolume, (mpInit,vPos,kTranslate("MainMenu","Sound Volume:"),20,eFontAlign_Right) );  
	AddWidgetToState(eMainMenuState_OptionsSound,pWidgetSoundVolume); 
	vPos.y += 29;
	cMainMenuWidget *pWidgetSoundHardware = hplNew( cMainMenuWidget_SoundHardware, (mpInit,vPos,kTranslate("MainMenu","Use Hardware:"),20,eFontAlign_Right) );
	AddWidgetToState(eMainMenuState_OptionsSound,pWidgetSoundHardware); 
	vPos.y += 29;
	cMainMenuWidget *pWidgetSoundOutputDevice = hplNew( cMainMenuWidget_SoundOutputDevice, (mpInit,vPos,kTranslate("MainMenu","Output Device:"),20,eFontAlign_Right) );
	AddWidgetToState(eMainMenuState_OptionsSound,pWidgetSoundOutputDevice);
	vPos.y += 35;
    AddWidgetToState(eMainMenuState_OptionsSound,hplNew( cMainMenuWidget_GfxBack, (mpInit,vPos,kTranslate("MainMenu","Back"),23,eFontAlign_Center)) ); 


	//Text
	vPos = cVector3f(vTextStart.x+12, vTextStart.y+37, vTextStart.z);

	sprintf(sTempVec,"%.0f",mpInit->mpGame->GetSound()->GetLowLevel()->GetVolume()*100);
	sText = cString::To16Char(sTempVec);
	gpSoundVolumeText = hplNew( cMainMenuWidget_Text, (mpInit,vPos,sText,20,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsSound,gpSoundVolumeText); 
	gpSoundVolumeText->SetExtraWidget(pWidgetSoundVolume);

	vPos.y += 29;
	sText = mpInit->mbUseSoundHardware ? kTranslate("MainMenu","On") : kTranslate("MainMenu","Off");
	gpSoundHardwareText = hplNew( cMainMenuWidget_Text, (mpInit,vPos,sText,20,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsSound,gpSoundHardwareText); 
	gpSoundHardwareText->SetExtraWidget(pWidgetSoundHardware);

	vPos.y += 29;
	// Set the default to what's really being used
	mpInit->msDeviceName = tString(OAL_Info_GetDeviceName());

	sText = cString::To16Char(mpInit->msDeviceName);
	gpSoundOutputDevice = hplNew( cMainMenuWidget_Text, (mpInit,vPos,sText,20,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsSound,gpSoundOutputDevice);
	gpSoundOutputDevice->SetExtraWidget(pWidgetSoundOutputDevice);

	///////////////////////////////////
	// Options Graphics
	///////////////////////////////////
	vPos = vTextStart;//cVector3f(400, 230, 40);
	//Head
	AddWidgetToState(eMainMenuState_OptionsGraphics,hplNew( cMainMenuWidget_Text,(mpInit,vPos,kTranslate("MainMenu","Graphics"),25,eFontAlign_Center)) ); 
	vPos.y += 37;
	/*AddWidgetToState(eMainMenuState_OptionsGraphics,hplNew( cMainMenuWidget_Image, (mpInit,
													cVector3f(400,vPos.y,30),
													cVector2f(200,150),
													"menu_gamma.bmp",
													"diffalpha2d",
													cColor(1,1))) );
	
	//Buttons
	vPos.x -= 130;*/
	cMainMenuWidget *pWidgetResolution = hplNew( cMainMenuWidget_Resolution,(mpInit,vPos,kTranslate("MainMenu","Resolution:"),20,eFontAlign_Right) );
	AddWidgetToState(eMainMenuState_OptionsGraphics,pWidgetResolution); 
	vPos.y += 29;
	cMainMenuWidget *pWidgetNoiseFilter = hplNew( cMainMenuWidget_NoiseFilter,(mpInit,vPos,kTranslate("MainMenu","Noise Filter:"),20,eFontAlign_Right) );
	AddWidgetToState(eMainMenuState_OptionsGraphics,pWidgetNoiseFilter); 
	vPos.y += 29;
	cMainMenuWidget *pWidgetBloom = hplNew( cMainMenuWidget_Bloom,(mpInit,vPos,kTranslate("MainMenu","Bloom:"),20,eFontAlign_Right) );
	AddWidgetToState(eMainMenuState_OptionsGraphics,pWidgetBloom); 
	vPos.y += 29;
	cMainMenuWidget *pWidgetGamma = hplNew( cMainMenuWidget_Gamma,(mpInit,vPos,kTranslate("MainMenu","Gamma:"),20,eFontAlign_Right,0) );
	AddWidgetToState(eMainMenuState_OptionsGraphics,pWidgetGamma); 
	vPos.y += 29;
	cMainMenuWidget *pWidgetShaderQuality = hplNew( cMainMenuWidget_ShaderQuality,(mpInit,vPos,kTranslate("MainMenu","Shader Quality:"),20,eFontAlign_Right) );
	AddWidgetToState(eMainMenuState_OptionsGraphics,pWidgetShaderQuality); 

	//vPos.x = 400;
	//vPos.y = 230 + 150;
	vPos.y += 35;
	AddWidgetToState(eMainMenuState_OptionsGraphics,hplNew( cMainMenuWidget_Button,(mpInit,vPos,kTranslate("MainMenu","Advanced"),eMainMenuState_OptionsGraphicsAdvanced,23,eFontAlign_Center)) );

	vPos.y += 35;
	AddWidgetToState(eMainMenuState_OptionsGraphics,hplNew( cMainMenuWidget_GfxBack,(mpInit,vPos,kTranslate("MainMenu","Back"),23,eFontAlign_Center)) ); 


	//Text
	vPos = cVector3f(vTextStart.x+12, vTextStart.y+37, vTextStart.z);

	sprintf(sTempVec,"%d x %d",mpInit->mvScreenSize.x, mpInit->mvScreenSize.y);
	sText = cString::To16Char(sTempVec);
	gpResolutionText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsGraphics,gpResolutionText); 
	gpResolutionText->SetExtraWidget(pWidgetResolution);

	vPos.y += 29;
	sText = mpInit->mpPlayer->GetNoiseFilter()->IsActive() ? kTranslate("MainMenu","On") : kTranslate("MainMenu","Off");
	gpNoiseFilterText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left ));
	AddWidgetToState(eMainMenuState_OptionsGraphics,gpNoiseFilterText); 
	gpNoiseFilterText->SetExtraWidget(pWidgetNoiseFilter);

	vPos.y += 29;
	sText = mpInit->mpGame->GetGraphics()->GetRendererPostEffects()->GetBloomActive() ? kTranslate("MainMenu","On") : 
	kTranslate("MainMenu","Off");
	gpBloomText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsGraphics,gpBloomText);
	gpBloomText->SetExtraWidget(pWidgetBloom);

	vPos.y += 29;
	sprintf(sTempVec,"%.1f",mpInit->mpGame->GetGraphics()->GetLowLevel()->GetGammaCorrection());
	sText = cString::To16Char(sTempVec);
	gpGammaText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsGraphics,gpGammaText);
	gpGammaText->SetExtraWidget(pWidgetGamma);

	vPos.y += 29;
	sText = kTranslate("MainMenu",gvShaderQuality[iMaterial::GetQuality()]);
	gpShaderQualityText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsGraphics,gpShaderQualityText); 
	gpShaderQualityText->SetExtraWidget(pWidgetShaderQuality);

    
	///////////////////////////////////
	// Options Advanced Graphics
	///////////////////////////////////
	vPos = vTextStart + cVector3f(40,0,0);//cVector3f(400, 260, 40);
	//Head
	AddWidgetToState(eMainMenuState_OptionsGraphicsAdvanced,hplNew( cMainMenuWidget_Text,(mpInit,vPos,kTranslate("MainMenu","Advanced Graphics"),25,eFontAlign_Center)) ); 
	vPos.y += 37;
	
	//Buttons
	cMainMenuWidget *pTextureQualityButton = hplNew( cMainMenuWidget_TextureQuality,(mpInit,vPos,kTranslate("MainMenu","Texture Quality:"),20,eFontAlign_Right) );
	AddWidgetToState(eMainMenuState_OptionsGraphicsAdvanced,pTextureQualityButton); 
	vPos.y += 29;
	cMainMenuWidget *pShadowsButton = hplNew( cMainMenuWidget_Shadows,(mpInit,vPos,kTranslate("MainMenu","Shadows:"),20,eFontAlign_Right) );
	AddWidgetToState(eMainMenuState_OptionsGraphicsAdvanced,pShadowsButton); 
	vPos.y += 29;
	cMainMenuWidget *pPostEffectsButton = hplNew( cMainMenuWidget_PostEffects,(mpInit,vPos,kTranslate("MainMenu","Post Effects:"),20,eFontAlign_Right) );
	AddWidgetToState(eMainMenuState_OptionsGraphicsAdvanced,pPostEffectsButton); 
	vPos.y += 29;
	cMainMenuWidget *pMotionBlurButton = hplNew( cMainMenuWidget_MotionBlur,(mpInit,vPos,kTranslate("MainMenu","Motion Blur:"),20,eFontAlign_Right) );
	AddWidgetToState(eMainMenuState_OptionsGraphicsAdvanced,pMotionBlurButton); 
	vPos.y += 29;
	cMainMenuWidget *pVSyncButton = hplNew( cMainMenuWidget_VSync,(mpInit,vPos,kTranslate("MainMenu","VSync:"),20,eFontAlign_Right) );
	AddWidgetToState(eMainMenuState_OptionsGraphicsAdvanced,pVSyncButton); 
	vPos.y += 29;
	cMainMenuWidget *pTextureFilterButton = hplNew( cMainMenuWidget_TextureFilter,(mpInit,vPos,kTranslate("MainMenu","Texture Filter:"),20,eFontAlign_Right) );
	AddWidgetToState(eMainMenuState_OptionsGraphicsAdvanced,pTextureFilterButton); 
	vPos.y += 29;
	cMainMenuWidget *pTextureAnisotropyButton = hplNew( cMainMenuWidget_TextureAnisotropy,(mpInit,vPos,kTranslate("MainMenu","Anisotropy:"),20,eFontAlign_Right) );
	AddWidgetToState(eMainMenuState_OptionsGraphicsAdvanced,pTextureAnisotropyButton); 
	vPos.y += 29;
	cMainMenuWidget *pFSAAButton = hplNew( cMainMenuWidget_FSAA,(mpInit,vPos,kTranslate("MainMenu","Anti-Aliasing:"),20,eFontAlign_Right) );
	AddWidgetToState(eMainMenuState_OptionsGraphicsAdvanced,pFSAAButton); 
	vPos.y += 29;
	cMainMenuWidget *pDOFButton = hplNew( cMainMenuWidget_DOF,(mpInit,vPos,kTranslate("MainMenu","Depth of Field:"),20,eFontAlign_Right) );
	AddWidgetToState(eMainMenuState_OptionsGraphicsAdvanced,pDOFButton); 
	vPos.y += 35;
    AddWidgetToState(eMainMenuState_OptionsGraphicsAdvanced,hplNew( cMainMenuWidget_Button,(mpInit,vPos,kTranslate("MainMenu","Back"),eMainMenuState_OptionsGraphics,23,eFontAlign_Center)) );

	//Text
	vPos = cVector3f(vTextStart.x+12, vTextStart.y+37, vTextStart.z) + cVector3f(40,0,0);

	sText = kTranslate("MainMenu",gvTextureQuality[mpInit->mpGame->GetResources()->GetMaterialManager()->GetTextureSizeLevel()]);
	gpTextureQualityText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsGraphicsAdvanced,gpTextureQualityText); 
	gpTextureQualityText->SetExtraWidget(pTextureQualityButton);

	vPos.y += 29;
	sText = kTranslate("MainMenu",gvShadowTypes[mpInit->mpGame->GetGraphics()->GetRenderer3D()->GetShowShadows()]);
	gpShadowsText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsGraphicsAdvanced,gpShadowsText); 
	gpShadowsText->SetExtraWidget(pShadowsButton);

	vPos.y += 29;
	sText = mpInit->mpGame->GetGraphics()->GetRendererPostEffects()->GetActive() ?	kTranslate("MainMenu","On") : 
																					kTranslate("MainMenu","Off");
	gpPostEffectsText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left ));
	AddWidgetToState(eMainMenuState_OptionsGraphicsAdvanced,gpPostEffectsText);
	gpPostEffectsText->SetExtraWidget(pPostEffectsButton);

	vPos.y += 29;
	sText = mpInit->mpGame->GetGraphics()->GetRendererPostEffects()->GetMotionBlurActive() ? kTranslate("MainMenu","On") : 
	kTranslate("MainMenu","Off");
	gpMotionBlurText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsGraphicsAdvanced,gpMotionBlurText);
	gpMotionBlurText->SetExtraWidget(pMotionBlurButton);

	vPos.y += 29;
	sText = mpInit->mbVsync ? kTranslate("MainMenu","On") : kTranslate("MainMenu","Off");
	gpVSyncText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsGraphicsAdvanced,gpVSyncText);
	gpVSyncText->SetExtraWidget(pVSyncButton);

	vPos.y += 29;
	sText = kTranslate("MainMenu",gvTextureFilter[mpInit->mpGame->GetResources()->GetMaterialManager()->GetTextureFilter()]);
	gpTextureFilterText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsGraphicsAdvanced,gpTextureFilterText);
	gpTextureFilterText->SetExtraWidget(pTextureFilterButton);

	vPos.y += 29;
	int lAniDeg = (int)mpInit->mpGame->GetResources()->GetMaterialManager()->GetTextureAnisotropy();
	if(lAniDeg!=1)	sText = cString::To16Char(cString::ToString(lAniDeg)+"x");
	else			sText = kTranslate("MainMenu","Off");
	gpTextureAnisotropyText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsGraphicsAdvanced,gpTextureAnisotropyText);
	gpTextureAnisotropyText->SetExtraWidget(pTextureAnisotropyButton);

	vPos.y += 29;
	int lFSAA = mpInit->mlFSAA;
	if(lFSAA!=0)	sText = cString::To16Char(cString::ToString(lFSAA)+"x");
	else			sText = kTranslate("MainMenu","Off");
	gpFSAAText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsGraphicsAdvanced,gpFSAAText);
	gpFSAAText->SetExtraWidget(pFSAAButton);

	vPos.y += 29;
	sText = mpInit->mpEffectHandler->GetDepthOfField()->IsDisabled() ? 
		kTranslate("MainMenu","Off") : kTranslate("MainMenu","On");

	gpDoFText = hplNew( cMainMenuWidget_Text,(mpInit,vPos,sText,20,eFontAlign_Left) );
	AddWidgetToState(eMainMenuState_OptionsGraphicsAdvanced,gpDoFText);
	gpDoFText->SetExtraWidget(pDOFButton);


	///////////////////////////////////
	// Graphics Restart
	///////////////////////////////////

	vPos = vTextStart;//cVector3f(400, 260, 40);
	AddWidgetToState(eMainMenuState_GraphicsRestart,hplNew( cMainMenuWidget_Text,(mpInit,vPos,kTranslate("MainMenu","GraphicsRestart"),16,eFontAlign_Center,
						NULL,400)) ); 
	vPos.y += 42;
	AddWidgetToState(eMainMenuState_GraphicsRestart,hplNew( cMainMenuWidget_Button,(mpInit,vPos,kTranslate("MainMenu","OK"),eMainMenuState_Options,22,eFontAlign_Center))); 
	
}

//-----------------------------------------------------------------------

void cMainMenu::AddWidgetToState(eMainMenuState aState, cMainMenuWidget *apWidget)
{
	mlstWidgets.push_back(apWidget);
	mvState[aState].push_back(apWidget);
}

//---------------------------------------------------------------------
