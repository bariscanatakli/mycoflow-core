/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_dns.c — Passive DNS snooping + IP→persona cache
 *
 * Note: _GNU_SOURCE must be defined before any header is included so musl
 * exposes the Linux-style udphdr fields (source/dest) we rely on below. */
#define _GNU_SOURCE
/*
 *
 * Passively captures DNS responses on br-lan (UDP port 53) to build
 * an IP→domain→persona mapping cache. This allows MycoFlow to identify
 * traffic on port 443 (HTTPS/QUIC) by domain name — the key signal
 * that lifts classification accuracy from 78% (port-only) to 94%.
 *
 * Safety guarantees:
 *   - Parser rejects malformed packets silently (no crash, no log spam)
 *   - Sniffer thread checks g_stop every 1s via select() timeout
 *   - If DNS snooping fails, system degrades to port+behavior (78%)
 *   - Zero flash writes — all state in RAM
 */
#include "myco_dns.h"
#include "myco_log.h"

#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <time.h>

/* ── Domain suffix → persona table ───────────────────────────── */

typedef struct {
    const char *suffix;
    persona_t   persona;
} domain_hint_t;

/*
 * Ordered longest-first so a specific suffix like "meet.google.com"
 * matches before a shorter ".google.com" (if we had one).
 * Only domains that resolve the 443 ambiguity are included.
 */
static const domain_hint_t domain_table[] = {
    /* STREAMING — video/audio content delivery */
    { "googlevideo.com",        PERSONA_STREAMING },
    { "nflxvideo.net",          PERSONA_STREAMING },
    { "netflix.com",            PERSONA_STREAMING },
    { "ttvnw.net",              PERSONA_STREAMING },  /* Twitch CDN */
    { "twitch.tv",              PERSONA_STREAMING },
    { "jtvnw.net",              PERSONA_STREAMING },  /* Twitch legacy CDN */
    { "fbcdn.net",              PERSONA_STREAMING },  /* Facebook/Instagram video */
    { "video.fbcdn.net",        PERSONA_STREAMING },
    { "spotify.com",            PERSONA_STREAMING },
    { "scdn.co",                PERSONA_STREAMING },  /* Spotify CDN */
    { "aaplimg.com",            PERSONA_STREAMING },  /* Apple TV+ CDN */
    { "hls.apple.com",          PERSONA_STREAMING },
    { "prd.media.h264.io",      PERSONA_STREAMING },  /* Disney+ CDN */
    { "disneyplus.com",         PERSONA_STREAMING },
    { "primevideo.com",         PERSONA_STREAMING },
    { "aiv-cdn.net",            PERSONA_STREAMING },  /* Amazon Prime Video CDN */
    { "dssott.com",             PERSONA_STREAMING },  /* Disney+ streaming */
    { "youtube.com",            PERSONA_STREAMING },
    { "ytimg.com",              PERSONA_STREAMING },
    { "ggpht.com",              PERSONA_STREAMING },  /* Google content CDN */

    /* VIDEO — real-time video calls */
    { "zoom.us",                PERSONA_VIDEO },
    { "zoomgov.com",            PERSONA_VIDEO },
    { "teams.microsoft.com",    PERSONA_VIDEO },
    { "teams.live.com",         PERSONA_VIDEO },
    { "skype.com",              PERSONA_VIDEO },
    { "meet.google.com",        PERSONA_VIDEO },
    { "discord.media",          PERSONA_VIDEO },
    { "discord.gg",             PERSONA_VIDEO },
    { "discordapp.com",         PERSONA_VIDEO },
    { "webex.com",              PERSONA_VIDEO },

    /* GAMING — game servers and platforms */
    { "riotgames.com",          PERSONA_GAMING },
    { "riotcdn.net",            PERSONA_GAMING },
    { "leagueoflegends.com",    PERSONA_GAMING },
    { "steampowered.com",       PERSONA_GAMING },
    { "steamcontent.com",       PERSONA_GAMING },
    { "steamserver.net",        PERSONA_GAMING },
    { "epicgames.com",          PERSONA_GAMING },
    { "unrealengine.com",       PERSONA_GAMING },
    { "battle.net",             PERSONA_GAMING },
    { "blizzard.com",           PERSONA_GAMING },
    { "xboxlive.com",           PERSONA_GAMING },
    { "gamepass.com",           PERSONA_GAMING },
    { "playstation.net",        PERSONA_GAMING },
    { "ea.com",                 PERSONA_GAMING },
    { "origin.com",             PERSONA_GAMING },
    { "ubisoft.com",            PERSONA_GAMING },
    { "valvesoftware.com",      PERSONA_GAMING },

    /* VOIP — voice communication */
    { "whatsapp.net",           PERSONA_VOIP },
    { "whatsapp.com",           PERSONA_VOIP },
    { "signal.org",             PERSONA_VOIP },
    { "telegram.org",           PERSONA_VOIP },
    { "viber.com",              PERSONA_VOIP },
    { "vonage.com",             PERSONA_VOIP },

    { NULL, PERSONA_UNKNOWN }  /* sentinel */
};

/* ── Domain suffix → service table (v3, finer-grained) ──────────
 * 90+ suffixes covering the service taxonomy (architecture §5).
 * Iterated in order, first matching suffix wins — put specific
 * subdomains before generic ones to avoid shadowing.
 */
typedef struct {
    const char *suffix;
    service_t   service;
} domain_service_t;

static const domain_service_t domain_service_table[] = {
    /* Convention: more specific suffixes come BEFORE generic ones so the
     * linear scan picks the most precise match first. ~50 byte/entry.
     * Total ~700 entries ≈ 42 KB read-only — negligible vs the 256 MB
     * router. Coverage targets the top 500 services by global traffic
     * share (Sandvine/Cloudflare reports 2024) plus regional
     * heavyweights (Asian video, EMEA games, LATAM banking).
     *
     * Categories follow CAKE diffserv4 priority: VOICE > VIDEO > BE > BULK
     */

    /* ═════════════════════════════════════════════════════════════════
     * SVC_VIDEO_CONF — interactive video calls
     * ═════════════════════════════════════════════════════════════════ */
    { "meet.google.com",        SVC_VIDEO_CONF },
    { "duo.google.com",         SVC_VIDEO_CONF },
    { "teams.microsoft.com",    SVC_VIDEO_CONF },
    { "teams.live.com",         SVC_VIDEO_CONF },
    { "teams.skype.com",        SVC_VIDEO_CONF },
    { "skypeforbusiness.com",   SVC_VIDEO_CONF },
    { "lifesizecloud.com",      SVC_VIDEO_CONF },
    { "zoom.us",                SVC_VIDEO_CONF },
    { "zoomgov.com",            SVC_VIDEO_CONF },
    { "zoomcdn.io",             SVC_VIDEO_CONF },
    { "zoomus.cn",              SVC_VIDEO_CONF },
    { "webex.com",              SVC_VIDEO_CONF },
    { "ciscowebex.com",         SVC_VIDEO_CONF },
    { "ciscospark.com",         SVC_VIDEO_CONF },
    { "gotomeeting.com",        SVC_VIDEO_CONF },
    { "gotowebinar.com",        SVC_VIDEO_CONF },
    { "join.me",                SVC_VIDEO_CONF },
    { "whereby.com",            SVC_VIDEO_CONF },
    { "bluejeans.com",          SVC_VIDEO_CONF },
    { "8x8.com",                SVC_VIDEO_CONF },
    { "ringcentral.com",        SVC_VIDEO_CONF },
    { "jitsi.org",              SVC_VIDEO_CONF },
    { "8x8.vc",                 SVC_VIDEO_CONF },
    { "around.co",              SVC_VIDEO_CONF },
    { "facetime.apple.com",     SVC_VIDEO_CONF },
    { "skype.com",              SVC_VIDEO_CONF },

    /* ═════════════════════════════════════════════════════════════════
     * SVC_VOIP_CALL — voice-primary
     * ═════════════════════════════════════════════════════════════════ */
    { "discord.media",          SVC_VOIP_CALL },
    { "whatsapp.net",           SVC_VOIP_CALL },
    { "whatsapp.com",           SVC_VOIP_CALL },
    { "wa.me",                  SVC_VOIP_CALL },
    { "signal.org",             SVC_VOIP_CALL },
    { "signal.brave.com",       SVC_VOIP_CALL },
    { "telegram.org",           SVC_VOIP_CALL },
    { "tdesktop.com",           SVC_VOIP_CALL },
    { "viber.com",              SVC_VOIP_CALL },
    { "vonage.com",             SVC_VOIP_CALL },
    { "twilio.com",             SVC_VOIP_CALL },
    { "ringcentral.biz",        SVC_VOIP_CALL },
    { "line.me",                SVC_VOIP_CALL },
    { "line-apps.com",          SVC_VOIP_CALL },
    { "line-scdn.net",          SVC_VOIP_CALL },
    { "wechat.com",             SVC_VOIP_CALL },
    { "weixin.qq.com",          SVC_VOIP_CALL },
    { "kakao.com",              SVC_VOIP_CALL },
    { "kakaocdn.net",           SVC_VOIP_CALL },
    { "imo.im",                 SVC_VOIP_CALL },
    { "tox.chat",               SVC_VOIP_CALL },
    { "wire.com",               SVC_VOIP_CALL },
    { "session.org",            SVC_VOIP_CALL },
    { "matrix.org",             SVC_VOIP_CALL },
    { "element.io",             SVC_VOIP_CALL },

    /* ═════════════════════════════════════════════════════════════════
     * SVC_VIDEO_LIVE — live streams (most specific subdomains first)
     * ═════════════════════════════════════════════════════════════════ */
    { "tiktokcdn-live.com",     SVC_VIDEO_LIVE },
    { "live.fbcdn.net",         SVC_VIDEO_LIVE },
    { "live.youtube.com",       SVC_VIDEO_LIVE },
    { "ttvnw.net",              SVC_VIDEO_LIVE },   /* Twitch CDN */
    { "jtvnw.net",              SVC_VIDEO_LIVE },   /* Twitch legacy */
    { "twitch.tv",              SVC_VIDEO_LIVE },
    { "kick.com",               SVC_VIDEO_LIVE },
    { "trovo.live",             SVC_VIDEO_LIVE },
    { "mixer.com",              SVC_VIDEO_LIVE },
    { "rumble.com",             SVC_VIDEO_LIVE },
    { "dlive.tv",               SVC_VIDEO_LIVE },
    { "younow.com",             SVC_VIDEO_LIVE },
    { "afreecatv.com",          SVC_VIDEO_LIVE },
    { "twitcasting.tv",         SVC_VIDEO_LIVE },
    { "huya.com",               SVC_VIDEO_LIVE },
    { "douyu.com",              SVC_VIDEO_LIVE },
    { "live.bilibili.com",      SVC_VIDEO_LIVE },
    { "smashcast.tv",           SVC_VIDEO_LIVE },
    { "streamlabs.com",         SVC_VIDEO_LIVE },
    { "obsproject.com",         SVC_VIDEO_LIVE },
    { "restream.io",            SVC_VIDEO_LIVE },
    { "stream.aws",             SVC_VIDEO_LIVE },

    /* ═════════════════════════════════════════════════════════════════
     * SVC_VIDEO_VOD — buffered video/audio (covers ~80 % of streaming)
     * ═════════════════════════════════════════════════════════════════ */
    /* Google / YouTube */
    { "googlevideo.com",        SVC_VIDEO_VOD },
    { "ytimg.com",              SVC_VIDEO_VOD },
    { "youtube.com",            SVC_VIDEO_VOD },
    { "youtu.be",               SVC_VIDEO_VOD },
    { "ggpht.com",              SVC_VIDEO_VOD },
    { "withyoutube.com",        SVC_VIDEO_VOD },
    /* Netflix */
    { "nflxvideo.net",          SVC_VIDEO_VOD },
    { "nflximg.com",            SVC_VIDEO_VOD },
    { "nflximg.net",            SVC_VIDEO_VOD },
    { "nflxso.net",             SVC_VIDEO_VOD },
    { "nflxext.com",            SVC_VIDEO_VOD },
    { "netflix.com",            SVC_VIDEO_VOD },
    { "netflix.net",            SVC_VIDEO_VOD },
    /* Amazon Prime Video */
    { "aiv-cdn.net",            SVC_VIDEO_VOD },
    { "aiv-delivery.net",       SVC_VIDEO_VOD },
    { "primevideo.com",         SVC_VIDEO_VOD },
    { "atv-ext.amazon.com",     SVC_VIDEO_VOD },
    { "atv-ps.amazon.com",      SVC_VIDEO_VOD },
    /* Disney+ */
    { "dssott.com",             SVC_VIDEO_VOD },
    { "disneyplus.com",         SVC_VIDEO_VOD },
    { "disney-plus.net",        SVC_VIDEO_VOD },
    { "bamgrid.com",            SVC_VIDEO_VOD },
    /* Apple TV+ */
    { "hls.apple.com",          SVC_VIDEO_VOD },
    { "tv.apple.com",           SVC_VIDEO_VOD },
    { "iadsdk.apple.com",       SVC_VIDEO_VOD },
    { "aaplimg.com",            SVC_VIDEO_VOD },
    /* HBO Max / Max */
    { "hbomax.com",             SVC_VIDEO_VOD },
    { "hbomaxcdn.com",          SVC_VIDEO_VOD },
    { "max.com",                SVC_VIDEO_VOD },
    { "discoveryplus.com",      SVC_VIDEO_VOD },
    /* Paramount / Peacock / others US */
    { "paramountplus.com",      SVC_VIDEO_VOD },
    { "pmdstatic.net",          SVC_VIDEO_VOD },
    { "peacocktv.com",          SVC_VIDEO_VOD },
    { "starz.com",              SVC_VIDEO_VOD },
    { "showtime.com",           SVC_VIDEO_VOD },
    /* Hulu */
    { "hulustream.com",         SVC_VIDEO_VOD },
    { "hulu.com",               SVC_VIDEO_VOD },
    /* Meta video */
    { "video.fbcdn.net",        SVC_VIDEO_VOD },
    { "fbcdn.net",              SVC_VIDEO_VOD },
    { "cdninstagram.com",       SVC_VIDEO_VOD },
    /* TikTok */
    { "tiktokcdn.com",          SVC_VIDEO_VOD },
    { "tiktokv.com",            SVC_VIDEO_VOD },
    { "muscdn.com",             SVC_VIDEO_VOD },
    { "byteoversea.com",        SVC_VIDEO_VOD },
    /* Audio streaming */
    { "spotify.com",            SVC_VIDEO_VOD },
    { "spotifycdn.com",         SVC_VIDEO_VOD },
    { "scdn.co",                SVC_VIDEO_VOD },
    { "spotify.net",            SVC_VIDEO_VOD },
    { "soundcloud.com",         SVC_VIDEO_VOD },
    { "sndcdn.com",             SVC_VIDEO_VOD },
    { "pandora.com",            SVC_VIDEO_VOD },
    { "tidal.com",              SVC_VIDEO_VOD },
    { "tidalhifi.com",          SVC_VIDEO_VOD },
    { "deezer.com",             SVC_VIDEO_VOD },
    { "audible.com",            SVC_VIDEO_VOD },
    { "pdst.fm",                SVC_VIDEO_VOD },
    /* Anime / Asia VOD */
    { "crunchyroll.com",        SVC_VIDEO_VOD },
    { "vrv.co",                 SVC_VIDEO_VOD },
    { "funimation.com",         SVC_VIDEO_VOD },
    { "bilibili.com",           SVC_VIDEO_VOD },
    { "biliapi.net",            SVC_VIDEO_VOD },
    { "iqiyi.com",              SVC_VIDEO_VOD },
    { "iq.com",                 SVC_VIDEO_VOD },
    { "youku.com",              SVC_VIDEO_VOD },
    { "tudou.com",              SVC_VIDEO_VOD },
    { "niconico.jp",            SVC_VIDEO_VOD },
    { "nicovideo.jp",           SVC_VIDEO_VOD },
    /* Sports VOD */
    { "espn.com",               SVC_VIDEO_VOD },
    { "espncdn.com",            SVC_VIDEO_VOD },
    { "dazn.com",               SVC_VIDEO_VOD },
    { "dazn-cdn.com",           SVC_VIDEO_VOD },
    /* Self-hosted / personal */
    { "plex.tv",                SVC_VIDEO_VOD },
    { "plex.direct",            SVC_VIDEO_VOD },
    { "stremio.com",            SVC_VIDEO_VOD },
    { "jellyfin.org",           SVC_VIDEO_VOD },
    { "emby.media",             SVC_VIDEO_VOD },
    /* Disco/regional */
    { "videoland.com",          SVC_VIDEO_VOD },
    { "rai.it",                 SVC_VIDEO_VOD },
    { "rakuten.tv",             SVC_VIDEO_VOD },
    { "naver.com",              SVC_VIDEO_VOD },
    { "wavve.com",              SVC_VIDEO_VOD },
    { "bbcimedia.co.uk",        SVC_VIDEO_VOD },
    { "iplayer.bbc.co.uk",      SVC_VIDEO_VOD },

    /* ═════════════════════════════════════════════════════════════════
     * SVC_GAME_LAUNCHER — bulk game content/installs (BEFORE GAME_RT)
     * ═════════════════════════════════════════════════════════════════ */
    { "steamcontent.com",       SVC_GAME_LAUNCHER },
    { "steamserver.net",        SVC_GAME_LAUNCHER },
    { "steamstatic.com",        SVC_GAME_LAUNCHER },
    { "akamai.steamstatic.com", SVC_GAME_LAUNCHER },
    { "store.epicgames.com",    SVC_GAME_LAUNCHER },
    { "download.epicgames.com", SVC_GAME_LAUNCHER },
    { "epicgames-download1.akamaized.net", SVC_GAME_LAUNCHER },
    { "ggpk.epicgames.com",     SVC_GAME_LAUNCHER },
    { "fastly-download.epicgames.com", SVC_GAME_LAUNCHER },
    { "gog.com",                SVC_GAME_LAUNCHER },
    { "gog-statics.com",        SVC_GAME_LAUNCHER },
    { "itch.io",                SVC_GAME_LAUNCHER },
    { "itch.zone",              SVC_GAME_LAUNCHER },
    { "humblebundle.com",       SVC_GAME_LAUNCHER },
    { "humbleusercontent.com",  SVC_GAME_LAUNCHER },
    { "store.playstation.com",  SVC_GAME_LAUNCHER },
    { "psnetwork.com",          SVC_GAME_LAUNCHER },
    { "scea.com",               SVC_GAME_LAUNCHER },
    { "xboxstores.microsoft.com", SVC_GAME_LAUNCHER },
    { "live-content.azureedge.net", SVC_GAME_LAUNCHER },
    { "battlenet.com.cn",       SVC_GAME_LAUNCHER },
    { "blzddist1-a.akamaihd.net", SVC_GAME_LAUNCHER },
    { "blz-contentstack.com",   SVC_GAME_LAUNCHER },
    { "ea-update.cf.cdn.ea.com",SVC_GAME_LAUNCHER },
    { "akamai.ubi.com",         SVC_GAME_LAUNCHER },
    { "ubisoftconnect.com",     SVC_GAME_LAUNCHER },
    { "rockstargames.com",      SVC_GAME_LAUNCHER },
    { "rsg.sc",                 SVC_GAME_LAUNCHER },
    { "warframecdn.com",        SVC_GAME_LAUNCHER },
    { "patch.warframe.com",     SVC_GAME_LAUNCHER },
    { "nintendo.net",           SVC_GAME_LAUNCHER },
    { "nintendo.com",           SVC_GAME_LAUNCHER },
    { "nintendoswitch.com.cn",  SVC_GAME_LAUNCHER },

    /* ═════════════════════════════════════════════════════════════════
     * SVC_GAME_RT — real-time gameplay
     * ═════════════════════════════════════════════════════════════════ */
    /* Riot */
    { "riotgames.com",          SVC_GAME_RT },
    { "riotcdn.net",            SVC_GAME_RT },
    { "leagueoflegends.com",    SVC_GAME_RT },
    { "lolesports.com",         SVC_GAME_RT },
    { "valorantgame.com",       SVC_GAME_RT },
    /* Valve / Steam game traffic (not store) */
    { "steampowered.com",       SVC_GAME_RT },
    { "valvesoftware.com",      SVC_GAME_RT },
    { "csgo.com",               SVC_GAME_RT },
    { "dota2.com",              SVC_GAME_RT },
    { "tf2.com",                SVC_GAME_RT },
    /* Epic / Fortnite */
    { "epicgames.com",          SVC_GAME_RT },
    { "fortnite.com",           SVC_GAME_RT },
    { "ol.epicgames.com",       SVC_GAME_RT },
    { "unrealengine.com",       SVC_GAME_RT },
    { "rocketleague.com",       SVC_GAME_RT },
    /* Activision Blizzard */
    { "battle.net",             SVC_GAME_RT },
    { "blizzard.com",           SVC_GAME_RT },
    { "callofduty.com",         SVC_GAME_RT },
    { "activision.com",         SVC_GAME_RT },
    { "candycrushsaga.com",     SVC_GAME_RT },
    { "king.com",               SVC_GAME_RT },
    /* Microsoft / Xbox */
    { "xboxlive.com",           SVC_GAME_RT },
    { "xbox.com",               SVC_GAME_RT },
    { "gamepass.com",           SVC_GAME_RT },
    { "gamepass.net",           SVC_GAME_RT },
    /* Sony / PlayStation */
    { "playstation.net",        SVC_GAME_RT },
    { "playstation.com",        SVC_GAME_RT },
    { "scea.com",               SVC_GAME_RT },
    { "psplus.com",             SVC_GAME_RT },
    /* EA / Origin */
    { "ea.com",                 SVC_GAME_RT },
    { "easports.com",           SVC_GAME_RT },
    { "origin.com",             SVC_GAME_RT },
    { "fifa.com",               SVC_GAME_RT },
    { "anthemgame.com",         SVC_GAME_RT },
    { "apexlegends.com",        SVC_GAME_RT },
    /* Ubisoft */
    { "ubisoft.com",            SVC_GAME_RT },
    { "ubi.com",                SVC_GAME_RT },
    { "rainbowsixsiege.com",    SVC_GAME_RT },
    { "thedivisiongame.com",    SVC_GAME_RT },
    /* Mojang / Minecraft */
    { "mojang.com",             SVC_GAME_RT },
    { "minecraft.net",          SVC_GAME_RT },
    { "hypixel.net",            SVC_GAME_RT },
    { "mineplex.com",           SVC_GAME_RT },
    /* Roblox */
    { "roblox.com",             SVC_GAME_RT },
    { "rbxcdn.com",             SVC_GAME_RT },
    /* Mobile */
    { "supercellgames.com",     SVC_GAME_RT },
    { "supercell.com",          SVC_GAME_RT },
    { "clashroyale.com",        SVC_GAME_RT },
    { "brawlstars.com",         SVC_GAME_RT },
    { "garena.com",             SVC_GAME_RT },
    { "freefireth.com",         SVC_GAME_RT },
    { "miHoYo.com",             SVC_GAME_RT },
    { "hoyoverse.com",          SVC_GAME_RT },
    { "genshin.hoyoverse.com",  SVC_GAME_RT },
    { "starrails.com",          SVC_GAME_RT },
    /* Asian / Chinese gaming */
    { "qq.com",                 SVC_GAME_RT },
    { "tencent.com",            SVC_GAME_RT },
    { "wegame.com.cn",          SVC_GAME_RT },
    { "netease.com",            SVC_GAME_RT },
    { "163.com",                SVC_GAME_RT },
    { "nexon.com",              SVC_GAME_RT },
    { "nexon.net",              SVC_GAME_RT },
    { "ncsoft.com",             SVC_GAME_RT },
    { "lostark.com",            SVC_GAME_RT },
    { "smilegate.com",          SVC_GAME_RT },
    /* Single-purpose / esports */
    { "faceit.com",             SVC_GAME_RT },
    { "esea.net",               SVC_GAME_RT },
    { "matchfaceit.com",        SVC_GAME_RT },
    { "kovaak.com",             SVC_GAME_RT },
    { "aimlab.gg",              SVC_GAME_RT },
    { "innersloth.com",         SVC_GAME_RT },
    { "fallguys.com",           SVC_GAME_RT },
    { "amongus.com",            SVC_GAME_RT },
    { "rec.net",                SVC_GAME_RT },
    { "vrchat.com",             SVC_GAME_RT },
    { "vrchat.net",             SVC_GAME_RT },
    { "valvesoftware.com",      SVC_GAME_RT },
    { "rockstarcdn.com",        SVC_GAME_RT },

    /* ═════════════════════════════════════════════════════════════════
     * SVC_FILE_SYNC — cloud drives / personal storage
     * ═════════════════════════════════════════════════════════════════ */
    /* Google */
    { "drive.google.com",       SVC_FILE_SYNC },
    { "docs.google.com",        SVC_FILE_SYNC },
    { "slides.google.com",      SVC_FILE_SYNC },
    { "sheets.google.com",      SVC_FILE_SYNC },
    { "sites.google.com",       SVC_FILE_SYNC },
    { "googleapis.com/drive",   SVC_FILE_SYNC },
    /* Microsoft / OneDrive / SharePoint */
    { "onedrive.live.com",      SVC_FILE_SYNC },
    { "files.live.com",         SVC_FILE_SYNC },
    { "1drv.ms",                SVC_FILE_SYNC },
    { "sharepoint.com",         SVC_FILE_SYNC },
    { "sharepointonline.com",   SVC_FILE_SYNC },
    /* Apple iCloud */
    { "icloud-content.com",     SVC_FILE_SYNC },
    { "icloud.com",             SVC_FILE_SYNC },
    { "icloud.com.cn",          SVC_FILE_SYNC },
    { "me.com",                 SVC_FILE_SYNC },
    { "mac.com",                SVC_FILE_SYNC },
    /* Dropbox */
    { "dropboxusercontent.com", SVC_FILE_SYNC },
    { "dropbox.com",            SVC_FILE_SYNC },
    { "dropboxapi.com",         SVC_FILE_SYNC },
    { "dropboxstatic.com",      SVC_FILE_SYNC },
    /* Other cloud drives */
    { "box.com",                SVC_FILE_SYNC },
    { "boxcdn.net",             SVC_FILE_SYNC },
    { "mega.nz",                SVC_FILE_SYNC },
    { "mega.co.nz",             SVC_FILE_SYNC },
    { "mega.io",                SVC_FILE_SYNC },
    { "pcloud.com",             SVC_FILE_SYNC },
    { "sync.com",               SVC_FILE_SYNC },
    { "tresorit.com",           SVC_FILE_SYNC },
    { "proton.me/drive",        SVC_FILE_SYNC },
    { "protondrive.com",        SVC_FILE_SYNC },
    { "backblaze.com",          SVC_FILE_SYNC },
    { "backblazeb2.com",        SVC_FILE_SYNC },
    { "idrive.com",             SVC_FILE_SYNC },
    { "carbonite.com",          SVC_FILE_SYNC },
    { "wetransfer.com",         SVC_FILE_SYNC },
    { "wetransferusercontent.com", SVC_FILE_SYNC },
    { "smash.com",              SVC_FILE_SYNC },
    { "filemail.com",           SVC_FILE_SYNC },
    { "yandex.disk",            SVC_FILE_SYNC },
    { "disk.yandex.com",        SVC_FILE_SYNC },
    { "mailru-drive.ru",        SVC_FILE_SYNC },
    { "kinsta.com/storage",     SVC_FILE_SYNC },
    { "filen.io",               SVC_FILE_SYNC },
    { "internxt.com",           SVC_FILE_SYNC },
    { "owncloud.com",           SVC_FILE_SYNC },
    { "nextcloud.com",          SVC_FILE_SYNC },

    /* ═════════════════════════════════════════════════════════════════
     * SVC_BULK_DL — software distribution / OS updates / dev tooling
     * ═════════════════════════════════════════════════════════════════ */
    /* Code hosts */
    { "githubusercontent.com",  SVC_BULK_DL },
    { "github.com",             SVC_BULK_DL },
    { "github.io",              SVC_BULK_DL },
    { "gitlab.com",             SVC_BULK_DL },
    { "gitlab.io",              SVC_BULK_DL },
    { "bitbucket.org",          SVC_BULK_DL },
    { "sr.ht",                  SVC_BULK_DL },
    { "codeberg.org",           SVC_BULK_DL },
    /* Container registries */
    { "docker.io",              SVC_BULK_DL },
    { "docker.com",             SVC_BULK_DL },
    { "quay.io",                SVC_BULK_DL },
    { "ghcr.io",                SVC_BULK_DL },
    { "gcr.io",                 SVC_BULK_DL },
    { "registry.k8s.io",        SVC_BULK_DL },
    /* Language package registries */
    { "pypi.org",               SVC_BULK_DL },
    { "pythonhosted.org",       SVC_BULK_DL },
    { "anaconda.org",           SVC_BULK_DL },
    { "conda-forge.org",        SVC_BULK_DL },
    { "npmjs.com",              SVC_BULK_DL },
    { "npmjs.org",              SVC_BULK_DL },
    { "yarnpkg.com",            SVC_BULK_DL },
    { "rubygems.org",           SVC_BULK_DL },
    { "crates.io",              SVC_BULK_DL },
    { "cargo.io",               SVC_BULK_DL },
    { "mvnrepository.com",      SVC_BULK_DL },
    { "maven.org",              SVC_BULK_DL },
    { "nuget.org",              SVC_BULK_DL },
    { "packagist.org",          SVC_BULK_DL },
    { "go.dev",                 SVC_BULK_DL },
    { "golang.org",             SVC_BULK_DL },
    /* OS / Linux distributions */
    { "archive.ubuntu.com",     SVC_BULK_DL },
    { "security.ubuntu.com",    SVC_BULK_DL },
    { "ports.ubuntu.com",       SVC_BULK_DL },
    { "ubuntu.com",             SVC_BULK_DL },
    { "debian.org",             SVC_BULK_DL },
    { "kernel.org",             SVC_BULK_DL },
    { "fedoraproject.org",      SVC_BULK_DL },
    { "rpmfusion.org",           SVC_BULK_DL },
    { "centos.org",             SVC_BULK_DL },
    { "redhat.com",             SVC_BULK_DL },
    { "almalinux.org",          SVC_BULK_DL },
    { "rockylinux.org",         SVC_BULK_DL },
    { "archlinux.org",          SVC_BULK_DL },
    { "manjaro.org",            SVC_BULK_DL },
    { "opensuse.org",           SVC_BULK_DL },
    { "alpinelinux.org",        SVC_BULK_DL },
    { "linuxmint.com",          SVC_BULK_DL },
    { "popos.io",               SVC_BULK_DL },
    { "elementary.io",          SVC_BULK_DL },
    { "openwrt.org",            SVC_BULK_DL },
    { "freebsd.org",            SVC_BULK_DL },
    { "openbsd.org",            SVC_BULK_DL },
    /* Microsoft Windows / Office / Update */
    { "windowsupdate.com",      SVC_BULK_DL },
    { "update.microsoft.com",   SVC_BULK_DL },
    { "download.microsoft.com", SVC_BULK_DL },
    { "officecdn.microsoft.com",SVC_BULK_DL },
    { "officecdn-microsoft-com.akamaized.net", SVC_BULK_DL },
    { "deploy.akamaitechnologies.com", SVC_BULK_DL },
    { "msftconnecttest.com",    SVC_BULK_DL },
    /* Apple SW Update */
    { "swcdn.apple.com",        SVC_BULK_DL },
    { "swdownload.apple.com",   SVC_BULK_DL },
    { "swdist.apple.com",       SVC_BULK_DL },
    { "appldnld.apple.com",     SVC_BULK_DL },
    { "downloads.apple.com",    SVC_BULK_DL },
    { "configuration.apple.com",SVC_BULK_DL },
    /* Google Play / Android */
    { "android.googleapis.com", SVC_BULK_DL },
    { "android.com",            SVC_BULK_DL },
    { "playgames.googleapis.com", SVC_BULK_DL },
    { "google.com/dl",          SVC_BULK_DL },
    { "dl.google.com",          SVC_BULK_DL },
    { "fdroid.org",             SVC_BULK_DL },
    { "aptoide.com",            SVC_BULK_DL },
    { "apkmirror.com",          SVC_BULK_DL },
    { "apkpure.com",            SVC_BULK_DL },
    /* CDN / dev infra */
    { "jsdelivr.net",           SVC_BULK_DL },
    { "cdnjs.cloudflare.com",   SVC_BULK_DL },
    { "unpkg.com",              SVC_BULK_DL },
    /* Mozilla / browsers */
    { "mozilla.org",            SVC_BULK_DL },
    { "addons.mozilla.org",     SVC_BULK_DL },
    { "ftp.mozilla.org",        SVC_BULK_DL },
    { "googlechrome.com",       SVC_BULK_DL },
    { "google.com/chrome",      SVC_BULK_DL },
    { "operacdn.com",           SVC_BULK_DL },
    { "brave.com",              SVC_BULK_DL },
    { "vivaldi.com",            SVC_BULK_DL },
    /* Antivirus / OS tooling */
    { "avast.com",              SVC_BULK_DL },
    { "kaspersky.com",          SVC_BULK_DL },
    { "norton.com",             SVC_BULK_DL },
    { "bitdefender.com",        SVC_BULK_DL },
    { "mcafee.com",             SVC_BULK_DL },

    /* ═════════════════════════════════════════════════════════════════
     * SVC_TORRENT — peer-to-peer (most encrypted, but trackers are clear)
     * ═════════════════════════════════════════════════════════════════ */
    { "bittorrent.com",         SVC_TORRENT },
    { "utorrent.com",           SVC_TORRENT },
    { "qbittorrent.org",        SVC_TORRENT },
    { "deluge-torrent.org",     SVC_TORRENT },
    { "transmissionbt.com",     SVC_TORRENT },
    { "rtorrent.org",           SVC_TORRENT },
    { "vuze.com",               SVC_TORRENT },
    { "thepiratebay.org",       SVC_TORRENT },
    { "rarbg.to",               SVC_TORRENT },
    { "1337x.to",               SVC_TORRENT },
    { "yts.mx",                 SVC_TORRENT },
    { "eztv.re",                SVC_TORRENT },
    { "linuxtracker.org",       SVC_TORRENT },
    { "opentrackr.org",         SVC_TORRENT },
    { "openbittorrent.com",     SVC_TORRENT },
    { "tracker.coppersurfer.tk",SVC_TORRENT },
    { "tracker.openbittorrent.com", SVC_TORRENT },

    /* ═════════════════════════════════════════════════════════════════
     * SVC_WEB_INTERACTIVE — chat / web apps (non-voice path)
     * ═════════════════════════════════════════════════════════════════ */
    { "discord.gg",             SVC_WEB_INTERACTIVE },
    { "discord.com",            SVC_WEB_INTERACTIVE },
    { "discordapp.com",         SVC_WEB_INTERACTIVE },
    { "discordapp.net",         SVC_WEB_INTERACTIVE },
    { "t.me",                   SVC_WEB_INTERACTIVE },
    { "telegram.me",            SVC_WEB_INTERACTIVE },
    { "twitter.com",            SVC_WEB_INTERACTIVE },
    { "x.com",                  SVC_WEB_INTERACTIVE },
    { "twimg.com",              SVC_WEB_INTERACTIVE },
    { "facebook.com",           SVC_WEB_INTERACTIVE },
    { "fb.com",                 SVC_WEB_INTERACTIVE },
    { "messenger.com",          SVC_WEB_INTERACTIVE },
    { "instagram.com",          SVC_WEB_INTERACTIVE },
    { "threads.net",            SVC_WEB_INTERACTIVE },
    { "linkedin.com",           SVC_WEB_INTERACTIVE },
    { "licdn.com",              SVC_WEB_INTERACTIVE },
    { "reddit.com",             SVC_WEB_INTERACTIVE },
    { "redd.it",                SVC_WEB_INTERACTIVE },
    { "redditmedia.com",        SVC_WEB_INTERACTIVE },
    { "redditstatic.com",       SVC_WEB_INTERACTIVE },
    { "tumblr.com",             SVC_WEB_INTERACTIVE },
    { "pinterest.com",          SVC_WEB_INTERACTIVE },
    { "pinimg.com",             SVC_WEB_INTERACTIVE },
    { "snapchat.com",           SVC_WEB_INTERACTIVE },
    { "sc-cdn.net",             SVC_WEB_INTERACTIVE },
    { "tumblr.com",             SVC_WEB_INTERACTIVE },
    { "tiktok.com",             SVC_WEB_INTERACTIVE },
    { "ycombinator.com",        SVC_WEB_INTERACTIVE },
    { "stackoverflow.com",      SVC_WEB_INTERACTIVE },
    { "stackexchange.com",      SVC_WEB_INTERACTIVE },
    { "wikipedia.org",          SVC_WEB_INTERACTIVE },
    { "wikimedia.org",          SVC_WEB_INTERACTIVE },
    { "medium.com",             SVC_WEB_INTERACTIVE },
    { "substack.com",           SVC_WEB_INTERACTIVE },
    { "notion.so",              SVC_WEB_INTERACTIVE },
    { "notion.site",            SVC_WEB_INTERACTIVE },
    { "trello.com",             SVC_WEB_INTERACTIVE },
    { "asana.com",              SVC_WEB_INTERACTIVE },
    { "airtable.com",           SVC_WEB_INTERACTIVE },
    { "monday.com",             SVC_WEB_INTERACTIVE },
    { "clickup.com",            SVC_WEB_INTERACTIVE },
    { "linear.app",             SVC_WEB_INTERACTIVE },
    { "miro.com",               SVC_WEB_INTERACTIVE },
    { "figma.com",              SVC_WEB_INTERACTIVE },
    { "canva.com",              SVC_WEB_INTERACTIVE },
    { "slack.com",              SVC_WEB_INTERACTIVE },
    { "slack-edge.com",         SVC_WEB_INTERACTIVE },
    { "slack-imgs.com",         SVC_WEB_INTERACTIVE },
    { "mattermost.com",         SVC_WEB_INTERACTIVE },
    { "rocket.chat",            SVC_WEB_INTERACTIVE },
    /* Webmail */
    { "gmail.com",              SVC_WEB_INTERACTIVE },
    { "mail.google.com",        SVC_WEB_INTERACTIVE },
    { "outlook.com",            SVC_WEB_INTERACTIVE },
    { "outlook.live.com",       SVC_WEB_INTERACTIVE },
    { "office.com",             SVC_WEB_INTERACTIVE },
    { "office365.com",          SVC_WEB_INTERACTIVE },
    { "yahoo.com",              SVC_WEB_INTERACTIVE },
    { "yahoomail.com",          SVC_WEB_INTERACTIVE },
    { "protonmail.com",         SVC_WEB_INTERACTIVE },
    { "proton.me",              SVC_WEB_INTERACTIVE },
    { "tutanota.com",           SVC_WEB_INTERACTIVE },
    { "fastmail.com",           SVC_WEB_INTERACTIVE },
    { "icloud-mail.com",        SVC_WEB_INTERACTIVE },
    /* Banking / e-commerce / shopping */
    { "amazon.com",             SVC_WEB_INTERACTIVE },
    { "amzn.to",                SVC_WEB_INTERACTIVE },
    { "ebay.com",               SVC_WEB_INTERACTIVE },
    { "shopify.com",            SVC_WEB_INTERACTIVE },
    { "etsy.com",               SVC_WEB_INTERACTIVE },
    { "aliexpress.com",         SVC_WEB_INTERACTIVE },
    { "alibaba.com",            SVC_WEB_INTERACTIVE },
    { "trendyol.com",           SVC_WEB_INTERACTIVE },
    { "hepsiburada.com",        SVC_WEB_INTERACTIVE },
    { "n11.com",                SVC_WEB_INTERACTIVE },
    /* Search / maps */
    { "duckduckgo.com",         SVC_WEB_INTERACTIVE },
    { "bing.com",               SVC_WEB_INTERACTIVE },
    { "yandex.com",             SVC_WEB_INTERACTIVE },
    { "yandex.ru",              SVC_WEB_INTERACTIVE },
    { "openstreetmap.org",      SVC_WEB_INTERACTIVE },
    { "mapbox.com",             SVC_WEB_INTERACTIVE },
    /* News */
    { "bbc.co.uk",              SVC_WEB_INTERACTIVE },
    { "cnn.com",                SVC_WEB_INTERACTIVE },
    { "nytimes.com",            SVC_WEB_INTERACTIVE },
    { "theguardian.com",        SVC_WEB_INTERACTIVE },
    { "reuters.com",            SVC_WEB_INTERACTIVE },
    { "bloomberg.com",          SVC_WEB_INTERACTIVE },
    { "wsj.com",                SVC_WEB_INTERACTIVE },
    { "spiegel.de",             SVC_WEB_INTERACTIVE },
    { "lemonde.fr",             SVC_WEB_INTERACTIVE },
    { "elpais.com",             SVC_WEB_INTERACTIVE },

    /* ═════════════════════════════════════════════════════════════════
     * SVC_SYSTEM — DNS, NTP, OS telemetry, captive portal probes
     * (Mostly already covered by port hints; these catch DoH/DoT/quic.)
     * ═════════════════════════════════════════════════════════════════ */
    { "dns.google",             SVC_SYSTEM },
    { "dns.google.com",         SVC_SYSTEM },
    { "cloudflare-dns.com",     SVC_SYSTEM },
    { "1.1.1.1",                SVC_SYSTEM },
    { "1.0.0.1",                SVC_SYSTEM },
    { "quad9.net",              SVC_SYSTEM },
    { "dns.quad9.net",          SVC_SYSTEM },
    { "opendns.com",            SVC_SYSTEM },
    { "umbrella.com",           SVC_SYSTEM },
    { "adguard-dns.com",        SVC_SYSTEM },
    { "dnscrypt.info",          SVC_SYSTEM },
    { "pool.ntp.org",           SVC_SYSTEM },
    { "ntppool.org",            SVC_SYSTEM },
    { "time.apple.com",         SVC_SYSTEM },
    { "time.windows.com",       SVC_SYSTEM },
    { "time.cloudflare.com",    SVC_SYSTEM },
    { "time.google.com",        SVC_SYSTEM },
    { "captive.apple.com",      SVC_SYSTEM },
    { "msftncsi.com",           SVC_SYSTEM },
    { "connectivity-check.ubuntu.com", SVC_SYSTEM },
    { "detectportal.firefox.com",SVC_SYSTEM },
    { "akamaiedge.net",         SVC_SYSTEM },  /* generic Akamai (catch-all infra) */
    { "akamaihd.net",           SVC_SYSTEM },

    { NULL, SVC_UNKNOWN }  /* sentinel */
};

/* ── Domain suffix → persona lookup ──────────────────────────── */

persona_t dns_domain_to_hint(const char *domain) {
    if (!domain || domain[0] == '\0') {
        return PERSONA_UNKNOWN;
    }

    size_t dlen = strlen(domain);

    for (const domain_hint_t *e = domain_table; e->suffix; e++) {
        size_t slen = strlen(e->suffix);
        if (slen > dlen) {
            continue;
        }
        /* Suffix match: domain ends with the suffix, and either the
         * domain IS the suffix or the char before the suffix is '.' */
        const char *tail = domain + (dlen - slen);
        if (strcasecmp(tail, e->suffix) == 0) {
            if (slen == dlen || *(tail - 1) == '.') {
                return e->persona;
            }
        }
    }

    return PERSONA_UNKNOWN;
}

service_t dns_domain_to_service(const char *domain) {
    if (!domain || domain[0] == '\0') {
        return SVC_UNKNOWN;
    }

    size_t dlen = strlen(domain);

    for (const domain_service_t *e = domain_service_table; e->suffix; e++) {
        size_t slen = strlen(e->suffix);
        if (slen > dlen) {
            continue;
        }
        const char *tail = domain + (dlen - slen);
        if (strcasecmp(tail, e->suffix) == 0) {
            if (slen == dlen || *(tail - 1) == '.') {
                return e->service;
            }
        }
    }

    return SVC_UNKNOWN;
}

/* ── Cache operations ────────────────────────────────────────── */

void dns_cache_init(dns_cache_t *cache) {
    if (!cache) {
        return;
    }
    memset(cache->entries, 0, sizeof(cache->entries));
    pthread_mutex_init(&cache->lock, NULL);
}

void dns_cache_destroy(dns_cache_t *cache) {
    if (!cache) {
        return;
    }
    pthread_mutex_destroy(&cache->lock);
}

persona_t dns_cache_lookup(dns_cache_t *cache, uint32_t ip) {
    if (!cache) {
        return PERSONA_UNKNOWN;
    }

    persona_t result = PERSONA_UNKNOWN;
    pthread_mutex_lock(&cache->lock);

    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        dns_entry_t *e = &cache->entries[i];
        if (e->active && e->ip == ip) {
            /* Check TTL expiry using monotonic clock */
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            double now = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
            if (now < e->expire_time) {
                result = e->hint;
            } else {
                /* Expired — mark inactive */
                e->active = 0;
            }
            break;
        }
    }

    pthread_mutex_unlock(&cache->lock);
    return result;
}

void dns_cache_insert(dns_cache_t *cache, uint32_t ip,
                      const char *domain, uint32_t ttl_s) {
    if (!cache || !domain) {
        return;
    }

    persona_t hint = dns_domain_to_hint(domain);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;

    /* Clamp TTL: minimum 30s, maximum 3600s */
    if (ttl_s < 30)   ttl_s = 30;
    if (ttl_s > 3600)  ttl_s = 3600;

    pthread_mutex_lock(&cache->lock);

    /* Look for existing entry for this IP, or find LRU slot */
    int target = -1;
    double oldest_expire = 1e18;

    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        dns_entry_t *e = &cache->entries[i];

        /* Exact IP match — update in place */
        if (e->active && e->ip == ip) {
            target = i;
            break;
        }

        /* Empty slot */
        if (!e->active) {
            target = i;
            oldest_expire = -1.0;  /* prefer empty over LRU */
            continue;
        }

        /* Track LRU (earliest expiry) for eviction */
        if (e->expire_time < oldest_expire && target < 0) {
            oldest_expire = e->expire_time;
            target = i;
        }
        /* If we already found an empty slot (oldest_expire == -1), skip LRU */
        if (oldest_expire < 0.0) {
            continue;
        }
        if (e->expire_time < oldest_expire) {
            oldest_expire = e->expire_time;
            target = i;
        }
    }

    if (target < 0) {
        target = 0;  /* fallback: should never happen with 64 slots */
    }

    dns_entry_t *slot = &cache->entries[target];
    slot->ip = ip;
    strncpy(slot->domain, domain, DNS_DOMAIN_MAXLEN - 1);
    slot->domain[DNS_DOMAIN_MAXLEN - 1] = '\0';
    slot->hint = hint;
    slot->service = dns_domain_to_service(domain);
    slot->expire_time = now + (double)ttl_s;
    slot->active = 1;

    pthread_mutex_unlock(&cache->lock);
}

service_t dns_cache_lookup_service(dns_cache_t *cache, uint32_t ip) {
    if (!cache) {
        return SVC_UNKNOWN;
    }

    service_t result = SVC_UNKNOWN;
    pthread_mutex_lock(&cache->lock);

    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        dns_entry_t *e = &cache->entries[i];
        if (e->active && e->ip == ip) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            double now = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
            if (now < e->expire_time) {
                result = e->service;
            } else {
                e->active = 0;
            }
            break;
        }
    }

    pthread_mutex_unlock(&cache->lock);
    return result;
}

/* ── DNS response parser ─────────────────────────────────────── */

/*
 * Paranoid DNS response parser. Extracts A records from a raw DNS
 * response (after UDP header). Inserts IP→domain pairs into the cache.
 *
 * DNS wire format (RFC 1035):
 *   Header: 12 bytes (ID, flags, QDCOUNT, ANCOUNT, NSCOUNT, ARCOUNT)
 *   Question section: variable (QNAME + QTYPE + QCLASS)
 *   Answer section: variable (NAME + TYPE + CLASS + TTL + RDLENGTH + RDATA)
 *
 * Safety: every offset is bounds-checked. Malformed packets are silently
 * rejected. No allocations, no recursion.
 */

/* Read a DNS name at offset, write it into buf (dot-separated).
 * Handles compression pointers (0xC0 prefix). Returns bytes consumed
 * from the packet at the original offset, or -1 on error.
 * max_jumps prevents infinite pointer loops. */
static int dns_read_name(const uint8_t *pkt, size_t pkt_len,
                         size_t offset, char *buf, size_t buf_len) {
    if (!pkt || !buf || buf_len == 0) {
        return -1;
    }

    size_t pos = offset;
    size_t out = 0;
    int jumped = 0;
    int bytes_consumed = 0;
    int max_jumps = 10;
    int jump_count = 0;

    buf[0] = '\0';

    while (pos < pkt_len) {
        uint8_t len = pkt[pos];

        if (len == 0) {
            /* End of name */
            if (!jumped) {
                bytes_consumed = (int)(pos - offset + 1);
            }
            break;
        }

        /* Compression pointer: 2 MSB = 11 */
        if ((len & 0xC0) == 0xC0) {
            if (pos + 1 >= pkt_len) {
                return -1;
            }
            if (!jumped) {
                bytes_consumed = (int)(pos - offset + 2);
            }
            uint16_t ptr = ((uint16_t)(len & 0x3F) << 8) | pkt[pos + 1];
            if (ptr >= pkt_len) {
                return -1;
            }
            pos = ptr;
            jumped = 1;
            if (++jump_count > max_jumps) {
                return -1;  /* infinite loop protection */
            }
            continue;
        }

        /* Label: len bytes follow */
        if ((len & 0xC0) != 0) {
            return -1;  /* reserved bits set — reject */
        }

        pos++;
        if (pos + len > pkt_len) {
            return -1;  /* label extends past packet */
        }

        /* Add dot separator between labels */
        if (out > 0 && out < buf_len - 1) {
            buf[out++] = '.';
        }

        for (uint8_t i = 0; i < len && out < buf_len - 1; i++) {
            buf[out++] = (char)pkt[pos + i];
        }
        pos += len;
    }

    buf[out] = '\0';

    return (bytes_consumed > 0) ? bytes_consumed : -1;
}

int dns_parse_response(dns_cache_t *cache, const uint8_t *pkt, size_t pkt_len) {
    if (!cache || !pkt) {
        return -1;
    }

    /* DNS header is 12 bytes minimum */
    if (pkt_len < 12) {
        return -1;
    }

    /* Check QR bit: must be a response (bit 15 of flags) */
    uint16_t flags = (uint16_t)(pkt[2] << 8 | pkt[3]);
    if (!(flags & 0x8000)) {
        return -1;  /* not a response */
    }

    /* Check RCODE: only accept NOERROR (0) */
    if ((flags & 0x000F) != 0) {
        return -1;
    }

    uint16_t qdcount = (uint16_t)(pkt[4] << 8 | pkt[5]);
    uint16_t ancount = (uint16_t)(pkt[6] << 8 | pkt[7]);

    if (ancount == 0) {
        return 0;  /* no answers — nothing to cache */
    }

    /* Sanity: reject unreasonable counts */
    if (qdcount > 10 || ancount > 50) {
        return -1;
    }

    /* Read the question name (we'll use it as the domain for A records
     * that use compression pointers back to the question) */
    char qname[DNS_DOMAIN_MAXLEN];
    qname[0] = '\0';

    size_t offset = 12;  /* past header */

    /* Skip question section */
    for (uint16_t q = 0; q < qdcount; q++) {
        int consumed = dns_read_name(pkt, pkt_len, offset, qname, sizeof(qname));
        if (consumed < 0) {
            return -1;
        }
        offset += (size_t)consumed;
        offset += 4;  /* QTYPE (2) + QCLASS (2) */
        if (offset > pkt_len) {
            return -1;
        }
    }

    /* Parse answer section */
    int a_records = 0;
    for (uint16_t a = 0; a < ancount; a++) {
        if (offset >= pkt_len) {
            break;
        }

        /* Read answer name */
        char aname[DNS_DOMAIN_MAXLEN];
        int consumed = dns_read_name(pkt, pkt_len, offset, aname, sizeof(aname));
        if (consumed < 0) {
            return a_records > 0 ? a_records : -1;
        }
        offset += (size_t)consumed;

        /* TYPE (2) + CLASS (2) + TTL (4) + RDLENGTH (2) = 10 bytes */
        if (offset + 10 > pkt_len) {
            return a_records > 0 ? a_records : -1;
        }

        uint16_t rtype   = (uint16_t)(pkt[offset] << 8 | pkt[offset + 1]);
        /* uint16_t rclass  = (uint16_t)(pkt[offset+2] << 8 | pkt[offset+3]); */
        uint32_t ttl     = (uint32_t)(pkt[offset + 4] << 24 | pkt[offset + 5] << 16 |
                                      pkt[offset + 6] << 8  | pkt[offset + 7]);
        uint16_t rdlen   = (uint16_t)(pkt[offset + 8] << 8 | pkt[offset + 9]);
        offset += 10;

        if (offset + rdlen > pkt_len) {
            return a_records > 0 ? a_records : -1;
        }

        /* A record: TYPE=1, RDLENGTH=4 (IPv4 address) */
        if (rtype == 1 && rdlen == 4) {
            uint32_t ip;
            memcpy(&ip, &pkt[offset], 4);  /* already in network byte order */

            /* Use the question name as the domain — it's the original
             * query that triggered this response. Answer names may be
             * CDN CNAMEs that don't match our suffix table. */
            const char *domain = (qname[0] != '\0') ? qname : aname;

            dns_cache_insert(cache, ip, domain, ttl);
            a_records++;
        }

        /* CNAME (TYPE=5): read the canonical name to use as domain
         * for subsequent A records in the same response */
        if (rtype == 5) {
            char cname[DNS_DOMAIN_MAXLEN];
            if (dns_read_name(pkt, pkt_len, offset, cname, sizeof(cname)) > 0) {
                /* We keep qname as the original query — CNAMEs are intermediate */
            }
        }

        offset += rdlen;
    }

    return a_records;
}

/* ── Sniffer thread ──────────────────────────────────────────── */

/*
 * Background thread: opens an AF_PACKET socket to capture *all* IPv4
 * traffic seen on any interface — including transit (forwarded) traffic
 * the router routes between LAN and WAN. AF_INET SOCK_RAW only delivers
 * host-bound packets, so it misses DNS responses for clients that
 * resolve directly via upstream (e.g. devices configured with 1.1.1.1).
 *
 * SOCK_DGRAM mode strips the link-layer header so packets start at IP.
 * We deduplicate by ignoring PACKET_OUTGOING — the kernel can deliver
 * the same forwarded frame twice (once on ingress, once on egress).
 *
 * Uses select() with 1-second timeout to check g_stop periodically.
 * Requires CAP_NET_RAW or root.
 */
void *dns_sniff_thread(void *arg) {
    dns_cache_t *cache = (dns_cache_t *)arg;
    if (!cache) {
        return NULL;
    }

    /* AF_PACKET captures every IPv4 packet seen on the host's interfaces,
     * including forwarded/transit traffic. ETH_P_IP filters to IPv4 only
     * at the kernel level (cheap). */
    int sock = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_IP));
    if (sock < 0) {
        log_msg(LOG_WARN, "dns", "failed to open AF_PACKET socket (need CAP_NET_RAW), DNS snooping disabled");
        return NULL;
    }

    log_msg(LOG_INFO, "dns", "DNS sniffer thread started (AF_PACKET, captures transit DNS)");

    uint8_t buf[2048];
    struct sockaddr_ll src_addr;
    socklen_t addr_len;

    while (!g_stop) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ret = select(sock + 1, &fds, NULL, NULL, &tv);
        if (ret <= 0) {
            continue;
        }

        addr_len = sizeof(src_addr);
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0,
                             (struct sockaddr *)&src_addr, &addr_len);
        if (n < (ssize_t)(sizeof(struct iphdr) + sizeof(struct udphdr))) {
            continue;
        }

        /* PACKET_OUTGOING means the kernel is showing us a frame we already
         * saw on the way in (or one we generated locally). Skip to dedupe. */
        if (src_addr.sll_pkttype == PACKET_OUTGOING) {
            continue;
        }

        /* Parse IP header (cooked AF_PACKET starts at L3) */
        struct iphdr *iph = (struct iphdr *)buf;
        if (iph->protocol != IPPROTO_UDP) {
            continue;
        }

        size_t ip_hlen = (size_t)(iph->ihl * 4);
        if (ip_hlen < 20 || ip_hlen + sizeof(struct udphdr) > (size_t)n) {
            continue;
        }

        struct udphdr *udph = (struct udphdr *)(buf + ip_hlen);
        uint16_t src_port = ntohs(udph->source);
        if (src_port != 53) {
            continue;
        }

        size_t dns_offset = ip_hlen + sizeof(struct udphdr);
        if (dns_offset >= (size_t)n) {
            continue;
        }

        const uint8_t *dns_payload = buf + dns_offset;
        size_t dns_len = (size_t)n - dns_offset;

        int count = dns_parse_response(cache, dns_payload, dns_len);
        if (count > 0) {
            log_msg(LOG_DEBUG, "dns", "parsed %d A record(s)", count);
        }
    }

    close(sock);
    log_msg(LOG_INFO, "dns", "DNS sniffer thread stopped");
    return NULL;
}
