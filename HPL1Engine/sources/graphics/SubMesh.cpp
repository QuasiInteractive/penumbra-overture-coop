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
#include "graphics/SubMesh.h"

#include "graphics/Mesh.h"
#include "resources/MaterialManager.h"
#include "graphics/VertexBuffer.h"
#include "graphics/Material.h"
#include "graphics/Skeleton.h"
#include "graphics/Bone.h"
#include "math/Math.h"

#include "system/MemoryManager.h"
#include "system/LowLevelSystem.h"

#include <string.h>

/* TEMP INSTRUMENTATION (multiplayer ghost models): mirrors GHOST_LOADER_TRACE
   in include/impl/MeshLoaderCollada.h — grep GHOST_LOADER_TRACE, strip both
   together. Set to 0 to silence. */
#ifndef GHOST_LOADER_TRACE
#define GHOST_LOADER_TRACE 1
#endif

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cSubMesh::cSubMesh(const tString &asName, cMaterialManager* apMaterialManager)
	{
		mpMaterialManager = apMaterialManager;

		msName = asName;
		msNodeName = "";

		mpMaterial = NULL;
		mpVtxBuffer = NULL;

		mbDoubleSided = false;

		mpVertexWeights = NULL;
		mpVertexBones = NULL;

		m_mtxLocalTransform = cMatrixf::Identity;

		mbIsOneSided = false;
		mvOneSidedNormal =0;
	}

	//-----------------------------------------------------------------------

	cSubMesh::~cSubMesh()
	{
		if(mpMaterial)mpMaterialManager->Destroy(mpMaterial);
		if(mpVtxBuffer) hplDelete(mpVtxBuffer);

		if(mpVertexBones) hplDeleteArray(mpVertexBones);
		if(mpVertexWeights) hplDeleteArray(mpVertexWeights);
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	void cSubMesh::SetMaterial(iMaterial* apMaterial)
	{
		if(mpMaterial) mpMaterialManager->Destroy(mpMaterial);
		mpMaterial = apMaterial;
	}

	//-----------------------------------------------------------------------

	void cSubMesh::SetVertexBuffer(iVertexBuffer* apVtxBuffer)
	{
		if(mpVtxBuffer == apVtxBuffer) return;

		mpVtxBuffer = apVtxBuffer;
	}

	//-----------------------------------------------------------------------

	iMaterial *cSubMesh::GetMaterial()
	{
		return mpMaterial;
	}

	//-----------------------------------------------------------------------

	iVertexBuffer* cSubMesh::GetVertexBuffer()
	{
		return mpVtxBuffer;
	}

	//-----------------------------------------------------------------------

	void cSubMesh::ResizeVertexBonePairs(int alSize)
	{
		mvVtxBonePairs.reserve(alSize);
	}

	int cSubMesh::GetVertexBonePairNum()
	{
		return (int)mvVtxBonePairs.size();
	}
	cVertexBonePair& cSubMesh::GetVertexBonePair(int alNum)
	{
		return mvVtxBonePairs[alNum];
	}

	void cSubMesh::AddVertexBonePair(const cVertexBonePair &aPair)
	{
		mvVtxBonePairs.push_back(aPair);
	}

	void cSubMesh::ClearVertexBonePairs()
	{
		mvVtxBonePairs.clear();
	}


	//-----------------------------------------------------------------------

	///normalize weights here?
	void cSubMesh::CompileBonePairs()
	{
		const int lVertexNum = mpVtxBuffer->GetVertexNum();

		mpVertexWeights = hplNewArray( float, 4 * lVertexNum);
		mpVertexBones = hplNewArray( unsigned char, 4 * lVertexNum) ;
		memset(mpVertexWeights,0,4 * lVertexNum*sizeof(float));

#if GHOST_LOADER_TRACE
		int lTraceSkippedOutOfRange = 0;
#endif
		int lOverfullPairs = 0; /* pairs dropped because their vertex already has 4 influences */
		for(int i=0; i < (int)mvVtxBonePairs.size(); i++)
		{
			cVertexBonePair &Pair = mvVtxBonePairs[i];

			//A pair pointing outside the vertex buffer would corrupt the heap
			//below — skip it (the mesh validation layers report it later).
			if(Pair.vtxIdx >= (unsigned int)lVertexNum)
			{
#if GHOST_LOADER_TRACE
				lTraceSkippedOutOfRange++;
#endif
				continue;
			}

			float *pWeight = &mpVertexWeights[Pair.vtxIdx*4];
			unsigned char *pBoneIdx = &mpVertexBones[Pair.vtxIdx*4];
			int lPos=-1;
			//Find out where to add the next weight.
			for(int j=0; j<4;j++)
			{
				if(pWeight[j]==0){
					lPos = j;
					break;
				}
			}
			//If no place was found there are too many bones on the vertex.
			//One aggregated warning after the loop — this used to print per
			//PAIR (hundreds of lines on >4-influence skins like Mixamo's).
			if(lPos==-1){
				++lOverfullPairs;
				continue;
			}

			pWeight[lPos] = Pair.weight;
			pBoneIdx[lPos] = Pair.boneIdx;
		}

		if(lOverfullPairs > 0)
			Warning("More than 4 bones on a vertex in sub mesh '%s' of '%s' — %d extra influence(s) dropped (weights renormalize below)\n",
				GetName().c_str(), mpParent ? mpParent->GetName().c_str() : "?", lOverfullPairs);

		bool bUnconnectedVertexes=false;

		//Normalize the weights
		//The original loop had two bugs that corrupted the weights of every
		//vertex FOLLOWING one with exactly 4 influences (the cause of the
		//"exploded vertices" on 4-influence skins like Mixamo exports):
		// 1. 'while(pWeight[lNum]!=0 && lNum<=4)' read pWeight[4], i.e. the
		//    next vertex's first weight, adding it into this vertex's total
		//    (and past the end of the array on the last vertex).
		// 2. The divide used pWeight[lNum] instead of pWeight[i], writing the
		//    next vertex's data instead of normalizing this one.
		for(int vtx =0; vtx < lVertexNum; ++vtx)
		{
			float *pWeight = &mpVertexWeights[vtx*4];

			//check if the vertex is missing bone connection
			if(pWeight[0] == 0)
			{
				bUnconnectedVertexes=true;
				continue;
			}

			float fTotal=0;
			int lNum=0;
			while(lNum<4 && pWeight[lNum]!=0)
			{
				fTotal += pWeight[lNum];
				lNum++;
			}
			if(fTotal > 0)
			{
				for(int i=0; i<lNum; ++i)
				{
					pWeight[i] = pWeight[i] / fTotal;
				}
			}
		}

		if(bUnconnectedVertexes)
		{
			Warning("Some vertices in sub mesh '%s' in mesh '%s' are not connected to a bone!\n",GetName().c_str(), mpParent->GetName().c_str());
		}

#if GHOST_LOADER_TRACE
		{
			//Influence histogram + coverage, to verify the bind path end-to-end.
			int vInfluenceCount[5] = {0,0,0,0,0};
			for(int vtx=0; vtx < lVertexNum; ++vtx)
			{
				const float *pWeight = &mpVertexWeights[vtx*4];
				int lNum=0;
				while(lNum<4 && pWeight[lNum]!=0) lNum++;
				vInfluenceCount[lNum]++;
			}
			Log(" loader-trace: submesh '%s' compiled bone pairs: %d pairs -> %d verts | influences 0:%d 1:%d 2:%d 3:%d 4:%d | %d out-of-range pairs skipped\n",
				GetName().c_str(), (int)mvVtxBonePairs.size(), lVertexNum,
				vInfluenceCount[0],vInfluenceCount[1],vInfluenceCount[2],vInfluenceCount[3],vInfluenceCount[4],
				lTraceSkippedOutOfRange);
		}
#endif
	}

	//-----------------------------------------------------------------------

	void cSubMesh::Compile()
	{
		CheckOneSided();
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PRIAVTE METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	void cSubMesh::CheckOneSided()
	{
		//Log("--- %s\n",GetName().c_str());

		if(mpVtxBuffer==NULL) return;

		int lIdxNum = mpVtxBuffer->GetIndexNum();

		if(lIdxNum > 400*3) return; //Just skip larger buffers for now, they should never be planes.

		unsigned int* pIndices = mpVtxBuffer->GetIndices();
		float *pPositions = mpVtxBuffer->GetArray(eVertexFlag_Position);

		bool bFirst = true;
		cVector3f vNormalSum;
		cVector3f vFirstNormal;
		int vTri[3];
		const int lVtxStride = kvVertexElements[cMath::Log2ToInt(eVertexFlag_Position)];
		float fCount=0;

		for(int i=0; i< lIdxNum; i+=3)
		{
			//Log("%d \n",i);

			vTri[0] = pIndices[i+0];
			vTri[1] = pIndices[i+1];
			vTri[2] = pIndices[i+2];

			const float *pVtx0 = &pPositions[vTri[0]*	lVtxStride];
			const float *pVtx1 = &pPositions[vTri[1]*	lVtxStride];
			const float *pVtx2 = &pPositions[vTri[2]*	lVtxStride];

			cVector3f vEdge1( pVtx1[0] - pVtx0[0], pVtx1[1] - pVtx0[1], pVtx1[2] - pVtx0[2]);
			cVector3f vEdge2( pVtx2[0] - pVtx0[0], pVtx2[1] - pVtx0[1], pVtx2[2] - pVtx0[2]);

			cVector3f vNormal = cMath::Vector3Normalize(cMath::Vector3Cross(vEdge2, vEdge1));

			//Log(" normal: %s\n",vNormal.ToString().c_str());

			if(bFirst)
			{
				bFirst = false;
				vFirstNormal = vNormal;
				vNormalSum = vNormal;
			}
			else
			{
				if(cMath::Vector3Dot(vFirstNormal, vNormal) < 0.9f) return;
				vNormalSum += vNormal;
			}

			fCount += 1;
		}

		mbIsOneSided = true;
		mvOneSidedNormal = cMath::Vector3Normalize(vNormalSum / fCount);
	}

	//-----------------------------------------------------------------------
}
