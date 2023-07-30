/**
 * vim: set ts=4 :
 * =============================================================================
 * SourceMod Sample Extension
 * Copyright (C) 2004-2008 AlliedModders LLC.  All rights reserved.
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, AlliedModders LLC gives you permission to link the
 * code of this program (as well as its derivative works) to "Half-Life 2," the
 * "Source Engine," the "SourcePawn JIT," and any Game MODs that run on software
 * by the Valve Corporation.  You must obey the GNU General Public License in
 * all respects for all other code used.  Additionally, AlliedModders LLC grants
 * this exception to all derivative works.  AlliedModders LLC defines further
 * exceptions, found in LICENSE.txt (as of this writing, version JULY-31-2007),
 * or <http://www.sourcemod.net/license.php>.
 *
 * Version: $Id$
 */
// #define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>

#include <iclient.h>
#include <iserver.h>
#include <ISDKTools.h>

#include "CDetour/detours.h"
#include "extension.h"

// voice packets are sent over unreliable netchannel
// #define NET_MAX_DATAGRAM_PAYLOAD	4000	// = maximum unreliable payload size
// voice packetsize = 64 | netchannel overflows at >4000 bytes
// with 22050 samplerate and 512 frames per packet -> 23.22ms per packet
// SVC_VoiceData overhead = 5 bytes
// sensible limit of 8 packets per frame = 552 bytes -> 185.76ms of voice data per frame
#define NET_MAX_VOICE_BYTES_FRAME (8 * (5 + 64))

/**
 * @file extension.cpp
 * @brief Implement extension code here.
 */

template <typename T>
inline T min(T a, T b) { return a < b ? a : b; }

VoiceInjector g_Interface;
SMEXT_LINK(&g_Interface);

CGlobalVars *gpGlobals = NULL;
ISDKTools *g_pSDKTools = NULL;
IServer *iserver = NULL;

double g_fLastVoiceData[SM_MAXPLAYERS + 1];
int g_aFrameVoiceBytes[SM_MAXPLAYERS + 1];

DETOUR_DECL_STATIC4(SV_BroadcastVoiceData, void, IClient *, pClient, int, nBytes, char *, data, int64, xuid)
{
  if (g_Interface.OnBroadcastVoiceData(pClient, nBytes, data))
    DETOUR_STATIC_CALL(SV_BroadcastVoiceData)
    (pClient, nBytes, data, xuid);
}

#ifdef _WIN32
DETOUR_DECL_STATIC2(SV_BroadcastVoiceData_LTCG, void, char *, data, int64, xuid)
{
  IClient *pClient = NULL;
  int nBytes = 0;

  __asm mov pClient, ecx;
  __asm mov nBytes, edx;

  bool ret = g_Interface.OnBroadcastVoiceData(pClient, nBytes, data);

  __asm mov ecx, pClient;
  __asm mov edx, nBytes;

  if (ret)
    DETOUR_STATIC_CALL(SV_BroadcastVoiceData_LTCG)
    (data, xuid);
}
#endif

double getTime()
{
  struct timespec tv;
  if (clock_gettime(CLOCK_REALTIME, &tv) != 0)
    return 0;

  return (tv.tv_sec + (tv.tv_nsec / 1000000000.0));
}

VoiceInjector::VoiceInjector()
{
  m_AvailableTime = 0.0;

  m_pMode = NULL;
  m_pCodec = NULL;

  m_VoiceDetour = NULL;
  m_SV_BroadcastVoiceData = NULL;
}

bool VoiceInjector::SDK_OnLoad(char *error, size_t maxlength, bool late)
{
  // Setup engine-specific data.
  Dl_info info;
  void *engineFactory = (void *)g_SMAPI->GetEngineFactory(false);
  if (dladdr(engineFactory, &info) == 0)
  {
    g_SMAPI->Format(error, maxlength, "dladdr(engineFactory) failed.");
    return false;
  }

  void *pEngineSo = dlopen(info.dli_fname, RTLD_NOW);
  if (pEngineSo == NULL)
  {
    g_SMAPI->Format(error, maxlength, "dlopen(%s) failed.", info.dli_fname);
    return false;
  }

  int engineVersion = g_SMAPI->GetSourceEngineBuild();
  void *adrVoiceData = NULL;

  switch (engineVersion)
  {
  case SOURCE_ENGINE_CSGO:
#ifdef _WIN32
    adrVoiceData = memutils->FindPattern(pEngineSo, "\x55\x8B\xEC\x81\xEC\xD0\x00\x00\x00\x53\x56\x57", 12);
#else
    adrVoiceData = memutils->ResolveSymbol(pEngineSo, "_Z21SV_BroadcastVoiceDataP7IClientiPcx");
#endif
    break;

  case SOURCE_ENGINE_LEFT4DEAD2:
#ifdef _WIN32
    adrVoiceData = memutils->FindPattern(pEngineSo, "\x55\x8B\xEC\x83\xEC\x70\xA1\x2A\x2A\x2A\x2A\x33\xC5\x89\x45\xFC\xA1\x2A\x2A\x2A\x2A\x53\x56", 23);
#else
    adrVoiceData = memutils->ResolveSymbol(pEngineSo, "_Z21SV_BroadcastVoiceDataP7IClientiPcx");
#endif
    break;

  case SOURCE_ENGINE_NUCLEARDAWN:
#ifdef _WIN32
    adrVoiceData = memutils->FindPattern(pEngineSo, "\x55\x8B\xEC\xA1\x2A\x2A\x2A\x2A\x83\xEC\x58\x57\x33\xFF", 14);
#else
    adrVoiceData = memutils->ResolveSymbol(pEngineSo, "_Z21SV_BroadcastVoiceDataP7IClientiPcx");
#endif
    break;

  case SOURCE_ENGINE_INSURGENCY:
#ifdef _WIN32
    adrVoiceData = memutils->FindPattern(pEngineSo, "\x55\x8B\xEC\x83\xEC\x74\x68\x2A\x2A\x2A\x2A\x8D\x4D\xE4\xE8", 15);
#else
    adrVoiceData = memutils->ResolveSymbol(pEngineSo, "_Z21SV_BroadcastVoiceDataP7IClientiPcx");
#endif
    break;

  case SOURCE_ENGINE_TF2:
  case SOURCE_ENGINE_CSS:
  case SOURCE_ENGINE_HL2DM:
  case SOURCE_ENGINE_DODS:
  case SOURCE_ENGINE_SDK2013:
#ifdef _WIN32
    adrVoiceData = memutils->FindPattern(pEngineSo, "\x55\x8B\xEC\xA1\x2A\x2A\x2A\x2A\x83\xEC\x50\x83\x78\x30", 14);
#else
    adrVoiceData = memutils->ResolveSymbol(pEngineSo, "_Z21SV_BroadcastVoiceDataP7IClientiPcx");
#endif
    break;

  default:
    g_SMAPI->Format(error, maxlength, "Unsupported game.");
    dlclose(pEngineSo);
    return false;
  }

  dlclose(pEngineSo);

  m_SV_BroadcastVoiceData = (t_SV_BroadcastVoiceData)adrVoiceData;
  if (!m_SV_BroadcastVoiceData)
  {
    g_SMAPI->Format(error, maxlength, "SV_BroadcastVoiceData sigscan failed.");
    return false;
  }

  // Setup voice detour.
  CDetourManager::Init(g_pSM->GetScriptingEngine(), NULL);

#ifdef _WIN32
  if (engineVersion == SOURCE_ENGINE_CSGO || engineVersion == SOURCE_ENGINE_INSURGENCY)
  {
    m_VoiceDetour = DETOUR_CREATE_STATIC(SV_BroadcastVoiceData_LTCG, adrVoiceData);
  }
  else
  {
    m_VoiceDetour = DETOUR_CREATE_STATIC(SV_BroadcastVoiceData, adrVoiceData);
  }
#else
  m_VoiceDetour = DETOUR_CREATE_STATIC(SV_BroadcastVoiceData, adrVoiceData);
#endif

  if (!m_VoiceDetour)
  {
    g_SMAPI->Format(error, maxlength, "SV_BroadcastVoiceData detour failed.");
    return false;
  }

  m_VoiceDetour->EnableDetour();

  // Encoder settings
  m_EncoderSettings.SampleRate_Hz = 22050;
  m_EncoderSettings.TargetBitRate_Kbps = 64;
  m_EncoderSettings.FrameSize = 512; // samples
  m_EncoderSettings.PacketSize = 64;
  m_EncoderSettings.Complexity = 10; // 0 - 10
  m_EncoderSettings.FrameTime = (double)m_EncoderSettings.FrameSize / (double)m_EncoderSettings.SampleRate_Hz;

  // Init CELT encoder
  int theError;
  m_pMode = celt_mode_create(m_EncoderSettings.SampleRate_Hz, m_EncoderSettings.FrameSize, &theError);
  if (!m_pMode)
  {
    g_SMAPI->Format(error, maxlength, "celt_mode_create error: %d", theError);
    SDK_OnUnload();
    return false;
  }

  m_pCodec = celt_encoder_create_custom(m_pMode, 1, &theError);
  if (!m_pCodec)
  {
    g_SMAPI->Format(error, maxlength, "celt_encoder_create_custom error: %d", theError);
    SDK_OnUnload();
    return false;
  }

  celt_encoder_ctl(m_pCodec, CELT_RESET_STATE_REQUEST, NULL);
  celt_encoder_ctl(m_pCodec, CELT_SET_BITRATE(m_EncoderSettings.TargetBitRate_Kbps * 1000));
  celt_encoder_ctl(m_pCodec, CELT_SET_COMPLEXITY(m_EncoderSettings.Complexity));

  m_LastTimePlayed = getTime();

  return true;
}

bool VoiceInjector::SDK_OnMetamodLoad(ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
  GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
  gpGlobals = ismm->GetCGlobals();
  ConVar_Register(0, this);

  return true;
}

bool VoiceInjector::RegisterConCommandBase(ConCommandBase *pVar)
{
  /* Always call META_REGCVAR instead of going through the engine. */
  return META_REGCVAR(pVar);
}

cell_t IsClientTalking(IPluginContext *pContext, const cell_t *params)
{
  int client = params[1];

  if (client < 1 || client > SM_MAXPLAYERS)
  {
    return pContext->ThrowNativeError("Client index %d is invalid", client);
  }

  double d = gpGlobals->curtime - g_fLastVoiceData[client];

  if (d < 0) // mapchange
    return false;

  if (d > 0.33)
    return false;

  return true;
}

cell_t PlayAudioOnVoiceChat(IPluginContext *pContext, const cell_t *params)
{
  int       clientIndex = params[1];
  ssize_t   samples     = params[2];
  char* pcData;
  pContext->LocalToString(params[3], &pcData);

  int16_t*  pData = (int16_t*)pcData;

  g_Interface.PlayAudio(clientIndex, samples, pData);

  return 0;
}

const sp_nativeinfo_t MyNatives[] =
{
  {"IsClientTalking"      , IsClientTalking     },
  {"PlayAudioOnVoiceChat" , PlayAudioOnVoiceChat},
  {NULL, NULL}
};

void VoiceInjector::SDK_OnAllLoaded()
{
  sharesys->AddNatives(myself, MyNatives);
  sharesys->RegisterLibrary(myself, "VoiceInject");

  SM_GET_LATE_IFACE(SDKTOOLS, g_pSDKTools);
  if (g_pSDKTools == NULL)
  {
    smutils->LogError(myself, "SDKTools interface not found");
    SDK_OnUnload();
    return;
  }

  iserver = g_pSDKTools->GetIServer();
  if (iserver == NULL)
  {
    smutils->LogError(myself, "Failed to get IServer interface from SDKTools!");
    SDK_OnUnload();
    return;
  }
}

void VoiceInjector::SDK_OnUnload()
{
  if (m_VoiceDetour)
  {
    m_VoiceDetour->Destroy();
    m_VoiceDetour = NULL;
  }

  if (m_pCodec)
    celt_encoder_destroy(m_pCodec);

  if (m_pMode)
    celt_mode_destroy(m_pMode);
}

bool VoiceInjector::OnBroadcastVoiceData(IClient *pClient, int nBytes, char *data)
{
  // Reject empty packets
  if (nBytes < 1)
    return false;

  int client = pClient->GetPlayerSlot() + 1;

  // Reject voice packet if we'd send more than NET_MAX_VOICE_BYTES_FRAME voice bytes from this client in the current frame.
  // 5 = SVC_VoiceData header/overhead
  // g_aFrameVoiceBytes[client] += 5 + nBytes;

  // if(g_aFrameVoiceBytes[client] > NET_MAX_VOICE_BYTES_FRAME)
    // return false;

  g_fLastVoiceData[client] = gpGlobals->curtime;

  return true;
}

void VoiceInjector::PlayAudio(int clientIndex, ssize_t samples, int16_t* pData)
{
  int SamplesPerFrame = m_EncoderSettings.FrameSize;
  int PacketSize = m_EncoderSettings.PacketSize;

  // 0 = SourceTV
  IClient *pClient = iserver->GetClient(clientIndex);

  // Encode it!
  unsigned char aFinal[PacketSize];
  size_t FinalSize = 0;

  FinalSize = celt_encode(m_pCodec, pData, SamplesPerFrame, aFinal, sizeof(aFinal));

  if (FinalSize <= 0)
  {
    smutils->LogError(myself, "Compress returned %d\n", FinalSize);
    return;
  }

  // Play the audio Buffer
  BroadcastVoiceData(pClient, FinalSize, aFinal);
}

void VoiceInjector::BroadcastVoiceData(IClient *pClient, int nBytes, unsigned char *pData)
{
#ifdef _WIN32
  __asm mov ecx, pClient;
  __asm mov edx, nBytes;

  DETOUR_STATIC_CALL(SV_BroadcastVoiceData_LTCG)
  ((char *)pData, 0);
#else
  DETOUR_STATIC_CALL(SV_BroadcastVoiceData)
  (pClient, nBytes, (char *)pData, 0);
#endif
}
