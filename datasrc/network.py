# pylint: skip-file
# See https://github.com/ddnet/ddnet/issues/3507

from datatypes import Enum, Flags, NetBool, NetEvent, NetIntAny, NetIntRange, NetMessage, NetMessageEx, NetObject, NetObjectEx, NetString, NetStringHalfStrict, NetStringStrict, NetTick

Emotes = ["NORMAL", "PAIN", "HAPPY", "SURPRISE", "ANGRY", "BLINK"]
PlayerFlags = ["PLAYING", "IN_MENU", "CHATTING", "SCOREBOARD", "AIM", "SPEC_CAM"]
GameFlags = ["TEAMS", "FLAGS"]
GameStateFlags = ["GAMEOVER", "SUDDENDEATH", "PAUSED", "RACETIME"]
CharacterFlags = ["SOLO", "JETPACK", "NO_COLLISION", "ENDLESS_HOOK", "ENDLESS_JUMP", "SUPER",
                  "NO_HAMMER_HIT", "NO_SHOTGUN_HIT", "NO_GRENADE_HIT", "NO_LASER_HIT", "NO_HOOK",
                  "TELEGUN_GUN", "TELEGUN_GRENADE", "TELEGUN_LASER",
                  "WEAPON_HAMMER", "WEAPON_GUN", "WEAPON_SHOTGUN", "WEAPON_GRENADE", "WEAPON_LASER", "WEAPON_NINJA",
                  "MOVEMENTS_DISABLED", "IN_FREEZE", "PRACTICE_MODE", "LOCK_MODE", "TEAM0_MODE", "INVINCIBLE"]
GameInfoFlags = [
	"TIMESCORE", "GAMETYPE_RACE", "GAMETYPE_FASTCAP", "GAMETYPE_FNG",
	"GAMETYPE_DDRACE", "GAMETYPE_DDNET", "GAMETYPE_BLOCK_WORLDS",
	"GAMETYPE_VANILLA", "GAMETYPE_PLUS", "FLAG_STARTS_RACE", "RACE",
	"UNLIMITED_AMMO", "DDRACE_RECORD_MESSAGE", "RACE_RECORD_MESSAGE",
	"ALLOW_EYE_WHEEL", "ALLOW_HOOK_COLL", "ALLOW_ZOOM", "BUG_DDRACE_GHOST",
	"BUG_DDRACE_INPUT", "BUG_FNG_LASER_RANGE", "BUG_VANILLA_BOUNCE",
	"PREDICT_FNG", "PREDICT_DDRACE", "PREDICT_DDRACE_TILES", "PREDICT_VANILLA",
	"ENTITIES_DDNET", "ENTITIES_DDRACE", "ENTITIES_RACE", "ENTITIES_FNG",
	"ENTITIES_VANILLA", "DONT_MASK_ENTITIES", "ENTITIES_BW"
	# Full, use GameInfoFlags2 for more flags
]
GameInfoFlags2 = [
	"ALLOW_X_SKINS", "GAMETYPE_CITY", "GAMETYPE_FDDRACE", "ENTITIES_FDDRACE", "HUD_HEALTH_ARMOR", "HUD_AMMO",
	"HUD_DDRACE", "NO_WEAK_HOOK", "NO_SKIN_CHANGE_FOR_FROZEN", "DDRACE_TEAM"
]

ExPlayerFlags = ["AFK", "PAUSED", "SPEC"]
ProjectileFlags = ["CLIENTID_BIT{}".format(i) for i in range(8)] + [
	"NO_OWNER", "IS_DDNET", "BOUNCE_HORIZONTAL", "BOUNCE_VERTICAL",
	"EXPLOSIVE", "FREEZE",
]
LaserFlags = [
	"NO_PREDICT",
]

LaserTypes = ["RIFLE", "SHOTGUN", "DOOR", "FREEZE", "DRAGGER", "GUN", "PLASMA"]
DraggerTypes = ["WEAK", "WEAK_NW", "NORMAL", "NORMAL_NW", "STRONG", "STRONG_NW"]
GunTypes = ["UNFREEZE", "EXPLOSIVE", "FREEZE", "EXPFREEZE"]

Emoticons = ["OOP", "EXCLAMATION", "HEARTS", "DROP", "DOTDOT", "MUSIC", "SORRY", "GHOST", "SUSHI", "SPLATTEE", "DEVILTEE", "ZOMG", "ZZZ", "WTF", "EYES", "QUESTION"]

Powerups = ["HEALTH", "ARMOR", "WEAPON", "NINJA", "ARMOR_SHOTGUN", "ARMOR_GRENADE", "ARMOR_NINJA", "ARMOR_LASER"]
Authed = ["NO", "HELPER", "MOD", "ADMIN"]
EntityClasses = ["PROJECTILE", "DOOR", "DRAGGER_WEAK", "DRAGGER_NORMAL", "DRAGGER_STRONG", "GUN_NORMAL", "GUN_EXPLOSIVE", "GUN_FREEZE", "GUN_UNFREEZE", "LIGHT", "PICKUP"]
#Teams = ["ALL", "SPECTATORS", "RED", "BLUE", "WHISPER_SEND", "WHISPER_RECV"]

RawHeader = '''

#include <engine/message.h>
#include <engine/shared/protocol_ex.h>

enum
{
	INPUT_STATE_MASK=0x3f
};

enum
{
	TEAM_SPECTATORS=-1,
	TEAM_RED,
	TEAM_BLUE,

	FLAG_MISSING=-3,
	FLAG_ATSTAND,
	FLAG_TAKEN,

	SPEC_FREEVIEW=-1,
	SPEC_FOLLOW=-2,
};

enum
{
	GAMEINFO_CURVERSION=7,
};
'''

RawSource = '''
#include <engine/message.h>
#include "protocol.h"
'''

Enums = [
	Enum("EMOTE", Emotes),
	Enum("POWERUP", Powerups),
	Enum("EMOTICON", Emoticons),
	Enum("AUTHED", Authed),
    Enum("ENTITYCLASS", EntityClasses),
	Enum("LASERTYPE", LaserTypes),
	Enum("LASERDRAGGERTYPE", DraggerTypes),
	Enum("LASERGUNTYPE", GunTypes),
#	Enum("TEAM", Teams, -2),
]

Flags = [
	Flags("PLAYERFLAG", PlayerFlags),
	Flags("GAMEFLAG", GameFlags),
	Flags("GAMESTATEFLAG", GameStateFlags),
	Flags("CHARACTERFLAG", CharacterFlags),
	Flags("GAMEINFOFLAG", GameInfoFlags),
	Flags("GAMEINFOFLAG2", GameInfoFlags2),
	Flags("EXPLAYERFLAG", ExPlayerFlags),
	Flags("PROJECTILEFLAG", ProjectileFlags),
    Flags("LASERFLAG", LaserFlags),
]

Objects = [

	NetObject("PlayerInput", [
		NetIntAny("m_Direction"),
		NetIntAny("m_TargetX"),
		NetIntAny("m_TargetY"),

		NetIntAny("m_Jump"),
		NetIntAny("m_Fire"),
		NetIntAny("m_Hook"),

		NetIntRange("m_PlayerFlags", 0, 256),

		NetIntAny("m_WantedWeapon"),
		NetIntAny("m_NextWeapon"),
		NetIntAny("m_PrevWeapon"),
	]),

	NetObject("Projectile", [
		NetIntAny("m_X"),
		NetIntAny("m_Y"),
		NetIntAny("m_VelX"),
		NetIntAny("m_VelY"),

		NetIntRange("m_Type", 0, 'NUM_WEAPONS-1'),
		NetTick("m_StartTick"),
	]),

	NetObject("Laser", [
		NetIntAny("m_X"),
		NetIntAny("m_Y"),
		NetIntAny("m_FromX"),
		NetIntAny("m_FromY"),

		NetTick("m_StartTick"),
	]),

	NetObject("Pickup", [
		NetIntAny("m_X"),
		NetIntAny("m_Y"),

		NetIntRange("m_Type", 0, 'max_int'),
		NetIntRange("m_Subtype", 0, 'max_int'),
	]),

	NetObject("Flag", [
		NetIntAny("m_X"),
		NetIntAny("m_Y"),

		NetIntRange("m_Team", 'TEAM_RED', 'TEAM_BLUE')
	]),

	NetObject("GameInfo", [
		NetIntRange("m_GameFlags", 0, 256),
		NetIntRange("m_GameStateFlags", 0, 256),
		NetTick("m_RoundStartTick"),
		NetIntRange("m_WarmupTimer", 'min_int', 'max_int'),

		NetIntRange("m_ScoreLimit", 0, 'max_int'),
		NetIntRange("m_TimeLimit", 0, 'max_int'),

		NetIntRange("m_RoundNum", 0, 'max_int'),
		NetIntRange("m_RoundCurrent", 0, 'max_int'),
	]),

	NetObject("GameData", [
		NetIntAny("m_TeamscoreRed"),
		NetIntAny("m_TeamscoreBlue"),

		NetIntRange("m_FlagCarrierRed", 'FLAG_MISSING', 'MAX_CLIENTS-1'),
		NetIntRange("m_FlagCarrierBlue", 'FLAG_MISSING', 'MAX_CLIENTS-1'),
	]),

	NetObject("CharacterCore", [
		NetIntAny("m_Tick"),
		NetIntAny("m_X"),
		NetIntAny("m_Y"),
		NetIntAny("m_VelX"),
		NetIntAny("m_VelY"),

		NetIntAny("m_Angle"),
		NetIntRange("m_Direction", -1, 1),

		NetIntRange("m_Jumped", 0, 3),
		NetIntRange("m_HookedPlayer", 0, 'MAX_CLIENTS-1'),
		NetIntRange("m_HookState", -1, 5),
		NetTick("m_HookTick"),

		NetIntAny("m_HookX"),
		NetIntAny("m_HookY"),
		NetIntAny("m_HookDx"),
		NetIntAny("m_HookDy"),
	]),

	NetObject("Character:CharacterCore", [
		NetIntRange("m_PlayerFlags", 0, 256),
		NetIntRange("m_Health", 0, 10),
		NetIntRange("m_Armor", 0, 10),
		NetIntRange("m_AmmoCount", 0, 10),
		NetIntRange("m_Weapon", 0, 'NUM_WEAPONS-1'),
		NetIntRange("m_Emote", 0, len(Emotes)),
		NetIntRange("m_AttackTick", 0, 'max_int'),
	]),

	NetObject("PlayerInfo", [
		NetIntRange("m_Local", 0, 1),
		NetIntRange("m_ClientID", 0, 'MAX_CLIENTS-1'),
		NetIntRange("m_Team", 'TEAM_SPECTATORS', 'TEAM_BLUE'),

		NetIntAny("m_Score"),
		NetIntAny("m_Latency"),
	]),

	NetObject("ClientInfo", [
		# 4*4 = 16 characters
		NetIntAny("m_Name0"), NetIntAny("m_Name1"), NetIntAny("m_Name2"),
		NetIntAny("m_Name3"),

		# 4*3 = 12 characters
		NetIntAny("m_Clan0"), NetIntAny("m_Clan1"), NetIntAny("m_Clan2"),

		NetIntAny("m_Country"),

		# 4*6 = 24 characters
		NetIntAny("m_Skin0"), NetIntAny("m_Skin1"), NetIntAny("m_Skin2"),
		NetIntAny("m_Skin3"), NetIntAny("m_Skin4"), NetIntAny("m_Skin5"),

		NetIntRange("m_UseCustomColor", 0, 1),

		NetIntAny("m_ColorBody"),
		NetIntAny("m_ColorFeet"),
	]),

	NetObject("SpectatorInfo", [
		NetIntRange("m_SpectatorID", 'SPEC_FREEVIEW', 'MAX_CLIENTS-1'),
		NetIntAny("m_X"),
		NetIntAny("m_Y"),
	]),

	NetObjectEx("MyOwnObject", "my-own-object@heinrich5991.de", [
		NetIntAny("m_Test"),
	]),

	NetObjectEx("DDNetCharacter", "character@netobj.ddnet.tw", [
		NetIntAny("m_Flags"),
		NetTick("m_FreezeEnd"),
		NetIntRange("m_Jumps", 0, 255),
		NetIntAny("m_TeleCheckpoint"),
		NetIntRange("m_StrongWeakID", 0, 'MAX_CLIENTS-1'),

		# New data fields for jump display, freeze bar and ninja bar
		# Default values indicate that these values should not be used
		NetIntRange("m_JumpedTotal", -1, 255),
		NetIntRange("m_NinjaActivationTick", -1, "max_int"),
		NetIntRange("m_FreezeStart", -1, "max_int"),
		# New data fields for improved target accuracy
		NetIntAny("m_TargetX"),
		NetIntAny("m_TargetY"),
        NetIntRange("m_TuneZoneOverride", -1, 255),
	]),

	NetObjectEx("DDNetPlayer", "player@netobj.ddnet.tw", [
		NetIntAny("m_Flags"),
		NetIntRange("m_AuthLevel", "AUTHED_NO", "AUTHED_ADMIN"),
	]),

	NetObjectEx("GameInfoEx", "gameinfo@netobj.ddnet.tw", [
		NetIntAny("m_Flags"),
		NetIntAny("m_Version"),
		NetIntAny("m_Flags2"),
	], validate_size=False),

	# The code assumes that this has the same in-memory representation as
	# the Projectile net object.
    #NetObjectEx("DDRaceProjectile", "projectile@netobj.ddnet.tw", [
	#	NetIntAny("m_X"),
	#	NetIntAny("m_Y"),
	#	NetIntAny("m_Angle"),
	#	NetIntAny("m_Data"),
	#	NetIntRange("m_Type", 0, 'NUM_WEAPONS-1'),
	#	NetTick("m_StartTick"),
	#]),

	NetObjectEx("DDNetLaser", "laser@netobj.ddnet.tw", [
		NetIntAny("m_ToX"),
		NetIntAny("m_ToY"),
		NetIntAny("m_FromX"),
		NetIntAny("m_FromY"),
		NetTick("m_StartTick"),
		NetIntRange("m_Owner", -1, 'MAX_CLIENTS-1'),
		NetIntAny("m_Type"),
		NetIntAny("m_SwitchNumber"),
		NetIntAny("m_Subtype"),
		NetIntAny("m_Flags"),
	]),

	NetObjectEx("DDNetProjectile", "projectile@netobj.ddnet.tw", [
		NetIntAny("m_X"),
		NetIntAny("m_Y"),
		NetIntAny("m_Angle"),
		NetIntAny("m_Data"),
		NetIntRange("m_Type", 0, 'NUM_WEAPONS-1'),
		NetTick("m_StartTick"),
	]),
    
	NetObjectEx("DDNetPickup", "pickup@netobj.ddnet.tw", [
		NetIntAny("m_X"),
		NetIntAny("m_Y"),
		NetIntRange("m_Type", 0, 'max_int'),
		NetIntRange("m_Subtype", 0, 'max_int'),
		NetIntAny("m_SwitchNumber"),
	]),

	NetObjectEx("DDNetSpectatorInfo", "spectator-info@netobj.ddnet.org", [
		NetBool("m_HasCameraInfo"),
		NetIntRange("m_Zoom", 0, 'max_int'),
		NetIntRange("m_Deadzone", 0, 'max_int'),
		NetIntRange("m_FollowFactor", 0, 'max_int'),
		NetIntRange("m_SpectatorCount", 0, 'MAX_CLIENTS-1'),
	]),

	## Events

	NetEvent("Common", [
		NetIntAny("m_X"),
		NetIntAny("m_Y"),
	]),


	NetEvent("Explosion:Common", []),
	NetEvent("Spawn:Common", []),
	NetEvent("HammerHit:Common", []),

	NetEvent("Death:Common", [
		NetIntRange("m_ClientID", 0, 'MAX_CLIENTS-1'),
	]),

	NetEvent("SoundGlobal:Common", [ #TODO 0.7: remove me
		NetIntRange("m_SoundID", 0, 'NUM_SOUNDS-1'),
	]),

	NetEvent("SoundWorld:Common", [
		NetIntRange("m_SoundID", 0, 'NUM_SOUNDS-1'),
	]),

	NetEvent("DamageInd:Common", [
		NetIntAny("m_Angle"),
	]),
	
	NetObjectEx("MyOwnEvent", "my-own-event@heinrich5991.de", [
		NetIntAny("m_Test"),
	]),

	NetObjectEx("SpecChar", "spec-char@netobj.ddnet.tw", [
		NetIntAny("m_X"),
		NetIntAny("m_Y"),
	]),
]

Messages = [

	### Server messages
	NetMessage("Sv_Motd", [
		NetString("m_pMessage"),
	]),

	NetMessage("Sv_Broadcast", [
		NetString("m_pMessage"),
	]),

	NetMessage("Sv_Chat", [
		NetIntRange("m_Team", -2, 3),
		NetIntRange("m_ClientID", -1, 'MAX_CLIENTS-1'),
		NetStringHalfStrict("m_pMessage"),
	]),

	NetMessage("Sv_KillMsg", [
		NetIntRange("m_Killer", 0, 'MAX_CLIENTS-1'),
		NetIntRange("m_Victim", 0, 'MAX_CLIENTS-1'),
		NetIntRange("m_Weapon", -3, 'NUM_WEAPONS-1'),
		NetIntAny("m_ModeSpecial"),
	]),

	NetMessage("Sv_SoundGlobal", [
		NetIntRange("m_SoundID", 0, 'NUM_SOUNDS-1'),
	]),

	NetMessage("Sv_TuneParams", []),
	NetMessage("Sv_ExtraProjectile", []),
	NetMessage("Sv_ReadyToEnter", []),

	NetMessage("Sv_WeaponPickup", [
		NetIntRange("m_Weapon", 0, 'NUM_WEAPONS-1'),
	]),

	NetMessage("Sv_Emoticon", [
		NetIntRange("m_ClientID", 0, 'MAX_CLIENTS-1'),
		NetIntRange("m_Emoticon", 0, 'NUM_EMOTICONS-1'),
	]),

	NetMessage("Sv_VoteClearOptions", [
	]),

	NetMessage("Sv_VoteOptionListAdd", [
		NetIntRange("m_NumOptions", 1, 15),
		NetStringStrict("m_pDescription0"), NetStringStrict("m_pDescription1"),	NetStringStrict("m_pDescription2"),
		NetStringStrict("m_pDescription3"),	NetStringStrict("m_pDescription4"),	NetStringStrict("m_pDescription5"),
		NetStringStrict("m_pDescription6"), NetStringStrict("m_pDescription7"), NetStringStrict("m_pDescription8"),
		NetStringStrict("m_pDescription9"), NetStringStrict("m_pDescription10"), NetStringStrict("m_pDescription11"),
		NetStringStrict("m_pDescription12"), NetStringStrict("m_pDescription13"), NetStringStrict("m_pDescription14"),
	]),

	NetMessage("Sv_VoteOptionAdd", [
		NetStringStrict("m_pDescription"),
	]),

	NetMessage("Sv_VoteOptionRemove", [
		NetStringStrict("m_pDescription"),
	]),

	NetMessage("Sv_VoteSet", [
		NetIntRange("m_Timeout", 0, 60),
		NetStringStrict("m_pDescription"),
		NetStringStrict("m_pReason"),
	]),

	NetMessage("Sv_VoteStatus", [
		NetIntRange("m_Yes", 0, 'MAX_CLIENTS'),
		NetIntRange("m_No", 0, 'MAX_CLIENTS'),
		NetIntRange("m_Pass", 0, 'MAX_CLIENTS'),
		NetIntRange("m_Total", 0, 'MAX_CLIENTS'),
	]),

	### Client messages
	NetMessage("Cl_Say", [
		NetBool("m_Team"),
		NetStringHalfStrict("m_pMessage"),
	]),

	NetMessage("Cl_SetTeam", [
		NetIntRange("m_Team", 'TEAM_SPECTATORS', 'TEAM_BLUE'),
	]),

	NetMessage("Cl_SetSpectatorMode", [
		NetIntRange("m_SpectatorID", 'SPEC_FREEVIEW', 'MAX_CLIENTS-1'),
	]),

	NetMessage("Cl_StartInfo", [
		NetStringStrict("m_pName"),
		NetStringStrict("m_pClan"),
		NetIntAny("m_Country"),
		NetStringStrict("m_pSkin"),
		NetBool("m_UseCustomColor"),
		NetIntAny("m_ColorBody"),
		NetIntAny("m_ColorFeet"),
	]),

	NetMessage("Cl_ChangeInfo", [
		NetStringStrict("m_pName"),
		NetStringStrict("m_pClan"),
		NetIntAny("m_Country"),
		NetStringStrict("m_pSkin"),
		NetBool("m_UseCustomColor"),
		NetIntAny("m_ColorBody"),
		NetIntAny("m_ColorFeet"),
	]),

	NetMessage("Cl_Kill", []),

	NetMessage("Cl_Emoticon", [
		NetIntRange("m_Emoticon", 0, 'NUM_EMOTICONS-1'),
	]),

	NetMessage("Cl_Vote", [
		NetIntRange("m_Vote", -1, 1),
	]),

	NetMessage("Cl_CallVote", [
		NetStringStrict("m_Type"),
		NetStringStrict("m_Value"),
		NetStringStrict("m_Reason"),
	]),

	NetMessage("Cl_IsDDNetLegacy", []),

	NetMessage("Sv_DDRaceTimeLegacy", [
		NetIntAny("m_Time"),
		NetIntAny("m_Check"),
		NetIntRange("m_Finish", 0, 1),
	]),

	NetMessage("Sv_RecordLegacy", [
		NetIntAny("m_ServerTimeBest"),
		NetIntAny("m_PlayerTimeBest"),
	]),

	NetMessage("Unused", []),

	NetMessage("Sv_TeamsStateLegacy", []),

	# deprecated, use showothers@netmsg.ddnet.tw instead
	NetMessage("Cl_ShowOthersLegacy", [
		NetBool("m_Show"),
	]),
# Can't add any NetMessages here!

	NetMessageEx("Sv_MyOwnMessage", "my-own-message@heinrich5991.de", [
		NetIntAny("m_Test"),
	]),

	NetMessageEx("Cl_ShowDistance", "show-distance@netmsg.ddnet.tw", [
		NetIntAny("m_X"),
		NetIntAny("m_Y"),
	]),

	NetMessageEx("Cl_ShowOthers", "showothers@netmsg.ddnet.tw", [
		NetIntRange("m_Show", 0, 2),
	]),

	NetMessageEx("Sv_TeamsState", "teamsstate@netmsg.ddnet.tw", []),

	NetMessageEx("Sv_DDRaceTime", "ddrace-time@netmsg.ddnet.tw", [
		NetIntAny("m_Time"),
		NetIntAny("m_Check"),
		NetIntRange("m_Finish", 0, 1),
	]),

	NetMessageEx("Sv_Record", "weird-record@netmsg.ddnet.tw", [
		NetIntAny("m_ServerTimeBest"),
		NetIntAny("m_PlayerTimeBest"),
	]),
]
