/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "huntern.h"

#include <engine/shared/console.h>
#include <game/server/entities/character.h>
#include <game/server/entities/textentity.h>
#include <game/server/weapons.h>
#include <game/version.h>

void CGameControllerHunterN::AddVote(class IGameController *pSelf, const char *pDescription, const char *pCommand)
{
	if(pSelf->m_NumVoteOptions == MAX_VOTE_OPTIONS)
	{
		pSelf->GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "maximum number of room vote options reached");
		return;
	}

	// check for valid option
	if(!pSelf->InstanceConsole()->LineIsValid(pCommand) || str_length(pCommand) >= VOTE_CMD_LENGTH)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "skipped invalid command '%s'", pCommand);
		pSelf->GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		return;
	}
	while(*pDescription == ' ')
		pDescription++;
	if(str_length(pDescription) >= VOTE_DESC_LENGTH || *pDescription == 0)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "skipped invalid option '%s'", pDescription);
		pSelf->GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		return;
	}

	// check for duplicate entry
	CVoteOptionServer *pOption = pSelf->m_pVoteOptionFirst;
	while(pOption)
	{
		if(str_comp_nocase(pDescription, pOption->m_aDescription + sizeof("☐ ")) == 0)
		{
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "option '%s' already exists", pDescription);
			pSelf->GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
			return;
		}
		pOption = pOption->m_pNext;
	}

	// add the option
	++pSelf->m_NumVoteOptions;
	int Len = str_length(pCommand);

	pOption = (CVoteOptionServer *)pSelf->m_pVoteOptionHeap->Allocate(sizeof(CVoteOptionServer) + Len);
	pOption->m_pNext = 0;
	pOption->m_pPrev = pSelf->m_pVoteOptionLast;
	if(pOption->m_pPrev)
		pOption->m_pPrev->m_pNext = pOption;
	pSelf->m_pVoteOptionLast = pOption;
	if(!pSelf->m_pVoteOptionFirst)
		pSelf->m_pVoteOptionFirst = pOption;

	if(str_comp(pCommand, "info") == 0)
		str_format(pOption->m_aDescription, sizeof(pOption->m_aDescription), "%s", pDescription);
	else
		str_format(pOption->m_aDescription, sizeof(pOption->m_aDescription), "☐ %s", pDescription);
	mem_copy(pOption->m_aCommand, pCommand, Len + 1);
	pSelf->m_ResendVotes = true;
}


void CGameControllerHunterN::ConMapRotations(IConsole::IResult *pResult, void *pUserData)
{
	CGameControllerHunterN *pSelf = (CGameControllerHunterN *)pUserData;

	pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "instance", "Value: ");

	char aBuf[256];
	for(int i = 0; i < CGameControllerHunterN::MAX_MAPROTATIONS; ++i)
	{
		if(!pSelf->m_aMapRotations[i])
			break;

		const char *pMapName = pSelf->GameServer()->Teams()->GetMapName(pSelf->m_aMapRotations[i]);
		if(!pMapName)
			continue;

		str_format(aBuf, sizeof(aBuf), "Slot%d | Map%d: %s, ", i, pSelf->m_aMapRotations[i], pMapName);
		pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "instance", aBuf);
	}
}

void CGameControllerHunterN::ConMapRotationsAdd(IConsole::IResult *pResult, void *pUserData)
{
	CGameControllerHunterN *pSelf = (CGameControllerHunterN *)pUserData;

	char aBuf[256];

	const char *pMapName;
	int MapIndex;
	int j = 0;
	bool IsError;

	for(int i = 0; i < pResult->NumArguments(); i++)
	{
		IsError = true;
		pMapName = pResult->GetString(i);

		for(; j < CGameControllerHunterN::MAX_MAPROTATIONS; j++)
		{
			if(pSelf->m_aMapRotations[j])
				continue;

			MapIndex = pSelf->GameServer()->Teams()->GetMapIndex(pMapName);
			if(MapIndex == 0)
				break;

			MapIndex -= 1; // magic in CGameTeams
			pSelf->m_aMapRotations[j] = MapIndex;

			str_format(aBuf, sizeof(aBuf), "Added map%d '%s' to slot %d", i, pMapName, j);
			pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "instance", aBuf);

			IsError = false;
			break;
		}
		if(!IsError)
			continue;
		str_format(aBuf, sizeof(aBuf), "Cannot add map%d '%s' to slot %d (%s)", i, pMapName, j, (j >= CGameControllerHunterN::MAX_MAPROTATIONS ? "Out of Range" : "No Map Found"));
		pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "instance", aBuf);
	}
}

void CGameControllerHunterN::ConMapRotationsRemove(IConsole::IResult *pResult, void *pUserData)
{
	CGameControllerHunterN *pSelf = (CGameControllerHunterN *)pUserData;

	if(pResult->NumArguments() < 1)
	{
		mem_zero(pSelf->m_aMapRotations, sizeof(pSelf->m_aMapRotations));
		pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "instance", "Remove all map from maprotations");
		return;
	}

	int TargetMap = pResult->GetInteger(0);

	if(!pSelf->m_aMapRotations[TargetMap])
	{
		char aBuf[64];
		str_format(aBuf, sizeof(aBuf), "Cannot remove map in empty slot %d", TargetMap);
		pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "instance", aBuf);
		return;
	}

	mem_copy(&pSelf->m_aMapRotations[TargetMap], &pSelf->m_aMapRotations[TargetMap + 1], CGameControllerHunterN::MAX_MAPROTATIONS - TargetMap - 1);
	pSelf->m_aMapRotations[CGameControllerHunterN::MAX_MAPROTATIONS - 1] = 0;

	char aBuf[32];
	str_format(aBuf, sizeof(aBuf), "Removed map in slot %d from maprotations", TargetMap);
	pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "instance", aBuf);
}

void CGameControllerHunterN::ConMapMask(IConsole::IResult *pResult, void *pUserData)
{
	CGameControllerHunterN *pSelf = (CGameControllerHunterN *)pUserData;

	pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "instance", "Value: ");

	char aBuf[256];
	for(int i = 0; i < CGameControllerHunterN::MAX_MAPROTATIONS; ++i)
	{
		const char *pMapName = pSelf->GameServer()->Teams()->GetMapName(i);
		if(!pMapName[0])
			break;

		str_format(aBuf, sizeof(aBuf), "Mask: 0x%x | Map%d: %s", pSelf->m_aMapMask[i], i, pMapName);
		pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "instance", aBuf);
	}
}

void CGameControllerHunterN::ConMapMaskAdd(IConsole::IResult *pResult, void *pUserData)
{
	CGameControllerHunterN *pSelf = (CGameControllerHunterN *)pUserData;

	const char *pMapName = pResult->GetString(0);
	int MapMask = pResult->GetInteger(1);
	char aBuf[64];

	int MapIndex = pSelf->GameServer()->Teams()->GetMapIndex(pMapName);
	if(MapIndex == 0)
	{
		str_format(aBuf, sizeof(aBuf), "Cannot find map: '%s'", pMapName);
		pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "instance", aBuf);
		return;
	}

	MapIndex -= 1; // magic in CGameTeams
	pSelf->m_aMapMask[MapIndex] = MapMask;

	str_format(aBuf, sizeof(aBuf), "Add mask 0x%x to map%d '%s'", MapMask, MapIndex, pMapName);
	pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "instance", aBuf);
	return;
}

void CGameControllerHunterN::ConMapMaskRemove(IConsole::IResult *pResult, void *pUserData)
{
	CGameControllerHunterN *pSelf = (CGameControllerHunterN *)pUserData;

	if(pResult->NumArguments() == 0)
	{
		mem_zero(pSelf->m_aMapMask, sizeof(pSelf->m_aMapMask));
		pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "instance", "Remove all mask from maps");
		return;
	}

	char aBuf[64];
	const char *pMapName = pResult->GetString(0);

	for(int i = 0; i < CGameControllerHunterN::MAX_MAPROTATIONS; i++)
	{
		int MapIndex = pSelf->GameServer()->Teams()->GetMapIndex(pMapName);
		if(MapIndex == 0)
		{
			str_format(aBuf, sizeof(aBuf), "Cannot find map: '%s'", pMapName);
			pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "instance", aBuf);
			return;
		}

		MapIndex -= 1; // magic in CGameTeams
		pSelf->m_aMapMask[MapIndex] = 0;

		str_format(aBuf, sizeof(aBuf), "Remove mask from map%d '%s'", MapIndex, pMapName);
		pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "instance", aBuf);
		return;
	}
}

void CGameControllerHunterN::ConMapMaskAddVote(IConsole::IResult *pResult, void *pUserData)
{
	CGameControllerHunterN *pSelf = (CGameControllerHunterN *)pUserData;

	char aBuf[64];
	char Description[1024];
	char Command[1024];
	int Mask = pResult->GetInteger(0);
	const char *pDescPrefix = pResult->GetString(1);

	for(int i = 0; i < CGameControllerHunterN::MAX_MAPROTATIONS; ++i)
	{
		const char *pMapName = pSelf->GameServer()->Teams()->GetMapName(i);
		if(!pMapName[0])
			break;
		if(!(pSelf->m_aMapMask[i] & Mask))
			continue;

		str_format(Description, sizeof(Description), "%s%s", pDescPrefix, pMapName);
		str_format(Command, sizeof(Command), "map %s", pMapName);
		AddVote(pSelf, Description, Command);
		str_format(aBuf, sizeof(aBuf), "Add map%d: %s to vote menu", i, pMapName);
		pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "instance", aBuf);
	}
}

void CGameControllerHunterN::ConShowRng(IConsole::IResult *pResult, void *pUserData)
{
	CGameControllerHunterN *pSelf = (CGameControllerHunterN *)pUserData;

	for(int i = 0; i < (int)(sizeof(CGameControllerHunterN::m_apRng) / sizeof(CGameControllerHunterN::m_apRng[0])); i++)
	{
		if(!pSelf->m_apRng[i] || !pSelf->m_apRng[i]->m_apNameDesc)
		{
			pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "instance", "");
			continue;
		}
		pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "instance", pSelf->m_apRng[i]->m_apNameDesc);
	}
}

void CGameControllerHunterN::ConSetRng(IConsole::IResult *pResult, void *pUserData)
{
	CGameControllerHunterN *pSelf = (CGameControllerHunterN *)pUserData;

	if(pResult->NumArguments() > 0)
	{
		int RngID = pResult->GetInteger(0);
		if(RngID < 0 || RngID >= (int)(sizeof(CGameControllerHunterN::m_apRng) / sizeof(CGameControllerHunterN::m_apRng[0])) || !pSelf->m_apRng[RngID])
		{
			pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "instance", "RngID is invaild");
			return;
		}
		pSelf->m_RngEnabled = RngID;
		pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "instance", "Rng is set");
	}
	else
	{
		CGameControllerHunterN::ConShowRng(pResult, pUserData);
	}
}

void CGameControllerHunterN::ConSetClass(IConsole::IResult *pResult, void *pUserData)
{
	CGameControllerHunterN *pSelf = (CGameControllerHunterN *)pUserData;

	int CID = pResult->NumArguments() > 1 ? pResult->GetInteger(1) : pResult->m_ClientID;
	CPlayer *pPlayer = pSelf->GetPlayerIfInRoom(CID);
	if(!pPlayer) // If the player does not exist
	{
		pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "huntern", "invalid client id");
		return;
	}
	bool IsClassSet = false;
#	define REGISTER_HUNTERCLASS(ID, MASK_ID, TEAM_ID, DEATHFUNC, HANDLEFIREFUNC, DODMGFUNC, SPAWNFUNC, TAKEDMGFUNC) \
		if(HunterClass::ID == pResult->GetInteger(0)) \
		{ \
			IsClassSet = true; \
			pSelf->SetClass(CID, HunterClass::ID); \
			pSelf->m_aTeam[CID] = TEAM_ID; \
			if(pPlayer->GetCharacter() && pPlayer->GetCharacter()->IsAlive()) \
				pSelf->OnCharacterSpawn(pPlayer->GetCharacter()); \
		}
#		include <game/server/gamemodes/hunter.h>
#	undef REGISTER_HUNTERCLASS
	if(pResult->NumArguments() > 2)
		pSelf->m_aTeam[CID] = pResult->GetInteger(2); // Team
	if(!IsClassSet)
		pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "huntern", "invalid class id");
}

void CGameControllerHunterN::ConGiveWeapon(IConsole::IResult *pResult, void *pUserData)
{
	IGameController *pSelf = (IGameController *)pUserData;

	CPlayer *pPlayer = pSelf->GetPlayerIfInRoom((pResult->NumArguments() > 2) ? pResult->GetInteger(2) : pResult->m_ClientID);
	if(!pPlayer) 
		pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "huntern", "invalid client id");
	else if(!pPlayer->GetCharacter() || !pPlayer->GetCharacter()->IsAlive())
		pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "huntern", "character is dead");
	else
	{	pPlayer->GetCharacter()->RemoveWeapon((pResult->GetInteger(1) < NUM_WEAPONS && pResult->GetInteger(1) >= 0) ? pResult->GetInteger(1) : 0); // Slot
		if(!pResult->GetInteger(4)) // Give Weapon
			pPlayer->GetCharacter()->GiveWeapon((pResult->GetInteger(1) < NUM_WEAPONS && pResult->GetInteger(1) >= 0) ? pResult->GetInteger(1) : 0, // Slot
				pResult->GetInteger(0), // Type
					(pResult->NumArguments() > 3) ? pResult->GetInteger(3) : -1); // ammo
		else // Powerup Weapon
			pPlayer->GetCharacter()->SetPowerUpWeapon(pResult->GetInteger(0), // Type
					(pResult->NumArguments() > 3) ? pResult->GetInteger(3) : -1); // ammo
	}
}

void CGameControllerHunterN::ConSetHeal(IConsole::IResult *pResult, void *pUserData)
{
	IGameController *pSelf = (IGameController *)pUserData;

	CPlayer *pPlayer = pSelf->GetPlayerIfInRoom((pResult->NumArguments() > 2) ? pResult->GetInteger(2) : pResult->m_ClientID);
	if(!pPlayer)
		pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "huntern", "invalid client id");
	else if(!pPlayer->GetCharacter() || !pPlayer->GetCharacter()->IsAlive())
		pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "huntern", "character is dead");
	else
	{	if(pResult->NumArguments() > 3) // Set m_MaxHealth
			pPlayer->GetCharacter()->SetMaxHealth(pResult->GetInteger(3) > 0 ? pResult->GetInteger(3) : 0); // math maximum(pResult->GetInteger(3), 0);
		if(pResult->NumArguments() > 4) // Set m_MaxArmor
			pPlayer->GetCharacter()->SetMaxArmor(pResult->GetInteger(4) > 0 ? pResult->GetInteger(4) : 0); // math maximum(pResult->GetInteger(4), 0);

		pPlayer->GetCharacter()->SetHealth(pResult->GetInteger(0)); // Set Health
		if(pResult->NumArguments() > 1)
			pPlayer->GetCharacter()->SetArmor(pResult->GetInteger(1));} // Set Armor
}

void CGameControllerHunterN::ConRevive(IConsole::IResult *pResult, void *pUserData)
{
	CGameControllerHunterN *pSelf = (CGameControllerHunterN *)pUserData;

	int CID = pResult->NumArguments() > 0 ? pResult->GetInteger(0) : pResult->m_ClientID;
	CPlayer *pPlayer = pSelf->GetPlayerIfInRoom(CID);
	if(!pPlayer) // If the player does not exist
		pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "huntern", "invalid client id");
	else if(pPlayer->GetCharacter() && pPlayer->GetCharacter()->IsAlive())
		pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "huntern", "character is alive");
	else
	{	if(pSelf->m_aTeam[CID] == TEAM_NONE)
			pSelf->m_aTeam[CID] = TEAM_CIVIC;
		pPlayer->TryRespawn();}
}

void CGameControllerHunterN::ConSign(IConsole::IResult *pResult, void *pUserData)
{
	IGameController *pSelf = (IGameController *)pUserData;

	CPlayer *pPlayer = pSelf->GetPlayerIfInRoom(pResult->m_ClientID);
	if(!pPlayer) // If the player does not exist
		pSelf->InstanceConsole()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "huntern", "invalid client id");
	else
		new CTextEntity(pSelf->GameWorld(), pPlayer->m_ViewPos, CTextEntity::TYPE_LASER, 12, CTextEntity::ALIGN_MIDDLE, (char *)pResult->GetString(0));
}

// void CGameControllerHunterN::ConSelectMask(IConsole::IResult *pResult, void *pUserData)
// {
// 	CGameControllerHunterN *pSelf = (CGameControllerHunterN *)pUserData;

// 	int Mask = pResult->GetInteger(0);

// 	for(int i = 0; i < ; ++i)
// 	{
		
// 	}
// }
