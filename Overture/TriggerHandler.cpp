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
#include "TriggerHandler.h"

#include "Init.h"
#include "MapHandler.h"
#include "GameEnemy.h"
#include "Triggers.h"
#include "multiplayer/NetworkManager.h"

#include <map>
#include <set>

//////////////////////////////////////////////////////////////////////////
// MULTIPLAYER: GHOST FOOTSTEP TRIGGERS (host only)
//////////////////////////////////////////////////////////////////////////

// Every cGameTrigger_Sound in the game comes from the LOCAL player's
// cPlayer::FootStep, so on the host the enemies were deaf to guests: a dog
// that had not seen anyone yet was pulled toward the host's steps every
// time, arrived, saw the host first — "it only ever targets me". Here the
// host synthesizes the same trigger for each ghost from its wire move state
// (walk / run / crouch = the "sneak" step, still and jump = silent), at the
// ghost's feet, rate-limited per stance, with the very
// player_step_<type>_<material> sound entities the host's own feet use —
// iGameEnemy::HandleSoundTrigger then derives loudness from that .snt exactly
// as for the host (the sneak variant is quieter as authored). Nothing is
// played: this is the enemies' ear, not the host's. Runs only while hosting
// with ghosts present; single-player never reaches it.

namespace
{
	class cNetGhostGroundRayCallback : public iPhysicsRayCallback
	{
	public:
		cNetGhostGroundRayCallback() : mpMaterial(NULL), mfMinDist(9999.0f) {}

		bool OnIntersect(iPhysicsBody *pBody, cPhysicsRayParams *apParams)
		{
			if(pBody->IsCharacter() || pBody->GetCollide()==false) return true;

			iPhysicsMaterial *pMaterial = pBody->GetMaterial();
			if(	pMaterial && pMaterial->GetSurfaceData() &&
				pMaterial->GetSurfaceData()->GetStepType() != "" &&
				apParams->mfDist < mfMinDist)
			{
				mpMaterial = pMaterial;
				mfMinDist = apParams->mfDist;
			}
			return true;
		}

		iPhysicsMaterial *mpMaterial;
		float mfMinDist;
	};

	struct cNetGhostStepState
	{
		cNetGhostStepState() : mbHasPrev(false), mfSinceStep(10.0f), mfMoved(0.0f) {}
		cVector3f mvPrevCam;
		bool mbHasPrev;
		float mfSinceStep; /* seconds since the last emitted step */
		float mfMoved;     /* planar metres walked since it */
	};

	std::map<uint8_t, cNetGhostStepState> gm_mapNetGhostSteps;
	std::set<tString> gm_setNetGhostStepSoundsMissing; /* tried once, logged once */

	/** The step .snt for this stance on this ground, falling back to the
	    rock and generic_hard variants. NULL (after one log line per name)
	    when none of them loads — the step is then silently skipped. */
	cSoundEntityData* NetGhostStepSound(cInit *apInit, const tString &asStepType,
										const tString &asMatStepType)
	{
		cSoundEntityManager *pManager = apInit->mpGame->GetResources()->GetSoundEntityManager();

		tString vMat[3];
		vMat[0] = asMatStepType;
		vMat[1] = "rock";
		vMat[2] = "generic_hard";

		for(int i=0; i<3; ++i)
		{
			if(vMat[i] == "") continue;
			const tString sName = "player_step_" + asStepType + "_" + vMat[i];
			if(gm_setNetGhostStepSoundsMissing.find(sName) != gm_setNetGhostStepSoundsMissing.end())
				continue;

			cSoundEntityData *pData = pManager->CreateSoundEntity(sName);
			if(pData) return pData;

			gm_setNetGhostStepSoundsMissing.insert(sName);
			Log(" multiplayer: guest footstep sound '%s' did not load%s\n", sName.c_str(),
				(i < 2) ? " - using a fallback" : " - guests are silent on this ground");
		}
		return NULL;
	}

	void NetGhostFootsteps_Update(cInit *apInit, cTriggerHandler *apHandler, float afTimeStep)
	{
		cNetworkManager *pNet = apInit->mpNetworkManager;
		if(pNet==NULL || pNet->IsHosting()==false)
		{
			if(gm_mapNetGhostSteps.empty()==false) gm_mapNetGhostSteps.clear();
			return;
		}
		if(	apInit->mpMapHandler==NULL || apInit->mpMapHandler->IsPreUpdating() ||
			apInit->mpMapHandler->IsChangingMap())
		{
			return;
		}
		cWorld3D *pWorld = apInit->mpGame->GetScene()->GetWorld3D();
		if(pWorld==NULL || pWorld->GetPhysicsWorld()==NULL) return;

		std::vector<std::pair<uint8_t, cVector3f> > vGhosts;
		pNet->GetGhostCamPositions(vGhosts);
		if(vGhosts.empty())
		{
			if(gm_mapNetGhostSteps.empty()==false) gm_mapNetGhostSteps.clear();
			return;
		}

		//Forget ghosts that left
		std::map<uint8_t, cNetGhostStepState>::iterator it = gm_mapNetGhostSteps.begin();
		while(it != gm_mapNetGhostSteps.end())
		{
			bool bPresent = false;
			for(size_t g=0; g<vGhosts.size(); ++g)
				if(vGhosts[g].first == it->first) { bPresent = true; break; }
			if(bPresent) ++it;
			else gm_mapNetGhostSteps.erase(it++);
		}

		for(size_t g=0; g<vGhosts.size(); ++g)
		{
			const uint8_t lId = vGhosts[g].first;
			const cVector3f vCam = vGhosts[g].second;

			//v12: a dead guest makes no footsteps. Its stride state is
			//dropped too, so the respawn teleport does not land as one
			//giant step when it comes back.
			float fHealth = 100.0f;
			if(pNet->GetGhostHealth(lId, &fHealth) && fHealth <= 0)
			{
				gm_mapNetGhostSteps.erase(lId);
				continue;
			}
			cNetGhostStepState &state = gm_mapNetGhostSteps[lId];

			state.mfSinceStep += afTimeStep;
			if(state.mbHasPrev)
			{
				cVector3f vA = vCam;            vA.y = 0;
				cVector3f vB = state.mvPrevCam; vB.y = 0;
				state.mfMoved += cMath::Vector3Dist(vA, vB); /* planar */
			}
			state.mvPrevCam = vCam;
			state.mbHasPrev = true;

			uint8_t lMove = (uint8_t)eNetMoveState_Run;
			pNet->GetGhostSense(lId, NULL, &lMove);

			//Stance -> step type (iPlayerMoveState::msStepType names), cadence
			//and the stride the ghost must actually have covered (a guest
			//pushing into a wall in Walk makes no noise, like the host's
			//head-bob driven steps).
			tString sType;
			float fRate, fStride;
			switch(lMove)
			{
			case eNetMoveState_Walk:   sType = "walk";  fRate = 0.45f; fStride = 0.35f; break;
			case eNetMoveState_Run:    sType = "run";   fRate = 0.30f; fStride = 0.50f; break;
			case eNetMoveState_Crouch: sType = "sneak"; fRate = 0.60f; fStride = 0.25f; break;
			default: /* Still, Jump, unknown: silent */
				state.mfMoved = 0.0f;
				continue;
			}
			if(state.mfSinceStep < fRate || state.mfMoved < fStride) continue;
			state.mfSinceStep = 0.0f;
			state.mfMoved = 0.0f;

			//Ground under the ghost: the step material and the exact foot
			//height (a ghost has no body, so cast from just below its camera).
			cNetGhostGroundRayCallback rayCallback;
			const cVector3f vStart = vCam - cVector3f(0, 0.5f, 0);
			const cVector3f vEnd = vStart - cVector3f(0, 2.0f, 0);
			pWorld->GetPhysicsWorld()->CastRay(&rayCallback, vStart, vEnd, true, false, false);

			//No geometry under the ghost (map-transition window, mid-air): no
			//step — exactly like cPlayer::FootStep with an empty step type.
			if(rayCallback.mpMaterial==NULL) continue;
			const tString sMatStepType = rayCallback.mpMaterial->GetSurfaceData()->GetStepType();
			if(sMatStepType=="") continue;
			const cVector3f vFeet = vStart - cVector3f(0, rayCallback.mfMinDist, 0);

			cSoundEntityData *pSoundData = NetGhostStepSound(apInit, sType, sMatStepType);
			if(pSoundData==NULL) continue;

			//Byte-for-byte the host's own trigger (cPlayer::FootStep)
			cGameTrigger_Sound *pSound = hplNew( cGameTrigger_Sound, () );
			pSound->mpSound = pSoundData;
			apHandler->Add(pSound, eGameTriggerType_Sound,
							vFeet + cVector3f(0,0.2f,0),
							10, 1.0f/60.0f, pSoundData->GetMaxDistance());
		}
	}
}

//////////////////////////////////////////////////////////////////////////
// TRIGGER
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cGameTrigger::cGameTrigger() : iEntity3D("")
{
	SetRadius(1);
}

//-----------------------------------------------------------------------

void cGameTrigger::SetRadius(float afX)
{
	mfRadius = afX;
	mBoundingVolume.SetSize(mfRadius*2);
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// CONSTRUCTORS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cTriggerHandler::cTriggerHandler(cInit *apInit)  : iUpdateable("TriggerHandler")
{
	mpInit = apInit;
}

//-----------------------------------------------------------------------

cTriggerHandler::~cTriggerHandler(void)
{
	STLMapDeleteAll(m_mapTriggers);
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// PUBLIC METHODS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cGameTrigger* cTriggerHandler::Add(cGameTrigger *apTrigger,eGameTriggerType aType ,
								   const cVector3f &avLocalPos,
								   int alPrio, float afTime, float afRadius)
{
	apTrigger->SetRadius(afRadius);
	apTrigger->mfTimeCount = afTime;
	apTrigger->mlPrio = alPrio;
	apTrigger->mType = aType;
	apTrigger->SetPosition(avLocalPos);
	
	m_mapTriggers.insert(tGameTriggerMap::value_type(alPrio, apTrigger));

	return apTrigger;
}

//-----------------------------------------------------------------------

void cTriggerHandler::OnStart()
{
	mpMapHandler = mpInit->mpMapHandler;
}

//-----------------------------------------------------------------------

void cTriggerHandler::Update(float afTimeStep)
{
	//////////////////////////////////////////////
	//Multiplayer host: guests' footsteps become sound triggers too
	//(no-op offline / on a guest — see the top of this file)
	NetGhostFootsteps_Update(mpInit, this, afTimeStep);

	//////////////////////////////////////////////
	//Go through the enemies and hand them triggers
	tGameEnemyIterator EnemyIt = mpMapHandler->GetGameEnemyIterator();
	while(EnemyIt.HasNext())
	{
		iGameEnemy *pEnemy = EnemyIt.Next();
		if(pEnemy->IsActive()==false || pEnemy->GetUsesTriggers()==false) continue;
		
		/////////////////////////////////////////
		//Check if it is time to update triggers.
		/*if(pEnemy->GetTriggerUpdateCount() >= pEnemy->GetTriggerUpdateRate())
		{
			pEnemy->SetTriggerUpdateCount(0);
		}
		else
		{
			pEnemy->SetTriggerUpdateCount(pEnemy->GetTriggerUpdateCount() + afTimeStep);
			continue;
		}*/
		
		/////////////////////////////////////
		//Go through triggers by priority
		tGameTriggerMapIt TriggerIt = m_mapTriggers.begin();
		for(; TriggerIt != m_mapTriggers.end(); ++TriggerIt)
		{
			cGameTrigger *pTrigger = TriggerIt->second;

            //Check if trigger is of right type
			if(!(pTrigger->GetType() & pEnemy->GetTriggerTypes()))
			{
				continue;
			}

			//Check if trigger is in reach
			if(cMath::PointBVCollision(pEnemy->GetPosition(),*pTrigger->GetBoundingVolume()) == false)
			{
				continue;
			}
			
			//Let enemy handle trigger, if false get next trigger else next enemy.
			if(pEnemy->HandleTrigger(pTrigger))
			{
				break;
			}
		}
	}
	
	//////////////////////////////////
	//Go through triggers and remove when timer is out.
	tGameTriggerMapIt TriggerIt = m_mapTriggers.begin();
	for(; TriggerIt != m_mapTriggers.end(); )
	{
		cGameTrigger *pTrigger = TriggerIt->second;
		
		pTrigger->mfTimeCount -= afTimeStep;

		if(pTrigger->mfTimeCount <= 0)
		{
			hplDelete( pTrigger );
			m_mapTriggers.erase(TriggerIt++);
		} else {
			++TriggerIt;
		}
	}
}

//-----------------------------------------------------------------------

void cTriggerHandler::Reset()
{

}
