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
