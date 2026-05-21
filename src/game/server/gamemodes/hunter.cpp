/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "hunter.h"

#include <game/version.h>
#include <game/server/entities/character.h>
#include <game/server/entities/laser.h>
#include <game/server/weapons.h>

CGameControllerHunter::CGameControllerHunter() :
	IGameController()
{
	INSTANCE_CONFIG_INT(&m_HunterFragNumHit, "htn_hunt_frag_num_hit", 16, 0, 512, CFGFLAG_CHAT | CFGFLAG_INSTANCE, "猎人榴弹直接砸到人产生的破片数量 (整数, 默认16, 限制0~512)");
	INSTANCE_CONFIG_INT(&m_HunterFragNumNoHit, "htn_hunt_frag_num_nohit", 32, 0, 512, CFGFLAG_CHAT | CFGFLAG_INSTANCE, "猎人榴弹没砸到人产生的破片数量 (整数, 默认32, 限制0~512)");
	INSTANCE_CONFIG_INT(&m_HunterTrackCooldown, "htn_hunt_track_cooldown", 5000, 0, 0x7FFFFFFF, CFGFLAG_CHAT | CFGFLAG_INSTANCE, "猎人锤子追踪玩家的冷却毫秒 (整数, 默认5000, 限制0~2147483647)");
	INSTANCE_CONFIG_INT(&m_HunterTrackTime, "htn_hunt_track_time", 2000, 0, 0x7FFFFFFF, CFGFLAG_CHAT | CFGFLAG_INSTANCE, "猎人锤子追踪玩家的时长毫秒 (整数, 默认2000, 限制0~2147483647)");

	m_pGameType = "hunter";
	m_GameFlags = IGF_ROUND_TIMER_ROUND;

	m_MinimumPlayers = 2;
}

CGameControllerHunter::~CGameControllerHunter()
{
	for(int i = 0; i < NUM_SNAP_IDS; i++)
		Server()->SnapFreeID(m_aSnapIDs[i]);
}

void CGameControllerHunter::OnInit()
{
	mem_zero(&m_aClass, sizeof(m_aClass));

	mem_zero(&m_TrackTick, sizeof(m_TrackTick));
	for(int i = 0; i < NUM_SNAP_IDS; i++)
		m_aSnapIDs[i] = Server()->SnapNewID();
}

int CGameControllerHunter::OnCharacterDeath(class CCharacter *pVictim, class CPlayer *pKiller, int Weapon)
{
	int Return = DEATH_NORMAL;
	int VictimCID = pVictim->GetPlayer()->GetCID();
	//int KillerCID = pKiller->GetCID();

#	define REGISTER_HUNTERCLASS(ID, MASK_ID, TEAM_ID, DEATHFUNC, HANDLEFIREFUNC, DODMGFUNC, SPAWNFUNC, TAKEDMGFUNC) \
		if(m_aClass[VictimCID] == HunterClass::ID) \
			Return |= DEATHFUNC(pVictim, pKiller, Weapon);
#	include <game/server/gamemodes/hunter.h>
#	undef REGISTER_HUNTERCLASS

	return Return;
}

bool CGameControllerHunter::OnCharacterHandleFire(class CWeapon *pWeapon, vec2 &Direction)
{
#	define REGISTER_HUNTERCLASS(ID, MASK_ID, TEAM_ID, DEATHFUNC, HANDLEFIREFUNC, DODMGFUNC, SPAWNFUNC, TAKEDMGFUNC) \
	if(m_aClass[pWeapon->Character()->GetPlayer()->GetCID()] == HunterClass::ID) \
		return HANDLEFIREFUNC(pWeapon, Direction);
#	include <game/server/gamemodes/hunter.h>
#	undef REGISTER_HUNTERCLASS
	return true;
}

void CGameControllerHunter::OnCharacterSpawn(CCharacter *pChr)
{
	int CID = pChr->GetPlayer()->GetCID();
#	define REGISTER_HUNTERCLASS(ID, MASK_ID, TEAM_ID, DEATHFUNC, HANDLEFIREFUNC, DODMGFUNC, SPAWNFUNC, TAKEDMGFUNC) \
		if(m_aClass[CID] == HunterClass::ID) \
		{ \
			SPAWNFUNC(pChr); \
			return; \
		}
#	include <game/server/gamemodes/hunter.h>
#	undef REGISTER_HUNTERCLASS
}

int CGameControllerHunter::OnCharacterTakeDamage(class CCharacter *pChr, vec2 &Force, int &Dmg, int From, int WeaponType, int WeaponID, bool IsExplosion)
{
	int CID = pChr->GetPlayer()->GetCID();
	int Return = DAMAGE_NORMAL;

#	define REGISTER_HUNTERCLASS(ID, MASK_ID, TEAM_ID, DEATHFUNC, HANDLEFIREFUNC, DODMGFUNC, SPAWNFUNC, TAKEDMGFUNC) \
		if(m_aClass[From] == HunterClass::ID) \
			Return |= DODMGFUNC(pChr, Force, Dmg, From, WeaponType, WeaponID, IsExplosion);
#	include <game/server/gamemodes/hunter.h>
#	undef REGISTER_HUNTERCLASS

#	define REGISTER_HUNTERCLASS(ID, MASK_ID, TEAM_ID, DEATHFUNC, HANDLEFIREFUNC, DODMGFUNC, SPAWNFUNC, TAKEDMGFUNC) \
		if(m_aClass[CID] == HunterClass::ID) \
			Return |= TAKEDMGFUNC(pChr, Force, Dmg, From, WeaponType, WeaponID, IsExplosion);
#	include <game/server/gamemodes/hunter.h>
#	undef REGISTER_HUNTERCLASS

	return Return;
}

void CGameControllerHunter::OnPlayerSnap(class CPlayer *pPlayer, int SnappingClient,
			CNetObj_ClientInfo *pClientInfo, CNetObj_PlayerInfo *pPlayerInfo, CNetObj_SpectatorInfo *pSpectatorInfo, // 0.6
			protocol7::CNetObj_PlayerInfo *pPlayerInfo7, protocol7::CNetObj_SpectatorInfo *pSpectatorInfo7, // 0.7
			CNetObj_DDNetSpectatorInfo *pDDNetSpectatorInfo, CNetObj_DDNetPlayer *pDDNetPlayer)
{
}

void CGameControllerHunter::OnSnap(int SnappingClient,
			CNetObj_GameInfo *pGameInfoObj, CNetObj_GameInfoEx *pGameInfoEx, CNetObj_GameData *pGameDataObj,
			protocol7::CNetObj_GameData *pGameData, protocol7::CNetObj_GameDataTeam *pGameDataTeam, protocol7::CNetObj_GameDataFlag *pGameDataFlag)
{
	CPlayer *pPlayer = GetPlayerIfInRoom(SnappingClient);
	if(!pPlayer)
		return;
	CCharacter *pChr = pPlayer->GetCharacter();
	if(m_aClass[SnappingClient] == HunterClass::ID_HUNTER)
	{
		if(pChr)
		{
			CWeapon *pWeapon = pChr->CurrentWeapon();
			if(pWeapon && pWeapon->GetWeaponID() == WEAPON_ID_HAMMER && !pWeapon->IsReloading() && pChr->GetLatestInput()->m_Fire & 1)
			{
				if(m_TrackTick[SnappingClient] + ((m_HunterTrackCooldown * Server()->TickSpeed()) / 1000) <= Server()->Tick())
					m_TrackTick[SnappingClient] = Server()->Tick();
				else
					GameServer()->SendBroadcast("锤子追踪正在冷却中", SnappingClient, false);
			}
			if(m_TrackTick[SnappingClient] + ((m_HunterTrackTime * Server()->TickSpeed()) / 1000) > Server()->Tick())
			{
				CEntity *pClosestChar = GameWorld()->ClosestEntity(pChr->m_Pos, 256.f * 32.f, CGameWorld::ENTTYPE_CHARACTER, pChr);
				if(pClosestChar)
				{
					vec2 From = pWeapon->Pos();
					vec2 ToDir = normalize(pClosestChar->m_Pos - From);
					vec2 To = From + (ToDir * 80.f);
					From -= ToDir * 56.f;

					if(!SnapLaser(m_aSnapIDs[0], From, To, Server()->Tick() - 3))
						return;
				}
			}
		}
	}
}

// Civic class
int CGameControllerHunter::OnCivicDeath(class CCharacter *pVictim, class CPlayer *pKiller, int Weapon)
{
	GameWorld()->CreateSoundGlobal(SOUND_CTF_DROP);
	return DEATH_NORMAL;
}
bool CGameControllerHunter::OnCivicFire(class CWeapon *pWeapon, vec2 Direction)
{
	return true;
}
int CGameControllerHunter::OnCivicDoDmg(class CCharacter *pChr, vec2 &Force, int &Dmg, int From, int WeaponType, int WeaponID, bool IsExplosion)
{
	return DAMAGE_NORMAL;
}
void CGameControllerHunter::OnCivicSpawn(class CCharacter *pChr)
{
	pChr->IncreaseHealth(10);
	pChr->GiveWeapon(WEAPON_GUN, WEAPON_ID_PISTOL, 10);

	GameWorld()->CreateSoundGlobal(SOUND_CTF_GRAB_PL, CmaskOne(pChr->GetPlayer()->GetCID()));
}
int CGameControllerHunter::OnCivicTakeDmg(class CCharacter *pChr, vec2 &Force, int &Dmg, int From, int WeaponType, int WeaponID, bool IsExplosion)
{
	return DAMAGE_NORMAL;
}

// Hunter class
bool CGameControllerHunter::OnHunterFire(class CWeapon *pWeapon, vec2 Direction)
{
	bool Ret = true;

	if(pWeapon->IsReloading() || !pWeapon->GetAmmo())
		return Ret;

	if(pWeapon->GetWeaponID() == WEAPON_ID_GRENADE)
	{
		static auto FragCallback = [](CProjectile *pProj, vec2 Pos, CCharacter *pHit, bool EndOfLife) -> bool
		{
			if(pHit)
			{
				if(pHit->GetPlayer()->GetCID() == pProj->GetOwner())
					return false;

				pHit->TakeDamage(vec2(0, 0), g_pData->m_Weapons.m_Shotgun.m_pBase->m_Damage, pProj->GetOwner(), WEAPON_GRENADE, pProj->GetWeaponID(), false);
			}

			return true;
		};

		static auto GrenadeCallback = [](CProjectile *pProj, vec2 Pos, CCharacter *pHit, bool EndOfLife) -> bool
		{
			if(pHit && pHit->GetPlayer()->GetCID() == pProj->GetOwner())
				return false;

			pProj->GameWorld()->CreateExplosion(Pos, pProj->GetOwner(), WEAPON_GRENADE, pProj->GetWeaponID(), g_pData->m_Weapons.m_aId[WEAPON_GRENADE].m_Damage, pProj->GetOwner() < 0);
			pProj->GameWorld()->CreateSound(Pos, SOUND_GRENADE_EXPLODE);

			static const float PerAngle = pi * 2 / RAND_MAX;
			int FragNum = pHit ?
					((CGameControllerHunter *)(pProj->Controller()))->m_HunterFragNumHit :
					((CGameControllerHunter *)(pProj->Controller()))->m_HunterFragNumNoHit;

			for(int i = 0; i < FragNum; i++) // Create Fragments
			{	
				vec2 Dir = direction(PerAngle * rand());
				new CProjectile(
					pProj->GameWorld(),
					WEAPON_SHOTGUN, //Type
					pProj->GetWeaponID(), //WeaponID
					pProj->GetOwner(), //Owner
					Pos + (Dir * 8.f), //Pos
					Dir * 0.5f, //Dir
					6.0f, // Radius
					10, //Span
					FragCallback);
			}

			float Angle = PerAngle * rand();
			for(int i = 0; i < HUNTERNCONFIG::NUM_HUNT_GRENADE_SFX; i++)
				pProj->GameWorld()->CreateExplosionParticle(Pos + direction(Angle + i * (pi * 2 / HUNTERNCONFIG::NUM_HUNT_GRENADE_SFX)) * (3.8f * 32.f), -1);

			return true;
		};

		int ClientID = pWeapon->Character()->GetPlayer()->GetCID();
		int Lifetime = pWeapon->Character()->CurrentTuning()->m_GrenadeLifetime * Server()->TickSpeed();

		vec2 ProjStartPos = pWeapon->Pos() + Direction * pWeapon->GetProximityRadius() * 0.75f;
	
		new CProjectile(
			GameWorld(),
			WEAPON_GRENADE, //Type
			pWeapon->GetWeaponID(), //WeaponID
			ClientID, //Owner
			ProjStartPos, //Pos
			Direction, //Dir
			6.0f, // Radius
			Lifetime, //Span
			GrenadeCallback);

		GameWorld()->CreateSound(pWeapon->Pos(), SOUND_GRENADE_FIRE);

		Ret = false;
	}
	else if(pWeapon->GetWeaponID() == WEAPON_ID_LASER)
	{
		static auto ImpactCallback = [](CLaser *pLaser, vec2 HitPoint, CCharacter *pHit, bool OutOfEnergy) -> bool
		{
			if(pHit)
			{
				if(pHit->GetPlayer()->GetCID() == pLaser->GetOwner())
					return false;

				float Dist = distance(pLaser->GetPos(), HitPoint);
				int Dmg = 12//pLaser->GameServer()->Tuning()->m_LaserDamage
						- Dist / (pLaser->GameServer()->Tuning()->m_LaserReach / 5);
				pHit->TakeDamage(vec2(0, 0), Dmg, pLaser->GetOwner(), WEAPON_LASER, pLaser->GetWeaponID(), false);
				return true;
			}

			return false;
		};

		int ClientID = pWeapon->Character()->GetPlayer()->GetCID();
		// SEntityCustomData CustomData;
		// CustomData.m_pData = new vec2(pWeapon->Pos());
		// CustomData.m_Callback = [](void *pData) { delete(vec2 *)pData; };
		
		new CLaser(
			GameWorld(),
			WEAPON_GUN, //Type
			pWeapon->GetWeaponID(), //WeaponID
			ClientID, //Owner
			pWeapon->Pos(), //Pos
			Direction, //Dir
			GameServer()->Tuning()->m_LaserReach, // StartEnergy
			ImpactCallback);

		GameWorld()->CreateSound(pWeapon->Pos(), SOUND_LASER_FIRE);
	
		Ret = false;
	}

	return Ret;
}
void CGameControllerHunter::OnHunterSpawn(class CCharacter *pChr)
{
	pChr->IncreaseHealth(10);
	pChr->GiveWeapon(WEAPON_GUN, WEAPON_ID_PISTOL, 10);
	pChr->ForceSetWeapon(WEAPON_HAMMER, WEAPON_ID_HAMMER, -1);

	GameWorld()->CreateSoundGlobal(SOUND_CTF_GRAB_EN, CmaskOne(pChr->GetPlayer()->GetCID()));
}
int CGameControllerHunter::OnHunterDeath(class CCharacter *pVictim, class CPlayer *pKiller, int Weapon)
{
	return DEATH_NORMAL;
}
int CGameControllerHunter::OnHunterDoDmg(class CCharacter *pChr, vec2 &Force, int &Dmg, int From, int WeaponType, int WeaponID, bool IsExplosion)
{
	if(WeaponID == WEAPON_ID_HAMMER && WeaponType == WEAPON_HAMMER)
		Dmg = 20;
	else if(WeaponID == WEAPON_ID_PISTOL || WeaponID == WEAPON_ID_SHOTGUN) // no more dmg for hunter grenade
		Dmg = 2;
	else if(WeaponID == WEAPON_ID_LASER)
	{
		
	}
	
	return DAMAGE_NORMAL;
}
int CGameControllerHunter::OnHunterTakeDmg(class CCharacter *pChr, vec2 &Force, int &Dmg, int From, int WeaponType, int WeaponID, bool IsExplosion)
{
	if(pChr->GetPlayer()->GetCID() == From)
		return DAMAGE_NO_DAMAGE | DAMAGE_NO_INDICATOR;
	return DAMAGE_NORMAL;
}
