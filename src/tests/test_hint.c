/*
 * test_hint.c - Unit tests for port-based persona hint lookup
 */
#include <stdio.h>
#include "../minunit.h"
#include "../myco_types.h"
#include "../myco_service.h"
#include "../myco_hint.h"

int tests_run = 0;

/* ── GAMING ports ─────────────────────────────────────────────── */

static char *test_valve_udp() {
    mu_assert("Valve lower bound", hint_from_port(17, 27015) == PERSONA_GAMING);
    mu_assert("Valve upper bound", hint_from_port(17, 27050) == PERSONA_GAMING);
    mu_assert("Valve mid-range",   hint_from_port(17, 27030) == PERSONA_GAMING);
    mu_assert("Valve below range", hint_from_port(17, 27014) != PERSONA_GAMING);
    mu_assert("Valve above range", hint_from_port(17, 27051) != PERSONA_GAMING);
    return 0;
}

static char *test_riot_tcp() {
    /* LoL game server: TCP 5000-5500 */
    mu_assert("Riot TCP lower", hint_from_port(6, 5000) == PERSONA_GAMING);
    mu_assert("Riot TCP upper", hint_from_port(6, 5500) == PERSONA_GAMING);
    mu_assert("Riot TCP mid",   hint_from_port(6, 5250) == PERSONA_GAMING);
    mu_assert("Riot TCP below", hint_from_port(6, 4999) != PERSONA_GAMING);
    mu_assert("Riot TCP above", hint_from_port(6, 5501) != PERSONA_GAMING);
    /* Riot auth */
    mu_assert("Riot auth",      hint_from_port(6, 2099) == PERSONA_GAMING);
    return 0;
}

static char *test_riot_valorant_udp() {
    /* Valorant/PUBG: UDP 7000-8000 */
    mu_assert("Valorant lower", hint_from_port(17, 7000) == PERSONA_GAMING);
    mu_assert("Valorant upper", hint_from_port(17, 8000) == PERSONA_GAMING);
    mu_assert("PUBG mid",       hint_from_port(17, 7500) == PERSONA_GAMING);
    return 0;
}

static char *test_epic_udp() {
    mu_assert("Fortnite 5222",  hint_from_port(17, 5222) == PERSONA_GAMING);
    mu_assert("Fortnite lower", hint_from_port(17, 5795) == PERSONA_GAMING);
    mu_assert("Fortnite upper", hint_from_port(17, 5847) == PERSONA_GAMING);
    mu_assert("Fortnite below", hint_from_port(17, 5794) != PERSONA_GAMING);
    return 0;
}

static char *test_minecraft() {
    mu_assert("MC Java TCP",    hint_from_port(6, 25565)  == PERSONA_GAMING);
    mu_assert("MC Bedrock UDP", hint_from_port(17, 19132) == PERSONA_GAMING);
    return 0;
}

static char *test_supercell_tcp() {
    mu_assert("Brawl Stars",    hint_from_port(6, 9339) == PERSONA_GAMING);
    return 0;
}

/* ── VOIP ports ───────────────────────────────────────────────── */

static char *test_stun_turn() {
    mu_assert("STUN 3478",      hint_from_port(17, 3478) == PERSONA_VOIP);
    mu_assert("STUN 3479",      hint_from_port(17, 3479) == PERSONA_VOIP);
    mu_assert("STUN below",     hint_from_port(17, 3477) != PERSONA_VOIP);
    return 0;
}

static char *test_webrtc_google() {
    mu_assert("Meet lower",     hint_from_port(17, 19302) == PERSONA_VOIP);
    mu_assert("Meet upper",     hint_from_port(17, 19309) == PERSONA_VOIP);
    mu_assert("Meet below",     hint_from_port(17, 19301) != PERSONA_VOIP);
    mu_assert("Meet above",     hint_from_port(17, 19310) != PERSONA_VOIP);
    return 0;
}

static char *test_sip() {
    mu_assert("SIP UDP 5060",   hint_from_port(17, 5060) == PERSONA_VOIP);
    mu_assert("SIP UDP 5061",   hint_from_port(17, 5061) == PERSONA_VOIP);
    mu_assert("SIP TCP 5060",   hint_from_port(6, 5060)  == PERSONA_VOIP);
    mu_assert("SIP TCP 5061",   hint_from_port(6, 5061)  == PERSONA_VOIP);
    return 0;
}

/* ── VIDEO ports ──────────────────────────────────────────────── */

static char *test_zoom() {
    mu_assert("Zoom lower",     hint_from_port(17, 8801) == PERSONA_VIDEO);
    mu_assert("Zoom upper",     hint_from_port(17, 8810) == PERSONA_VIDEO);
    mu_assert("Zoom below",     hint_from_port(17, 8800) != PERSONA_VIDEO);
    return 0;
}

/* ── STREAMING ports ──────────────────────────────────────────── */

static char *test_rtmp() {
    mu_assert("RTMP TCP",       hint_from_port(6, 1935)  == PERSONA_STREAMING);
    mu_assert("RTMP UDP",       hint_from_port(17, 1935) == PERSONA_STREAMING);
    return 0;
}

/* ── TORRENT ports ────────────────────────────────────────────── */

static char *test_bittorrent() {
    mu_assert("BT TCP lower",   hint_from_port(6, 6881)  == PERSONA_TORRENT);
    mu_assert("BT TCP upper",   hint_from_port(6, 6889)  == PERSONA_TORRENT);
    mu_assert("BT tracker",     hint_from_port(6, 6969)   == PERSONA_TORRENT);
    mu_assert("BT DHT UDP",     hint_from_port(17, 6881)  == PERSONA_TORRENT);
    mu_assert("BT TCP below",   hint_from_port(6, 6880)   != PERSONA_TORRENT);
    return 0;
}

/* ── UNKNOWN (no hint) ────────────────────────────────────────── */

static char *test_unknown_ports() {
    mu_assert("HTTPS 443 TCP",  hint_from_port(6, 443)   == PERSONA_UNKNOWN);
    mu_assert("HTTPS 443 UDP",  hint_from_port(17, 443)  == PERSONA_UNKNOWN);
    mu_assert("HTTP 80",        hint_from_port(6, 80)    == PERSONA_UNKNOWN);
    mu_assert("DNS 53",         hint_from_port(17, 53)   == PERSONA_UNKNOWN);
    mu_assert("SSH 22",         hint_from_port(6, 22)    == PERSONA_UNKNOWN);
    mu_assert("random high",    hint_from_port(6, 54321) == PERSONA_UNKNOWN);
    /* ICMP (protocol 1) */
    mu_assert("ICMP proto",     hint_from_port(1, 0)     == PERSONA_UNKNOWN);
    return 0;
}

/* ── v3: service_from_port ────────────────────────────────────── */

static char *test_svc_game_rt() {
    mu_assert("Valve UDP → GAME_RT",    service_from_port(17, 27020) == SVC_GAME_RT);
    mu_assert("Riot UDP → GAME_RT",     service_from_port(17, 7500)  == SVC_GAME_RT);
    mu_assert("Riot TCP → GAME_RT",     service_from_port(6, 5222)   == SVC_GAME_RT);
    mu_assert("Epic 5222 → GAME_RT",    service_from_port(17, 5222)  == SVC_GAME_RT);
    mu_assert("Epic 5795 → GAME_RT",    service_from_port(17, 5795)  == SVC_GAME_RT);
    mu_assert("MC Bedrock → GAME_RT",   service_from_port(17, 19132) == SVC_GAME_RT);
    mu_assert("MC Java → GAME_RT",      service_from_port(6, 25565)  == SVC_GAME_RT);
    mu_assert("Supercell → GAME_RT",    service_from_port(6, 9339)   == SVC_GAME_RT);
    mu_assert("Riot auth → GAME_RT",    service_from_port(6, 2099)   == SVC_GAME_RT);
    return 0;
}

static char *test_svc_voip_call() {
    mu_assert("STUN → VOIP_CALL",       service_from_port(17, 3478) == SVC_VOIP_CALL);
    mu_assert("TURN → VOIP_CALL",       service_from_port(17, 3479) == SVC_VOIP_CALL);
    mu_assert("SIP UDP → VOIP_CALL",    service_from_port(17, 5060) == SVC_VOIP_CALL);
    mu_assert("SIP TCP → VOIP_CALL",    service_from_port(6, 5061)  == SVC_VOIP_CALL);
    return 0;
}

static char *test_svc_video_conf() {
    mu_assert("Meet → VIDEO_CONF",      service_from_port(17, 19305) == SVC_VIDEO_CONF);
    mu_assert("Zoom → VIDEO_CONF",      service_from_port(17, 8801)  == SVC_VIDEO_CONF);
    mu_assert("Zoom upper → CONF",      service_from_port(17, 8810)  == SVC_VIDEO_CONF);
    return 0;
}

static char *test_svc_video_live() {
    mu_assert("RTMP UDP → VIDEO_LIVE",  service_from_port(17, 1935) == SVC_VIDEO_LIVE);
    mu_assert("RTMP TCP → VIDEO_LIVE",  service_from_port(6, 1935)  == SVC_VIDEO_LIVE);
    return 0;
}

static char *test_svc_torrent() {
    mu_assert("BT TCP → TORRENT",       service_from_port(6, 6881)  == SVC_TORRENT);
    mu_assert("BT TCP upper → TORRENT", service_from_port(6, 6889)  == SVC_TORRENT);
    mu_assert("BT tracker → TORRENT",   service_from_port(6, 6969)  == SVC_TORRENT);
    mu_assert("BT DHT UDP → TORRENT",   service_from_port(17, 6881) == SVC_TORRENT);
    return 0;
}

static char *test_svc_system() {
    mu_assert("DNS → SYSTEM",           service_from_port(17, 53)   == SVC_SYSTEM);
    mu_assert("NTP → SYSTEM",           service_from_port(17, 123)  == SVC_SYSTEM);
    mu_assert("DHCP 67 → SYSTEM",       service_from_port(17, 67)   == SVC_SYSTEM);
    mu_assert("DHCP 68 → SYSTEM",       service_from_port(17, 68)   == SVC_SYSTEM);
    mu_assert("mDNS → SYSTEM",          service_from_port(17, 5353) == SVC_SYSTEM);
    return 0;
}

static char *test_svc_web_interactive() {
    mu_assert("SSH → WEB_INTERACTIVE",  service_from_port(6, 22) == SVC_WEB_INTERACTIVE);
    return 0;
}

static char *test_svc_unknown() {
    mu_assert("HTTPS 443 TCP",          service_from_port(6, 443)   == SVC_UNKNOWN);
    mu_assert("HTTPS 443 UDP",          service_from_port(17, 443)  == SVC_UNKNOWN);
    mu_assert("HTTP 80",                service_from_port(6, 80)    == SVC_UNKNOWN);
    mu_assert("random high",            service_from_port(6, 54321) == SVC_UNKNOWN);
    mu_assert("ICMP proto",             service_from_port(1, 0)     == SVC_UNKNOWN);
    return 0;
}

static char *all_tests() {
    mu_run_test(test_valve_udp);
    mu_run_test(test_riot_tcp);
    mu_run_test(test_riot_valorant_udp);
    mu_run_test(test_epic_udp);
    mu_run_test(test_minecraft);
    mu_run_test(test_supercell_tcp);
    mu_run_test(test_stun_turn);
    mu_run_test(test_webrtc_google);
    mu_run_test(test_sip);
    mu_run_test(test_zoom);
    mu_run_test(test_rtmp);
    mu_run_test(test_bittorrent);
    mu_run_test(test_unknown_ports);
    mu_run_test(test_svc_game_rt);
    mu_run_test(test_svc_voip_call);
    mu_run_test(test_svc_video_conf);
    mu_run_test(test_svc_video_live);
    mu_run_test(test_svc_torrent);
    mu_run_test(test_svc_system);
    mu_run_test(test_svc_web_interactive);
    mu_run_test(test_svc_unknown);
    return 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    char *result = all_tests();
    if (result != 0) {
        printf("FAILED: %s\n", result);
    } else {
        printf("ALL TESTS PASSED\n");
    }
    printf("Tests run: %d\n", tests_run);
    return result != 0;
}
