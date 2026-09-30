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

//////////////////////////////////////////////////////////////////////////
// MP3 music streams.
//
// The 2006 Penumbra tech demo (FMOD era) ships its music as .mp3; OALWrapper
// only decodes Ogg Vorbis and WAV. On Windows the MP3 is decoded while it
// plays by Media Foundation (part of Windows, nothing extra to ship) and fed
// to OpenAL through an OALWrapper custom stream, so looping, fading and
// seeking work like any Ogg music.
//////////////////////////////////////////////////////////////////////////

#include "impl/Mp3Stream.h"
#include "system/LowLevelSystem.h"

#ifdef WIN32

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <vector>
#include <string.h>

#include "OALWrapper/OAL_Funcs.h"
#include "OALWrapper/OAL_Buffer.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace hpl {

	static bool gbMediaFoundationUp = false;

	struct cMp3StreamData
	{
		IMFSourceReader *mpReader;
		std::vector<char> mvPending;	/* decoded PCM not yet handed to OpenAL */
		size_t mlPendingPos;
		LONGLONG mlDuration;			/* 100 ns units */
		LONGLONG mlPosition;			/* 100 ns units, start of mvPending */
		int mlBytesPerSec;
		bool mbEnd;
	};

	//-----------------------------------------------------------------------

	static bool Mp3ReadMore(cMp3StreamData *apData)
	{
		apData->mvPending.clear();
		apData->mlPendingPos = 0;
		if (apData->mbEnd) return false;

		for (;;)
		{
			DWORD lFlags = 0;
			LONGLONG lTime = 0;
			IMFSample *pSample = NULL;
			HRESULT hr = apData->mpReader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
													  0, NULL, &lFlags, &lTime, &pSample);
			if (FAILED(hr) || (lFlags & MF_SOURCE_READERF_ENDOFSTREAM))
			{
				if (pSample) pSample->Release();
				apData->mbEnd = true;
				return false;
			}
			if (pSample == NULL) continue; /* a gap or format note: read on */

			IMFMediaBuffer *pBuffer = NULL;
			if (SUCCEEDED(pSample->ConvertToContiguousBuffer(&pBuffer)))
			{
				BYTE *pAudio = NULL;
				DWORD lLen = 0;
				if (SUCCEEDED(pBuffer->Lock(&pAudio, NULL, &lLen)))
				{
					apData->mvPending.assign((char *)pAudio, (char *)pAudio + lLen);
					pBuffer->Unlock();
				}
				pBuffer->Release();
			}
			pSample->Release();
			apData->mlPosition = lTime;
			if (!apData->mvPending.empty()) return true;
		}
	}

	//-----------------------------------------------------------------------

	static bool Mp3Stream(void *apVoid, cOAL_Buffer *apDestBuffer, char *apBuffer, unsigned int alBufferSize, bool &abEOF)
	{
		cMp3StreamData *pData = (cMp3StreamData *)apVoid;
		const double fStartTime = (double)pData->mlPosition / 1.0e7 +
			(pData->mlBytesPerSec > 0 ? (double)pData->mlPendingPos / (double)pData->mlBytesPerSec : 0.0);

		unsigned int lDataSize = 0;
		while (lDataSize < alBufferSize)
		{
			if (pData->mlPendingPos >= pData->mvPending.size())
			{
				if (!Mp3ReadMore(pData)) break;
			}
			size_t lCopy = pData->mvPending.size() - pData->mlPendingPos;
			if (lCopy > alBufferSize - lDataSize) lCopy = alBufferSize - lDataSize;
			memcpy(apBuffer + lDataSize, &pData->mvPending[pData->mlPendingPos], lCopy);
			pData->mlPendingPos += lCopy;
			lDataSize += (unsigned int)lCopy;
		}

		if (lDataSize == 0)
		{
			abEOF = true;
			return false;
		}
		return apDestBuffer->Feed(apBuffer, (int)lDataSize, fStartTime);
	}

	//-----------------------------------------------------------------------

	static void Mp3Seek(void *apVoid, float afWhere, bool abForceRebuffer)
	{
		cMp3StreamData *pData = (cMp3StreamData *)apVoid;
		PROPVARIANT var;
		PropVariantInit(&var);
		var.vt = VT_I8;
		var.hVal.QuadPart = (LONGLONG)((double)afWhere * (double)pData->mlDuration);
		pData->mpReader->SetCurrentPosition(GUID_NULL, var);
		PropVariantClear(&var);
		pData->mvPending.clear();
		pData->mlPendingPos = 0;
		pData->mlPosition = (LONGLONG)((double)afWhere * (double)pData->mlDuration);
		pData->mbEnd = false;
	}

	static double Mp3GetTime(void *apVoid)
	{
		cMp3StreamData *pData = (cMp3StreamData *)apVoid;
		return (double)pData->mlPosition / 1.0e7;
	}

	static void Mp3Destroy(void *apVoid)
	{
		cMp3StreamData *pData = (cMp3StreamData *)apVoid;
		if (pData->mpReader) pData->mpReader->Release();
		delete pData;
	}

	//-----------------------------------------------------------------------

	cOAL_Stream *LoadMp3Stream(const tString &asFile)
	{
		if (!gbMediaFoundationUp)
		{
			CoInitializeEx(NULL, COINIT_MULTITHREADED); /* S_FALSE/RPC_E_CHANGED_MODE are fine */
			if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
			{
				Error("MP3: Media Foundation is not available, cannot play '%s'\n", asFile.c_str());
				return NULL;
			}
			gbMediaFoundationUp = true;
		}

		/* the path to a wide string (the engine's file names are UTF-8 or ANSI) */
		int lWide = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, asFile.c_str(), -1, NULL, 0);
		UINT lCodePage = lWide > 0 ? CP_UTF8 : CP_ACP;
		if (lWide <= 0) lWide = MultiByteToWideChar(CP_ACP, 0, asFile.c_str(), -1, NULL, 0);
		std::vector<wchar_t> vPath(lWide > 0 ? lWide : 1, 0);
		MultiByteToWideChar(lCodePage, 0, asFile.c_str(), -1, &vPath[0], (int)vPath.size());

		IMFSourceReader *pReader = NULL;
		if (FAILED(MFCreateSourceReaderFromURL(&vPath[0], NULL, &pReader)))
		{
			Error("MP3: could not open '%s'\n", asFile.c_str());
			return NULL;
		}

		/* ask for plain 16-bit PCM */
		IMFMediaType *pWant = NULL;
		MFCreateMediaType(&pWant);
		pWant->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		pWant->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
		pWant->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
		HRESULT hr = pReader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, pWant);
		pWant->Release();

		IMFMediaType *pGot = NULL;
		if (FAILED(hr) || FAILED(pReader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &pGot)))
		{
			Error("MP3: could not decode '%s' to PCM\n", asFile.c_str());
			pReader->Release();
			return NULL;
		}
		UINT32 lChannels = 0, lRate = 0;
		pGot->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &lChannels);
		pGot->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &lRate);
		pGot->Release();
		pReader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
		pReader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

		if ((lChannels != 1 && lChannels != 2) || lRate == 0)
		{
			Error("MP3: '%s' has %u channels at %u Hz, not supported\n", asFile.c_str(), lChannels, lRate);
			pReader->Release();
			return NULL;
		}

		LONGLONG lDuration = 0;
		PROPVARIANT var;
		PropVariantInit(&var);
		if (SUCCEEDED(pReader->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var)))
			lDuration = (LONGLONG)var.uhVal.QuadPart;
		PropVariantClear(&var);

		cMp3StreamData *pData = new cMp3StreamData();
		pData->mpReader = pReader;
		pData->mlPendingPos = 0;
		pData->mlDuration = lDuration;
		pData->mlPosition = 0;
		pData->mlBytesPerSec = (int)(lRate * lChannels * 2);
		pData->mbEnd = false;

		tStreamCallbacks callbacks;
		callbacks.Init = NULL;
		callbacks.GetTime = Mp3GetTime;
		callbacks.Seek = Mp3Seek;
		callbacks.Stream = Mp3Stream;
		callbacks.Destroy = Mp3Destroy;

		tStreamInfo info;
		info.channels = (ALint)lChannels;
		info.frequency = (ALint)lRate;
		info.format = lChannels == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;
		info.totalTime = (double)lDuration / 1.0e7;
		info.samples = (long int)(info.totalTime * (double)lRate);

		cOAL_Stream *pStream = OAL_Stream_LoadCustom(callbacks, info, pData);
		if (pStream == NULL)
		{
			Mp3Destroy(pData);
			Error("MP3: OpenAL refused the stream for '%s'\n", asFile.c_str());
		}
		else
			Log(" MP3: streaming '%s' (%u Hz, %u ch, %.0f s)\n", asFile.c_str(), lRate, lChannels, info.totalTime);
		return pStream;
	}
}

#else

namespace hpl {
	cOAL_Stream *LoadMp3Stream(const tString &asFile)
	{
		Error("MP3 music is only supported on Windows: '%s'\n", asFile.c_str());
		return NULL;
	}
}

#endif
