/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_hint.c — Port-based persona hint lookup
 *
 * Static port→persona table covering major application categories.
 * Returns PERSONA_UNKNOWN for unrecognized ports (including 443/80).
 *
 * Port ranges sourced from:
 *   - Valve (CS2, Dota 2): UDP 27015-27050
 *   - Riot (LoL, Valorant): TCP 5000-5500, UDP 7000-8000, TCP 2099
 *   - Epic (Fortnite): UDP 5222, UDP 5795-5847
 *   - Minecraft Java: TCP 25565, Bedrock: UDP 19132
 *   - PUBG/Krafton: UDP 7086-7995 (covered by Riot UDP range)
 *   - Supercell (Brawl Stars): TCP 9339
 *   - STUN/TURN (WebRTC): UDP 3478-3479
 *   - Google Meet/WebRTC: UDP 19302-19309
 *   - Zoom: UDP 8801-8810
 *   - SIP: UDP/TCP 5060-5061
 *   - RTMP: TCP 1935
 *   - BitTorrent: TCP 6881-6889, TCP 6969, UDP 6881
 */
#include "myco_hint.h"

persona_t hint_from_port(uint8_t protocol, uint16_t dst_port) {
    /* ── UDP ports ─────────────────────────────────────────────── */
    if (protocol == 17) {
        /* VOIP / WebRTC */
        if (dst_port >= 3478 && dst_port <= 3479)  return PERSONA_VOIP;    /* STUN/TURN */
        if (dst_port >= 19302 && dst_port <= 19309) return PERSONA_VOIP;   /* Google Meet/WebRTC */
        if (dst_port >= 5060 && dst_port <= 5061)   return PERSONA_VOIP;   /* SIP */
        if (dst_port >= 8801 && dst_port <= 8810)   return PERSONA_VIDEO;  /* Zoom media */

        /* GAMING — Valve */
        if (dst_port >= 27015 && dst_port <= 27050) return PERSONA_GAMING; /* CS2, Dota 2 */

        /* GAMING — Riot (Valorant), PUBG/Krafton */
        if (dst_port >= 7000 && dst_port <= 8000)   return PERSONA_GAMING;

        /* GAMING — Epic (Fortnite) */
        if (dst_port == 5222)                        return PERSONA_GAMING;
        if (dst_port >= 5795 && dst_port <= 5847)    return PERSONA_GAMING;

        /* GAMING — Minecraft Bedrock */
        if (dst_port == 19132)                       return PERSONA_GAMING;

        /* STREAMING — RTMP */
        if (dst_port == 1935)                        return PERSONA_STREAMING;

        /* TORRENT — DHT */
        if (dst_port >= 6881 && dst_port <= 6889)    return PERSONA_TORRENT;

        return PERSONA_UNKNOWN;
    }

    /* ── TCP ports ─────────────────────────────────────────────── */
    if (protocol == 6) {
        /* VOIP — SIP over TCP */
        if (dst_port >= 5060 && dst_port <= 5061)    return PERSONA_VOIP;

        /* GAMING — Riot (LoL, Valorant) */
        if (dst_port >= 5000 && dst_port <= 5500)    return PERSONA_GAMING;
        if (dst_port == 2099)                         return PERSONA_GAMING; /* Riot auth */

        /* GAMING — Minecraft Java */
        if (dst_port == 25565)                        return PERSONA_GAMING;

        /* GAMING — Supercell (Brawl Stars, Clash Royale) */
        if (dst_port == 9339)                         return PERSONA_GAMING;

        /* STREAMING — RTMP */
        if (dst_port == 1935)                         return PERSONA_STREAMING;

        /* TORRENT — BitTorrent */
        if (dst_port >= 6881 && dst_port <= 6889)     return PERSONA_TORRENT;
        if (dst_port == 6969)                          return PERSONA_TORRENT; /* tracker */

        return PERSONA_UNKNOWN;
    }

    return PERSONA_UNKNOWN;
}

/* ─────────────────────────────────────────────────────────────────
 * service_from_port — v3 finer-grained port→service mapping
 *
 * Maps well-known ports to service_t (12 classes) rather than the
 * coarser persona_t (6 classes). Returns SVC_UNKNOWN for ports that
 * cannot be reliably attributed (e.g. 443, 80).
 *
 * Port 1935 (RTMP) maps to SVC_VIDEO_LIVE rather than generic
 * streaming — RTMP is real-time push, not buffered VOD. Zoom/Meet
 * are SVC_VIDEO_CONF (interactive). STUN/TURN/SIP are SVC_VOIP_CALL.
 * System ports (DNS/NTP/DHCP/mDNS) are SVC_SYSTEM so they get
 * explicit best-effort treatment instead of falling through.
 * ─────────────────────────────────────────────────────────────── */
service_t service_from_port(uint8_t protocol, uint16_t dst_port) {
    /* ── UDP ports ─────────────────────────────────────────────── */
    if (protocol == 17) {
        /* SYSTEM */
        if (dst_port == 53)                         return SVC_SYSTEM;   /* DNS */
        if (dst_port == 123)                        return SVC_SYSTEM;   /* NTP */
        if (dst_port == 67 || dst_port == 68)       return SVC_SYSTEM;   /* DHCP */
        if (dst_port == 5353)                       return SVC_SYSTEM;   /* mDNS */

        /* VOIP_CALL — signalling + real-time voice */
        if (dst_port >= 3478 && dst_port <= 3479)   return SVC_VOIP_CALL; /* STUN/TURN */
        if (dst_port >= 5060 && dst_port <= 5061)   return SVC_VOIP_CALL; /* SIP */

        /* VIDEO_CONF — interactive video calls */
        if (dst_port >= 19302 && dst_port <= 19309) return SVC_VIDEO_CONF; /* Google Meet */
        if (dst_port >= 8801 && dst_port <= 8810)   return SVC_VIDEO_CONF; /* Zoom */

        /* GAME_RT — real-time game traffic */
        if (dst_port >= 27015 && dst_port <= 27050) return SVC_GAME_RT;   /* Valve */
        if (dst_port >= 7000 && dst_port <= 8000)   return SVC_GAME_RT;   /* Riot/PUBG */
        if (dst_port == 5222)                       return SVC_GAME_RT;   /* Epic */
        if (dst_port >= 5795 && dst_port <= 5847)   return SVC_GAME_RT;   /* Epic */
        if (dst_port == 19132)                      return SVC_GAME_RT;   /* MC Bedrock */

        /* VIDEO_LIVE — RTMP push streaming */
        if (dst_port == 1935)                       return SVC_VIDEO_LIVE;

        /* TORRENT — DHT */
        if (dst_port >= 6881 && dst_port <= 6889)   return SVC_TORRENT;

        return SVC_UNKNOWN;
    }

    /* ── TCP ports ─────────────────────────────────────────────── */
    if (protocol == 6) {
        /* WEB_INTERACTIVE — remote shell / control channels */
        if (dst_port == 22)                         return SVC_WEB_INTERACTIVE; /* SSH */

        /* VOIP_CALL — SIP over TCP */
        if (dst_port >= 5060 && dst_port <= 5061)   return SVC_VOIP_CALL;

        /* GAME_RT */
        if (dst_port >= 5000 && dst_port <= 5500)   return SVC_GAME_RT;   /* Riot */
        if (dst_port == 2099)                       return SVC_GAME_RT;   /* Riot auth */
        if (dst_port == 25565)                      return SVC_GAME_RT;   /* MC Java */
        if (dst_port == 9339)                       return SVC_GAME_RT;   /* Supercell */

        /* VIDEO_LIVE — RTMP */
        if (dst_port == 1935)                       return SVC_VIDEO_LIVE;

        /* TORRENT */
        if (dst_port >= 6881 && dst_port <= 6889)   return SVC_TORRENT;
        if (dst_port == 6969)                       return SVC_TORRENT;   /* tracker */

        return SVC_UNKNOWN;
    }

    return SVC_UNKNOWN;
}
