/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "server.h"

#include <base/math.h>
#include <base/system.h>

#include <engine/config.h>
#include <engine/console.h>
#include <engine/engine.h>
#include <engine/map.h>
#include <engine/masterserver.h>
#include <engine/server.h>
#include <engine/storage.h>

#include <engine/shared/compression.h>
#include <engine/shared/config.h>
#include <engine/shared/datafile.h>
#include <engine/shared/demo.h>
#include <engine/shared/econ.h>
#include <engine/shared/fifo.h>
#include <engine/shared/filecollection.h>
#include <engine/shared/jsonwriter.h>
#include <engine/shared/netban.h>
#include <engine/shared/network.h>
#include <engine/shared/packer.h>
#include <engine/shared/protocol.h>
#include <engine/shared/protocol_ex.h>
#include <engine/shared/snapshot.h>

// DDRace
#include <cstring>
#include <engine/shared/linereader.h>
#include <game/extrainfo.h>
#include <vector>
#include <zlib.h>

#include "databases/connection.h"
#include "databases/connection_pool.h"
// DDNet
#include <engine/shared/uuid_manager.h>

#include "register.h"

#if defined(CONF_FAMILY_WINDOWS)
#include <windows.h>
#endif

CSnapIDPool::CSnapIDPool()
{
	Reset();
}

void CSnapIDPool::Reset()
{
	for(int i = 0; i < MAX_IDS; i++)
	{
		m_aIDs[i].m_Next = i + 1;
		m_aIDs[i].m_State = 0;
	}

	m_aIDs[MAX_IDS - 1].m_Next = -1;
	m_FirstFree = 0;
	m_FirstTimed = -1;
	m_LastTimed = -1;
	m_Usage = 0;
	m_InUsage = 0;
}

void CSnapIDPool::RemoveFirstTimeout()
{
	int NextTimed = m_aIDs[m_FirstTimed].m_Next;

	// add it to the free list
	m_aIDs[m_FirstTimed].m_Next = m_FirstFree;
	m_aIDs[m_FirstTimed].m_State = 0;
	m_FirstFree = m_FirstTimed;

	// remove it from the timed list
	m_FirstTimed = NextTimed;
	if(m_FirstTimed == -1)
		m_LastTimed = -1;

	m_Usage--;
}

int CSnapIDPool::NewID()
{
	int64 Now = time_get();

	// process timed ids
	while(m_FirstTimed != -1 && m_aIDs[m_FirstTimed].m_Timeout < Now)
		RemoveFirstTimeout();

	int ID = m_FirstFree;
	dbg_assert(ID != -1, "id error");
	if(ID == -1)
		return ID;
	m_FirstFree = m_aIDs[m_FirstFree].m_Next;
	m_aIDs[ID].m_State = 1;
	m_Usage++;
	m_InUsage++;
	return ID;
}

void CSnapIDPool::TimeoutIDs()
{
	// process timed ids
	while(m_FirstTimed != -1)
		RemoveFirstTimeout();
}

void CSnapIDPool::FreeID(int ID)
{
	if(ID < 0)
		return;
	dbg_assert(m_aIDs[ID].m_State == 1, "id is not allocated");

	m_InUsage--;
	m_aIDs[ID].m_State = 2;
	m_aIDs[ID].m_Timeout = time_get() + time_freq() * 5;
	m_aIDs[ID].m_Next = -1;

	if(m_LastTimed != -1)
	{
		m_aIDs[m_LastTimed].m_Next = ID;
		m_LastTimed = ID;
	}
	else
	{
		m_FirstTimed = ID;
		m_LastTimed = ID;
	}
}

void CServerBan::InitServerBan(IConsole *pConsole, IStorage *pStorage, CServer *pServer)
{
	CNetBan::Init(pConsole, pStorage);

	m_pServer = pServer;

	// overwrites base command, todo: improve this
	Console()->Register("ban", "s[ip|id] ?i[minutes] r[reason]", CFGFLAG_SERVER | CFGFLAG_STORE, ConBanExt, this, "Ban player with ip/client id for x minutes for any reason");
	Console()->Register("ban_region", "s[region] s[ip|id] ?i[minutes] r[reason]", CFGFLAG_SERVER | CFGFLAG_STORE, ConBanRegion, this, "Ban player in a region");
	Console()->Register("ban_region_range", "s[region] s[first ip] s[last ip] ?i[minutes] r[reason]", CFGFLAG_SERVER | CFGFLAG_STORE, ConBanRegionRange, this, "Ban range in a region");
}

template<class T>
int CServerBan::BanExt(T *pBanPool, const typename T::CDataType *pData, int Seconds, const char *pReason)
{
	// validate address
	if(Server()->m_RconClientID >= 0 && Server()->m_RconClientID < MAX_CLIENTS &&
		Server()->m_aClients[Server()->m_RconClientID].m_State != CServer::CClient::STATE_EMPTY)
	{
		if(NetMatch(pData, Server()->m_NetServer.ClientAddr(Server()->m_RconClientID)))
		{
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "net_ban", "ban error (you can't ban yourself)");
			return -1;
		}

		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(i == Server()->m_RconClientID || Server()->m_aClients[i].m_State == CServer::CClient::STATE_EMPTY)
				continue;

			if(Server()->m_aClients[i].m_Authed >= Server()->m_RconAuthLevel && NetMatch(pData, Server()->m_NetServer.ClientAddr(i)))
			{
				Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "net_ban", "ban error (command denied)");
				return -1;
			}
		}
	}
	else if(Server()->m_RconClientID == IServer::RCON_CID_VOTE)
	{
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(Server()->m_aClients[i].m_State == CServer::CClient::STATE_EMPTY)
				continue;

			if(Server()->m_aClients[i].m_Authed != AUTHED_NO && NetMatch(pData, Server()->m_NetServer.ClientAddr(i)))
			{
				Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "net_ban", "ban error (command denied)");
				return -1;
			}
		}
	}

	int Result = Ban(pBanPool, pData, Seconds, pReason);
	if(Result != 0)
		return Result;

	// drop banned clients
	typename T::CDataType Data = *pData;
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(Server()->m_aClients[i].m_State == CServer::CClient::STATE_EMPTY)
			continue;

		if(NetMatch(&Data, Server()->m_NetServer.ClientAddr(i)))
		{
			CNetHash NetHash(&Data);
			char aBuf[256];
			MakeBanInfo(pBanPool->Find(&Data, &NetHash), aBuf, sizeof(aBuf), MSGTYPE_PLAYER);
			Server()->m_NetServer.Drop(i, aBuf);
		}
	}

	return Result;
}

int CServerBan::BanAddr(const NETADDR *pAddr, int Seconds, const char *pReason)
{
	return BanExt(&m_BanAddrPool, pAddr, Seconds, pReason);
}

int CServerBan::BanRange(const CNetRange *pRange, int Seconds, const char *pReason)
{
	if(pRange->IsValid())
		return BanExt(&m_BanRangePool, pRange, Seconds, pReason);

	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "net_ban", "ban failed (invalid range)");
	return -1;
}

void CServerBan::ConBanExt(IConsole::IResult *pResult, void *pUser)
{
	CServerBan *pThis = static_cast<CServerBan *>(pUser);

	const char *pStr = pResult->GetString(0);
	int Minutes = pResult->NumArguments() > 1 ? clamp(pResult->GetInteger(1), 0, 525600) : 30;
	const char *pReason = pResult->NumArguments() > 2 ? pResult->GetString(2) : "No reason given";

	if(str_isallnum(pStr))
	{
		int ClientID = str_toint(pStr);
		if(ClientID < 0 || ClientID >= MAX_CLIENTS || pThis->Server()->m_aClients[ClientID].m_State == CServer::CClient::STATE_EMPTY)
			pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "net_ban", "ban error (invalid client id)");
		else
			pThis->BanAddr(pThis->Server()->m_NetServer.ClientAddr(ClientID), Minutes * 60, pReason);
	}
	else
		ConBan(pResult, pUser);
}

void CServerBan::ConBanRegion(IConsole::IResult *pResult, void *pUser)
{
	const char *pRegion = pResult->GetString(0);
	if(str_comp_nocase(pRegion, g_Config.m_SvRegionName))
		return;

	pResult->RemoveArgument(0);
	ConBanExt(pResult, pUser);
}

void CServerBan::ConBanRegionRange(IConsole::IResult *pResult, void *pUser)
{
	CServerBan *pServerBan = static_cast<CServerBan *>(pUser);

	const char *pRegion = pResult->GetString(0);
	if(str_comp_nocase(pRegion, g_Config.m_SvRegionName))
		return;

	pResult->RemoveArgument(0);
	ConBanRange(pResult, static_cast<CNetBan *>(pServerBan));
}

void CServer::CClient::Reset()
{
	// reset input
	for(auto &Input : m_aInputs)
		Input.m_GameTick = -1;
	m_CurrentInput = 0;
	mem_zero(&m_LatestInput, sizeof(m_LatestInput));

	m_Snapshots.PurgeAll();
	m_LastAckedSnapshot = -1;
	m_LastInputTick = -1;
	m_SnapRate = CClient::SNAPRATE_INIT;
	m_Score = 0;
	m_NextMapChunk = 0;
	m_Flags = 0;
	m_DDNetVersion = VERSION_NONE;
	m_GotDDNetVersionPacket = false;
	m_DDNetVersionSettled = false;
}

CServer::CServer()
{
	for(int i = 0; i < MAX_CLIENTS; i++)
		m_aDemoRecorder[i] = CDemoRecorder(&m_SnapshotDelta, true);
	m_aDemoRecorder[MAX_CLIENTS] = CDemoRecorder(&m_SnapshotDelta, false);

	m_TickSpeed = SERVER_TICK_SPEED;

	m_pGameServer = 0;

	m_CurrentGameTick = 0;
	m_RunServer = UNINITIALIZED;

	for(int i = 0; i < 2; i++)
	{
		m_apCurrentMapData[i] = 0;
		m_aCurrentMapSize[i] = 0;
	}

	m_MapReload = false;
	m_ReloadedWhenEmpty = false;

	m_RconClientID = IServer::RCON_CID_SERV;
	m_RconAuthLevel = AUTHED_ADMIN;

	m_RconRestrict = -1;

	m_ServerInfoFirstRequest = 0;
	m_ServerInfoNumRequests = 0;
	m_ServerInfoNeedsUpdate = false;

#ifdef CONF_FAMILY_UNIX
	m_ConnLoggingSocketCreated = false;
#endif

	m_pConnectionPool = new CDbConnectionPool();

	m_aErrorShutdownReason[0] = 0;

	Init();
}

CServer::~CServer()
{
	for(auto &pCurrentMapData : m_apCurrentMapData)
	{
		if(pCurrentMapData)
			free(pCurrentMapData);
	}

	delete m_pConnectionPool;
}

bool CServer::IsClientNameAvailable(int ClientID, const char *pNameRequest)
{
	// check for empty names
	if(!pNameRequest[0])
		return false;

	// check for names starting with /, as they can be abused to make people
	// write chat commands
	if(pNameRequest[0] == '/')
		return false;

	// make sure that two clients don't have the same name
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i != ClientID && m_aClients[i].m_State >= CClient::STATE_READY)
		{
			if(str_utf8_comp_confusable(pNameRequest, m_aClients[i].m_aName) == 0)
				return false;
		}
	}

	return true;
}

bool CServer::SetClientNameImpl(int ClientID, const char *pNameRequest, bool Set)
{
	dbg_assert(0 <= ClientID && ClientID < MAX_CLIENTS, "invalid client id");
	if(m_aClients[ClientID].m_State < CClient::STATE_READY)
		return false;

	CNameBan *pBanned = IsNameBanned(pNameRequest, m_aNameBans.base_ptr(), m_aNameBans.size());
	if(pBanned)
	{
		if(m_aClients[ClientID].m_State == CClient::STATE_READY && Set)
		{
			char aBuf[256];
			if(pBanned->m_aReason[0])
			{
				str_format(aBuf, sizeof(aBuf), "Kicked (your name is banned: %s)", pBanned->m_aReason);
			}
			else
			{
				str_copy(aBuf, "Kicked (your name is banned)", sizeof(aBuf));
			}
			Kick(ClientID, aBuf);
		}
		return false;
	}

	// trim the name
	char aTrimmedName[MAX_NAME_LENGTH];
	str_copy(aTrimmedName, str_utf8_skip_whitespaces(pNameRequest), sizeof(aTrimmedName));
	str_utf8_trim_right(aTrimmedName);

	char aNameTry[MAX_NAME_LENGTH];
	str_copy(aNameTry, aTrimmedName, sizeof(aNameTry));

	if(!IsClientNameAvailable(ClientID, aNameTry))
	{
		// auto rename
		for(int i = 1;; i++)
		{
			char aNameTryBrokenEnd[MAX_NAME_LENGTH];
			str_format(aNameTryBrokenEnd, sizeof(aNameTryBrokenEnd), "(%d)%s", i, aTrimmedName);
			str_utf8_copy(aNameTry, aNameTryBrokenEnd, sizeof(aNameTry));
			if(IsClientNameAvailable(ClientID, aNameTry))
				break;
		}
	}

	bool Changed = str_comp(m_aClients[ClientID].m_aName, aNameTry) != 0;

	if(Set)
	{
		// set the client name
		str_copy(m_aClients[ClientID].m_aName, aNameTry, MAX_NAME_LENGTH);
	}

	return Changed;
}

bool CServer::WouldClientNameChange(int ClientID, const char *pNameRequest)
{
	return SetClientNameImpl(ClientID, pNameRequest, false);
}

void CServer::SetClientName(int ClientID, const char *pName)
{
	SetClientNameImpl(ClientID, pName, true);
}

void CServer::SetClientClan(int ClientID, const char *pClan)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || m_aClients[ClientID].m_State < CClient::STATE_READY || !pClan)
		return;

	str_copy(m_aClients[ClientID].m_aClan, pClan, MAX_CLAN_LENGTH);
}

void CServer::SetClientCountry(int ClientID, int Country)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || m_aClients[ClientID].m_State < CClient::STATE_READY)
		return;

	m_aClients[ClientID].m_Country = Country;
}

void CServer::SetClientScore(int ClientID, int Score)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || m_aClients[ClientID].m_State < CClient::STATE_READY)
		return;

	if(m_aClients[ClientID].m_Score != Score)
		ExpireServerInfo();

	m_aClients[ClientID].m_Score = Score;
}

void CServer::SetClientFlags(int ClientID, int Flags)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || m_aClients[ClientID].m_State < CClient::STATE_READY)
		return;

	if(Flags > m_aClients[ClientID].m_Flags)
		m_aClients[ClientID].m_Flags = Flags;
}

void CServer::Kick(int ClientID, const char *pReason)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || m_aClients[ClientID].m_State == CClient::STATE_EMPTY)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "invalid client id to kick");
		return;
	}
	else if(m_RconClientID == ClientID)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "you can't kick yourself");
		return;
	}
	else if(m_aClients[ClientID].m_Authed > m_RconAuthLevel)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "kick command denied");
		return;
	}

	m_NetServer.Drop(ClientID, pReason);
}

void CServer::Ban(int ClientID, int Seconds, const char *pReason)
{
	NETADDR Addr;
	GetClientAddr(ClientID, &Addr);
	m_NetServer.NetBan()->BanAddr(&Addr, Seconds, pReason);
}

/*int CServer::Tick()
{
	return m_CurrentGameTick;
}*/

int64 CServer::TickStartTime(int Tick)
{
	return m_GameStartTime + (time_freq() * Tick) / SERVER_TICK_SPEED;
}

/*int CServer::TickSpeed()
{
	return SERVER_TICK_SPEED;
}*/

int CServer::Init()
{
	for(auto &Client : m_aClients)
	{
		Client.m_State = CClient::STATE_EMPTY;
		Client.m_aName[0] = 0;
		Client.m_aClan[0] = 0;
		Client.m_Country = -1;
		Client.m_Snapshots.Init();
		Client.m_Traffic = 0;
		Client.m_TrafficSince = 0;
		Client.m_ShowIps = false;
		Client.m_AuthKey = -1;
		Client.m_Latency = 0;
		Client.m_Sixup = false;
		Client.m_DisruptiveLeave = false;
		Client.m_LoginAxiom = false;
	}

	m_CurrentGameTick = 0;

	m_AnnouncementLastLine = 0;
	memset(m_aPrevStates, CClient::STATE_EMPTY, MAX_CLIENTS * sizeof(int));

	return 0;
}

void CServer::SetRconCID(int ClientID)
{
	m_RconClientID = ClientID;
}

int CServer::GetAuthedState(int ClientID) const
{
	return m_aClients[ClientID].m_Authed;
}

const char *CServer::GetAuthName(int ClientID) const
{
	int Key = m_aClients[ClientID].m_AuthKey;
	if(Key == -1)
	{
		return 0;
	}
	return m_AuthManager.KeyIdent(Key);
}

int CServer::GetClientInfo(int ClientID, CClientInfo *pInfo) const
{
	dbg_assert(ClientID >= 0 && ClientID < MAX_CLIENTS, "client_id is not valid");
	dbg_assert(pInfo != 0, "info can not be null");

	if(m_aClients[ClientID].m_State == CClient::STATE_INGAME)
	{
		pInfo->m_pName = m_aClients[ClientID].m_aName;
		pInfo->m_Latency = m_aClients[ClientID].m_Latency;
		pInfo->m_GotDDNetVersion = m_aClients[ClientID].m_DDNetVersionSettled;
		pInfo->m_DDNetVersion = m_aClients[ClientID].m_DDNetVersion >= 0 ? m_aClients[ClientID].m_DDNetVersion : VERSION_VANILLA;
		if(m_aClients[ClientID].m_GotDDNetVersionPacket)
		{
			pInfo->m_pConnectionID = &m_aClients[ClientID].m_ConnectionID;
			pInfo->m_pDDNetVersionStr = m_aClients[ClientID].m_aDDNetVersionStr;
		}
		else
		{
			pInfo->m_pConnectionID = 0;
			pInfo->m_pDDNetVersionStr = 0;
		}
		return 1;
	}
	return 0;
}

void CServer::SetClientDDNetVersion(int ClientID, int DDNetVersion)
{
	dbg_assert(ClientID >= 0 && ClientID < MAX_CLIENTS, "client_id is not valid");

	if(m_aClients[ClientID].m_State == CClient::STATE_INGAME)
	{
		m_aClients[ClientID].m_DDNetVersion = DDNetVersion;
		m_aClients[ClientID].m_DDNetVersionSettled = true;
	}
}

void CServer::GetClientAddr(int ClientID, char *pAddrStr, int Size) const
{
	if(ClientID >= 0 && ClientID < MAX_CLIENTS && m_aClients[ClientID].m_State == CClient::STATE_INGAME)
		net_addr_str(m_NetServer.ClientAddr(ClientID), pAddrStr, Size, false);
}

const char *CServer::ClientName(int ClientID) const
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || m_aClients[ClientID].m_State == CServer::CClient::STATE_EMPTY)
		return "(invalid)";
	if(m_aClients[ClientID].m_State == CServer::CClient::STATE_INGAME)
		return m_aClients[ClientID].m_aName;
	else
		return "(connecting)";
}

const char *CServer::ClientClan(int ClientID) const
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || m_aClients[ClientID].m_State == CServer::CClient::STATE_EMPTY)
		return "";
	if(m_aClients[ClientID].m_State == CServer::CClient::STATE_INGAME)
		return m_aClients[ClientID].m_aClan;
	else
		return "";
}

int CServer::ClientCountry(int ClientID) const
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || m_aClients[ClientID].m_State == CServer::CClient::STATE_EMPTY)
		return -1;
	if(m_aClients[ClientID].m_State == CServer::CClient::STATE_INGAME)
		return m_aClients[ClientID].m_Country;
	else
		return -1;
}

bool CServer::ClientIngame(int ClientID) const
{
	return ClientID >= 0 && ClientID < MAX_CLIENTS && m_aClients[ClientID].m_State == CServer::CClient::STATE_INGAME;
}

bool CServer::ClientAuthed(int ClientID) const
{
	return ClientID >= 0 && ClientID < MAX_CLIENTS && m_aClients[ClientID].m_Authed;
}

int CServer::Port() const
{
	return m_NetServer.Address().port;
}

int CServer::MaxClients() const
{
	return m_NetServer.MaxClients();
}

int CServer::ClientCount() const
{
	int ClientCount = 0;
	for(const auto &Client : m_aClients)
	{
		if(Client.m_State != CClient::STATE_EMPTY)
		{
			ClientCount++;
		}
	}

	return ClientCount;
}

int CServer::DistinctClientCount() const
{
	NETADDR aAddresses[MAX_CLIENTS];
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_aClients[i].m_State != CClient::STATE_EMPTY)
		{
			GetClientAddr(i, &aAddresses[i]);
		}
	}

	int ClientCount = 0;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_aClients[i].m_State != CClient::STATE_EMPTY)
		{
			ClientCount++;
			for(int j = 0; j < i; j++)
			{
				if(!net_addr_comp_noport(&aAddresses[i], &aAddresses[j]))
				{
					ClientCount--;
					break;
				}
			}
		}
	}

	return ClientCount;
}

static inline bool RepackMsg(const CMsgPacker *pMsg, CPacker &Packer, bool Sixup)
{
	int MsgId = pMsg->m_MsgID;
	Packer.Reset();

	if(Sixup && !pMsg->m_NoTranslate)
	{
		if(pMsg->m_System)
		{
			if(MsgId >= OFFSET_UUID)
				;
			else if(MsgId >= NETMSG_MAP_CHANGE && MsgId <= NETMSG_MAP_DATA)
				;
			else if(MsgId >= NETMSG_CON_READY && MsgId <= NETMSG_INPUTTIMING)
				MsgId += 1;
			else if(MsgId == NETMSG_RCON_LINE)
				MsgId = 13;
			else if(MsgId >= NETMSG_AUTH_CHALLANGE && MsgId <= NETMSG_AUTH_RESULT)
				MsgId += 4;
			else if(MsgId >= NETMSG_PING && MsgId <= NETMSG_ERROR)
				MsgId += 4;
			else if(MsgId >= NETMSG_RCON_CMD_ADD && MsgId <= NETMSG_RCON_CMD_REM)
				MsgId -= 11;
			else
			{
				dbg_msg("net", "DROP send sys %d", MsgId);
				return true;
			}
		}
		else
		{
			if(MsgId >= 0 && MsgId < OFFSET_UUID)
				MsgId = Msg_SixToSeven(MsgId);

			if(MsgId < 0)
				return true;
		}
	}

	if(MsgId < OFFSET_UUID)
	{
		Packer.AddInt((MsgId << 1) | (pMsg->m_System ? 1 : 0));
	}
	else
	{
		Packer.AddInt((0 << 1) | (pMsg->m_System ? 1 : 0)); // NETMSG_EX, NETMSGTYPE_EX
		g_UuidManager.PackUuid(MsgId, &Packer);
	}
	Packer.AddRaw(pMsg->Data(), pMsg->Size());

	return false;
}

int CServer::SendMsg(CMsgPacker *pMsg, int Flags, int ClientID)
{
	CNetChunk Packet;
	if(!pMsg)
		return -1;

	mem_zero(&Packet, sizeof(CNetChunk));
	if(Flags & MSGFLAG_VITAL)
		Packet.m_Flags |= NETSENDFLAG_VITAL;
	if(Flags & MSGFLAG_FLUSH)
		Packet.m_Flags |= NETSENDFLAG_FLUSH;

	if(ClientID < 0)
	{
		CPacker Pack6, Pack7;
		if(RepackMsg(pMsg, Pack6, false))
			return -1;
		if(RepackMsg(pMsg, Pack7, true))
			return -1;

		// write message to demo recorder
		if(!(Flags & MSGFLAG_NORECORD))
			m_aDemoRecorder[MAX_CLIENTS].RecordMessage(Pack6.Data(), Pack6.Size());

		if(!(Flags & MSGFLAG_NOSEND))
		{
			for(int i = 0; i < MAX_CLIENTS; i++)
			{
				if(m_aClients[i].m_State == CClient::STATE_INGAME)
				{
					CPacker *pPack = m_aClients[i].m_Sixup ? &Pack7 : &Pack6;
					Packet.m_pData = pPack->Data();
					Packet.m_DataSize = pPack->Size();
					Packet.m_ClientID = i;
					m_NetServer.Send(&Packet);
				}
			}
		}
	}
	else
	{
		CPacker Pack;
		if(RepackMsg(pMsg, Pack, m_aClients[ClientID].m_Sixup))
			return -1;

		Packet.m_ClientID = ClientID;
		Packet.m_pData = Pack.Data();
		Packet.m_DataSize = Pack.Size();

		if(!(Flags & MSGFLAG_NORECORD))
		{
			m_aDemoRecorder[ClientID].RecordMessage(Pack.Data(), Pack.Size());
			m_aDemoRecorder[MAX_CLIENTS].RecordMessage(Pack.Data(), Pack.Size());
		}

		if(!(Flags & MSGFLAG_NOSEND))
			m_NetServer.Send(&Packet);
	}

	return 0;
}

void CServer::SendMsgRaw(int ClientID, const void *pData, int Size, int Flags)
{
	CNetChunk Packet;
	mem_zero(&Packet, sizeof(CNetChunk));
	Packet.m_ClientID = ClientID;
	Packet.m_pData = pData;
	Packet.m_DataSize = Size;
	Packet.m_Flags = 0;
	if(Flags & MSGFLAG_VITAL)
	{
		Packet.m_Flags |= NETSENDFLAG_VITAL;
	}
	if(Flags & MSGFLAG_FLUSH)
	{
		Packet.m_Flags |= NETSENDFLAG_FLUSH;
	}
	m_NetServer.Send(&Packet);
}

void CServer::DoSnapshot()
{
	GameServer()->OnPreSnap();

	// create snapshot for demo recording
	if(m_aDemoRecorder[MAX_CLIENTS].IsRecording())
	{
		char aData[CSnapshot::MAX_SIZE];
		int SnapshotSize;

		// build snap and possibly add some messages
		m_SnapshotBuilder.Init();
		GameServer()->OnSnap(-1);
		SnapshotSize = m_SnapshotBuilder.Finish(aData);

		// for antiping: if the projectile netobjects contains extra data, this is removed and the original content restored before recording demo
		unsigned char aExtraInfoRemoved[CSnapshot::MAX_SIZE];
		mem_copy(aExtraInfoRemoved, aData, SnapshotSize);
		SnapshotRemoveExtraInfo(aExtraInfoRemoved);
		// write snapshot
		m_aDemoRecorder[MAX_CLIENTS].RecordSnapshot(Tick(), aExtraInfoRemoved, SnapshotSize);
	}

	// create snapshots for all clients
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		// client must be ingame to receive snapshots
		if(m_aClients[i].m_State != CClient::STATE_INGAME)
			continue;

		// this client is trying to recover, don't spam snapshots
		if(m_aClients[i].m_SnapRate == CClient::SNAPRATE_RECOVER && (Tick() % 50) != 0)
			continue;

		// this client is trying to recover, don't spam snapshots
		if(m_aClients[i].m_SnapRate == CClient::SNAPRATE_INIT && (Tick() % 10) != 0)
			continue;

		{
			char aData[CSnapshot::MAX_SIZE];
			CSnapshot *pData = (CSnapshot *)aData; // Fix compiler warning for strict-aliasing
			char aDeltaData[CSnapshot::MAX_SIZE];
			char aCompData[CSnapshot::MAX_SIZE];
			int SnapshotSize;
			int Crc;
			static CSnapshot s_EmptySnap;
			CSnapshot *pDeltashot = &s_EmptySnap;
			int DeltashotSize;
			int DeltaTick = -1;
			int DeltaSize;

			m_SnapshotBuilder.Init(m_aClients[i].m_Sixup);

			GameServer()->OnSnap(i);

			// finish snapshot
			SnapshotSize = m_SnapshotBuilder.Finish(pData);

			if(m_aDemoRecorder[i].IsRecording())
			{
				// for antiping: if the projectile netobjects contains extra data, this is removed and the original content restored before recording demo
				unsigned char aExtraInfoRemoved[CSnapshot::MAX_SIZE];
				mem_copy(aExtraInfoRemoved, aData, SnapshotSize);
				SnapshotRemoveExtraInfo(aExtraInfoRemoved);
				// write snapshot
				m_aDemoRecorder[i].RecordSnapshot(Tick(), aExtraInfoRemoved, SnapshotSize);
			}

			Crc = pData->Crc();

			// remove old snapshos
			// keep 3 seconds worth of snapshots
			m_aClients[i].m_Snapshots.PurgeUntil(m_CurrentGameTick - SERVER_TICK_SPEED * 3);

			// save it the snapshot
			m_aClients[i].m_Snapshots.Add(m_CurrentGameTick, time_get(), SnapshotSize, pData, 0);

			// find snapshot that we can perform delta against
			s_EmptySnap.Clear();

			{
				DeltashotSize = m_aClients[i].m_Snapshots.Get(m_aClients[i].m_LastAckedSnapshot, 0, &pDeltashot, 0);
				if(DeltashotSize >= 0)
					DeltaTick = m_aClients[i].m_LastAckedSnapshot;
				else
				{
					// no acked package found, force client to recover rate
					if(m_aClients[i].m_SnapRate == CClient::SNAPRATE_FULL)
						m_aClients[i].m_SnapRate = CClient::SNAPRATE_RECOVER;
				}
			}

			// create delta
			m_SnapshotDelta.SetStaticsize(protocol7::NETEVENTTYPE_SOUNDWORLD, m_aClients[i].m_Sixup);
			m_SnapshotDelta.SetStaticsize(protocol7::NETEVENTTYPE_DAMAGE, m_aClients[i].m_Sixup);
			DeltaSize = m_SnapshotDelta.CreateDelta(pDeltashot, pData, aDeltaData);

			if(DeltaSize)
			{
				// compress it
				int SnapshotSize;
				const int MaxSize = MAX_SNAPSHOT_PACKSIZE;
				int NumPackets;

				SnapshotSize = CVariableInt::Compress(aDeltaData, DeltaSize, aCompData, sizeof(aCompData));
				NumPackets = (SnapshotSize + MaxSize - 1) / MaxSize;

				for(int n = 0, Left = SnapshotSize; Left > 0; n++)
				{
					int Chunk = Left < MaxSize ? Left : MaxSize;
					Left -= Chunk;

					if(NumPackets == 1)
					{
						CMsgPacker Msg(NETMSG_SNAPSINGLE, true);
						Msg.AddInt(m_CurrentGameTick);
						Msg.AddInt(m_CurrentGameTick - DeltaTick);
						Msg.AddInt(Crc);
						Msg.AddInt(Chunk);
						Msg.AddRaw(&aCompData[n * MaxSize], Chunk);
						SendMsg(&Msg, MSGFLAG_FLUSH, i);
					}
					else
					{
						CMsgPacker Msg(NETMSG_SNAP, true);
						Msg.AddInt(m_CurrentGameTick);
						Msg.AddInt(m_CurrentGameTick - DeltaTick);
						Msg.AddInt(NumPackets);
						Msg.AddInt(n);
						Msg.AddInt(Crc);
						Msg.AddInt(Chunk);
						Msg.AddRaw(&aCompData[n * MaxSize], Chunk);
						SendMsg(&Msg, MSGFLAG_FLUSH, i);
					}
				}
			}
			else
			{
				CMsgPacker Msg(NETMSG_SNAPEMPTY, true);
				Msg.AddInt(m_CurrentGameTick);
				Msg.AddInt(m_CurrentGameTick - DeltaTick);
				SendMsg(&Msg, MSGFLAG_FLUSH, i);
			}
		}
	}

	GameServer()->OnPostSnap();
}

int CServer::ClientRejoinCallback(int ClientID, void *pUser)
{
	CServer *pThis = (CServer *)pUser;

	pThis->m_aClients[ClientID].m_Authed = AUTHED_NO;
	pThis->m_aClients[ClientID].m_AuthKey = -1;
	pThis->m_aClients[ClientID].m_pRconCmdToSend = 0;

	pThis->m_aClients[ClientID].Reset();

	pThis->SendMap(ClientID);

	return 0;
}

int CServer::NewClientNoAuthCallback(int ClientID, void *pUser)
{
	CServer *pThis = (CServer *)pUser;

	pThis->m_aClients[ClientID].m_DnsblState = CClient::DNSBL_STATE_NONE;

	pThis->m_aClients[ClientID].m_State = CClient::STATE_CONNECTING;
	pThis->m_aClients[ClientID].m_SupportsMapSha256 = false;
	pThis->m_aClients[ClientID].m_aName[0] = 0;
	pThis->m_aClients[ClientID].m_aClan[0] = 0;
	pThis->m_aClients[ClientID].m_Country = -1;
	pThis->m_aClients[ClientID].m_Authed = AUTHED_NO;
	pThis->m_aClients[ClientID].m_AuthKey = -1;
	pThis->m_aClients[ClientID].m_AuthTries = 0;
	pThis->m_aClients[ClientID].m_pRconCmdToSend = 0;
	pThis->m_aClients[ClientID].m_ShowIps = false;
	pThis->m_aClients[ClientID].m_DisruptiveLeave = false;
	pThis->m_aClients[ClientID].m_LoginAxiom = false;
	pThis->m_aClients[ClientID].Reset();

	pThis->SendCapabilities(ClientID);
	pThis->SendMap(ClientID);
#if defined(CONF_FAMILY_UNIX)
	pThis->SendConnLoggingCommand(OPEN_SESSION, pThis->m_NetServer.ClientAddr(ClientID));
#endif
	return 0;
}

int CServer::NewClientCallback(int ClientID, void *pUser, bool Sixup)
{
	CServer *pThis = (CServer *)pUser;
	pThis->m_aClients[ClientID].m_State = CClient::STATE_PREAUTH;
	pThis->m_aClients[ClientID].m_SupportsMapSha256 = false;
	pThis->m_aClients[ClientID].m_DnsblState = CClient::DNSBL_STATE_NONE;
	pThis->m_aClients[ClientID].m_aName[0] = 0;
	pThis->m_aClients[ClientID].m_aClan[0] = 0;
	pThis->m_aClients[ClientID].m_Country = -1;
	pThis->m_aClients[ClientID].m_Authed = AUTHED_NO;
	pThis->m_aClients[ClientID].m_AuthKey = -1;
	pThis->m_aClients[ClientID].m_AuthTries = 0;
	pThis->m_aClients[ClientID].m_pRconCmdToSend = 0;
	pThis->m_aClients[ClientID].m_Traffic = 0;
	pThis->m_aClients[ClientID].m_TrafficSince = 0;
	pThis->m_aClients[ClientID].m_ShowIps = false;
	pThis->m_aClients[ClientID].m_DisruptiveLeave = false;
	pThis->m_aClients[ClientID].m_LoginAxiom = false;
	memset(&pThis->m_aClients[ClientID].m_Addr, 0, sizeof(NETADDR));
	pThis->m_aClients[ClientID].Reset();

	pThis->GameServer()->OnClientEngineJoin(ClientID, Sixup);
	pThis->Antibot()->OnEngineClientJoin(ClientID, Sixup);

	pThis->m_aClients[ClientID].m_Sixup = Sixup;

#if defined(CONF_FAMILY_UNIX)
	pThis->SendConnLoggingCommand(OPEN_SESSION, pThis->m_NetServer.ClientAddr(ClientID));
#endif
	return 0;
}

int CServer::ClientCheckDisruptive(int ClientID, void *pUser)
{
	CServer *pThis = (CServer *)pUser;
	if(pThis->GameServer()->CheckDisruptiveLeave(ClientID))
		return 1;
	return 0;
}

void CServer::InitDnsbl(int ClientID)
{
	NETADDR Addr = *m_NetServer.ClientAddr(ClientID);

	//TODO: support ipv6
	if(Addr.type != NETTYPE_IPV4)
		return;

	// build dnsbl host lookup
	char aBuf[256];
	if(g_Config.m_SvDnsblKey[0] == '\0')
	{
		// without key
		str_format(aBuf, sizeof(aBuf), "%d.%d.%d.%d.%s", Addr.ip[3], Addr.ip[2], Addr.ip[1], Addr.ip[0], g_Config.m_SvDnsblHost);
	}
	else
	{
		// with key
		str_format(aBuf, sizeof(aBuf), "%s.%d.%d.%d.%d.%s", g_Config.m_SvDnsblKey, Addr.ip[3], Addr.ip[2], Addr.ip[1], Addr.ip[0], g_Config.m_SvDnsblHost);
	}

	IEngine *pEngine = Kernel()->RequestInterface<IEngine>();
	pEngine->AddJob(m_aClients[ClientID].m_pDnsblLookup = std::make_shared<CHostLookup>(aBuf, NETTYPE_IPV4));
	m_aClients[ClientID].m_DnsblState = CClient::DNSBL_STATE_PENDING;
}

#ifdef CONF_FAMILY_UNIX
void CServer::SendConnLoggingCommand(CONN_LOGGING_CMD Cmd, const NETADDR *pAddr)
{
	if(!g_Config.m_SvConnLoggingServer[0] || !m_ConnLoggingSocketCreated)
		return;

	// pack the data and send it
	unsigned char aData[23] = {0};
	aData[0] = Cmd;
	mem_copy(&aData[1], &pAddr->type, 4);
	mem_copy(&aData[5], pAddr->ip, 16);
	mem_copy(&aData[21], &pAddr->port, 2);

	net_unix_send(m_ConnLoggingSocket, &m_ConnLoggingDestAddr, aData, sizeof(aData));
}
#endif

int CServer::DelClientCallback(int ClientID, const char *pReason, void *pUser)
{
	CServer *pThis = (CServer *)pUser;

	char aAddrStr[NETADDR_MAXSTRSIZE];
	net_addr_str(pThis->m_NetServer.ClientAddr(ClientID), aAddrStr, sizeof(aAddrStr), true);

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "client dropped. cid=%d addr=<{%s}> reason='%s'", ClientID, aAddrStr, pReason);
	pThis->Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "server", aBuf);

	// notify the mod about the drop
	if(pThis->m_aClients[ClientID].m_State >= CClient::STATE_READY)
		pThis->GameServer()->OnClientDrop(ClientID, pReason);

	pThis->m_aClients[ClientID].m_State = CClient::STATE_EMPTY;
	pThis->m_aClients[ClientID].m_SupportsMapSha256 = false;
	pThis->m_aClients[ClientID].m_aName[0] = 0;
	pThis->m_aClients[ClientID].m_aClan[0] = 0;
	pThis->m_aClients[ClientID].m_Country = -1;
	pThis->m_aClients[ClientID].m_Authed = AUTHED_NO;
	pThis->m_aClients[ClientID].m_AuthKey = -1;
	pThis->m_aClients[ClientID].m_AuthTries = 0;
	pThis->m_aClients[ClientID].m_pRconCmdToSend = 0;
	pThis->m_aClients[ClientID].m_Traffic = 0;
	pThis->m_aClients[ClientID].m_TrafficSince = 0;
	pThis->m_aClients[ClientID].m_ShowIps = false;
	pThis->m_aClients[ClientID].m_DisruptiveLeave = false;
	pThis->m_aClients[ClientID].m_LoginAxiom = false;
	pThis->m_aPrevStates[ClientID] = CClient::STATE_EMPTY;
	pThis->m_aClients[ClientID].m_Snapshots.PurgeAll();
	pThis->m_aClients[ClientID].m_Sixup = false;

	pThis->GameServer()->OnClientEngineDrop(ClientID, pReason);
	pThis->Antibot()->OnEngineClientDrop(ClientID, pReason);
#if defined(CONF_FAMILY_UNIX)
	pThis->SendConnLoggingCommand(CLOSE_SESSION, pThis->m_NetServer.ClientAddr(ClientID));
#endif
	return 0;
}

void CServer::SendRconType(int ClientID, bool UsernameReq)
{
	CMsgPacker Msg(NETMSG_RCONTYPE, true);
	Msg.AddInt(UsernameReq);
	SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CServer::GetMapInfo(char *pMapName, int MapNameSize, int *pMapSize, SHA256_DIGEST *pMapSha256, int *pMapCrc)
{
	str_copy(pMapName, GetMapName(), MapNameSize);
	*pMapSize = m_aCurrentMapSize[SIX];
	*pMapSha256 = m_aCurrentMapSha256[SIX];
	*pMapCrc = m_aCurrentMapCrc[SIX];
}

void CServer::SendCapabilities(int ClientID)
{
	CMsgPacker Msg(NETMSG_CAPABILITIES, true);
	Msg.AddInt(SERVERCAP_CURVERSION); // version
	Msg.AddInt(SERVERCAPFLAG_DDNET | SERVERCAPFLAG_CHATTIMEOUTCODE | SERVERCAPFLAG_ANYPLAYERFLAG | SERVERCAPFLAG_PINGEX); // flags
	SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CServer::SendMap(int ClientID)
{
	int Sixup = IsSixup(ClientID);
	{
		CMsgPacker Msg(NETMSG_MAP_DETAILS, true);
		Msg.AddString(GetMapName(), 0);
		Msg.AddRaw(&m_aCurrentMapSha256[Sixup].data, sizeof(m_aCurrentMapSha256[Sixup].data));
		Msg.AddInt(m_aCurrentMapCrc[Sixup]);
		Msg.AddInt(m_aCurrentMapSize[Sixup]);
		SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
	}
	{
		CMsgPacker Msg(NETMSG_MAP_CHANGE, true);
		Msg.AddString(GetMapName(), 0);
		Msg.AddInt(m_aCurrentMapCrc[Sixup]);
		Msg.AddInt(m_aCurrentMapSize[Sixup]);
		if(Sixup)
		{
			Msg.AddInt(g_Config.m_SvMapWindow);
			Msg.AddInt(1024 - 128);
			Msg.AddRaw(m_aCurrentMapSha256[Sixup].data, sizeof(m_aCurrentMapSha256[Sixup].data));
		}
		SendMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_FLUSH, ClientID);
	}

	m_aClients[ClientID].m_NextMapChunk = 0;
}

void CServer::SendMapData(int ClientID, int Chunk)
{
	int Sixup = IsSixup(ClientID);
	unsigned int ChunkSize = 1024 - 128;
	unsigned int Offset = Chunk * ChunkSize;
	int Last = 0;

	// drop faulty map data requests
	if(Chunk < 0 || Offset > m_aCurrentMapSize[Sixup])
		return;

	if(Offset + ChunkSize >= m_aCurrentMapSize[Sixup])
	{
		ChunkSize = m_aCurrentMapSize[Sixup] - Offset;
		Last = 1;
	}

	CMsgPacker Msg(NETMSG_MAP_DATA, true);
	if(!Sixup)
	{
		Msg.AddInt(Last);
		Msg.AddInt(m_aCurrentMapCrc[SIX]);
		Msg.AddInt(Chunk);
		Msg.AddInt(ChunkSize);
	}
	Msg.AddRaw(&m_apCurrentMapData[Sixup][Offset], ChunkSize);
	SendMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_FLUSH, ClientID);

	if(g_Config.m_Debug)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "sending chunk %d with size %d", Chunk, ChunkSize);
		Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "server", aBuf);
	}
}

void CServer::SendConnectionReady(int ClientID)
{
	CMsgPacker Msg(NETMSG_CON_READY, true);
	SendMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_FLUSH, ClientID);
}

void CServer::SendRconLine(int ClientID, const char *pLine)
{
	CMsgPacker Msg(NETMSG_RCON_LINE, true);
	Msg.AddString(pLine, 512);
	SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CServer::SendRconLineAuthed(const char *pLine, void *pUser)
{
	CServer *pThis = (CServer *)pUser;
	static volatile int s_ReentryGuard = 0;
	int i;

	if(s_ReentryGuard)
		return;
	s_ReentryGuard++;

	const char *pStart = str_find(pLine, "<{");
	const char *pEnd = pStart == NULL ? NULL : str_find(pStart + 2, "}>");
	const char *pLineWithoutIps;
	char aLine[512];
	char aLineWithoutIps[512];
	aLine[0] = '\0';
	aLineWithoutIps[0] = '\0';

	if(pStart == NULL || pEnd == NULL)
	{
		pLineWithoutIps = pLine;
	}
	else
	{
		str_append(aLine, pLine, pStart - pLine + 1);
		str_append(aLine, pStart + 2, pStart - pLine + pEnd - pStart - 1);
		str_append(aLine, pEnd + 2, sizeof(aLine));

		str_append(aLineWithoutIps, pLine, pStart - pLine + 1);
		str_append(aLineWithoutIps, "XXX", sizeof(aLineWithoutIps));
		str_append(aLineWithoutIps, pEnd + 2, sizeof(aLineWithoutIps));

		pLine = aLine;
		pLineWithoutIps = aLineWithoutIps;
	}

	for(i = 0; i < MAX_CLIENTS; i++)
	{
		if(pThis->m_aClients[i].m_State != CClient::STATE_EMPTY && pThis->m_aClients[i].m_Authed >= pThis->m_RconAuthLevel && (pThis->m_RconRestrict == -1 || pThis->m_RconRestrict == i))
			pThis->SendRconLine(i, pThis->m_aClients[i].m_ShowIps ? pLine : pLineWithoutIps);
	}

	s_ReentryGuard--;
}

void CServer::SendRconCmdAdd(const IConsole::CCommandInfo *pCommandInfo, int ClientID)
{
	CMsgPacker Msg(NETMSG_RCON_CMD_ADD, true);
	Msg.AddString(pCommandInfo->m_pName, IConsole::TEMPCMD_NAME_LENGTH);
	Msg.AddString(pCommandInfo->m_pHelp, IConsole::TEMPCMD_HELP_LENGTH);
	Msg.AddString(pCommandInfo->m_pParams, IConsole::TEMPCMD_PARAMS_LENGTH);
	SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CServer::SendRconCmdRem(const IConsole::CCommandInfo *pCommandInfo, int ClientID)
{
	CMsgPacker Msg(NETMSG_RCON_CMD_REM, true);
	Msg.AddString(pCommandInfo->m_pName, 256);
	SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CServer::UpdateClientRconCommands()
{
	int ClientID = Tick() % MAX_CLIENTS;

	if(m_aClients[ClientID].m_State != CClient::STATE_EMPTY && m_aClients[ClientID].m_Authed)
	{
		int ConsoleAccessLevel = m_aClients[ClientID].m_Authed == AUTHED_ADMIN ? IConsole::ACCESS_LEVEL_ADMIN : m_aClients[ClientID].m_Authed == AUTHED_MOD ? IConsole::ACCESS_LEVEL_MOD :
                                                                                                                                                                      IConsole::ACCESS_LEVEL_HELPER;
		for(int i = 0; i < MAX_RCONCMD_SEND && m_aClients[ClientID].m_pRconCmdToSend; ++i)
		{
			SendRconCmdAdd(m_aClients[ClientID].m_pRconCmdToSend, ClientID);
			m_aClients[ClientID].m_pRconCmdToSend = m_aClients[ClientID].m_pRconCmdToSend->NextCommandInfo(ConsoleAccessLevel, CFGFLAG_SERVER);
		}
	}
}

static inline int MsgFromSixup(int Msg, bool System)
{
	if(System)
	{
		if(Msg == NETMSG_INFO)
			;
		else if(Msg >= 14 && Msg <= 15)
			Msg += 11;
		else if(Msg >= 18 && Msg <= 28)
			Msg = NETMSG_READY + Msg - 18;
		else if(Msg < OFFSET_UUID)
			return -1;
	}

	return Msg;
}

void CServer::ProcessClientPacket(CNetChunk *pPacket)
{
	int ClientID = pPacket->m_ClientID;
	CUnpacker Unpacker;
	Unpacker.Reset(pPacket->m_pData, pPacket->m_DataSize);
	CMsgPacker Packer(NETMSG_EX, true);

	int GameFlags = 0;
	if(pPacket->m_Flags & NET_CHUNKFLAG_VITAL)
	{
		GameFlags |= MSGFLAG_VITAL;
	}
	Antibot()->OnEngineClientMessage(ClientID, pPacket->m_pData, pPacket->m_DataSize, GameFlags);

	// unpack msgid and system flag
	int Msg;
	bool Sys;
	CUuid Uuid;

	int Result = UnpackMessageID(&Msg, &Sys, &Uuid, &Unpacker, &Packer);
	if(Result == UNPACKMESSAGE_ERROR)
	{
		return;
	}

	if(m_aClients[ClientID].m_Sixup && (Msg = MsgFromSixup(Msg, Sys)) < 0)
	{
		return;
	}

	if(g_Config.m_SvNetlimit && Msg != NETMSG_REQUEST_MAP_DATA)
	{
		int64 Now = time_get();
		int64 Diff = Now - m_aClients[ClientID].m_TrafficSince;
		float Alpha = g_Config.m_SvNetlimitAlpha / 100.0f;
		float Limit = (float)g_Config.m_SvNetlimit * 1024 / time_freq();

		if(m_aClients[ClientID].m_Traffic > Limit)
		{
			m_NetServer.NetBan()->BanAddr(&pPacket->m_Address, 600, "Stressing network");
			return;
		}
		if(Diff > 100)
		{
			m_aClients[ClientID].m_Traffic = (Alpha * ((float)pPacket->m_DataSize / Diff)) + (1.0f - Alpha) * m_aClients[ClientID].m_Traffic;
			m_aClients[ClientID].m_TrafficSince = Now;
		}
	}

	if(Result == UNPACKMESSAGE_ANSWER)
	{
		SendMsg(&Packer, MSGFLAG_VITAL, ClientID);
	}

	if(Sys)
	{
		// system message
		if(Msg == NETMSG_CLIENTVER)
		{
			if((pPacket->m_Flags & NET_CHUNKFLAG_VITAL) != 0 && m_aClients[ClientID].m_State == CClient::STATE_PREAUTH)
			{
				CUuid *pConnectionID = (CUuid *)Unpacker.GetRaw(sizeof(*pConnectionID));
				int DDNetVersion = Unpacker.GetInt();
				const char *pDDNetVersionStr = Unpacker.GetString(CUnpacker::SANITIZE_CC);
				if(Unpacker.Error() || !str_utf8_check(pDDNetVersionStr) || DDNetVersion < 0)
				{
					return;
				}
				m_aClients[ClientID].m_ConnectionID = *pConnectionID;
				m_aClients[ClientID].m_DDNetVersion = DDNetVersion;
				str_copy(m_aClients[ClientID].m_aDDNetVersionStr, pDDNetVersionStr, sizeof(m_aClients[ClientID].m_aDDNetVersionStr));
				m_aClients[ClientID].m_DDNetVersionSettled = true;
				m_aClients[ClientID].m_GotDDNetVersionPacket = true;
				m_aClients[ClientID].m_State = CClient::STATE_AUTH;
			}
		}
		else if(Msg == NETMSG_INFO)
		{
			if((pPacket->m_Flags & NET_CHUNKFLAG_VITAL) != 0 && (m_aClients[ClientID].m_State == CClient::STATE_PREAUTH || m_aClients[ClientID].m_State == CClient::STATE_AUTH))
			{
				const char *pVersion = Unpacker.GetString(CUnpacker::SANITIZE_CC);
				if(!str_utf8_check(pVersion))
				{
					return;
				}
				if(str_comp(pVersion, GameServer()->NetVersion()) != 0 && str_comp(pVersion, "0.7 802f1be60a05665f") != 0)
				{
					// wrong version
					char aReason[256];
					str_format(aReason, sizeof(aReason), "Wrong version. Server is running '%s' and client '%s'", GameServer()->NetVersion(), pVersion);
					m_NetServer.Drop(ClientID, aReason);
					return;
				}

				const char *pPassword = Unpacker.GetString(CUnpacker::SANITIZE_CC);
				if(!str_utf8_check(pPassword))
				{
					return;
				}
				if(g_Config.m_Password[0] != 0 && str_comp(g_Config.m_Password, pPassword) != 0)
				{
					// wrong password
					m_NetServer.Drop(ClientID, "Wrong password");
					return;
				}

				// reserved slot
				if(ClientID >= (g_Config.m_SvMaxClients - g_Config.m_SvReservedSlots) && g_Config.m_SvReservedSlotsPass[0] != 0 && str_comp(g_Config.m_SvReservedSlotsPass, pPassword) != 0)
				{
					m_NetServer.Drop(ClientID, "This server is full");
					return;
				}

				m_aClients[ClientID].m_State = CClient::STATE_CONNECTING;
				SendRconType(ClientID, m_AuthManager.NumNonDefaultKeys() > 0);
				SendCapabilities(ClientID);
				SendMap(ClientID);
			}
		}
		else if(Msg == NETMSG_REQUEST_MAP_DATA)
		{
			if((pPacket->m_Flags & NET_CHUNKFLAG_VITAL) == 0 || m_aClients[ClientID].m_State < CClient::STATE_CONNECTING)
				return;

			if(m_aClients[ClientID].m_Sixup)
			{
				for(int i = 0; i < g_Config.m_SvMapWindow; i++)
				{
					SendMapData(ClientID, m_aClients[ClientID].m_NextMapChunk++);
				}
				return;
			}

			int Chunk = Unpacker.GetInt();
			if(Chunk != m_aClients[ClientID].m_NextMapChunk || !g_Config.m_SvFastDownload)
			{
				SendMapData(ClientID, Chunk);
				return;
			}

			if(Chunk == 0)
			{
				for(int i = 0; i < g_Config.m_SvMapWindow; i++)
				{
					SendMapData(ClientID, i);
				}
			}
			SendMapData(ClientID, g_Config.m_SvMapWindow + m_aClients[ClientID].m_NextMapChunk);
			m_aClients[ClientID].m_NextMapChunk++;
		}
		else if(Msg == NETMSG_READY)
		{
			if((pPacket->m_Flags & NET_CHUNKFLAG_VITAL) != 0 && (m_aClients[ClientID].m_State == CClient::STATE_CONNECTING))
			{
				char aAddrStr[NETADDR_MAXSTRSIZE];
				net_addr_str(m_NetServer.ClientAddr(ClientID), aAddrStr, sizeof(aAddrStr), true);

				char aBuf[256];
				str_format(aBuf, sizeof(aBuf), "player is ready. ClientID=%d addr=<{%s}> secure=%s", ClientID, aAddrStr, m_NetServer.HasSecurityToken(ClientID) ? "yes" : "no");
				Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "server", aBuf);

				void *pPersistentData = 0;
				if(m_aClients[ClientID].m_HasPersistentData)
				{
					pPersistentData = m_aClients[ClientID].m_pPersistentData;
					m_aClients[ClientID].m_HasPersistentData = false;
				}
				m_aClients[ClientID].m_State = CClient::STATE_READY;
				GameServer()->OnClientConnected(ClientID, pPersistentData);
			}

			SendConnectionReady(ClientID);
		}
		else if(Msg == NETMSG_ENTERGAME)
		{
			if((pPacket->m_Flags & NET_CHUNKFLAG_VITAL) != 0 && m_aClients[ClientID].m_State == CClient::STATE_READY && GameServer()->IsClientReadyToEnter(ClientID))
			{
				char aAddrStr[NETADDR_MAXSTRSIZE];
				net_addr_str(m_NetServer.ClientAddr(ClientID), aAddrStr, sizeof(aAddrStr), true);

				char aBuf[256];
				str_format(aBuf, sizeof(aBuf), "player has entered the game. ClientID=%d addr=<{%s}> sixup=%d", ClientID, aAddrStr, IsSixup(ClientID));
				Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
				m_aClients[ClientID].m_State = CClient::STATE_INGAME;
				if(IsSixup(ClientID))
				{
					CMsgPacker Msg(4, true, true); //NETMSG_SERVERINFO //TODO: Import the shared protocol from 7 aswell
					GetServerInfoSixup(&Msg, -1, false);
					SendMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_FLUSH, ClientID);
				}
				GameServer()->OnClientEnter(ClientID);
			}
		}
		else if(Msg == NETMSG_INPUT)
		{
			CClient::CInput *pInput;
			int64 TagTime;

			m_aClients[ClientID].m_LastAckedSnapshot = Unpacker.GetInt();
			int IntendedTick = Unpacker.GetInt();
			int Size = Unpacker.GetInt();

			// check for errors
			if(Unpacker.Error() || Size / 4 > MAX_INPUT_SIZE)
				return;

			if(m_aClients[ClientID].m_LastAckedSnapshot > 0)
				m_aClients[ClientID].m_SnapRate = CClient::SNAPRATE_FULL;

			if(m_aClients[ClientID].m_Snapshots.Get(m_aClients[ClientID].m_LastAckedSnapshot, &TagTime, 0, 0) >= 0)
				m_aClients[ClientID].m_Latency = (int)(((time_get() - TagTime) * 1000) / time_freq());

			// add message to report the input timing
			// skip packets that are old
			if(IntendedTick > m_aClients[ClientID].m_LastInputTick)
			{
				int TimeLeft = ((TickStartTime(IntendedTick) - time_get()) * 1000) / time_freq();

				CMsgPacker Msg(NETMSG_INPUTTIMING, true);
				Msg.AddInt(IntendedTick);
				Msg.AddInt(TimeLeft);
				SendMsg(&Msg, 0, ClientID);
			}

			m_aClients[ClientID].m_LastInputTick = IntendedTick;

			pInput = &m_aClients[ClientID].m_aInputs[m_aClients[ClientID].m_CurrentInput];

			if(IntendedTick <= Tick())
				IntendedTick = Tick() + 1;

			pInput->m_GameTick = IntendedTick;

			for(int i = 0; i < Size / 4; i++)
				pInput->m_aData[i] = Unpacker.GetInt();

			mem_copy(m_aClients[ClientID].m_LatestInput.m_aData, pInput->m_aData, MAX_INPUT_SIZE * sizeof(int));

			m_aClients[ClientID].m_CurrentInput++;
			m_aClients[ClientID].m_CurrentInput %= 200;

			// call the mod with the fresh input data
			if(m_aClients[ClientID].m_State == CClient::STATE_INGAME)
				GameServer()->OnClientDirectInput(ClientID, m_aClients[ClientID].m_LatestInput.m_aData);
		}
		else if(Msg == NETMSG_RCON_CMD)
		{
			const char *pCmd = Unpacker.GetString();
			if(!str_utf8_check(pCmd))
			{
				return;
			}
			if(Unpacker.Error() == 0 && !str_comp(pCmd, "crashmeplx"))
			{
				int Version = m_aClients[ClientID].m_DDNetVersion;
				if(GameServer()->PlayerExists(ClientID) && Version < VERSION_DDNET_OLD)
				{
					m_aClients[ClientID].m_DDNetVersion = VERSION_DDNET_OLD;
				}
			}
			else if((pPacket->m_Flags & NET_CHUNKFLAG_VITAL) != 0 && Unpacker.Error() == 0 && m_aClients[ClientID].m_Authed)
			{
				if(GameServer()->PlayerExists(ClientID))
				{
					char aBuf[256];
					str_format(aBuf, sizeof(aBuf), "ClientID=%d rcon='%s'", ClientID, pCmd);
					Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "server", aBuf);
					m_RconClientID = ClientID;
					m_RconAuthLevel = m_aClients[ClientID].m_Authed;
					Console()->SetAccessLevel(m_aClients[ClientID].m_Authed == AUTHED_ADMIN ? IConsole::ACCESS_LEVEL_ADMIN : m_aClients[ClientID].m_Authed == AUTHED_MOD ? IConsole::ACCESS_LEVEL_MOD :
																	 m_aClients[ClientID].m_Authed == AUTHED_HELPER      ? IConsole::ACCESS_LEVEL_HELPER :
                                                                                                                                                                                               IConsole::ACCESS_LEVEL_USER);
					Console()->ExecuteLineFlag(pCmd, CFGFLAG_SERVER, ClientID);
					Console()->SetAccessLevel(IConsole::ACCESS_LEVEL_ADMIN);
					m_RconClientID = IServer::RCON_CID_SERV;
					m_RconAuthLevel = AUTHED_ADMIN;
				}
			}
		}
		else if(Msg == NETMSG_RCON_AUTH)
		{
			const char *pName = "";
			if(!IsSixup(ClientID))
				pName = Unpacker.GetString(CUnpacker::SANITIZE_CC); // login name, now used
			const char *pPw = Unpacker.GetString(CUnpacker::SANITIZE_CC);
			if(!str_utf8_check(pPw) || !str_utf8_check(pName))
			{
				return;
			}

			if((pPacket->m_Flags & NET_CHUNKFLAG_VITAL) != 0 && Unpacker.Error() == 0)
			{
				int AuthLevel = -1;
				int KeySlot = -1;

				if(!pName[0])
				{
					if(m_AuthManager.CheckKey((KeySlot = m_AuthManager.DefaultKey(AUTHED_ADMIN)), pPw))
						AuthLevel = AUTHED_ADMIN;
					else if(m_AuthManager.CheckKey((KeySlot = m_AuthManager.DefaultKey(AUTHED_MOD)), pPw))
						AuthLevel = AUTHED_MOD;
					else if(m_AuthManager.CheckKey((KeySlot = m_AuthManager.DefaultKey(AUTHED_HELPER)), pPw))
						AuthLevel = AUTHED_HELPER;
				}
				else
				{
					KeySlot = m_AuthManager.FindKey(pName);
					if(m_AuthManager.CheckKey(KeySlot, pPw))
						AuthLevel = m_AuthManager.KeyLevel(KeySlot);
				}

				if(AuthLevel != -1)
				{
					if(m_aClients[ClientID].m_Authed != AuthLevel)
					{
						if(!IsSixup(ClientID))
						{
							CMsgPacker Msg(NETMSG_RCON_AUTH_STATUS, true);
							Msg.AddInt(1); //authed
							Msg.AddInt(1); //cmdlist
							SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
						}
						else
						{
							CMsgPacker Msg(11, true, true); //NETMSG_RCON_AUTH_ON
							SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
						}

						m_aClients[ClientID].m_Authed = AuthLevel; // Keeping m_Authed around is unwise...
						m_aClients[ClientID].m_AuthKey = KeySlot;
						int SendRconCmds = IsSixup(ClientID) ? true : Unpacker.GetInt();
						if(Unpacker.Error() == 0 && SendRconCmds)
							// AUTHED_ADMIN - AuthLevel gets the proper IConsole::ACCESS_LEVEL_<x>
							m_aClients[ClientID].m_pRconCmdToSend = Console()->FirstCommandInfo(AUTHED_ADMIN - AuthLevel, CFGFLAG_SERVER);

						char aBuf[256];
						const char *pIdent = m_AuthManager.KeyIdent(KeySlot);
						switch(AuthLevel)
						{
						case AUTHED_ADMIN:
						{
							SendRconLine(ClientID, "Admin authentication successful. Full remote console access granted.");
							str_format(aBuf, sizeof(aBuf), "ClientID=%d authed with key=%s (admin)", ClientID, pIdent);
							break;
						}
						case AUTHED_MOD:
						{
							SendRconLine(ClientID, "Moderator authentication successful. Limited remote console access granted.");
							str_format(aBuf, sizeof(aBuf), "ClientID=%d authed with key=%s (moderator)", ClientID, pIdent);
							break;
						}
						case AUTHED_HELPER:
						{
							SendRconLine(ClientID, "Helper authentication successful. Limited remote console access granted.");
							str_format(aBuf, sizeof(aBuf), "ClientID=%d authed with key=%s (helper)", ClientID, pIdent);
							break;
						}
						}
						Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);

						// DDRace
						GameServer()->OnSetAuthed(ClientID, AuthLevel);
					}
				}
				else if(g_Config.m_SvRconMaxTries)
				{
					m_aClients[ClientID].m_AuthTries++;
					char aBuf[128];
					str_format(aBuf, sizeof(aBuf), "Wrong password %d/%d.", m_aClients[ClientID].m_AuthTries, g_Config.m_SvRconMaxTries);
					SendRconLine(ClientID, aBuf);
					if(m_aClients[ClientID].m_AuthTries >= g_Config.m_SvRconMaxTries)
					{
						if(!g_Config.m_SvRconBantime)
							m_NetServer.Drop(ClientID, "Too many remote console authentication tries");
						else
							m_ServerBan.BanAddr(m_NetServer.ClientAddr(ClientID), g_Config.m_SvRconBantime * 60, "Too many remote console authentication tries");
					}
				}
				else
				{
					SendRconLine(ClientID, "Wrong password.");
				}
			}
		}
		else if(Msg == NETMSG_PING)
		{
			CMsgPacker Msg(NETMSG_PING_REPLY, true);
			SendMsg(&Msg, 0, ClientID);
		}
		else if(Msg == NETMSG_PINGEX)
		{
			CUuid *pID = (CUuid *)Unpacker.GetRaw(sizeof(*pID));
			if(Unpacker.Error())
			{
				return;
			}
			CMsgPacker Msg(NETMSG_PONGEX, true);
			Msg.AddRaw(pID, sizeof(*pID));
			SendMsg(&Msg, MSGFLAG_FLUSH, ClientID);
		}
		else
		{
			if(g_Config.m_Debug)
			{
				char aHex[] = "0123456789ABCDEF";
				char aBuf[512];

				for(int b = 0; b < pPacket->m_DataSize && b < 32; b++)
				{
					aBuf[b * 3] = aHex[((const unsigned char *)pPacket->m_pData)[b] >> 4];
					aBuf[b * 3 + 1] = aHex[((const unsigned char *)pPacket->m_pData)[b] & 0xf];
					aBuf[b * 3 + 2] = ' ';
					aBuf[b * 3 + 3] = 0;
				}

				char aBufMsg[256];
				str_format(aBufMsg, sizeof(aBufMsg), "strange message ClientID=%d msg=%d data_size=%d", ClientID, Msg, pPacket->m_DataSize);
				Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "server", aBufMsg);
				Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "server", aBuf);
			}
		}
	}
	else
	{
		// game message
		if((pPacket->m_Flags & NET_CHUNKFLAG_VITAL) != 0 && m_aClients[ClientID].m_State >= CClient::STATE_READY)
			GameServer()->OnMessage(Msg, &Unpacker, ClientID);
	}
}

bool CServer::RateLimitServerInfoConnless()
{
	bool SendClients = true;
	if(g_Config.m_SvServerInfoPerSecond)
	{
		SendClients = m_ServerInfoNumRequests <= g_Config.m_SvServerInfoPerSecond;
		const int64 Now = Tick();

		if(Now <= m_ServerInfoFirstRequest + TickSpeed())
		{
			m_ServerInfoNumRequests++;
		}
		else
		{
			m_ServerInfoNumRequests = 1;
			m_ServerInfoFirstRequest = Now;
		}
	}

	return SendClients;
}

void CServer::SendServerInfoConnless(const NETADDR *pAddr, int Token, int Type)
{
	SendServerInfo(pAddr, Token, Type, RateLimitServerInfoConnless());
}

static inline int GetCacheIndex(int Type, bool SendClient)
{
	if(Type == SERVERINFO_INGAME)
		Type = SERVERINFO_VANILLA;
	else if(Type == SERVERINFO_EXTENDED_MORE)
		Type = SERVERINFO_EXTENDED;

	return Type * 2 + SendClient;
}

CServer::CCache::CCache()
{
	m_Cache.clear();
}

CServer::CCache::~CCache()
{
	Clear();
}

CServer::CCache::CCacheChunk::CCacheChunk(const void *pData, int Size)
{
	mem_copy(m_aData, pData, Size);
	m_DataSize = Size;
}

void CServer::CCache::AddChunk(const void *pData, int Size)
{
	m_Cache.emplace_back(pData, Size);
}

void CServer::CCache::Clear()
{
	m_Cache.clear();
}

void CServer::SendServerInfo(const NETADDR *pAddr, int Token, int Type, bool SendClients)
{
	// One chance to improve the protocol!
	CPacker p;
	char aBuf[256];

	// count the players
	int PlayerCount = 0, ClientCount = 0;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_aClients[i].m_State != CClient::STATE_EMPTY)
		{
			if(GameServer()->IsClientPlayer(i))
				PlayerCount++;

			ClientCount++;
		}
	}

	p.Reset();

#define ADD_RAW(p, x) (p).AddRaw(x, sizeof(x))
#define ADD_INT(p, x) \
	do \
	{ \
		str_format(aBuf, sizeof(aBuf), "%d", x); \
		(p).AddString(aBuf, 0); \
	} while(0)

	switch(Type)
	{
	case SERVERINFO_EXTENDED: ADD_RAW(p, SERVERBROWSE_INFO_EXTENDED); break;
	case SERVERINFO_64_LEGACY: ADD_RAW(p, SERVERBROWSE_INFO_64_LEGACY); break;
	case SERVERINFO_VANILLA: ADD_RAW(p, SERVERBROWSE_INFO); break;
	case SERVERINFO_INGAME: ADD_RAW(p, SERVERBROWSE_INFO); break;
	default: dbg_assert(false, "unknown serverinfo type");
	}

	ADD_INT(p, Token);

	p.AddString(GameServer()->Version(), 32);

	const char *pMapName = GetMapName();

	if(Type != SERVERINFO_VANILLA)
	{
		p.AddString(g_Config.m_SvName, 256);
	}
	else
	{
		if(m_NetServer.MaxClients() <= VANILLA_MAX_CLIENTS)
		{
			p.AddString(g_Config.m_SvName, 64);
		}
		else
		{
			const int MaxClients = maximum(ClientCount, m_NetServer.MaxClients() - g_Config.m_SvReservedSlots);
			str_format(aBuf, sizeof(aBuf), "%s [%d/%d]", g_Config.m_SvName, ClientCount, MaxClients);
			p.AddString(aBuf, 64);
		}
	}
	p.AddString(pMapName, 32);

	if(Type == SERVERINFO_EXTENDED)
	{
		ADD_INT(p, m_aCurrentMapCrc[SIX]);
		ADD_INT(p, m_aCurrentMapSize[SIX]);
	}

	// gametype
	p.AddString(GameServer()->GameType(), 16);

	// flags
	ADD_INT(p, g_Config.m_Password[0] ? SERVER_FLAG_PASSWORD : 0);

	int MaxClients = m_NetServer.MaxClients();
	// How many clients the used serverinfo protocol supports, has to be tracked
	// separately to make sure we don't subtract the reserved slots from it
	int MaxClientsProtocol = MAX_CLIENTS;
	if(Type == SERVERINFO_VANILLA || Type == SERVERINFO_INGAME)
	{
		if(ClientCount >= VANILLA_MAX_CLIENTS)
		{
			if(ClientCount < MaxClients)
				ClientCount = VANILLA_MAX_CLIENTS - 1;
			else
				ClientCount = VANILLA_MAX_CLIENTS;
		}
		MaxClientsProtocol = VANILLA_MAX_CLIENTS;
		if(PlayerCount > ClientCount)
			PlayerCount = ClientCount;
	}

	ADD_INT(p, PlayerCount); // num players
	ADD_INT(p, minimum(MaxClientsProtocol, maximum(MaxClients, PlayerCount))); // max players
	ADD_INT(p, ClientCount); // num clients
	ADD_INT(p, minimum(MaxClientsProtocol, maximum(MaxClients, ClientCount))); // max clients

	if(Type == SERVERINFO_EXTENDED)
		p.AddString("", 0); // extra info, reserved

	const void *pPrefix = p.Data();
	int PrefixSize = p.Size();

	CPacker q;
	CNetChunk Packet;
	int PacketsSent = 0;
	int PlayersSent = 0;
	Packet.m_ClientID = -1;
	Packet.m_Address = *pAddr;
	Packet.m_Flags = NETSENDFLAG_CONNLESS;

	#define SEND(size) \
		do \
		{ \
			Packet.m_pData = q.Data(); \
			Packet.m_DataSize = size; \
			m_NetServer.Send(&Packet); \
			PacketsSent++; \
		} while(0)

	#define RESET() \
		do \
		{ \
			q.Reset(); \
			q.AddRaw(pPrefix, PrefixSize); \
		} while(0)

	RESET();

	if(Type == SERVERINFO_64_LEGACY)
		q.AddInt(PlayersSent); // offset

	if(!SendClients)
	{
		SEND(q.Size());
		return;
	}

	if(Type == SERVERINFO_EXTENDED)
	{
		pPrefix = "";
		PrefixSize = 0;
	}

	int Remaining;
	switch(Type)
	{
	case SERVERINFO_EXTENDED: Remaining = -1; break;
	case SERVERINFO_64_LEGACY: Remaining = 24; break;
	case SERVERINFO_VANILLA: Remaining = VANILLA_MAX_CLIENTS; break;
	case SERVERINFO_INGAME: Remaining = VANILLA_MAX_CLIENTS; break;
	default: dbg_assert(0, "caught earlier, unreachable"); return;
	}

	// Use the following strategy for sending:
	// For vanilla, send the first 16 players.
	// For legacy 64p, send 24 players per packet.
	// For extended, send as much players as possible.

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_aClients[i].m_State != CClient::STATE_EMPTY)
		{
			if(Remaining == 0)
			{
				if(Type == SERVERINFO_VANILLA || Type == SERVERINFO_INGAME)
					break;

				// Otherwise we're SERVERINFO_64_LEGACY.
				SEND(q.Size());
				RESET();
				q.AddInt(PlayersSent); // offset
				Remaining = 24;
			}
			if(Remaining > 0)
			{
				Remaining--;
			}

			int PreviousSize = q.Size();

			q.AddString(ClientName(i), MAX_NAME_LENGTH); // client name
			q.AddString(ClientClan(i), MAX_CLAN_LENGTH); // client clan

			ADD_INT(q, m_aClients[i].m_Country); // client country
			ADD_INT(q, m_aClients[i].m_Score); // client score
			ADD_INT(q, GameServer()->IsClientPlayer(i) ? 1 : 0); // is player?
			if(Type == SERVERINFO_EXTENDED)
				q.AddString("", 0); // extra info, reserved

			if(Type == SERVERINFO_EXTENDED)
			{
				if(q.Size() >= NET_MAX_PAYLOAD - 18) // 8 bytes for type, 10 bytes for the largest token
				{
					// Retry current player.
					i--;
					SEND(PreviousSize);
					RESET();
					ADD_INT(q, PlayersSent);
					q.AddString("", 0); // extra info, reserved
					continue;
				}
			}
			PlayersSent++;
		}
	}

	SEND(q.Size());
	#undef SEND
	#undef RESET
	#undef ADD_RAW
	#undef ADD_INT
}

void CServer::GetServerInfoSixup(CPacker *pPacker, int Token, bool SendClients)
{
	if(Token != -1)
	{
		pPacker->Reset();
		pPacker->AddRaw(SERVERBROWSE_INFO, sizeof(SERVERBROWSE_INFO));
		pPacker->AddInt(Token);
	}

	SendClients = SendClients && Token != -1;

	CCache::CCacheChunk &FirstChunk = m_aSixupServerInfoCache[SendClients].m_Cache.front();
	pPacker->AddRaw(FirstChunk.m_aData, FirstChunk.m_DataSize);
}

void CServer::ExpireServerInfo()
{
	m_ServerInfoNeedsUpdate = true;
}

void CServer::UpdateRegisterServerInfo()
{
	// count the players
	int PlayerCount = 0, ClientCount = 0;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_aClients[i].IncludedInServerInfo())
		{
			if(GameServer()->IsClientPlayer(i))
				PlayerCount++;

			ClientCount++;
		}
	}

	int MaxPlayers = std::max(m_NetServer.MaxClients(), PlayerCount);
	int MaxClients = std::max(m_NetServer.MaxClients(), ClientCount);
	char aMapSha256[SHA256_MAXSTRSIZE];

	sha256_str(m_aCurrentMapSha256[SIX], aMapSha256, sizeof(aMapSha256));

	CJsonStringWriter JsonWriter;

	JsonWriter.BeginObject();
	JsonWriter.WriteAttribute("max_clients");
	JsonWriter.WriteIntValue(MaxClients);

	JsonWriter.WriteAttribute("max_players");
	JsonWriter.WriteIntValue(MaxPlayers);

	JsonWriter.WriteAttribute("passworded");
	JsonWriter.WriteBoolValue(g_Config.m_Password[0]);

	JsonWriter.WriteAttribute("game_type");
	JsonWriter.WriteStrValue(GameServer()->GameType());

	JsonWriter.WriteAttribute("name");
	JsonWriter.WriteStrValue(g_Config.m_SvName);

	JsonWriter.WriteAttribute("map");
	JsonWriter.BeginObject();
	JsonWriter.WriteAttribute("name");
	JsonWriter.WriteStrValue(GetMapName());
	JsonWriter.WriteAttribute("sha256");
	JsonWriter.WriteStrValue(aMapSha256);
	JsonWriter.WriteAttribute("size");
	JsonWriter.WriteIntValue(m_aCurrentMapSize[SIX]);
	JsonWriter.EndObject();

	JsonWriter.WriteAttribute("version");
	JsonWriter.WriteStrValue(GameServer()->Version());

	JsonWriter.WriteAttribute("client_score_kind");
	JsonWriter.WriteStrValue("points"); // "points" or "time"

	JsonWriter.WriteAttribute("requires_login");
	JsonWriter.WriteBoolValue(false);

	JsonWriter.WriteAttribute("clients");
	JsonWriter.BeginArray();

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_aClients[i].IncludedInServerInfo())
		{
			JsonWriter.BeginObject();

			JsonWriter.WriteAttribute("name");
			JsonWriter.WriteStrValue(ClientName(i));

			JsonWriter.WriteAttribute("clan");
			JsonWriter.WriteStrValue(ClientClan(i));

			JsonWriter.WriteAttribute("country");
			JsonWriter.WriteIntValue(m_aClients[i].m_Country); // ISO 3166-1 numeric

			JsonWriter.WriteAttribute("score");
			JsonWriter.WriteIntValue(m_aClients[i].m_Score);

			JsonWriter.WriteAttribute("is_player");
			JsonWriter.WriteBoolValue(GameServer()->IsClientPlayer(i));

			GameServer()->OnUpdatePlayerServerInfo(&JsonWriter, i);

			JsonWriter.EndObject();
		}
	}

	JsonWriter.EndArray();
	JsonWriter.EndObject();

	m_pRegister->OnNewInfo(JsonWriter.GetOutputString().c_str());
}

void CServer::UpdateServerInfo(bool Resend)
{
	if(!m_pRegister || m_RunServer == false)
		return;

	UpdateRegisterServerInfo();

	if(Resend)
	{
		for(int i = 0; i < m_NetServer.MaxClients(); ++i)
		{
			if(m_aClients[i].m_State != CClient::STATE_EMPTY)
			{
				SendServerInfo(m_NetServer.ClientAddr(i), -1, SERVERINFO_INGAME, false);
			}
		}
	}

	m_ServerInfoNeedsUpdate = false;
}

void CServer::PumpNetwork(bool PacketWaiting)
{
	CNetChunk Packet;
	SECURITY_TOKEN ResponseToken;

	m_NetServer.Update();

	if(PacketWaiting)
	{
		// process packets
		while(m_NetServer.Recv(&Packet, &ResponseToken))
		{
			if(ResponseToken == NET_SECURITY_TOKEN_UNKNOWN && m_pRegister->OnPacket(&Packet))
				continue;
			if(Packet.m_DataSize >= int(sizeof(SERVERBROWSE_GETINFO)) &&
				mem_comp(Packet.m_pData, SERVERBROWSE_GETINFO, sizeof(SERVERBROWSE_GETINFO)) == 0)
			{
				// stateless
				int ExtraToken = 0;
				int Type = -1;
				if(Packet.m_DataSize >= (int)sizeof(SERVERBROWSE_GETINFO) + 1 &&
					mem_comp(Packet.m_pData, SERVERBROWSE_GETINFO, sizeof(SERVERBROWSE_GETINFO)) == 0)
				{
					if(Packet.m_Flags & NETSENDFLAG_EXTENDED)
					{
						Type = SERVERINFO_EXTENDED;
						ExtraToken = (Packet.m_aExtraData[0] << 8) | Packet.m_aExtraData[1];
					}
					else
						Type = SERVERINFO_VANILLA;
				}
				else if(Packet.m_DataSize >= (int)sizeof(SERVERBROWSE_GETINFO_64_LEGACY) + 1 &&
						mem_comp(Packet.m_pData, SERVERBROWSE_GETINFO_64_LEGACY, sizeof(SERVERBROWSE_GETINFO_64_LEGACY)) == 0)
				{
					Type = SERVERINFO_64_LEGACY;
				}
				if(Type != -1)
				{
					int Token = ((unsigned char *)Packet.m_pData)[sizeof(SERVERBROWSE_GETINFO)];
					Token |= ExtraToken << 8;
					SendServerInfoConnless(&Packet.m_Address, Token, Type);
				}
			}
			else
			{
				ProcessClientPacket(&Packet);
			}
		}
	}

	m_ServerBan.Update();
	m_Econ.Update();
}

char *CServer::GetMapName() const
{
	// get the name of the map without his path
	char *pMapShortName = &g_Config.m_SvMap[0];
	for(int i = 0; i < str_length(g_Config.m_SvMap) - 1; i++)
	{
		if(g_Config.m_SvMap[i] == '/' || g_Config.m_SvMap[i] == '\\')
			pMapShortName = &g_Config.m_SvMap[i + 1];
	}
	return pMapShortName;
}

void CServer::ChangeMap(const char *pMap)
{
	str_copy(g_Config.m_SvMap, pMap, sizeof(g_Config.m_SvMap));
	m_MapReload = str_comp(g_Config.m_SvMap, m_aCurrentMap) != 0;
}

int CServer::LoadMap(const char *pMapName)
{
	char aBuf[512];
	str_format(aBuf, sizeof(aBuf), "maps/%s.map", pMapName);

	if(!m_pMap->Load(aBuf))
		return 0;

	// stop recording when we change map
	for(int i = 0; i < MAX_CLIENTS + 1; i++)
	{
		if(!m_aDemoRecorder[i].IsRecording())
			continue;

		m_aDemoRecorder[i].Stop();

		// remove tmp demos
		if(i < MAX_CLIENTS)
		{
			char aPath[256];
			str_format(aPath, sizeof(aPath), "demos/%s_%d_%d_tmp.demo", m_aCurrentMap, m_NetServer.Address().port, i);
			Storage()->RemoveFile(aPath, IStorage::TYPE_SAVE);
		}
	}

	// reinit snapshot ids
	m_IDPool.TimeoutIDs();

	// get the crc of the map
	m_aCurrentMapSha256[SIX] = m_pMap->Sha256();
	m_aCurrentMapCrc[SIX] = m_pMap->Crc();
	char aBufMsg[256];
	char aSha256[SHA256_MAXSTRSIZE];
	sha256_str(m_aCurrentMapSha256[SIX], aSha256, sizeof(aSha256));
	str_format(aBufMsg, sizeof(aBufMsg), "%s sha256 is %s", aBuf, aSha256);
	Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "server", aBufMsg);

	str_copy(m_aCurrentMap, pMapName, sizeof(m_aCurrentMap));

	// load complete map into memory for download
	{
		IOHANDLE File = Storage()->OpenFile(aBuf, IOFLAG_READ, IStorage::TYPE_ALL);
		m_aCurrentMapSize[SIX] = (unsigned int)io_length(File);
		free(m_apCurrentMapData[SIX]);
		m_apCurrentMapData[SIX] = (unsigned char *)malloc(m_aCurrentMapSize[SIX]);
		io_read(File, m_apCurrentMapData[SIX], m_aCurrentMapSize[SIX]);
		io_close(File);
	}

	// load sixup version of the map
	if(g_Config.m_SvSixup)
	{
		str_format(aBuf, sizeof(aBuf), "maps7/%s.map", pMapName);
		IOHANDLE File = Storage()->OpenFile(aBuf, IOFLAG_READ, IStorage::TYPE_ALL);
		if(!File)
		{
			g_Config.m_SvSixup = 0;
			dbg_msg("sixup", "couldn't load map %s", aBuf);
			dbg_msg("sixup", "disabling 0.7 compatibility");
		}
		else
		{
			m_aCurrentMapSize[SIXUP] = (unsigned int)io_length(File);
			free(m_apCurrentMapData[SIXUP]);
			m_apCurrentMapData[SIXUP] = (unsigned char *)malloc(m_aCurrentMapSize[SIXUP]);
			io_read(File, m_apCurrentMapData[SIXUP], m_aCurrentMapSize[SIXUP]);
			io_close(File);

			m_aCurrentMapSha256[SIXUP] = sha256(m_apCurrentMapData[SIXUP], m_aCurrentMapSize[SIXUP]);
			m_aCurrentMapCrc[SIXUP] = crc32(0, m_apCurrentMapData[SIXUP], m_aCurrentMapSize[SIXUP]);
			sha256_str(m_aCurrentMapSha256[SIXUP], aSha256, sizeof(aSha256));
			str_format(aBufMsg, sizeof(aBufMsg), "%s sha256 is %s", aBuf, aSha256);
			Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "sixup", aBufMsg);
		}
	}

	for(int i = 0; i < MAX_CLIENTS; i++)
		m_aPrevStates[i] = m_aClients[i].m_State;

	return 1;
}

int CServer::Run()
{
	if(m_RunServer == UNINITIALIZED)
		m_RunServer = RUNNING;

	m_AuthManager.Init();

	if(g_Config.m_Debug)
	{
		g_UuidManager.DebugDump();
	}

	m_PrintCBIndex = Console()->RegisterPrintCallback(g_Config.m_ConsoleOutputLevel, SendRconLineAuthed, this);

	{
		int Size = GameServer()->PersistentClientDataSize();
		for(auto &Client : m_aClients)
		{
			Client.m_HasPersistentData = false;
			Client.m_pPersistentData = malloc(Size);
		}
	}

	// load map
	if(!LoadMap(g_Config.m_SvMap))
	{
		dbg_msg("server", "failed to load map. mapname='%s'", g_Config.m_SvMap);
		return -1;
	}

	// Late chain to avoid double map load
	Console()->Chain("sv_map", ConchainMapUpdate, this);

	if(g_Config.m_SvSqliteFile[0] != '\0')
	{
		auto pSqlServers = std::unique_ptr<IDbConnection>(CreateSqliteConnection(
			g_Config.m_SvSqliteFile, true));

		if(g_Config.m_SvUseSQL)
		{
			DbPool()->RegisterDatabase(std::move(pSqlServers), CDbConnectionPool::WRITE_BACKUP);
		}
		else
		{
			auto pCopy = std::unique_ptr<IDbConnection>(pSqlServers->Copy());
			DbPool()->RegisterDatabase(std::move(pSqlServers), CDbConnectionPool::READ);
			DbPool()->RegisterDatabase(std::move(pCopy), CDbConnectionPool::WRITE);
		}
	}

	// start server
	NETADDR BindAddr;
	int NetType = g_Config.m_SvIpv4Only ? NETTYPE_IPV4 : NETTYPE_ALL;

	if(!g_Config.m_Bindaddr[0] || net_host_lookup(g_Config.m_Bindaddr, &BindAddr, NetType) != 0)
		mem_zero(&BindAddr, sizeof(BindAddr));

	BindAddr.type = NetType;

	int Port = g_Config.m_SvPort;
	for(BindAddr.port = Port != 0 ? Port : 8303; !m_NetServer.Open(BindAddr, &m_ServerBan, g_Config.m_SvMaxClients, g_Config.m_SvMaxClientsPerIP, 0); BindAddr.port++)
	{
		if(Port != 0 || BindAddr.port >= 8310)
		{
			dbg_msg("server", "couldn't open socket. port %d might already be in use", BindAddr.port);
			return -1;
		}
	}

	if(Port == 0)
		dbg_msg("server", "using port %d", BindAddr.port);

#if defined(CONF_UPNP)
	m_UPnP.Open(BindAddr);
#endif

	if(Port == 0)
		dbg_msg("server", "using port %d", BindAddr.port);

#if defined(CONF_UPNP)
	m_UPnP.Open(BindAddr);
#endif

	m_NetServer.SetCallbacks(NewClientCallback, NewClientNoAuthCallback, ClientRejoinCallback, DelClientCallback, ClientCheckDisruptive, this);

	if(!m_Http.Init(std::chrono::seconds{2}, &g_Config))
	{
		dbg_msg("server", "Failed to initialize the HTTP client.");
		return -1;
	}

	m_pRegister = CreateRegister(&g_Config, m_pConsole, Kernel()->RequestInterface<IEngine>(), &m_Http, g_Config.m_SvPort, NET_SECURITY_TOKEN_UNSUPPORTED);
	m_Econ.Init(Config(), Console(), &m_ServerBan);

#if defined(CONF_FAMILY_UNIX)
	m_Fifo.Init(Console(), g_Config.m_SvInputFifo, CFGFLAG_SERVER);
#endif

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "server name is '%s'", g_Config.m_SvName);
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);

	Antibot()->Init();
	GameServer()->OnInit();
	GameServer()->RegisterHttp(&m_Http);

	if(ErrorShutdown())
	{
		m_RunServer = STOPPING;
	}
	str_format(aBuf, sizeof(aBuf), "version %s", GameServer()->NetVersion());
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);

	// process pending commands
	m_pConsole->StoreCommands(false);
	m_pRegister->OnConfigChange();

	if(m_AuthManager.IsGenerated())
	{
		dbg_msg("server", "+-------------------------+");
		dbg_msg("server", "| rcon password: '%s' |", g_Config.m_SvRconPassword);
		dbg_msg("server", "+-------------------------+");
	}

	// start game
	{
		bool NonActive = false;
		bool PacketWaiting = false;

		m_Lastheartbeat = 0;
		m_GameStartTime = time_get();

		UpdateServerInfo();
		while(m_RunServer < STOPPING)
		{
			if(NonActive)
				PumpNetwork(PacketWaiting);

			set_new_tick();

			int64 t = time_get();
			int NewTicks = 0;

			// load new map TODO: don't poll this
			if(m_MapReload || m_CurrentGameTick >= 0x6FFFFFFF) //	force reload to make sure the ticks stay within a valid range
			{
				m_MapReload = false;

				// load map
				if(LoadMap(g_Config.m_SvMap))
				{
					// new map loaded

					// ask the game to for the data it wants to persist past a map change
					for(int i = 0; i < MAX_CLIENTS; i++)
					{
						if(m_aClients[i].m_State == CClient::STATE_INGAME)
						{
							m_aClients[i].m_HasPersistentData = GameServer()->OnClientDataPersist(i, m_aClients[i].m_pPersistentData);
						}
					}

					GameServer()->OnShutdown(false);

					for(int ClientID = 0; ClientID < MAX_CLIENTS; ClientID++)
					{
						if(m_aClients[ClientID].m_State <= CClient::STATE_AUTH)
							continue;

						SendMap(ClientID);
						bool HasPersistentData = m_aClients[ClientID].m_HasPersistentData;
						m_aClients[ClientID].Reset();
						m_aClients[ClientID].m_HasPersistentData = HasPersistentData;
						m_aClients[ClientID].m_State = CClient::STATE_CONNECTING;
					}

					m_GameStartTime = time_get();
					m_CurrentGameTick = 0;
					m_ServerInfoFirstRequest = 0;
					Kernel()->ReregisterInterface(GameServer());
					GameServer()->OnInit();
					if(ErrorShutdown())
					{
						break;
					}
					UpdateServerInfo(true);
				}
				else
				{
					str_format(aBuf, sizeof(aBuf), "failed to load map. mapname='%s'", g_Config.m_SvMap);
					Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
					str_copy(g_Config.m_SvMap, m_aCurrentMap, sizeof(g_Config.m_SvMap));
				}
			}

			// handle dnsbl
			if(g_Config.m_SvDnsbl)
			{
				for(int ClientID = 0; ClientID < MAX_CLIENTS; ClientID++)
				{
					if(m_aClients[ClientID].m_State == CClient::STATE_EMPTY)
						continue;

					if(m_aClients[ClientID].m_DnsblState == CClient::DNSBL_STATE_NONE)
					{
						// initiate dnsbl lookup
						InitDnsbl(ClientID);
					}
					else if(m_aClients[ClientID].m_DnsblState == CClient::DNSBL_STATE_PENDING &&
						m_aClients[ClientID].m_pDnsblLookup->State() == IJob::STATE_DONE)
					{
						if(m_aClients[ClientID].m_pDnsblLookup->m_Result != 0)
						{
							// entry not found -> whitelisted
							m_aClients[ClientID].m_DnsblState = CClient::DNSBL_STATE_WHITELISTED;
						}
						else
						{
							// entry found -> blacklisted
							m_aClients[ClientID].m_DnsblState = CClient::DNSBL_STATE_BLACKLISTED;

							// console output
							char aAddrStr[NETADDR_MAXSTRSIZE];
							net_addr_str(m_NetServer.ClientAddr(ClientID), aAddrStr, sizeof(aAddrStr), true);

							char aBuf[256];

							str_format(aBuf, sizeof(aBuf), "ClientID=%d addr=<{%s}> secure=%s blacklisted", ClientID, aAddrStr, m_NetServer.HasSecurityToken(ClientID) ? "yes" : "no");

							Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "dnsbl", aBuf);
						}
					}

					if(m_aClients[ClientID].m_DnsblState == CClient::DNSBL_STATE_BLACKLISTED &&
						g_Config.m_SvDnsblBan)
						m_NetServer.NetBan()->BanAddr(m_NetServer.ClientAddr(ClientID), 60 * 10, "VPN detected, try connecting without. Contact admin if mistaken");
				}
			}

			while(t > TickStartTime(m_CurrentGameTick + 1))
			{
				for(int c = 0; c < MAX_CLIENTS; c++)
					if(m_aClients[c].m_State == CClient::STATE_INGAME)
						for(auto &Input : m_aClients[c].m_aInputs)
							if(Input.m_GameTick == Tick() + 1)
								GameServer()->OnClientPredictedEarlyInput(c, Input.m_aData);

				m_CurrentGameTick++;
				NewTicks++;

				// apply new input
				for(int c = 0; c < MAX_CLIENTS; c++)
				{
					if(m_aClients[c].m_State != CClient::STATE_INGAME)
						continue;
					for(auto &Input : m_aClients[c].m_aInputs)
					{
						if(Input.m_GameTick == Tick())
						{
							GameServer()->OnClientPredictedInput(c, Input.m_aData);
							break;
						}
					}
				}

				GameServer()->OnTick();
				if(ErrorShutdown())
				{
					break;
				}
			}

			// snap game
			if(NewTicks)
			{
				if(g_Config.m_SvHighBandwidth || (m_CurrentGameTick % 2) == 0)
					DoSnapshot();

				UpdateClientRconCommands();

#if defined(CONF_FAMILY_UNIX)
				m_Fifo.Update();
#endif
			}

			// master server stuff
			m_pRegister->Update();

			if(m_ServerInfoNeedsUpdate)
				UpdateServerInfo();

			Antibot()->OnEngineTick();

			if(!NonActive)
				PumpNetwork(PacketWaiting);

			NonActive = true;

			for(auto &Client : m_aClients)
				if(Client.m_State != CClient::STATE_EMPTY)
					NonActive = false;

			// wait for incoming data
			if(NonActive)
			{
				if(g_Config.m_SvReloadWhenEmpty == 1)
				{
					m_MapReload = true;
					g_Config.m_SvReloadWhenEmpty = 0;
				}
				else if(g_Config.m_SvReloadWhenEmpty == 2 && !m_ReloadedWhenEmpty)
				{
					m_MapReload = true;
					m_ReloadedWhenEmpty = true;
				}

				if(g_Config.m_SvShutdownWhenEmpty)
					m_RunServer = STOPPING;
				else
					PacketWaiting = net_socket_read_wait(m_NetServer.Socket(), 1000000);
			}
			else
			{
				m_ReloadedWhenEmpty = false;

				set_new_tick();
				int64 t = time_get();
				int x = (TickStartTime(m_CurrentGameTick + 1) - t) * 1000000 / time_freq() + 1;

				PacketWaiting = x > 0 ? net_socket_read_wait(m_NetServer.Socket(), x) : true;
			}
		}
	}
	const char *pDisconnectReason = "Server shutdown";
	if(ErrorShutdown())
	{
		dbg_msg("server", "shutdown from game server (%s)", m_aErrorShutdownReason);
		pDisconnectReason = m_aErrorShutdownReason;
	}
	// disconnect all clients on shutdown
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(m_aClients[i].m_State != CClient::STATE_EMPTY)
			m_NetServer.Drop(i, pDisconnectReason);
	}

	m_NetServer.Close();
	m_pRegister->OnShutdown();
	m_Econ.Shutdown();
	m_Http.Shutdown();

#if defined(CONF_FAMILY_UNIX)
	m_Fifo.Shutdown();
#endif

	GameServer()->OnShutdown(true);
	m_pMap->Unload();

	DbPool()->OnShutdown();

#if defined(CONF_UPNP)
	m_UPnP.Shutdown();
#endif

	for(auto &Client : m_aClients)
	{
		free(Client.m_pPersistentData);
	}

	return ErrorShutdown();
}

void CServer::ConTestingCommands(IConsole::IResult *pResult, void *pUser)
{
	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "Value: %d", g_Config.m_SvTestingCommands);
	((CConsole *)pUser)->Print(CConsole::OUTPUT_LEVEL_STANDARD, "console", aBuf);
}

void CServer::ConKick(IConsole::IResult *pResult, void *pUser)
{
	if(pResult->NumArguments() > 1)
	{
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "Kicked (%s)", pResult->GetString(1));
		((CServer *)pUser)->Kick(pResult->GetInteger(0), aBuf);
	}
	else
		((CServer *)pUser)->Kick(pResult->GetInteger(0), "Kicked by console");
}

void CServer::ConStatus(IConsole::IResult *pResult, void *pUser)
{
	char aBuf[1024];
	char aAddrStr[NETADDR_MAXSTRSIZE];
	CServer *pThis = static_cast<CServer *>(pUser);
	const char *pName = pResult->NumArguments() == 1 ? pResult->GetString(0) : "";

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(pThis->m_aClients[i].m_State == CClient::STATE_EMPTY)
			continue;

		if(!str_utf8_find_nocase(pThis->m_aClients[i].m_aName, pName))
			continue;

		net_addr_str(pThis->m_NetServer.ClientAddr(i), aAddrStr, sizeof(aAddrStr), true);
		if(pThis->m_aClients[i].m_State == CClient::STATE_INGAME)
		{
			char aDnsblStr[64];
			aDnsblStr[0] = '\0';
			if(g_Config.m_SvDnsbl)
			{
				const char *pDnsblStr = pThis->m_aClients[i].m_DnsblState == CClient::DNSBL_STATE_WHITELISTED ? "white" :
							pThis->m_aClients[i].m_DnsblState == CClient::DNSBL_STATE_BLACKLISTED ? "black" :
							pThis->m_aClients[i].m_DnsblState == CClient::DNSBL_STATE_PENDING     ? "pending" :
                                                                                                                                "n/a";

				str_format(aDnsblStr, sizeof(aDnsblStr), " dnsbl=%s", pDnsblStr);
			}

			char aAuthStr[128];
			aAuthStr[0] = '\0';
			if(pThis->m_aClients[i].m_AuthKey >= 0)
			{
				const char *pAuthStr = pThis->m_aClients[i].m_Authed == AUTHED_ADMIN  ? "(Admin)" :
						       pThis->m_aClients[i].m_Authed == AUTHED_MOD    ? "(Mod)" :
						       pThis->m_aClients[i].m_Authed == AUTHED_HELPER ? "(Helper)" :
                                                                                                        "";

				str_format(aAuthStr, sizeof(aAuthStr), " key=%s %s", pThis->m_AuthManager.KeyIdent(pThis->m_aClients[i].m_AuthKey), pAuthStr);
			}

			const char *pClientPrefix = "";
			if(pThis->m_aClients[i].m_Sixup)
			{
				pClientPrefix = "0.7:";
			}
			str_format(aBuf, sizeof(aBuf), "id=%d addr=<{%s}> name='%s' client=%s%d secure=%s flags=%d%s%s",
				i, aAddrStr, pThis->m_aClients[i].m_aName, pClientPrefix, pThis->m_aClients[i].m_DDNetVersion,
				pThis->m_NetServer.HasSecurityToken(i) ? "yes" : "no", pThis->m_aClients[i].m_Flags, aDnsblStr, aAuthStr);
		}
		else
		{
			str_format(aBuf, sizeof(aBuf), "id=%d addr=<{%s}> connecting", i, aAddrStr);
		}
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
	}
}

static int GetAuthLevel(const char *pLevel)
{
	int Level = -1;
	if(!str_comp_nocase(pLevel, "admin"))
		Level = AUTHED_ADMIN;
	else if(!str_comp_nocase_num(pLevel, "mod", 3))
		Level = AUTHED_MOD;
	else if(!str_comp_nocase(pLevel, "helper"))
		Level = AUTHED_HELPER;

	return Level;
}

void CServer::AuthRemoveKey(int KeySlot)
{
	int NewKeySlot = KeySlot;
	int OldKeySlot = m_AuthManager.RemoveKey(KeySlot);
	LogoutKey(KeySlot, "key removal");

	// Update indices.
	if(OldKeySlot != NewKeySlot)
	{
		for(auto &Client : m_aClients)
			if(Client.m_AuthKey == OldKeySlot)
				Client.m_AuthKey = NewKeySlot;
	}
}

void CServer::ConAuthAdd(IConsole::IResult *pResult, void *pUser)
{
	CServer *pThis = (CServer *)pUser;
	CAuthManager *pManager = &pThis->m_AuthManager;

	const char *pIdent = pResult->GetString(0);
	const char *pLevel = pResult->GetString(1);
	const char *pPw = pResult->GetString(2);

	int Level = GetAuthLevel(pLevel);
	if(Level == -1)
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "level can be one of {\"admin\", \"mod(erator)\", \"helper\"}");
		return;
	}

	bool NeedUpdate = !pManager->NumNonDefaultKeys();
	if(pManager->AddKey(pIdent, pPw, Level) < 0)
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "ident already exists");
	else
	{
		if(NeedUpdate)
			pThis->SendRconType(-1, true);
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "key added");
	}
}

void CServer::ConAuthAddHashed(IConsole::IResult *pResult, void *pUser)
{
	CServer *pThis = (CServer *)pUser;
	CAuthManager *pManager = &pThis->m_AuthManager;

	const char *pIdent = pResult->GetString(0);
	const char *pLevel = pResult->GetString(1);
	const char *pPw = pResult->GetString(2);
	const char *pSalt = pResult->GetString(3);

	int Level = GetAuthLevel(pLevel);
	if(Level == -1)
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "level can be one of {\"admin\", \"mod(erator)\", \"helper\"}");
		return;
	}

	MD5_DIGEST Hash;
	unsigned char aSalt[SALT_BYTES];

	if(md5_from_str(&Hash, pPw))
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "Malformed password hash");
		return;
	}
	if(str_hex_decode(aSalt, sizeof(aSalt), pSalt))
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "Malformed salt hash");
		return;
	}

	bool NeedUpdate = !pManager->NumNonDefaultKeys();

	if(pManager->AddKeyHash(pIdent, Hash, aSalt, Level) < 0)
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "ident already exists");
	else
	{
		if(NeedUpdate)
			pThis->SendRconType(-1, true);
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "key added");
	}
}

void CServer::ConAuthUpdate(IConsole::IResult *pResult, void *pUser)
{
	CServer *pThis = (CServer *)pUser;
	CAuthManager *pManager = &pThis->m_AuthManager;

	const char *pIdent = pResult->GetString(0);
	const char *pLevel = pResult->GetString(1);
	const char *pPw = pResult->GetString(2);

	int KeySlot = pManager->FindKey(pIdent);
	if(KeySlot == -1)
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "ident couldn't be found");
		return;
	}

	int Level = GetAuthLevel(pLevel);
	if(Level == -1)
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "level can be one of {\"admin\", \"mod(erator)\", \"helper\"}");
		return;
	}

	pManager->UpdateKey(KeySlot, pPw, Level);
	pThis->LogoutKey(KeySlot, "key update");

	pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "key updated");
}

void CServer::ConAuthUpdateHashed(IConsole::IResult *pResult, void *pUser)
{
	CServer *pThis = (CServer *)pUser;
	CAuthManager *pManager = &pThis->m_AuthManager;

	const char *pIdent = pResult->GetString(0);
	const char *pLevel = pResult->GetString(1);
	const char *pPw = pResult->GetString(2);
	const char *pSalt = pResult->GetString(3);

	int KeySlot = pManager->FindKey(pIdent);
	if(KeySlot == -1)
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "ident couldn't be found");
		return;
	}

	int Level = GetAuthLevel(pLevel);
	if(Level == -1)
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "level can be one of {\"admin\", \"mod(erator)\", \"helper\"}");
		return;
	}

	MD5_DIGEST Hash;
	unsigned char aSalt[SALT_BYTES];

	if(md5_from_str(&Hash, pPw))
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "Malformed password hash");
		return;
	}
	if(str_hex_decode(aSalt, sizeof(aSalt), pSalt))
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "Malformed salt hash");
		return;
	}

	pManager->UpdateKeyHash(KeySlot, Hash, aSalt, Level);
	pThis->LogoutKey(KeySlot, "key update");

	pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "key updated");
}

void CServer::ConAuthRemove(IConsole::IResult *pResult, void *pUser)
{
	CServer *pThis = (CServer *)pUser;
	CAuthManager *pManager = &pThis->m_AuthManager;

	const char *pIdent = pResult->GetString(0);

	int KeySlot = pManager->FindKey(pIdent);
	if(KeySlot == -1)
	{
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "ident couldn't be found");
		return;
	}

	pThis->AuthRemoveKey(KeySlot);

	if(!pManager->NumNonDefaultKeys())
		pThis->SendRconType(-1, false);

	pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", "key removed, all users logged out");
}

static void ListKeysCallback(const char *pIdent, int Level, void *pUser)
{
	static const char LSTRING[][10] = {"helper", "moderator", "admin"};

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "%s %s", pIdent, LSTRING[Level - 1]);
	((CServer *)pUser)->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auth", aBuf);
}

void CServer::ConAuthList(IConsole::IResult *pResult, void *pUser)
{
	CServer *pThis = (CServer *)pUser;
	CAuthManager *pManager = &pThis->m_AuthManager;

	pManager->ListKeys(ListKeysCallback, pThis);
}

void CServer::ConNameBan(IConsole::IResult *pResult, void *pUser)
{
	CServer *pThis = (CServer *)pUser;
	char aBuf[256];
	const char *pName = pResult->GetString(0);
	const char *pReason = pResult->NumArguments() > 3 ? pResult->GetString(3) : "";
	int Distance = pResult->NumArguments() > 1 ? pResult->GetInteger(1) : str_length(pName) / 3;
	int IsSubstring = pResult->NumArguments() > 2 ? pResult->GetInteger(2) : 0;

	for(int i = 0; i < pThis->m_aNameBans.size(); i++)
	{
		CNameBan *pBan = &pThis->m_aNameBans[i];
		if(str_comp(pBan->m_aName, pName) == 0)
		{
			str_format(aBuf, sizeof(aBuf), "changed name='%s' distance=%d old_distance=%d is_substring=%d old_is_substring=%d reason='%s' old_reason='%s'", pName, Distance, pBan->m_Distance, IsSubstring, pBan->m_IsSubstring, pReason, pBan->m_aReason);
			pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "name_ban", aBuf);
			pBan->m_Distance = Distance;
			pBan->m_IsSubstring = IsSubstring;
			str_copy(pBan->m_aReason, pReason, sizeof(pBan->m_aReason));
			return;
		}
	}

	pThis->m_aNameBans.add(CNameBan(pName, Distance, IsSubstring, pReason));
	str_format(aBuf, sizeof(aBuf), "added name='%s' distance=%d is_substring=%d reason='%s'", pName, Distance, IsSubstring, pReason);
	pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "name_ban", aBuf);
}

void CServer::ConNameUnban(IConsole::IResult *pResult, void *pUser)
{
	CServer *pThis = (CServer *)pUser;
	const char *pName = pResult->GetString(0);

	for(int i = 0; i < pThis->m_aNameBans.size(); i++)
	{
		CNameBan *pBan = &pThis->m_aNameBans[i];
		if(str_comp(pBan->m_aName, pName) == 0)
		{
			char aBuf[128];
			str_format(aBuf, sizeof(aBuf), "removed name='%s' distance=%d is_substring=%d reason='%s'", pBan->m_aName, pBan->m_Distance, pBan->m_IsSubstring, pBan->m_aReason);
			pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "name_ban", aBuf);
			pThis->m_aNameBans.remove_index(i);
		}
	}
}

void CServer::ConNameBans(IConsole::IResult *pResult, void *pUser)
{
	CServer *pThis = (CServer *)pUser;

	for(int i = 0; i < pThis->m_aNameBans.size(); i++)
	{
		CNameBan *pBan = &pThis->m_aNameBans[i];
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "name='%s' distance=%d is_substring=%d reason='%s'", pBan->m_aName, pBan->m_Distance, pBan->m_IsSubstring, pBan->m_aReason);
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "name_ban", aBuf);
	}
}

void CServer::ConShutdown(IConsole::IResult *pResult, void *pUser)
{
	((CServer *)pUser)->m_RunServer = STOPPING;
}

void CServer::DemoRecorder_HandleAutoStart()
{
	if(g_Config.m_SvAutoDemoRecord)
	{
		m_aDemoRecorder[MAX_CLIENTS].Stop();
		char aFilename[128];
		char aDate[20];
		str_timestamp(aDate, sizeof(aDate));
		str_format(aFilename, sizeof(aFilename), "demos/%s_%s.demo", "auto/autorecord", aDate);
		m_aDemoRecorder[MAX_CLIENTS].Start(Storage(), m_pConsole, aFilename, GameServer()->NetVersion(), m_aCurrentMap, &m_aCurrentMapSha256[SIX], m_aCurrentMapCrc[SIX], "server", m_aCurrentMapSize[SIX], m_apCurrentMapData[SIX]);
		if(g_Config.m_SvAutoDemoMax)
		{
			// clean up auto recorded demos
			CFileCollection AutoDemos;
			AutoDemos.Init(Storage(), "demos/server", "autorecord", ".demo", g_Config.m_SvAutoDemoMax);
		}
	}
}

bool CServer::DemoRecorder_IsRecording()
{
	return m_aDemoRecorder[MAX_CLIENTS].IsRecording();
}

void CServer::SaveDemo(int ClientID, float Time)
{
	if(IsRecording(ClientID))
	{
		m_aDemoRecorder[ClientID].Stop();

		// rename the demo
		char aOldFilename[256];
		char aNewFilename[256];
		str_format(aOldFilename, sizeof(aOldFilename), "demos/%s_%d_%d_tmp.demo", m_aCurrentMap, m_NetServer.Address().port, ClientID);
		str_format(aNewFilename, sizeof(aNewFilename), "demos/%s_%s_%5.2f.demo", m_aCurrentMap, m_aClients[ClientID].m_aName, Time);
		Storage()->RenameFile(aOldFilename, aNewFilename, IStorage::TYPE_SAVE);
	}
}

void CServer::StartRecord(int ClientID)
{
	if(g_Config.m_SvPlayerDemoRecord)
	{
		char aFilename[128];
		str_format(aFilename, sizeof(aFilename), "demos/%s_%d_%d_tmp.demo", m_aCurrentMap, m_NetServer.Address().port, ClientID);
		m_aDemoRecorder[ClientID].Start(Storage(), Console(), aFilename, GameServer()->NetVersion(), m_aCurrentMap, &m_aCurrentMapSha256[SIX], m_aCurrentMapCrc[SIX], "server", m_aCurrentMapSize[SIX], m_apCurrentMapData[SIX]);
	}
}

void CServer::StopRecord(int ClientID)
{
	if(IsRecording(ClientID))
	{
		m_aDemoRecorder[ClientID].Stop();

		char aFilename[128];
		str_format(aFilename, sizeof(aFilename), "demos/%s_%d_%d_tmp.demo", m_aCurrentMap, m_NetServer.Address().port, ClientID);
		Storage()->RemoveFile(aFilename, IStorage::TYPE_SAVE);
	}
}

bool CServer::IsRecording(int ClientID)
{
	return m_aDemoRecorder[ClientID].IsRecording();
}

void CServer::ConRecord(IConsole::IResult *pResult, void *pUser)
{
	CServer *pServer = (CServer *)pUser;
	char aFilename[128];

	if(pResult->NumArguments())
		str_format(aFilename, sizeof(aFilename), "demos/%s.demo", pResult->GetString(0));
	else
	{
		char aDate[20];
		str_timestamp(aDate, sizeof(aDate));
		str_format(aFilename, sizeof(aFilename), "demos/demo_%s.demo", aDate);
	}
	pServer->m_aDemoRecorder[MAX_CLIENTS].Start(pServer->Storage(), pServer->Console(), aFilename, pServer->GameServer()->NetVersion(), pServer->m_aCurrentMap, &pServer->m_aCurrentMapSha256[SIX], pServer->m_aCurrentMapCrc[SIX], "server", pServer->m_aCurrentMapSize[SIX], pServer->m_apCurrentMapData[SIX]);
}

void CServer::ConStopRecord(IConsole::IResult *pResult, void *pUser)
{
	((CServer *)pUser)->m_aDemoRecorder[MAX_CLIENTS].Stop();
}

void CServer::ConMapReload(IConsole::IResult *pResult, void *pUser)
{
	((CServer *)pUser)->m_MapReload = true;
}

void CServer::ConLogout(IConsole::IResult *pResult, void *pUser)
{
	CServer *pServer = (CServer *)pUser;

	if(pServer->m_RconClientID >= 0 && pServer->m_RconClientID < MAX_CLIENTS &&
		pServer->m_aClients[pServer->m_RconClientID].m_State != CServer::CClient::STATE_EMPTY)
	{
		pServer->LogoutClient(pServer->m_RconClientID, "");
	}
}

void CServer::ConShowIps(IConsole::IResult *pResult, void *pUser)
{
	CServer *pServer = (CServer *)pUser;

	if(pServer->m_RconClientID >= 0 && pServer->m_RconClientID < MAX_CLIENTS &&
		pServer->m_aClients[pServer->m_RconClientID].m_State != CServer::CClient::STATE_EMPTY)
	{
		if(pResult->NumArguments())
		{
			pServer->m_aClients[pServer->m_RconClientID].m_ShowIps = pResult->GetInteger(0);
		}
		else
		{
			char aStr[9];
			str_format(aStr, sizeof(aStr), "Value: %d", pServer->m_aClients[pServer->m_RconClientID].m_ShowIps);
			char aBuf[32];
			pServer->SendRconLine(pServer->m_RconClientID, pServer->Console()->Format(aBuf, sizeof(aBuf), "server", aStr));
		}
	}
}

void CServer::ConAddSqlServer(IConsole::IResult *pResult, void *pUserData)
{
	if(!g_Config.m_SvUseSQL)
		return;
	CServer *pSelf = (CServer *)pUserData;

	if(pResult->NumArguments() != 7 && pResult->NumArguments() != 8)
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "7 or 8 arguments are required");
		return;
	}

	bool ReadOnly;
	if(str_comp_nocase(pResult->GetString(0), "w") == 0)
		ReadOnly = false;
	else if(str_comp_nocase(pResult->GetString(0), "r") == 0)
		ReadOnly = true;
	else
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "choose either 'r' for SqlReadServer or 'w' for SqlWriteServer");
		return;
	}

	bool SetUpDb = pResult->NumArguments() == 8 ? pResult->GetInteger(7) : true;

	auto pSqlServers = std::unique_ptr<IDbConnection>(CreateMysqlConnection(
		pResult->GetString(1), pResult->GetString(2), pResult->GetString(3),
		pResult->GetString(4), pResult->GetString(5), pResult->GetInteger(6),
		SetUpDb));

	if(!pSqlServers)
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "can't add MySQL server: compiled without MySQL support");
		return;
	}

	char aBuf[512];
	str_format(aBuf, sizeof(aBuf),
		"Added new Sql%sServer: DB: '%s' Prefix: '%s' User: '%s' IP: <{%s}> Port: %d",
		ReadOnly ? "Read" : "Write",
		pResult->GetString(1), pResult->GetString(2), pResult->GetString(3),
		pResult->GetString(5), pResult->GetInteger(6));
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
	pSelf->DbPool()->RegisterDatabase(std::move(pSqlServers), ReadOnly ? CDbConnectionPool::READ : CDbConnectionPool::WRITE);
}

void CServer::ConDumpSqlServers(IConsole::IResult *pResult, void *pUserData)
{
	CServer *pSelf = (CServer *)pUserData;

	if(str_comp_nocase(pResult->GetString(0), "w") == 0)
	{
		pSelf->DbPool()->Print(pSelf->Console(), CDbConnectionPool::WRITE);
		pSelf->DbPool()->Print(pSelf->Console(), CDbConnectionPool::WRITE_BACKUP);
	}
	else if(str_comp_nocase(pResult->GetString(0), "r") == 0)
	{
		pSelf->DbPool()->Print(pSelf->Console(), CDbConnectionPool::READ);
	}
	else
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "choose either 'r' for SqlReadServer or 'w' for SqlWriteServer");
		return;
	}
}

void CServer::ConchainSpecialInfoupdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
	{
		str_clean_whitespaces(g_Config.m_SvName);
		((CServer *)pUserData)->UpdateServerInfo(true);
	}
}

void CServer::ConchainMaxclientsperipUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
		((CServer *)pUserData)->m_NetServer.SetMaxClientsPerIP(pResult->GetInteger(0));
}

void CServer::ConchainCommandAccessUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	if(pResult->NumArguments() == 2)
	{
		CServer *pThis = static_cast<CServer *>(pUserData);
		const IConsole::CCommandInfo *pInfo = pThis->Console()->GetCommandInfo(pResult->GetString(0), CFGFLAG_SERVER, false);
		int OldAccessLevel = 0;
		if(pInfo)
			OldAccessLevel = pInfo->GetAccessLevel();
		pfnCallback(pResult, pCallbackUserData);
		if(pInfo && OldAccessLevel != pInfo->GetAccessLevel())
		{
			for(int i = 0; i < MAX_CLIENTS; ++i)
			{
				if(pThis->m_aClients[i].m_State == CServer::CClient::STATE_EMPTY ||
					(pInfo->GetAccessLevel() > AUTHED_ADMIN - pThis->m_aClients[i].m_Authed && AUTHED_ADMIN - pThis->m_aClients[i].m_Authed < OldAccessLevel) ||
					(pInfo->GetAccessLevel() < AUTHED_ADMIN - pThis->m_aClients[i].m_Authed && AUTHED_ADMIN - pThis->m_aClients[i].m_Authed > OldAccessLevel) ||
					(pThis->m_aClients[i].m_pRconCmdToSend && str_comp(pResult->GetString(0), pThis->m_aClients[i].m_pRconCmdToSend->m_pName) >= 0))
					continue;

				if(OldAccessLevel < pInfo->GetAccessLevel())
					pThis->SendRconCmdAdd(pInfo, i);
				else
					pThis->SendRconCmdRem(pInfo, i);
			}
		}
	}
	else
		pfnCallback(pResult, pCallbackUserData);
}

void CServer::ConchainConsoleOutputLevelUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments() == 1)
	{
		CServer *pThis = static_cast<CServer *>(pUserData);
		pThis->Console()->SetPrintOutputLevel(pThis->m_PrintCBIndex, pResult->GetInteger(0));
	}
}

void CServer::ConchainMapUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments() >= 1)
	{
		CServer *pThis = static_cast<CServer *>(pUserData);
		pThis->m_MapReload = str_comp(g_Config.m_SvMap, pThis->m_aCurrentMap) != 0;
	}
}

void CServer::LogoutClient(int ClientID, const char *pReason)
{
	if(!IsSixup(ClientID))
	{
		CMsgPacker Msg(NETMSG_RCON_AUTH_STATUS, true);
		Msg.AddInt(0); //authed
		Msg.AddInt(0); //cmdlist
		SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
	}
	else
	{
		CMsgPacker Msg(12, true, true); //NETMSG_RCON_AUTH_OFF
		SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
	}

	m_aClients[ClientID].m_AuthTries = 0;
	m_aClients[ClientID].m_pRconCmdToSend = 0;

	char aBuf[64];
	if(*pReason)
	{
		str_format(aBuf, sizeof(aBuf), "Logged out by %s.", pReason);
		SendRconLine(ClientID, aBuf);
		str_format(aBuf, sizeof(aBuf), "ClientID=%d with key=%s logged out by %s", ClientID, m_AuthManager.KeyIdent(m_aClients[ClientID].m_AuthKey), pReason);
	}
	else
	{
		SendRconLine(ClientID, "Logout successful.");
		str_format(aBuf, sizeof(aBuf), "ClientID=%d with key=%s logged out", ClientID, m_AuthManager.KeyIdent(m_aClients[ClientID].m_AuthKey));
	}

	m_aClients[ClientID].m_Authed = AUTHED_NO;
	m_aClients[ClientID].m_AuthKey = -1;

	GameServer()->OnSetAuthed(ClientID, AUTHED_NO);

	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
}

void CServer::LogoutKey(int Key, const char *pReason)
{
	for(int i = 0; i < MAX_CLIENTS; i++)
		if(m_aClients[i].m_AuthKey == Key)
			LogoutClient(i, pReason);
}

void CServer::ConchainRconPasswordChangeGeneric(int Level, const char *pCurrent, IConsole::IResult *pResult)
{
	if(pResult->NumArguments() == 1)
	{
		int KeySlot = m_AuthManager.DefaultKey(Level);
		const char *pNew = pResult->GetString(0);
		if(str_comp(pCurrent, pNew) == 0)
		{
			return;
		}
		if(KeySlot == -1 && pNew[0])
		{
			m_AuthManager.AddDefaultKey(Level, pNew);
		}
		else if(KeySlot >= 0)
		{
			if(!pNew[0])
			{
				AuthRemoveKey(KeySlot);
				// Already logs users out.
			}
			else
			{
				m_AuthManager.UpdateKey(KeySlot, pNew, Level);
				LogoutKey(KeySlot, "key update");
			}
		}
	}
}

void CServer::ConchainRconPasswordChange(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	((CServer *)pUserData)->ConchainRconPasswordChangeGeneric(AUTHED_ADMIN, g_Config.m_SvRconPassword, pResult);
	pfnCallback(pResult, pCallbackUserData);
}

void CServer::ConchainRconModPasswordChange(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	((CServer *)pUserData)->ConchainRconPasswordChangeGeneric(AUTHED_MOD, g_Config.m_SvRconModPassword, pResult);
	pfnCallback(pResult, pCallbackUserData);
}

void CServer::ConchainRconHelperPasswordChange(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	((CServer *)pUserData)->ConchainRconPasswordChangeGeneric(AUTHED_HELPER, g_Config.m_SvRconHelperPassword, pResult);
	pfnCallback(pResult, pCallbackUserData);
}

#if defined(CONF_FAMILY_UNIX)
void CServer::ConchainConnLoggingServerChange(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments() == 1)
	{
		CServer *pServer = (CServer *)pUserData;

		// open socket to send new connections
		if(!pServer->m_ConnLoggingSocketCreated)
		{
			pServer->m_ConnLoggingSocket = net_unix_create_unnamed();
			if(pServer->m_ConnLoggingSocket == -1)
			{
				pServer->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "Failed to created socket for communication with the connection logging server.");
			}
			else
			{
				pServer->m_ConnLoggingSocketCreated = true;
			}
		}

		// set the destination address for the connection logging
		net_unix_set_addr(&pServer->m_ConnLoggingDestAddr, pResult->GetString(0));
	}
}
#endif

void CServer::RegisterCommands()
{
	m_pConsole = Kernel()->RequestInterface<IConsole>();
	m_pGameServer = Kernel()->RequestInterface<IGameServer>();
	m_pMap = Kernel()->RequestInterface<IEngineMap>();
	m_pStorage = Kernel()->RequestInterface<IStorage>();
	m_pAntibot = Kernel()->RequestInterface<IEngineAntibot>();

	// register console commands
	Console()->Register("kick", "i[id] ?r[reason]", CFGFLAG_SERVER, ConKick, this, "Kick player with specified id for any reason");
	Console()->Register("status", "?r[name]", CFGFLAG_SERVER, ConStatus, this, "List players containing name or all players");
	Console()->Register("shutdown", "", CFGFLAG_SERVER, ConShutdown, this, "Shut down");
	Console()->Register("logout", "", CFGFLAG_SERVER, ConLogout, this, "Logout of rcon");
	Console()->Register("show_ips", "?i[show]", CFGFLAG_SERVER, ConShowIps, this, "Show IP addresses in rcon commands (1 = on, 0 = off)");

	Console()->Register("record", "?s[file]", CFGFLAG_SERVER | CFGFLAG_STORE, ConRecord, this, "Record to a file");
	Console()->Register("stoprecord", "", CFGFLAG_SERVER, ConStopRecord, this, "Stop recording");

	Console()->Register("reload", "", CFGFLAG_SERVER, ConMapReload, this, "Reload the map");

	Console()->Register("add_sqlserver", "s['r'|'w'] s[Database] s[Prefix] s[User] s[Password] s[IP] i[Port] ?i[SetUpDatabase ?]", CFGFLAG_SERVER | CFGFLAG_NONTEEHISTORIC, ConAddSqlServer, this, "add a sqlserver");
	Console()->Register("dump_sqlservers", "s['r'|'w']", CFGFLAG_SERVER, ConDumpSqlServers, this, "dumps all sqlservers readservers = r, writeservers = w");

	Console()->Register("auth_add", "s[ident] s[level] r[pw]", CFGFLAG_SERVER | CFGFLAG_NONTEEHISTORIC, ConAuthAdd, this, "Add a rcon key");
	Console()->Register("auth_add_p", "s[ident] s[level] s[hash] s[salt]", CFGFLAG_SERVER | CFGFLAG_NONTEEHISTORIC, ConAuthAddHashed, this, "Add a prehashed rcon key");
	Console()->Register("auth_change", "s[ident] s[level] r[pw]", CFGFLAG_SERVER | CFGFLAG_NONTEEHISTORIC, ConAuthUpdate, this, "Update a rcon key");
	Console()->Register("auth_change_p", "s[ident] s[level] s[hash] s[salt]", CFGFLAG_SERVER | CFGFLAG_NONTEEHISTORIC, ConAuthUpdateHashed, this, "Update a rcon key with prehashed data");
	Console()->Register("auth_remove", "s[ident]", CFGFLAG_SERVER | CFGFLAG_NONTEEHISTORIC, ConAuthRemove, this, "Remove a rcon key");
	Console()->Register("auth_list", "", CFGFLAG_SERVER, ConAuthList, this, "List all rcon keys");

	Console()->Register("name_ban", "s[name] ?i[distance] ?i[is_substring] ?r[reason]", CFGFLAG_SERVER, ConNameBan, this, "Ban a certain nickname");
	Console()->Register("name_unban", "s[name]", CFGFLAG_SERVER, ConNameUnban, this, "Unban a certain nickname");
	Console()->Register("name_bans", "", CFGFLAG_SERVER, ConNameBans, this, "List all name bans");

	Console()->Chain("sv_name", ConchainSpecialInfoupdate, this);
	Console()->Chain("password", ConchainSpecialInfoupdate, this);

	Console()->Chain("sv_max_clients_per_ip", ConchainMaxclientsperipUpdate, this);
	Console()->Chain("access_level", ConchainCommandAccessUpdate, this);
	Console()->Chain("console_output_level", ConchainConsoleOutputLevelUpdate, this);

	Console()->Chain("sv_rcon_password", ConchainRconPasswordChange, this);
	Console()->Chain("sv_rcon_mod_password", ConchainRconModPasswordChange, this);
	Console()->Chain("sv_rcon_helper_password", ConchainRconHelperPasswordChange, this);

#if defined(CONF_FAMILY_UNIX)
	Console()->Chain("sv_conn_logging_server", ConchainConnLoggingServerChange, this);
#endif

	// register console commands in sub parts
	m_ServerBan.InitServerBan(Console(), Storage(), this);
	m_pGameServer->OnConsoleInit();
}

int CServer::SnapNewID()
{
	return m_IDPool.NewID();
}

void CServer::SnapFreeID(int ID)
{
	m_IDPool.FreeID(ID);
}

void *CServer::SnapNewItem(int Type, int ID, int Size)
{
	if(Type > 0xffff)
	{
		g_UuidManager.GetUuid(Type);
	}
	dbg_assert(ID >= 0 && ID <= 0xffff, "incorrect id");
	return ID < 0 ? 0 : m_SnapshotBuilder.NewItem(Type, ID, Size);
}

void CServer::SnapSetStaticsize(int ItemType, int Size)
{
	m_SnapshotDelta.SetStaticsize(ItemType, Size);
}

static CServer *CreateServer() { return new CServer(); }

int main(int argc, const char **argv) // ignore_convention
{
	bool Silent = false;

	for(int i = 1; i < argc; i++) // ignore_convention
	{
		if(str_comp("-s", argv[i]) == 0 || str_comp("--silent", argv[i]) == 0) // ignore_convention
		{
			Silent = true;
#if defined(CONF_FAMILY_WINDOWS)
			ShowWindow(GetConsoleWindow(), SW_HIDE);
#endif
			break;
		}
	}

	if(secure_random_init() != 0)
	{
		dbg_msg("secure", "could not initialize secure RNG");
		return -1;
	}
	if(MysqlInit() != 0)
	{
		dbg_msg("mysql", "failed to initialize MySQL library");
		return -1;
	}

	CServer *pServer = CreateServer();
	IKernel *pKernel = IKernel::Create();

	// create the components
	IEngine *pEngine = CreateEngine("DDNet", Silent, 2);
	IEngineMap *pEngineMap = CreateEngineMap();
	IGameServer *pGameServer = CreateGameServer();
	IConsole *pConsole = CreateConsole(CFGFLAG_SERVER | CFGFLAG_ECON);
	IStorage *pStorage = CreateStorage("Teeworlds", IStorage::STORAGETYPE_SERVER, argc, argv); // ignore_convention
	IConfigManager *pConfigManager = CreateConfigManager();
	IEngineAntibot *pEngineAntibot = CreateEngineAntibot();

	{
		bool RegisterFail = false;

		RegisterFail = RegisterFail || !pKernel->RegisterInterface(pServer);
		RegisterFail = RegisterFail || !pKernel->RegisterInterface(pEngine);
		RegisterFail = RegisterFail || !pKernel->RegisterInterface(pEngineMap); // register as both
		RegisterFail = RegisterFail || !pKernel->RegisterInterface(static_cast<IMap *>(pEngineMap), false);
		RegisterFail = RegisterFail || !pKernel->RegisterInterface(pGameServer);
		RegisterFail = RegisterFail || !pKernel->RegisterInterface(pConsole);
		RegisterFail = RegisterFail || !pKernel->RegisterInterface(pStorage);
		RegisterFail = RegisterFail || !pKernel->RegisterInterface(pConfigManager);
		RegisterFail = RegisterFail || !pKernel->RegisterInterface(pEngineAntibot);
		RegisterFail = RegisterFail || !pKernel->RegisterInterface(static_cast<IAntibot *>(pEngineAntibot), false);

		if(RegisterFail)
		{
			delete pKernel;
			return -1;
		}
	}

	pEngine->Init();
	pConfigManager->Init();
	pConsole->Init();

	// register all console commands
	pServer->RegisterCommands();

	// execute autoexec file
	IOHANDLE File = pStorage->OpenFile(AUTOEXEC_SERVER_FILE, IOFLAG_READ, IStorage::TYPE_ALL);
	if(File)
	{
		io_close(File);
		pConsole->ExecuteFile(AUTOEXEC_SERVER_FILE);
	}
	else // fallback
	{
		pConsole->ExecuteFile(AUTOEXEC_FILE);
	}

	// parse the command line arguments
	if(argc > 1) // ignore_convention
		pConsole->ParseArguments(argc - 1, &argv[1]); // ignore_convention

	pConsole->Register("sv_test_cmds", "", CFGFLAG_SERVER, CServer::ConTestingCommands, pConsole, "Turns testing commands aka cheats on/off (setting only works in initial config)");

	pEngine->InitLogfile();

	// run the server
	dbg_msg("server", "starting...");
	int Ret = pServer->Run();

	MysqlUninit();

	// free
	delete pKernel;

	return Ret;
}

// DDRace

void CServer::GetClientAddr(int ClientID, NETADDR *pAddr) const
{
	if(ClientID >= 0 && ClientID < MAX_CLIENTS && m_aClients[ClientID].m_State == CClient::STATE_INGAME)
	{
		*pAddr = *m_NetServer.ClientAddr(ClientID);
	}
}

const char *CServer::GetAnnouncementLine(char const *pFileName)
{
	IOHANDLE File = m_pStorage->OpenFile(pFileName, IOFLAG_READ, IStorage::TYPE_ALL);
	if(!File)
		return 0;

	std::vector<char *> Lines;
	char *pLine;
	CLineReader Reader;
	Reader.Init(File);
	while((pLine = Reader.Get()))
		if(str_length(pLine))
			if(pLine[0] != '#')
				Lines.push_back(pLine);
	if(Lines.size() == 1)
	{
		m_AnnouncementLastLine = 0;
	}
	else if(!g_Config.m_SvAnnouncementRandom)
	{
		if(++m_AnnouncementLastLine >= Lines.size())
			m_AnnouncementLastLine %= Lines.size();
	}
	else
	{
		unsigned Rand;
		do
			Rand = rand() % Lines.size();
		while(Rand == m_AnnouncementLastLine);

		m_AnnouncementLastLine = Rand;
	}

	io_close(File);

	return Lines[m_AnnouncementLastLine];
}

int *CServer::GetIdMap(int ClientID)
{
	return m_aIdMap + VANILLA_MAX_CLIENTS * ClientID;
}

bool CServer::SetTimedOut(int ClientID, int OrigID)
{
	if(!m_NetServer.SetTimedOut(ClientID, OrigID))
	{
		return false;
	}
	m_aClients[ClientID].m_Sixup = m_aClients[OrigID].m_Sixup;

	if(m_aClients[OrigID].m_Authed != AUTHED_NO)
	{
		LogoutClient(ClientID, "Timeout Protection");
	}
	if(g_Config.m_SvRagequitBanTime > 0 && HasMarkedDisruptiveLeave(ClientID))
	{
		Ban(OrigID, g_Config.m_SvRagequitBanTime * 60, "Leave abuse");
		Ban(ClientID, g_Config.m_SvRagequitBanTime * 60, "Leave abuse");
	}
	else
	{
		if(HasMarkedDisruptiveLeave(ClientID))
			DelClientCallback(OrigID, "Rejoined", this);
		else
			DelClientCallback(OrigID, "Timeout Protection used", this);
	}

	m_aClients[ClientID].m_Authed = AUTHED_NO;
	m_aClients[ClientID].m_Flags = m_aClients[OrigID].m_Flags;
	m_aClients[ClientID].m_DisruptiveLeave = false;
	return true;
}

void CServer::SetErrorShutdown(const char *pReason)
{
	str_copy(m_aErrorShutdownReason, pReason, sizeof(m_aErrorShutdownReason));
}

void CServer::SetLoggingAxiom(int ClientID, bool State)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || m_aClients[ClientID].m_State < CClient::STATE_READY)
		return;
	
	m_aClients[ClientID].m_LoggingAxiom = State;
}

bool CServer::IsLoggingAxiom(int ClientID)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || m_aClients[ClientID].m_State < CClient::STATE_READY)
		return false;

	return m_aClients[ClientID].m_LoggingAxiom;
}

void CServer::SetLoginAxiom(int ClientID, bool State)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || m_aClients[ClientID].m_State < CClient::STATE_READY)
		return;

	m_aClients[ClientID].m_LoginAxiom = State;
}

bool CServer::IsLoginAxiom(int ClientID)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || m_aClients[ClientID].m_State < CClient::STATE_READY)
		return false;

	return m_aClients[ClientID].m_LoginAxiom;
}

void CServer::SetAxiomId(int ClientID, int Id)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || m_aClients[ClientID].m_State < CClient::STATE_READY)
		return;
	
	m_aClients[ClientID].m_AxiomId = Id;
}

int CServer::GetAxiomId(int ClientID)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || m_aClients[ClientID].m_State < CClient::STATE_READY)
		return false;

	return m_aClients[ClientID].m_AxiomId;
}