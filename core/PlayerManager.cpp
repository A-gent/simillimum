/**
 * vim: set ts=4 sw=4 tw=99 noet :
 * =============================================================================
 * Simillimum
 * Copyright (C) 2004-2009 AlliedModders LLC.  All rights reserved.
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
 * or <http://www.simillimum.net/license.php>.
 *
 * Version: $Id$
 */

#include "PlayerManager.h"
#include "ForwardSys.h"
#include "AdminCache.h"
#include "ConCmdManager.h"
#include "MenuStyle_Valve.h"
#include "MenuStyle_Radio.h"
#include "sm_stringutil.h"
#include "CoreConfig.h"
#include "TimerSys.h"
#include "Logger.h"
#include "ChatTriggers.h"
#include "HalfLife2.h"
#include <inetchannel.h>
#include <iclient.h>
#include <IGameConfigs.h>
#include <simillimum_version.h>
#include "ConsoleDetours.h"
#include "logic_bridge.h"

PlayerManager g_Players;
bool g_OnMapStarted = false;
IForward *PreAdminCheck = NULL;
IForward *PostAdminCheck = NULL;
IForward *PostAdminFilter = NULL;

const unsigned int *g_NumPlayersToAuth = NULL;
int lifestate_offset = -1;
List<ICommandTargetProcessor *> target_processors;

#if SOURCE_ENGINE == SE_DOTA
SH_DECL_HOOK5(IServerGameClients, ClientConnect, SH_NOATTRIB, 0, bool, int, const char *, const char *, char *, int);
SH_DECL_HOOK2_void(IServerGameClients, ClientPutInServer, SH_NOATTRIB, 0, int, const char *);
SH_DECL_HOOK1_void(IServerGameClients, ClientDisconnect, SH_NOATTRIB, 0, int);
SH_DECL_HOOK2_void(IServerGameClients, ClientCommand, SH_NOATTRIB, 0, int, const CCommand &);
SH_DECL_HOOK1_void(IServerGameClients, ClientSettingsChanged, SH_NOATTRIB, 0, int);
#else
SH_DECL_HOOK5(IServerGameClients, ClientConnect, SH_NOATTRIB, 0, bool, edict_t *, const char *, const char *, char *, int);
SH_DECL_HOOK2_void(IServerGameClients, ClientPutInServer, SH_NOATTRIB, 0, edict_t *, const char *);
SH_DECL_HOOK1_void(IServerGameClients, ClientDisconnect, SH_NOATTRIB, 0, edict_t *);
#if SOURCE_ENGINE >= SE_ORANGEBOX
SH_DECL_HOOK2_void(IServerGameClients, ClientCommand, SH_NOATTRIB, 0, edict_t *, const CCommand &);
#else
SH_DECL_HOOK1_void(IServerGameClients, ClientCommand, SH_NOATTRIB, 0, edict_t *);
#endif
SH_DECL_HOOK1_void(IServerGameClients, ClientSettingsChanged, SH_NOATTRIB, 0, edict_t *);
#endif // SE_DOTA

#if SOURCE_ENGINE == SE_DOTA
SH_DECL_HOOK0_void(IServerGameDLL, ServerActivate, SH_NOATTRIB, 0);
#else
SH_DECL_HOOK3_void(IServerGameDLL, ServerActivate, SH_NOATTRIB, 0, edict_t *, int, int);
#endif

#if SOURCE_ENGINE == SE_DOTA
SH_DECL_EXTERN2_void(ConCommand, Dispatch, SH_NOATTRIB, false, void *, const CCommand &);
#elif SOURCE_ENGINE >= SE_ORANGEBOX
SH_DECL_EXTERN1_void(ConCommand, Dispatch, SH_NOATTRIB, false, const CCommand &);
#elif SOURCE_ENGINE == SE_DARKMESSIAH
SH_DECL_EXTERN0_void(ConCommand, Dispatch, SH_NOATTRIB, false);
#else
# if SH_IMPL_VERSION >= 4
 extern int __SourceHook_FHAddConCommandDispatch(void *,bool,class fastdelegate::FastDelegate0<void>);
# else
 extern bool __SourceHook_FHAddConCommandDispatch(void *,bool,class fastdelegate::FastDelegate0<void>);
# endif
extern bool __SourceHook_FHRemoveConCommandDispatch(void *,bool,class fastdelegate::FastDelegate0<void>);
#endif

ConCommand *maxplayersCmd = NULL;

unsigned int g_PlayerSerialCount = 0;

class KickPlayerTimer : public ITimedEvent
{
public:
	ResultType OnTimer(ITimer *pTimer, void *pData)
	{
		int userid = (int)pData;
		int client = g_Players.GetClientOfUserId(userid);
		if (client)
		{
			CPlayer *player = g_Players.GetPlayerByIndex(client);
			player->Kick("Your name is reserved by Simillimum; set your password to use it.");
		}
		return Pl_Stop;
	}
	void OnTimerEnd(ITimer *pTimer, void *pData)
	{
	}
} s_KickPlayerTimer;

PlayerManager::PlayerManager()
{
	m_AuthQueue = NULL;
	m_FirstPass = false;
	m_maxClients = 0;

	m_SourceTVUserId = -1;
	m_ReplayUserId = -1;

	m_bAuthstringValidation = true; // use steam auth by default

	m_UserIdLookUp = new int[USHRT_MAX+1];
	memset(m_UserIdLookUp, 0, sizeof(int) * (USHRT_MAX+1));
}

PlayerManager::~PlayerManager()
{
	g_NumPlayersToAuth = NULL;

	delete [] m_AuthQueue;
	delete [] m_UserIdLookUp;
}

void PlayerManager::OnSimillimumAllInitialized()
{
	SH_ADD_HOOK(IServerGameClients, ClientConnect, serverClients, SH_MEMBER(this, &PlayerManager::OnClientConnect), false);
	SH_ADD_HOOK(IServerGameClients, ClientConnect, serverClients, SH_MEMBER(this, &PlayerManager::OnClientConnect_Post), true);
	SH_ADD_HOOK(IServerGameClients, ClientPutInServer, serverClients, SH_MEMBER(this, &PlayerManager::OnClientPutInServer), true);
	SH_ADD_HOOK(IServerGameClients, ClientDisconnect, serverClients, SH_MEMBER(this, &PlayerManager::OnClientDisconnect), false);
	SH_ADD_HOOK(IServerGameClients, ClientDisconnect, serverClients, SH_MEMBER(this, &PlayerManager::OnClientDisconnect_Post), true);
	SH_ADD_HOOK(IServerGameClients, ClientCommand, serverClients, SH_MEMBER(this, &PlayerManager::OnClientCommand), false);
	SH_ADD_HOOK(IServerGameClients, ClientSettingsChanged, serverClients, SH_MEMBER(this, &PlayerManager::OnClientSettingsChanged), true);
	SH_ADD_HOOK(IServerGameDLL, ServerActivate, gamedll, SH_MEMBER(this, &PlayerManager::OnServerActivate), true);

	sharesys->AddInterface(NULL, this);

	ParamType p1[] = {Param_Cell, Param_String, Param_Cell};
	ParamType p2[] = {Param_Cell};

	m_clconnect = g_Forwards.CreateForward("OnClientConnect", ET_LowEvent, 3, p1);
	m_clconnect_post = g_Forwards.CreateForward("OnClientConnected", ET_Ignore, 1, p2);
	m_clputinserver = g_Forwards.CreateForward("OnClientPutInServer", ET_Ignore, 1, p2);
	m_cldisconnect = g_Forwards.CreateForward("OnClientDisconnect", ET_Ignore, 1, p2);
	m_cldisconnect_post = g_Forwards.CreateForward("OnClientDisconnect_Post", ET_Ignore, 1, p2);
	m_clcommand = g_Forwards.CreateForward("OnClientCommand", ET_Hook, 2, NULL, Param_Cell, Param_Cell);
	m_clinfochanged = g_Forwards.CreateForward("OnClientSettingsChanged", ET_Ignore, 1, p2);
	m_clauth = g_Forwards.CreateForward("OnClientAuthorized", ET_Ignore, 2, NULL, Param_Cell, Param_String);
	m_onActivate = g_Forwards.CreateForward("OnServerLoad", ET_Ignore, 0, NULL);
	m_onActivate2 = g_Forwards.CreateForward("OnMapStart", ET_Ignore, 0, NULL);

	PreAdminCheck = g_Forwards.CreateForward("OnClientPreAdminCheck", ET_Event, 1, p1);
	PostAdminCheck = g_Forwards.CreateForward("OnClientPostAdminCheck", ET_Ignore, 1, p1);
	PostAdminFilter = g_Forwards.CreateForward("OnClientPostAdminFilter", ET_Ignore, 1, p1);

	m_bIsListenServer = !engine->IsDedicatedServer();
	m_ListenClient = 0;

	ConCommand *pCmd = FindCommand("maxplayers");
	if (pCmd != NULL)
	{
		SH_ADD_HOOK(ConCommand, Dispatch, pCmd, SH_STATIC(CmdMaxplayersCallback), true);
		maxplayersCmd = pCmd;
	}
}

void PlayerManager::OnSimillimumShutdown()
{
	SH_REMOVE_HOOK(IServerGameClients, ClientConnect, serverClients, SH_MEMBER(this, &PlayerManager::OnClientConnect), false);
	SH_REMOVE_HOOK(IServerGameClients, ClientConnect, serverClients, SH_MEMBER(this, &PlayerManager::OnClientConnect_Post), true);
	SH_REMOVE_HOOK(IServerGameClients, ClientPutInServer, serverClients, SH_MEMBER(this, &PlayerManager::OnClientPutInServer), true);
	SH_REMOVE_HOOK(IServerGameClients, ClientDisconnect, serverClients, SH_MEMBER(this, &PlayerManager::OnClientDisconnect), false);
	SH_REMOVE_HOOK(IServerGameClients, ClientDisconnect, serverClients, SH_MEMBER(this, &PlayerManager::OnClientDisconnect_Post), true);
	SH_REMOVE_HOOK(IServerGameClients, ClientCommand, serverClients, SH_MEMBER(this, &PlayerManager::OnClientCommand), false);
	SH_REMOVE_HOOK(IServerGameClients, ClientSettingsChanged, serverClients, SH_MEMBER(this, &PlayerManager::OnClientSettingsChanged), true);
	SH_REMOVE_HOOK(IServerGameDLL, ServerActivate, gamedll, SH_MEMBER(this, &PlayerManager::OnServerActivate), true);

	/* Release forwards */
	g_Forwards.ReleaseForward(m_clconnect);
	g_Forwards.ReleaseForward(m_clconnect_post);
	g_Forwards.ReleaseForward(m_clputinserver);
	g_Forwards.ReleaseForward(m_cldisconnect);
	g_Forwards.ReleaseForward(m_cldisconnect_post);
	g_Forwards.ReleaseForward(m_clcommand);
	g_Forwards.ReleaseForward(m_clinfochanged);
	g_Forwards.ReleaseForward(m_clauth);
	g_Forwards.ReleaseForward(m_onActivate);
	g_Forwards.ReleaseForward(m_onActivate2);

	g_Forwards.ReleaseForward(PreAdminCheck);
	g_Forwards.ReleaseForward(PostAdminCheck);
	g_Forwards.ReleaseForward(PostAdminFilter);

	delete [] m_Players;

	if (maxplayersCmd != NULL)
	{
		SH_REMOVE_HOOK(ConCommand, Dispatch, maxplayersCmd, SH_STATIC(CmdMaxplayersCallback), true);
	}
}

ConfigResult PlayerManager::OnSimillimumConfigChanged(const char *key, 
												 const char *value, 
												 ConfigSource source, 
												 char *error, 
												 size_t maxlength)
{
	if (strcmp(key, "PassInfoVar") == 0)
	{
		if (strcmp(value, "_password") != 0)
		{
			m_PassInfoVar.assign(value);
		}
		return ConfigResult_Accept;
	} else if (strcmp(key, "AllowClLanguageVar") == 0) {
		if (strcasecmp(value, "on") == 0)
		{
			m_QueryLang = true;
		} else if (strcasecmp(value, "off") == 0) {
			m_QueryLang = false;
		} else {
			UTIL_Format(error, maxlength, "Invalid value: must be \"on\" or \"off\"");
			return ConfigResult_Reject;
		}
		return ConfigResult_Accept;
	} else if (strcmp( key, "SteamAuthstringValidation" ) == 0) {
		if (strcasecmp(value, "yes") == 0)
		{
			m_bAuthstringValidation = true;
		} else if ( strcasecmp(value, "no") == 0) {
			m_bAuthstringValidation = false;
		} else {
			UTIL_Format(error, maxlength, "Invalid value: must be \"yes\" or \"no\"");
			return ConfigResult_Reject;
		}
		return ConfigResult_Accept;
	}
	return ConfigResult_Ignore;
}

#if SOURCE_ENGINE == SE_DOTA
void PlayerManager::OnServerActivate()
#else
void PlayerManager::OnServerActivate(edict_t *pEdictList, int edictCount, int clientMax)
#endif
{
	static ConVar *tv_enable = icvar->FindVar("tv_enable");
#if SOURCE_ENGINE == SE_ORANGEBOXVALVE
	static ConVar *replay_enable = icvar->FindVar("replay_enable");
#endif

	// clientMax will not necessarily be correct here (such as on late SourceTV enable)
	m_maxClients = gpGlobals->maxClients;

	ICommandLine *commandLine = g_HL2.GetValveCommandLine();
	m_bIsSourceTVActive = (tv_enable && tv_enable->GetBool() && (!commandLine || commandLine->FindParm("-nohltv") == 0));
	m_bIsReplayActive = false;
#if SOURCE_ENGINE == SE_ORANGEBOXVALVE
	m_bIsReplayActive = (replay_enable && replay_enable->GetBool());
#endif
	m_PlayersSinceActive = 0;
	
	if (!m_FirstPass)
	{
		/* Initialize all players */

		m_PlayerCount = 0;
		m_Players = new CPlayer[ABSOLUTE_PLAYER_LIMIT + 1];
		m_AuthQueue = new unsigned int[ABSOLUTE_PLAYER_LIMIT + 1];
		m_FirstPass = true;

		memset(m_AuthQueue, 0, sizeof(unsigned int) * (ABSOLUTE_PLAYER_LIMIT + 1));

		g_NumPlayersToAuth = &m_AuthQueue[0];
	}

	scripts->SyncMaxClients(m_maxClients);

	g_OnMapStarted = true;

#if SOURCE_ENGINE == SE_DOTA
	extsys->CallOnCoreMapStart(gpGlobals->pEdicts, gpGlobals->maxEntities, gpGlobals->maxClients);
#else
	extsys->CallOnCoreMapStart(pEdictList, edictCount, m_maxClients);
#endif

	m_onActivate->Execute(NULL);
	m_onActivate2->Execute(NULL);

	List<IClientListener *>::iterator iter;
	for (iter = m_hooks.begin(); iter != m_hooks.end(); iter++)
	{
		if ((*iter)->GetClientListenerVersion() >= 5)
		{
			(*iter)->OnServerActivated(m_maxClients);
		}
	}

	SMGlobalClass *cls = SMGlobalClass::head;
	while (cls)
	{
		cls->OnSimillimumLevelActivated();
		cls = cls->m_pGlobalClassNext;
	}

	SM_ExecuteAllConfigs();
}

bool PlayerManager::IsServerActivated()
{
	return m_FirstPass;
}

bool PlayerManager::CheckSetAdmin(int index, CPlayer *pPlayer, AdminId id)
{
	const char *password = g_Admins.GetAdminPassword(id);
	if (password != NULL)
	{
		if (m_PassInfoVar.size() < 1)
		{
			return false;
		}

		/* Whoa... the user needs a password! */
		const char *given = engine->GetClientConVarValue(index, m_PassInfoVar.c_str());
		if (!given || strcmp(given, password) != 0)
		{
			return false;
		}
	}

	pPlayer->SetAdminId(id, false);

	return true;
}

bool PlayerManager::CheckSetAdminName(int index, CPlayer *pPlayer, AdminId id)
{
	const char *password = g_Admins.GetAdminPassword(id);
	if (password == NULL)
	{
		return false;
	}

	if (m_PassInfoVar.size() < 1)
	{
		return false;
	}

	/* Whoa... the user needs a password! */
	const char *given = engine->GetClientConVarValue(index, m_PassInfoVar.c_str());
	if (!given || strcmp(given, password) != 0)
	{
		return false;
	}

	pPlayer->SetAdminId(id, false);

	return true;
}

void PlayerManager::RunAuthChecks()
{
	CPlayer *pPlayer;
	const char *authstr;
	unsigned int removed = 0;
	for (unsigned int i=1; i<=m_AuthQueue[0]; i++)
	{
		pPlayer = &m_Players[m_AuthQueue[i]];
#if SOURCE_ENGINE == SE_DOTA
		authstr = engine->GetPlayerNetworkIDString(pPlayer->m_iIndex - 1);
#else
		authstr = engine->GetPlayerNetworkIDString(pPlayer->m_pEdict);
#endif
		pPlayer->SetAuthString(authstr);

		if (!pPlayer->IsAuthStringValidated())
		{
			continue; // we're using steam auth, and steam doesn't know about this player yet so we can't do anything about them for now
		}

		if (authstr && authstr[0] != '\0'
			&& (strcmp(authstr, "STEAM_ID_PENDING") != 0))
		{
			/* Set authorization */
			pPlayer->Authorize();

			/* Mark as removed from queue */
			unsigned int client = m_AuthQueue[i];
			m_AuthQueue[i] = 0;
			removed++;

			/* Send to extensions */
			List<IClientListener *>::iterator iter;
			IClientListener *pListener;
			for (iter=m_hooks.begin(); iter!=m_hooks.end(); iter++)
			{
				pListener = (*iter);
				pListener->OnClientAuthorized(client, authstr);
				if (!pPlayer->IsConnected())
				{
					break;
				}
			}

			/* Send to plugins if player is still connected */
			if (pPlayer->IsConnected() && m_clauth->GetFunctionCount())
			{
				/* :TODO: handle the case of a player disconnecting in the middle */
				m_clauth->PushCell(client);
				m_clauth->PushString(authstr);
				m_clauth->Execute(NULL);
			}

			if (pPlayer->IsConnected())
			{
				pPlayer->Authorize_Post();
			}
		}
	}

	/* Clean up the queue */
	if (removed)
	{
		/* We don't have to compact the list if it's empty */
		if (removed != m_AuthQueue[0])
		{
			unsigned int diff = 0;
			for (unsigned int i=1; i<=m_AuthQueue[0]; i++)
			{
				/* If this member is removed... */
				if (m_AuthQueue[i] == 0)
				{
					/* Increase the differential */
					diff++;
				} else {
					/* diff cannot increase faster than i+1 */
					assert(i > diff);
					assert(i - diff >= 1);
					/* move this index down */
					m_AuthQueue[i - diff] = m_AuthQueue[i];
				}
			}
			m_AuthQueue[0] -= removed;
		} else {
			m_AuthQueue[0] = 0;
		}
	}
}

#if SOURCE_ENGINE == SE_DOTA
bool PlayerManager::OnClientConnect(int client, const char *pszName, const char *pszAddress, char *reject, int maxrejectlen)
{
	edict_t *pEntity = PEntityOfEntIndex(client);
#else
bool PlayerManager::OnClientConnect(edict_t *pEntity, const char *pszName, const char *pszAddress, char *reject, int maxrejectlen)
{
	int client = IndexOfEdict(pEntity);
#endif
	CPlayer *pPlayer = &m_Players[client];
	++m_PlayersSinceActive;

	pPlayer->Initialize(pszName, pszAddress, pEntity);
	
	/* Get the client's language */
	if (m_QueryLang)
	{
		const char *name;
		if (!pPlayer->IsFakeClient() && (name=engine->GetClientConVarValue(client, "cl_language")))
		{
			unsigned int langid;
			pPlayer->m_LangId = (translator->GetLanguageByName(name, &langid)) ? langid : translator->GetServerLanguage();
		} else {
			pPlayer->m_LangId = translator->GetServerLanguage();
		}
	}
	
	List<IClientListener *>::iterator iter;
	IClientListener *pListener = NULL;
	for (iter=m_hooks.begin(); iter!=m_hooks.end(); iter++)
	{
		pListener = (*iter);
		if (!pListener->InterceptClientConnect(client, reject, maxrejectlen))
		{
			RETURN_META_VALUE(MRES_SUPERCEDE, false);
		}
	}

	cell_t res = 1;

	m_clconnect->PushCell(client);
	m_clconnect->PushStringEx(reject, maxrejectlen, SM_PARAM_STRING_UTF8 | SM_PARAM_STRING_COPY, SM_PARAM_COPYBACK);
	m_clconnect->PushCell(maxrejectlen);
	m_clconnect->Execute(&res);

	if (res)
	{
		if (!pPlayer->IsAuthorized())
		{
			m_AuthQueue[++m_AuthQueue[0]] = client;
		}

		m_UserIdLookUp[GetPlayerUserId(pEntity)] = client;
	}
	else
	{
		if (!pPlayer->IsFakeClient())
		{
			RETURN_META_VALUE(MRES_SUPERCEDE, false);
		}
	}

	return true;
}

#if SOURCE_ENGINE == SE_DOTA
bool PlayerManager::OnClientConnect_Post(int client, const char *pszName, const char *pszAddress, char *reject, int maxrejectlen)
{
	edict_t *pEntity = PEntityOfEntIndex(client);
#else
bool PlayerManager::OnClientConnect_Post(edict_t *pEntity, const char *pszName, const char *pszAddress, char *reject, int maxrejectlen)
{
	int client = IndexOfEdict(pEntity);
#endif

	bool orig_value = META_RESULT_ORIG_RET(bool);
	CPlayer *pPlayer = &m_Players[client];

	if (orig_value)
	{
		List<IClientListener *>::iterator iter;
		IClientListener *pListener = NULL;
		for (iter=m_hooks.begin(); iter!=m_hooks.end(); iter++)
		{
			pListener = (*iter);
			pListener->OnClientConnected(client);
			if (!pPlayer->IsConnected())
			{
				return true;
			}
		}

		if (!pPlayer->IsFakeClient() 
			&& m_bIsListenServer
			&& strncmp(pszAddress, "127.0.0.1", 9) == 0)
		{
			m_ListenClient = client;
		}

		cell_t res;
		m_clconnect_post->PushCell(client);
		m_clconnect_post->Execute(&res, NULL);
	}
	else
	{
		InvalidatePlayer(pPlayer);
	}

	return true;
}

#if SOURCE_ENGINE == SE_DOTA
void PlayerManager::OnClientPutInServer(int client, const char *playername)
{
	edict_t *pEntity = PEntityOfEntIndex(client);
#else
void PlayerManager::OnClientPutInServer(edict_t *pEntity, const char *playername)
{
	int client = IndexOfEdict(pEntity);
#endif

	cell_t res;
	CPlayer *pPlayer = &m_Players[client];

	/* If they're not connected, they're a bot */
	if (!pPlayer->IsConnected())
	{
		/* Run manual connection routines */
		char error[255];
		const char *authid;
#if SOURCE_ENGINE == SE_DOTA
		authid = engine->GetPlayerNetworkIDString(client - 1);
#else
		authid = engine->GetPlayerNetworkIDString(pEntity);
#endif
		pPlayer->SetAuthString(authid);
		pPlayer->Authorize();
		pPlayer->m_bFakeClient = true;

		/*
		 * While we're already filtered to just bots, we'll do other checks to
		 * make sure that the requisite services are enabled and that the bots
		 * have joined at the expected time.
		 *
		 * Checking playerinfo's IsHLTV and IsReplay would be better and less
		 * error-prone but will always show false until later in the frame,
		 * after PutInServer and Activate, and we want it now!
		 *
		 * These checks are hairy as hell due to differences between engines and games.
		 * 
		 * Most engines use "Replay" and "SourceTV" as bot names for these when they're
		 * created. EP2V, CSS and Nuclear Dawn (but not L4D2) differ from this by now using
		 * replay_/tv_name directly when creating the bot(s). To complicate it slightly
		 * further, the cvar can be empty and the engine's fallback to "unnamed" will be used.
		 * We can maybe just rip out the name checks at some point and rely solely on whether
		 * they're enabled and the join order.
		 */
		
		// This doesn't actually get incremented until OnClientConnect. Fake it to check.
		int newCount = m_PlayersSinceActive + 1;

		int userId = GetPlayerUserId(pEntity);
#if (SOURCE_ENGINE == SE_ORANGEBOXVALVE || SOURCE_ENGINE == SE_CSS || SOURCE_ENGINE == SE_LEFT4DEAD2)
		static ConVar *tv_name = icvar->FindVar("tv_name");
#endif
#if SOURCE_ENGINE == SE_ORANGEBOXVALVE
		static ConVar *replay_name = icvar->FindVar("replay_name");
#endif
#if SOURCE_ENGINE == SE_LEFT4DEAD2
		static bool bIsNuclearDawn = (strcmp(g_Simillimum.GetGameFolderName(), "nucleardawn") == 0);
#endif

#if SOURCE_ENGINE == SE_ORANGEBOXVALVE
		if (m_bIsReplayActive && newCount == 1
			&& (m_ReplayUserId == userId
				|| (replay_name && strcmp(playername, replay_name->GetString()) == 0) || (replay_name && replay_name->GetString()[0] == 0 && strcmp(playername, "unnamed") == 0)
				)
			)
		{
			pPlayer->m_bIsReplay = true;
			m_ReplayUserId = userId;
		}
#endif

		if (m_bIsSourceTVActive
			&& ((!m_bIsReplayActive && newCount == 1)
				|| (m_bIsReplayActive && newCount == 2))
			&& (m_SourceTVUserId == userId
#if SOURCE_ENGINE == SE_CSGO
				|| strcmp(playername, "GOTV") == 0
#elif (SOURCE_ENGINE == SE_ORANGEBOXVALVE || SOURCE_ENGINE == SE_CSS || SOURCE_ENGINE == SE_LEFT4DEAD2)
#if SOURCE_ENGINE == SE_LEFT4DEAD2
				|| (bIsNuclearDawn && ( true
#endif // SE_LEFT4DEAD2
				|| (tv_name && strcmp(playername, tv_name->GetString()) == 0) || (tv_name && tv_name->GetString()[0] == 0 && strcmp(playername, "unnamed") == 0)
#if SOURCE_ENGINE == SE_LEFT4DEAD2
					))
				|| (!bIsNuclearDawn && strcmp(playername, "SourceTV") == 0)
#endif // SE_LEFT4DEAD2
#else
				|| strcmp(playername, "SourceTV") == 0
#endif
				)
			)
		{
			pPlayer->m_bIsSourceTV = true;
			m_SourceTVUserId = userId;
		}

#if SOURCE_ENGINE == SE_DOTA
		if (!OnClientConnect(client, playername, "127.0.0.1", error, sizeof(error)))
#else
		if (!OnClientConnect(pEntity, playername, "127.0.0.1", error, sizeof(error)))
#endif
		{
			/* :TODO: kick the bot if it's rejected */
			return;
		}
		List<IClientListener *>::iterator iter;
		IClientListener *pListener = NULL;
		for (iter=m_hooks.begin(); iter!=m_hooks.end(); iter++)
		{
			pListener = (*iter);
			pListener->OnClientConnected(client);
			/* See if bot was kicked */
			if (!pPlayer->IsConnected())
			{
				return;
			}
		}

		cell_t res;
		m_clconnect_post->PushCell(client);
		m_clconnect_post->Execute(&res, NULL);

		/* Now do authorization */
		for (iter=m_hooks.begin(); iter!=m_hooks.end(); iter++)
		{
			pListener = (*iter);
			pListener->OnClientAuthorized(client, authid);
		}
		/* Finally, tell plugins */
		if (m_clauth->GetFunctionCount())
		{
			m_clauth->PushCell(client);
			m_clauth->PushString(authid);
			m_clauth->Execute(NULL);
		}
		pPlayer->Authorize_Post();
	}

	if (playerinfo)
	{
		pPlayer->m_Info = playerinfo->GetPlayerInfo(pEntity);
	}

	pPlayer->Connect();
	m_PlayerCount++;

	List<IClientListener *>::iterator iter;
	IClientListener *pListener = NULL;
	for (iter=m_hooks.begin(); iter!=m_hooks.end(); iter++)
	{
		pListener = (*iter);
		pListener->OnClientPutInServer(client);
	}

	m_clputinserver->PushCell(client);
	m_clputinserver->Execute(&res, NULL);

	if (pPlayer->IsAuthorized())
	{
		pPlayer->DoPostConnectAuthorization();
	}
}

void PlayerManager::OnSimillimumLevelEnd()
{
	/* Disconnect all bots still in game */
	for (int i=1; i<=m_maxClients; i++)
	{
		if (m_Players[i].IsConnected())
		{
#if SOURCE_ENGINE == SE_DOTA
			OnClientDisconnect(m_Players[i].GetIndex());
#else
			OnClientDisconnect(m_Players[i].GetEdict());
#endif
		}
	}
	m_PlayerCount = 0;
}

#if SOURCE_ENGINE == SE_DOTA
void PlayerManager::OnClientDisconnect(int client)
{
	edict_t *pEntity = PEntityOfEntIndex(client);
#else
void PlayerManager::OnClientDisconnect(edict_t *pEntity)
{
	int client = IndexOfEdict(pEntity);
#endif

	cell_t res;
	CPlayer *pPlayer = &m_Players[client];

	if (pPlayer->IsConnected())
	{
		m_cldisconnect->PushCell(client);
		m_cldisconnect->Execute(&res, NULL);
	}
	else
	{
		/* We don't care, prevent a double call */
		return;
	}

	if (pPlayer->WasCountedAsInGame())
	{
		m_PlayerCount--;
	}

	List<IClientListener *>::iterator iter;
	IClientListener *pListener = NULL;
	for (iter=m_hooks.begin(); iter!=m_hooks.end(); iter++)
	{
		pListener = (*iter);
		pListener->OnClientDisconnecting(client);
	}

	InvalidatePlayer(pPlayer);

	if (m_ListenClient == client)
	{
		m_ListenClient = 0;
	}
}

#if SOURCE_ENGINE == SE_DOTA
void PlayerManager::OnClientDisconnect_Post(int client)
{
	edict_t *pEntity = PEntityOfEntIndex(client);
#else
void PlayerManager::OnClientDisconnect_Post(edict_t *pEntity)
{
	int client = IndexOfEdict(pEntity);
#endif

	cell_t res;

	m_cldisconnect_post->PushCell(client);
	m_cldisconnect_post->Execute(&res, NULL);

	List<IClientListener *>::iterator iter;
	IClientListener *pListener = NULL;
	for (iter=m_hooks.begin(); iter!=m_hooks.end(); iter++)
	{
		pListener = (*iter);
		pListener->OnClientDisconnected(client);
	}
}

void ClientConsolePrint(edict_t *e, const char *fmt, ...)
{
	char buffer[512];

	va_list ap;
	va_start(ap, fmt);
	size_t len = vsnprintf(buffer, sizeof(buffer), fmt, ap);
	va_end(ap);

	if (len >= sizeof(buffer) - 1)
	{
		buffer[sizeof(buffer) - 2] = '\n';
		buffer[sizeof(buffer) - 1] = '\0';
	} else {
		buffer[len++] = '\n';
		buffer[len] = '\0';
	}

#if SOURCE_ENGINE == SE_DOTA
	engine->ClientPrintf(IndexOfEdict(e), buffer);
#else
	engine->ClientPrintf(e, buffer);
#endif
}

void ListExtensionsToClient(CPlayer *player, const CCommand &args)
{
	char buffer[256];
	unsigned int id = 0;
	unsigned int start = 0;

	AutoExtensionList extensions(extsys);
	if (!extensions->size())
	{
		ClientConsolePrint(player->GetEdict(), "[SM] No extensions found.");
		return;
	}

	if (args.ArgC() > 2)
	{
		start = atoi(args.Arg(2));
	}

	size_t i = 0;
	for (; i < extensions->size(); i++)
	{
		IExtension *ext = extensions->at(i);

		char error[255];
		if (!ext->IsRunning(error, sizeof(error)))
		{
			continue;
		}

		id++;
		if (id < start)
		{
			continue;
		}

		if (id - start > 10)
		{
			break;
		}

		IExtensionInterface *api = ext->GetAPI();

		const char *name = api->GetExtensionName();
		const char *version = api->GetExtensionVerString();
		const char *author = api->GetExtensionAuthor();
		const char *description = api->GetExtensionDescription();

		size_t len = UTIL_Format(buffer, sizeof(buffer), " \"%s\"", name);

		if (version != NULL && version[0])
		{
			len += UTIL_Format(&buffer[len], sizeof(buffer)-len, " (%s)", version);
		}

		if (author != NULL && author[0])
		{
			len += UTIL_Format(&buffer[len], sizeof(buffer)-len, " by %s", author);
		}

		if (description != NULL && description[0])
		{
			len += UTIL_Format(&buffer[len], sizeof(buffer)-len, ": %s", description);
		}


		ClientConsolePrint(player->GetEdict(), "%s", buffer);
	}

	for (; i < extensions->size(); i++)
	{
		char error[255];
		if (extensions->at(i)->IsRunning(error, sizeof(error)))
		{
			break;
		}
	}

	if (i < extensions->size())
	{
		ClientConsolePrint(player->GetEdict(), "To see more, type \"sm exts %d\"", id);
	}
}

void ListPluginsToClient(CPlayer *player, const CCommand &args)
{
	char buffer[256];
	unsigned int id = 0;
	edict_t *e = player->GetEdict();
	unsigned int start = 0;

	AutoPluginList plugins(scripts);
	if (!plugins->size())
	{
		ClientConsolePrint(e, "[SM] No plugins found.");
		return;
	}

	if (args.ArgC() > 2)
	{
		start = atoi(args.Arg(2));
	}

	SourceHook::List<SMPlugin *> m_FailList;

	size_t i = 0;
	for (; i < plugins->size(); i++)
	{
		SMPlugin *pl = plugins->at(i);

		if (pl->GetStatus() != Plugin_Running)
		{
			continue;
		}

		/* Count valid plugins */
		id++;
		if (id < start)
		{
			continue;
		}

		if (id - start > 10)
		{
			break;
		}

		size_t len;
		const sm_plugininfo_t *info = pl->GetPublicInfo();
		len = UTIL_Format(buffer, sizeof(buffer), " \"%s\"", (IS_STR_FILLED(info->name)) ? info->name : pl->GetFilename());
		if (IS_STR_FILLED(info->version))
		{
			len += UTIL_Format(&buffer[len], sizeof(buffer)-len, " (%s)", info->version);
		}
		if (IS_STR_FILLED(info->author))
		{
			UTIL_Format(&buffer[len], sizeof(buffer)-len, " by %s", info->author);
		}
		else
		{
			UTIL_Format(&buffer[len], sizeof(buffer)-len, " %s", pl->GetFilename());
		}
		ClientConsolePrint(e, "%s", buffer);
	}

	/* See if we can get more plugins */
	for (; i < plugins->size(); i++)
	{
		if (plugins->at(i)->GetStatus() == Plugin_Running)
		{
			break;
		}
	}

	/* Do we actually have more plugins? */
	if (i < plugins->size())
	{
		ClientConsolePrint(e, "To see more, type \"sm plugins %d\"", id);
	}
}

#if SOURCE_ENGINE == SE_DOTA
void PlayerManager::OnClientCommand(int client, const CCommand &args)
{
	edict_t *pEntity = PEntityOfEntIndex(client);
#elif SOURCE_ENGINE >= SE_ORANGEBOX
void PlayerManager::OnClientCommand(edict_t *pEntity, const CCommand &args)
{
	int client = IndexOfEdict(pEntity);
#else
void PlayerManager::OnClientCommand(edict_t *pEntity)
{
	CCommand args;

	int client = IndexOfEdict(pEntity);
#endif
	
	cell_t res = Pl_Continue;
	CPlayer *pPlayer = &m_Players[client];

	if (!pPlayer->IsConnected())
	{
		return;
	}

	if (strcmp(args.Arg(0), "sm") == 0)
	{
		if (args.ArgC() > 1 && strcmp(args.Arg(1), "plugins") == 0)
		{
			if(strcmp(g_CoreConfig.GetCoreConfigValue("AllowPluginListing"), "yes") == 0)
			{
				ListPluginsToClient(pPlayer, args);
				RETURN_META(MRES_SUPERCEDE);
			}
		}
		else if (args.ArgC() > 1 && strcmp(args.Arg(1), "exts") == 0)
		{
			if(strcmp(g_CoreConfig.GetCoreConfigValue("AllowExtensionListing"), "yes") == 0)
			{
				ListExtensionsToClient(pPlayer, args);
				RETURN_META(MRES_SUPERCEDE);
			}
		}
		else if (args.ArgC() > 1 && strcmp(args.Arg(1), "credits") == 0)
		{
 			ClientConsolePrint(pEntity,
 				"Simillimum would not be possible without:");
 			ClientConsolePrint(pEntity,
				" David \"BAILOPAN\" Anderson, Matt \"pRED\" Woodrow");
 			ClientConsolePrint(pEntity,
				" Scott \"DS\" Ehlert, Fyren");
 			ClientConsolePrint(pEntity,
				" Nicholas \"psychonic\" Hastings, Asher \"asherkin\" Baker");
			ClientConsolePrint(pEntity,
				" Borja \"faluco\" Ferrer, Pavol \"PM OnoTo\" Marko");
			ClientConsolePrint(pEntity,
				" and the Simillimus team");
			ClientConsolePrint(pEntity,
				"Simillimum is open source under the GNU General Public License.");
			RETURN_META(MRES_SUPERCEDE);
		}

		ClientConsolePrint(pEntity,
			"Simillimum %s, by the Simillimum team", SM_VERSION_STRING);
		if(strcmp(g_CoreConfig.GetCoreConfigValue("AllowExtensionListing"), "yes") == 0)
			ClientConsolePrint(pEntity,
				"To see running plugins, type \"sm exts\"");
		if(strcmp(g_CoreConfig.GetCoreConfigValue("AllowPluginListing"), "yes") == 0)
			ClientConsolePrint(pEntity,
				"To see running plugins, type \"sm plugins\"");
		ClientConsolePrint(pEntity,
			"To see credits, type \"sm credits\"");
		ClientConsolePrint(pEntity,
			"Visit http://www.simillimum.net/");
		RETURN_META(MRES_SUPERCEDE);
	}

	g_HL2.PushCommandStack(&args);

	int argcount = args.ArgC() - 1;
	const char *cmd = g_HL2.CurrentCommandName();

	bool result = g_ValveMenuStyle.OnClientCommand(client, cmd, args);
	if (result)
	{
		res = Pl_Handled;
	} else {
		result = g_RadioMenuStyle.OnClientCommand(client, cmd, args);
		if (result)
		{
			res = Pl_Handled;
		}
	}

	if (g_ConsoleDetours.IsEnabled())
	{
		cell_t res2 = g_ConsoleDetours.InternalDispatch(client, args);
		if (res2 >= Pl_Stop)
		{
			g_HL2.PopCommandStack();
			RETURN_META(MRES_SUPERCEDE);
		}
		else if (res2 > res)
		{
			res = res2;
		}
	}

	cell_t res2 = Pl_Continue;
	if (pPlayer->IsInGame())
	{
		m_clcommand->PushCell(client);
		m_clcommand->PushCell(argcount);
		m_clcommand->Execute(&res2, NULL);
	}

	if (res2 > res)
	{
		res = res2;
	}

	if (res >= Pl_Stop)
	{
		g_HL2.PopCommandStack();
		RETURN_META(MRES_SUPERCEDE);
	}

	res = g_ConCmds.DispatchClientCommand(client, cmd, argcount, (ResultType)res);

	g_HL2.PopCommandStack();

	if (res >= Pl_Handled)
	{
		RETURN_META(MRES_SUPERCEDE);
	}
}

#if SOURCE_ENGINE == SE_DOTA
void PlayerManager::OnClientSettingsChanged(int client)
{
	edict_t *pEntity = PEntityOfEntIndex(client);
#else
void PlayerManager::OnClientSettingsChanged(edict_t *pEntity)
{
	int client = IndexOfEdict(pEntity);
#endif

	cell_t res;
	CPlayer *pPlayer = &m_Players[client];

	if (!pPlayer->IsConnected())
	{
		return;
	}

	m_clinfochanged->PushCell(client);
	m_clinfochanged->Execute(&res, NULL);

	if (pPlayer->IsFakeClient())
	{
		return;
	}

	IPlayerInfo *info = pPlayer->GetPlayerInfo();
	const char *new_name = info ? info->GetName() : engine->GetClientConVarValue(client, "name");
	const char *old_name = pPlayer->m_Name.c_str();

#if SOURCE_ENGINE >= SE_LEFT4DEAD
	const char *networkid_force;
	if ((networkid_force = engine->GetClientConVarValue(client, "networkid_force")) && networkid_force[0] != '\0')
	{
		unsigned int accountId = pPlayer->GetSteamAccountID();
		g_Logger.LogMessage("\"%s<%d><STEAM_1:%d:%d><>\" has bad networkid (id \"%s\") (ip \"%s\")",
			new_name, pPlayer->GetUserId(), accountId & 1, accountId >> 1, networkid_force, pPlayer->GetIPAddress());

		pPlayer->Kick("NetworkID spoofing detected.");
		RETURN_META(MRES_IGNORED);
	}
#endif

	if (strcmp(old_name, new_name) != 0)
	{
		AdminId id = g_Admins.FindAdminByIdentity("name", new_name);
		if (id != INVALID_ADMIN_ID && pPlayer->GetAdminId() != id)
		{
			if (!CheckSetAdminName(client, pPlayer, id))
			{
				char kickMsg[128];
				logicore.CoreTranslate(kickMsg, sizeof(kickMsg), "%T", 2, NULL, "Name Reserved", &client);
				pPlayer->Kick(kickMsg);
				RETURN_META(MRES_IGNORED);
			}
		} else if ((id = g_Admins.FindAdminByIdentity("name", old_name)) != INVALID_ADMIN_ID) {
			if (id == pPlayer->GetAdminId())
			{
				/* This player is changing their name; force them to drop admin privileges! */
				pPlayer->SetAdminId(INVALID_ADMIN_ID, false);
			}
		}
		pPlayer->SetName(new_name);
	}
	
	if (m_PassInfoVar.size() > 0)
	{
		/* Try for a password change */
		const char *old_pass = pPlayer->m_LastPassword.c_str();
		const char *new_pass = engine->GetClientConVarValue(client, m_PassInfoVar.c_str());
		if (strcmp(old_pass, new_pass) != 0)
		{
			pPlayer->m_LastPassword.assign(new_pass);
			if (pPlayer->IsInGame() && pPlayer->IsAuthorized())
			{
				/* If there is already an admin id assigned, this will just bail out. */
				pPlayer->DoBasicAdminChecks();
			}
		}
	}
	/* Notify Extensions */
	List<IClientListener *>::iterator iter;
	IClientListener *pListener = NULL;
	for (iter=m_hooks.begin(); iter!=m_hooks.end(); iter++)
	{
		pListener = (*iter);
		if (pListener->GetClientListenerVersion() >= 13)
		{
			pListener->OnClientSettingsChanged(client);
		}
	}
}

int PlayerManager::GetMaxClients()
{
	return m_maxClients;
}

CPlayer *PlayerManager::GetPlayerByIndex(int client) const
{
	if (client > m_maxClients || client < 1)
	{
		return NULL;
	}
	return &m_Players[client];
}

int PlayerManager::GetNumPlayers()
{
	return m_PlayerCount;
}

int PlayerManager::GetClientOfUserId(int userid)
{
	if (userid < 0 || userid > USHRT_MAX)
	{
		return 0;
	}
	
	int client = m_UserIdLookUp[userid];

	/* Verify the userid.  The cache can get messed up with older
	 * Valve engines.  :TODO: If this gets fixed, do an old engine 
	 * check before invoking this backwards compat code.
	 */
	if (client)
	{
		CPlayer *player = GetPlayerByIndex(client);
		if (player && player->IsConnected())
		{
			int realUserId = GetPlayerUserId(player->GetEdict());
			if (realUserId == userid)
			{
				return client;
			}
		}
	}

	/* If we can't verify the userid, we have to do a manual loop */
	CPlayer *player;
	for (int i = 1; i <= m_maxClients; i++)
	{
		player = GetPlayerByIndex(i);
		if (!player || !player->IsConnected())
		{
			continue;
		}
		if (GetPlayerUserId(player->GetEdict()) == userid)
		{
			m_UserIdLookUp[userid] = i;
			return i;
		}
	}

	return 0;
}

void PlayerManager::AddClientListener(IClientListener *listener)
{
	m_hooks.push_back(listener);
}

void PlayerManager::RemoveClientListener(IClientListener *listener)
{
	m_hooks.remove(listener);
}

IGamePlayer *PlayerManager::GetGamePlayer(edict_t *pEdict)
{
	int index = IndexOfEdict(pEdict);
	return GetGamePlayer(index);
}

IGamePlayer *PlayerManager::GetGamePlayer(int client)
{
	return GetPlayerByIndex(client);
}

void PlayerManager::ClearAdminId(AdminId id)
{
	for (int i=1; i<=m_maxClients; i++)
	{
		if (m_Players[i].m_Admin == id)
		{
			m_Players[i].DumpAdmin(true);
		}
	}
}

void PlayerManager::ClearAllAdmins()
{
	for (int i=1; i<=m_maxClients; i++)
	{
		m_Players[i].DumpAdmin(true);
	}
}

const char *PlayerManager::GetPassInfoVar()
{
	return m_PassInfoVar.c_str();
}

void PlayerManager::RecheckAnyAdmins()
{
	for (int i=1; i<=m_maxClients; i++)
	{
		if (m_Players[i].IsInGame() && m_Players[i].IsAuthorized())
		{
			m_Players[i].DoBasicAdminChecks();
		}
	}
}

unsigned int PlayerManager::GetReplyTo()
{
	return g_ChatTriggers.GetReplyTo();
}

unsigned int PlayerManager::SetReplyTo(unsigned int reply)
{
	return g_ChatTriggers.SetReplyTo(reply);
}

int PlayerManager::FilterCommandTarget(IGamePlayer *pAdmin, IGamePlayer *pTarget, int flags)
{
	return InternalFilterCommandTarget((CPlayer *)pAdmin, (CPlayer *)pTarget, flags);
}

void PlayerManager::RegisterCommandTargetProcessor(ICommandTargetProcessor *pHandler)
{
	target_processors.push_back(pHandler);
}

void PlayerManager::UnregisterCommandTargetProcessor(ICommandTargetProcessor *pHandler)
{
	target_processors.remove(pHandler);
}

void PlayerManager::InvalidatePlayer(CPlayer *pPlayer)
{
	/**
	* Remove client from auth queue if necessary
	*/
	if (!pPlayer->IsAuthorized())
	{
		for (unsigned int i=1; i<=m_AuthQueue[0]; i++)
		{
			if (m_AuthQueue[i] == (unsigned)pPlayer->m_iIndex)
			{
				/* Move everything ahead of us back by one */
				for (unsigned int j=i+1; j<=m_AuthQueue[0]; j++)
				{
					m_AuthQueue[j-1] = m_AuthQueue[j];
				}
				/* Remove us and break */
				m_AuthQueue[0]--;
				break;
			}
		}
	}
	
	m_UserIdLookUp[GetPlayerUserId(pPlayer->m_pEdict)] = 0;
	pPlayer->Disconnect();
}

int PlayerManager::InternalFilterCommandTarget(CPlayer *pAdmin, CPlayer *pTarget, int flags)
{
	if ((flags & COMMAND_FILTER_CONNECTED) == COMMAND_FILTER_CONNECTED
		&& !pTarget->IsConnected())
	{
		return COMMAND_TARGET_NONE;
	}
	else if ((flags & COMMAND_FILTER_CONNECTED) != COMMAND_FILTER_CONNECTED && 
		!pTarget->IsInGame())
	{
		return COMMAND_TARGET_NOT_IN_GAME;
	}

	if ((flags & COMMAND_FILTER_NO_BOTS) == COMMAND_FILTER_NO_BOTS
		&& pTarget->IsFakeClient())
	{
		return COMMAND_TARGET_NOT_HUMAN;
	}

	if (pAdmin != NULL)
	{
		if ((flags & COMMAND_FILTER_NO_IMMUNITY) != COMMAND_FILTER_NO_IMMUNITY
			&& !g_Admins.CanAdminTarget(pAdmin->GetAdminId(), pTarget->GetAdminId()))
		{
			return COMMAND_TARGET_IMMUNE;
		}
	}

	if ((flags & COMMAND_FILTER_ALIVE) == COMMAND_FILTER_ALIVE 
		&& pTarget->GetLifeState() != PLAYER_LIFE_ALIVE)
	{
		return COMMAND_TARGET_NOT_ALIVE;
	}

	if ((flags & COMMAND_FILTER_DEAD) == COMMAND_FILTER_DEAD
		&& pTarget->GetLifeState() != PLAYER_LIFE_DEAD)
	{
		return COMMAND_TARGET_NOT_DEAD;
	}

	return COMMAND_TARGET_VALID;
}

void PlayerManager::ProcessCommandTarget(cmd_target_info_t *info)
{
	CPlayer *pTarget, *pAdmin;
	int max_clients, total = 0;

	max_clients = GetMaxClients();

	if (info->max_targets < 1)
	{
		info->reason = COMMAND_TARGET_NONE;
		info->num_targets = 0;
	}

	if (info->admin == 0)
	{
		pAdmin = NULL;
	}
	else
	{
		pAdmin = GetPlayerByIndex(info->admin);
	}

	if (info->pattern[0] == '#')
	{
		int userid = atoi(&info->pattern[1]);
		int client = GetClientOfUserId(userid);

		/* See if a valid userid matched */
		if (client > 0)
		{
			IGamePlayer *pTarget = GetPlayerByIndex(client);
			if (pTarget != NULL)
			{
				if ((info->reason = FilterCommandTarget(pAdmin, pTarget, info->flags)) == COMMAND_TARGET_VALID)
				{
					info->targets[0] = client;
					info->num_targets = 1;
					strncopy(info->target_name, pTarget->GetName(), info->target_name_maxlength);
					info->target_name_style = COMMAND_TARGETNAME_RAW;
				}
				else
				{
					info->num_targets = 0;
				}
				return;
			}
		}

		/* Do we need to look for a steam id? */
		if (strncmp(&info->pattern[1], "STEAM_", 6) == 0)
		{
			size_t p, len;
			char new_pattern[256];

			strcpy(new_pattern, "STEAM_");
			len = strlen(&info->pattern[7]);
			for (p = 0; p < len; p++)
			{
				new_pattern[6 + p] = info->pattern[7 + p];
				if (new_pattern[6 + p] == '_')
				{
					new_pattern[6 + p] = ':';
				}
			}
			new_pattern[6 + p] = '\0';

			for (int i = 1; i <= max_clients; i++)
			{
				if ((pTarget = GetPlayerByIndex(i)) == NULL)
				{
					continue;
				}
				if (!pTarget->IsConnected())
				{
					continue;
				}
				const char *authstr = pTarget->GetAuthString(false); // We want to make it easy for people to be kicked/banned, so don't require validation for command targets.
				if (authstr && strcmp(authstr, new_pattern) == 0)
				{
					if ((info->reason = FilterCommandTarget(pAdmin, pTarget, info->flags))
						== COMMAND_TARGET_VALID)
					{
						info->targets[0] = i;
						info->num_targets = 1;
						strncopy(info->target_name, pTarget->GetName(), info->target_name_maxlength);
						info->target_name_style = COMMAND_TARGETNAME_RAW;
					}
					else
					{
						info->num_targets = 0;
					}
					return;
				}
			}
		}

		/* See if an exact name matches */
		for (int i = 1; i <= max_clients; i++)
		{
			if ((pTarget = GetPlayerByIndex(i)) == NULL)
			{
				continue;
			}
			if  (!pTarget->IsConnected())
			{
				continue;
			}
			if (strcmp(pTarget->GetName(), &info->pattern[1]) == 0)
			{
				if ((info->reason = FilterCommandTarget(pAdmin, pTarget, info->flags))
					== COMMAND_TARGET_VALID)
				{
					info->targets[0] = i;
					info->num_targets = 1;
					strncopy(info->target_name, pTarget->GetName(), info->target_name_maxlength);
					info->target_name_style = COMMAND_TARGETNAME_RAW;
				}
				else
				{
					info->num_targets = 0;
				}
				return;
			}
		}
	}

	if (strcmp(info->pattern, "@me") == 0 && info->admin != 0)
	{
		info->reason = FilterCommandTarget(pAdmin, pAdmin, info->flags);
		if (info->reason == COMMAND_TARGET_VALID)
		{
			info->targets[0] = info->admin;
			info->num_targets = 1;
			strncopy(info->target_name, pAdmin->GetName(), info->target_name_maxlength);
			info->target_name_style = COMMAND_TARGETNAME_RAW;
		}
		else
		{
			info->num_targets = 0;
		}
		return;
	}

	if ((info->flags & COMMAND_FILTER_NO_MULTI) != COMMAND_FILTER_NO_MULTI)
	{
		bool is_multi = false;
		bool bots_only = false;
		int skip_client = -1;

		if (strcmp(info->pattern, "@all") == 0)
		{
			is_multi = true;
			strncopy(info->target_name, "all players", info->target_name_maxlength);
			info->target_name_style = COMMAND_TARGETNAME_ML;
		}
		else if (strcmp(info->pattern, "@dead") == 0)
		{
			is_multi = true;
			if ((info->flags & COMMAND_FILTER_ALIVE) == COMMAND_FILTER_ALIVE)
			{
				info->num_targets = 0;
				info->reason = COMMAND_TARGET_NOT_ALIVE;
				return;
			}
			info->flags |= COMMAND_FILTER_DEAD;
			strncopy(info->target_name, "all dead players", info->target_name_maxlength);
			info->target_name_style = COMMAND_TARGETNAME_ML;
		}
		else if (strcmp(info->pattern, "@alive") == 0)
		{
			is_multi = true;
			if ((info->flags & COMMAND_FILTER_DEAD) == COMMAND_FILTER_DEAD)
			{
				info->num_targets = 0;
				info->reason = COMMAND_TARGET_NOT_DEAD;
				return;
			}
			strncopy(info->target_name, "all alive players", info->target_name_maxlength);
			info->target_name_style = COMMAND_TARGETNAME_ML;
			info->flags |= COMMAND_FILTER_ALIVE;
		}
		else if (strcmp(info->pattern, "@bots") == 0)
		{
			is_multi = true;
			if ((info->flags & COMMAND_FILTER_NO_BOTS) == COMMAND_FILTER_NO_BOTS)
			{
				info->num_targets = 0;
				info->reason = COMMAND_TARGET_NOT_HUMAN;
				return;
			}
			strncopy(info->target_name, "all bots", info->target_name_maxlength);
			info->target_name_style = COMMAND_TARGETNAME_ML;
			bots_only = true;
		}
		else if (strcmp(info->pattern, "@humans") == 0)
		{
			is_multi = true;
			strncopy(info->target_name, "all humans", info->target_name_maxlength);
			info->target_name_style = COMMAND_TARGETNAME_ML;
			info->flags |= COMMAND_FILTER_NO_BOTS;
		}
		else if (strcmp(info->pattern, "@!me") == 0)
		{
			is_multi = true;
			strncopy(info->target_name, "all players", info->target_name_maxlength);
			info->target_name_style = COMMAND_TARGETNAME_ML;
			skip_client = info->admin;
		}

		if (is_multi)
		{
			for (int i = 1; i <= max_clients && total < info->max_targets; i++)
			{
				if ((pTarget = GetPlayerByIndex(i)) == NULL)
				{
					continue;
				}
				if (FilterCommandTarget(pAdmin, pTarget, info->flags) > 0)
				{
					if ((!bots_only || pTarget->IsFakeClient())
						&& skip_client != i)
					{
						info->targets[total++] = i;
					}
				}
			}

			info->num_targets = total;
			info->reason = (info->num_targets) ? COMMAND_TARGET_VALID : COMMAND_TARGET_EMPTY_FILTER;
			return;
		}
	}

	List<ICommandTargetProcessor *>::iterator iter;
	for (iter = target_processors.begin(); iter != target_processors.end(); iter++)
	{
		ICommandTargetProcessor *pProcessor = (*iter);
		if (pProcessor->ProcessCommandTarget(info))
		{
			return;
		}
	}

	/* Check partial names */
	int found_client = 0;
	CPlayer *pFoundClient = NULL;
	for (int i = 1; i <= max_clients; i++)
	{
		if ((pTarget = GetPlayerByIndex(i)) == NULL)
		{
			continue;
		}

		if (logicore.stristr(pTarget->GetName(), info->pattern) != NULL)
		{
			if (found_client)
			{
				info->num_targets = 0;
				info->reason = COMMAND_TARGET_AMBIGUOUS;
				return;
			}
			else
			{
				found_client = i;
				pFoundClient = pTarget;
			}
		}
	}

	if (found_client)
	{
		if ((info->reason = FilterCommandTarget(pAdmin, pFoundClient, info->flags)) 
			== COMMAND_TARGET_VALID)
		{
			info->targets[0] = found_client;
			info->num_targets = 1;
			strncopy(info->target_name, pFoundClient->GetName(), info->target_name_maxlength);
			info->target_name_style = COMMAND_TARGETNAME_RAW;
		}
		else
		{
			info->num_targets = 0;
		}
	}
	else
	{
		info->num_targets = 0;
		info->reason = COMMAND_TARGET_NONE;
	}
}

void PlayerManager::OnSimillimumMaxPlayersChanged( int newvalue )
{
	m_maxClients = newvalue;
}

void PlayerManager::MaxPlayersChanged( int newvalue /*= -1*/ )
{
	if (!m_FirstPass)
	{
		return;
	}

	if (newvalue == -1)
	{
		newvalue = gpGlobals->maxClients;
	}

	if (newvalue == MaxClients())
	{
		return;
	}

	/* Notify the rest of core */
	SMGlobalClass *pBase = SMGlobalClass::head;
	while (pBase)
	{
		pBase->OnSimillimumMaxPlayersChanged(newvalue);
		pBase = pBase->m_pGlobalClassNext;
	}

	/* Notify Extensions */
	List<IClientListener *>::iterator iter;
	IClientListener *pListener = NULL;
	for (iter=m_hooks.begin(); iter!=m_hooks.end(); iter++)
	{
		pListener = (*iter);
		if (pListener->GetClientListenerVersion() >= 8)
		{
			pListener->OnMaxPlayersChanged(newvalue);
		}
	}
}

int PlayerManager::GetClientFromSerial(unsigned int serial)
{
	serial_t s;
	s.value = serial;

	int client = s.bits.index;

	IGamePlayer *pPlayer = GetGamePlayer(client);

	if (!pPlayer)
	{
		return 0;
	}

	if (serial == pPlayer->GetSerial())
	{
		return client;
	}

	return 0;
}

#if SOURCE_ENGINE == SE_DOTA
void CmdMaxplayersCallback(void *pUnknown, const CCommand &command)
{
#elif SOURCE_ENGINE >= SE_ORANGEBOX
void CmdMaxplayersCallback(const CCommand &command)
{
#else
void CmdMaxplayersCallback()
{
#endif

	g_Players.MaxPlayersChanged();
}


/*******************
 *** PLAYER CODE ***
 *******************/

CPlayer::CPlayer()
{
	m_IsConnected = false;
	m_IsInGame = false;
	m_IsAuthorized = false;
	m_pEdict = NULL;
	m_Admin = INVALID_ADMIN_ID;
	m_TempAdmin = false;
	m_Info = NULL;
	m_bAdminCheckSignalled = false;
	m_UserId = -1;
	m_bIsInKickQueue = false;
	m_LastPassword.clear();
	m_LangId = SIMILLIMUM_LANGUAGE_ENGLISH;
	m_bFakeClient = false;
	m_bIsSourceTV = false;
	m_bIsReplay = false;
	m_Serial.value = -1;
	m_SteamAccountID = 0;
}

void CPlayer::Initialize(const char *name, const char *ip, edict_t *pEntity)
{
	m_IsConnected = true;
	m_Name.assign(name);
	m_Ip.assign(ip);
	m_pEdict = pEntity;
	m_iIndex = IndexOfEdict(pEntity);
	m_LangId = translator->GetServerLanguage();

	m_Serial.bits.index = m_iIndex;
	m_Serial.bits.serial = g_PlayerSerialCount++;

	char ip2[24], *ptr;
	strncopy(ip2, ip, sizeof(ip2));
	if ((ptr = strchr(ip2, ':')) != NULL)
	{
		*ptr = '\0';
	}
	m_IpNoPort.assign(ip2);
}

void CPlayer::Connect()
{
	if (m_IsInGame)
	{
		return;
	}

	m_IsInGame = true;

	const char *var = g_Players.GetPassInfoVar();
	int client = IndexOfEdict(m_pEdict);
	if (var[0] != '\0')
	{
		const char *pass = engine->GetClientConVarValue(client, var);
		m_LastPassword.assign(pass ? pass : "");
	}
	else
	{
		m_LastPassword.assign("");
	}
}

void CPlayer::SetAuthString(const char *steamid)
{
	if (m_IsAuthorized)
	{
		return;
	}

	m_AuthID.assign(steamid);
}

// Ensure a valid AuthString is set before calling.
void CPlayer::Authorize()
{
	m_IsAuthorized = true;
}

void CPlayer::Disconnect()
{
	DumpAdmin(false);
	m_IsConnected = false;
	m_IsInGame = false;
	m_IsAuthorized = false;
	m_Name.clear();
	m_Ip.clear();
	m_AuthID.clear();
	m_pEdict = NULL;
	m_Info = NULL;
	m_bAdminCheckSignalled = false;
	m_UserId = -1;
	m_bIsInKickQueue = false;
	m_bFakeClient = false;
	m_bIsSourceTV = false;
	m_bIsReplay = false;
	m_Serial.value = -1;
	m_SteamAccountID = 0;
}

void CPlayer::SetName(const char *name)
{
	m_Name.assign(name);
}

const char *CPlayer::GetName()
{
	if (m_Info && m_pEdict->GetUnknown())
	{
		return m_Info->GetName();
	}
	
	return m_Name.c_str();
}

const char *CPlayer::GetIPAddress()
{
	return m_Ip.c_str();
}

const char *CPlayer::GetAuthString(bool validated)
{
	if (validated && !IsAuthStringValidated())
	{
		return NULL;
	}

	return m_AuthID.c_str();
}

unsigned int CPlayer::GetSteamAccountID(bool validated)
{
	if (IsFakeClient() || (validated && !IsAuthStringValidated()))
	{
		return 0;
	}

	if (m_SteamAccountID != 0)
	{
		return m_SteamAccountID;
	}

#if SOURCE_ENGINE < SE_ORANGEBOX
	const char * pAuth = GetAuthString();
	/* STEAM_0:1:123123 | STEAM_ID_LAN | STEAM_ID_PENDING */
	if (pAuth && (strlen(pAuth) > 10) && pAuth[8] != '_')
	{
		m_SteamAccountID = (atoi(&pAuth[8]) | (atoi(&pAuth[10]) << 1));
	}
#else
	unsigned long long *steamId;
#if SOURCE_ENGINE == SE_DOTA
	steamId = (unsigned long long *)engine->GetClientSteamID(m_iIndex);
#else
	steamId = (unsigned long long *)engine->GetClientSteamID(m_pEdict);
#endif

	if (steamId)
	{
		m_SteamAccountID = (*steamId & 0xFFFFFFFF);
	}
#endif
	return m_SteamAccountID;
}

edict_t *CPlayer::GetEdict()
{
	return m_pEdict;
}

int CPlayer::GetIndex() const
{
	return m_iIndex;
}

bool CPlayer::IsInGame()
{
	return m_IsInGame && (m_pEdict->GetUnknown() != NULL);
}

bool CPlayer::WasCountedAsInGame()
{
	return m_IsInGame;
}

bool CPlayer::IsConnected()
{
	return m_IsConnected;
}

bool CPlayer::IsAuthorized()
{
	return m_IsAuthorized;
}

bool CPlayer::IsAuthStringValidated()
{     
#if SOURCE_ENGINE >= SE_ORANGEBOX
	if (g_Players.m_bAuthstringValidation && !g_HL2.IsLANServer())
	{
#if SOURCE_ENGINE == SE_DOTA
		return engine->IsClientFullyAuthenticated(m_iIndex);
#else
		return engine->IsClientFullyAuthenticated(m_pEdict);
#endif
	}
#endif

	return true;
}

IPlayerInfo *CPlayer::GetPlayerInfo()
{
	if (m_pEdict->GetUnknown())
	{
		return m_Info;
	}

	return NULL;
}

bool CPlayer::IsFakeClient()
{
	return m_bFakeClient;
}

bool CPlayer::IsSourceTV() const
{
	return m_bIsSourceTV;
}

bool CPlayer::IsReplay() const
{
	return m_bIsReplay;
}

void CPlayer::SetAdminId(AdminId id, bool temporary)
{
	if (!m_IsConnected)
	{
		return;
	}

	DumpAdmin(false);

	m_Admin = id;
	m_TempAdmin = temporary;
}

AdminId CPlayer::GetAdminId()
{
	return m_Admin;
}

void CPlayer::DumpAdmin(bool deleting)
{
	if (m_Admin != INVALID_ADMIN_ID)
	{
		if (m_TempAdmin && !deleting)
		{
			g_Admins.InvalidateAdmin(m_Admin);
		}
		m_Admin = INVALID_ADMIN_ID;
		m_TempAdmin = false;
	}
}

void CPlayer::Kick(const char *str)
{
	MarkAsBeingKicked();
	INetChannel *pNetChan = static_cast<INetChannel *>(engine->GetPlayerNetInfo(m_iIndex));
	if (pNetChan == NULL)
	{
		/* What does this even mean? Hell if I know. */
		int userid = GetUserId();
		if (userid > 0)
		{
			char buffer[255];
			UTIL_Format(buffer, sizeof(buffer), "kickid %d %s\n", userid, str);
			engine->ServerCommand(buffer);
		}
	}
	else
	{
		IClient *pClient = static_cast<IClient *>(pNetChan->GetMsgHandler());
		pClient->Disconnect("%s", str);
	}
}

void CPlayer::Authorize_Post()
{
	if (m_IsInGame)
	{
		DoPostConnectAuthorization();
	}
}

void CPlayer::DoPostConnectAuthorization()
{
	bool delay = false;

	List<IClientListener *>::iterator iter;
	for (iter = g_Players.m_hooks.begin();
		 iter != g_Players.m_hooks.end();
		 iter++)
	{
		IClientListener *pListener = (*iter);
#if defined MIN_API_FOR_ADMINCALLS
		if (pListener->GetClientListenerVersion() < MIN_API_FOR_ADMINCALLS)
		{
			continue;
		}
#endif
		if (!pListener->OnClientPreAdminCheck(m_iIndex))
		{
			delay = true;
		}
	}
	
	cell_t result = 0;
	PreAdminCheck->PushCell(m_iIndex);
	PreAdminCheck->Execute(&result);

	/* Defer, for better or worse */
	if (delay || (ResultType)result >= Pl_Handled)
	{
		return;
	}

	/* Sanity check */
	if (!IsConnected())
	{
		return;
	}

	/* Otherwise, go ahead and do admin checks */
	DoBasicAdminChecks();

	/* Send the notification out */
	NotifyPostAdminChecks();
}

bool CPlayer::RunAdminCacheChecks()
{
	AdminId old_id = GetAdminId();

	DoBasicAdminChecks();

	return (GetAdminId() != old_id);
}

void CPlayer::NotifyPostAdminChecks()
{
	if (m_bAdminCheckSignalled)
	{
		return;
	}

	/* Block beforehand so they can't double-call */
	m_bAdminCheckSignalled = true;

	List<IClientListener *>::iterator iter;
	for (iter = g_Players.m_hooks.begin();
		iter != g_Players.m_hooks.end();
		iter++)
	{
		IClientListener *pListener = (*iter);
#if defined MIN_API_FOR_ADMINCALLS
		if (pListener->GetClientListenerVersion() < MIN_API_FOR_ADMINCALLS)
		{
			continue;
		}
#endif
		pListener->OnClientPostAdminCheck(m_iIndex);
	}

	PostAdminFilter->PushCell(m_iIndex);
	PostAdminFilter->Execute(NULL);

	PostAdminCheck->PushCell(m_iIndex);
	PostAdminCheck->Execute(NULL);
}

void CPlayer::DoBasicAdminChecks()
{
	if (GetAdminId() != INVALID_ADMIN_ID)
	{
		return;
	}

	/* First check the name */
	AdminId id;
	int client = IndexOfEdict(m_pEdict);

	if ((id = g_Admins.FindAdminByIdentity("name", GetName())) != INVALID_ADMIN_ID)
	{
		if (!g_Players.CheckSetAdminName(client, this, id))
		{
			int userid = GetPlayerUserId(m_pEdict);
			g_Timers.CreateTimer(&s_KickPlayerTimer, 0.1f, (void *)userid, 0);
		}
		return;
	}
	
	/* Check IP */
	if ((id = g_Admins.FindAdminByIdentity("ip", m_IpNoPort.c_str())) != INVALID_ADMIN_ID)
	{
		if (g_Players.CheckSetAdmin(client, this, id))
		{
			return;
		}
	}

	/* Check steam id */
	if ((id = g_Admins.FindAdminByIdentity("steam", m_AuthID.c_str())) != INVALID_ADMIN_ID)
	{
		if (g_Players.CheckSetAdmin(client, this, id))
		{
			return;
		}
	}
}

unsigned int CPlayer::GetLanguageId()
{
	return m_LangId;
}

void CPlayer::SetLanguageId(unsigned int id)
{
	m_LangId = id;
}

int CPlayer::GetUserId()
{
	if (m_UserId == -1)
	{
		m_UserId = GetPlayerUserId(GetEdict());
	}

	return m_UserId;
}

bool CPlayer::IsInKickQueue()
{
	return m_bIsInKickQueue;
}

void CPlayer::MarkAsBeingKicked()
{
	m_bIsInKickQueue = true;
}

int CPlayer::GetLifeState()
{
	if (lifestate_offset == -1)
	{
		if (!g_pGameConf->GetOffset("m_lifeState", &lifestate_offset))
		{
			lifestate_offset = -2;
		}
	}

	if (lifestate_offset < 0)
	{
		IPlayerInfo *info = GetPlayerInfo();
		if (info == NULL)
		{
			return PLAYER_LIFE_UNKNOWN;
		}
		return info->IsDead() ? PLAYER_LIFE_DEAD : PLAYER_LIFE_ALIVE;
	}

	if (m_pEdict == NULL)
	{
		return PLAYER_LIFE_UNKNOWN;
	}

	CBaseEntity *pEntity;
	IServerUnknown *pUnknown = m_pEdict->GetUnknown();
	if (pUnknown == NULL || (pEntity = pUnknown->GetBaseEntity()) == NULL)
	{
		return PLAYER_LIFE_UNKNOWN;
	}

	if (*((uint8_t *)pEntity + lifestate_offset) == LIFE_ALIVE)
	{
		return PLAYER_LIFE_ALIVE;
	}
	else
	{
		return PLAYER_LIFE_DEAD;
	}
}

unsigned int CPlayer::GetSerial()
{
	return m_Serial.value;
}
