/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_dns.h — Passive DNS snooping + IP→persona cache
 *
 * Passively captures DNS responses on br-lan (UDP port 53) to build
 * an IP→domain→persona mapping cache. This allows MycoFlow to identify
 * traffic on port 443 (HTTPS/QUIC) by domain name.
 *
 * Architecture:
 *   - dns_cache_t: LRU cache mapping IP → (domain, persona, TTL)
 *   - dns_parse_response(): paranoid parser for DNS response packets
 *   - dns_sniff_thread(): background thread with raw socket on UDP 53
 *   - dns_domain_to_hint(): suffix-match domain → persona lookup
 *
 * Safety: the parser rejects malformed packets silently. A crash in the
 * DNS thread must never take down the main loop. If DNS snooping fails,
 * the system degrades gracefully to port+behavior classification (78%).
 */
#ifndef MYCO_DNS_H
#define MYCO_DNS_H

#include "myco_types.h"
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

/* ── DNS cache entry ──────────────────────────────────────────── */

#define DNS_CACHE_SIZE    64
#define DNS_DOMAIN_MAXLEN 128

typedef struct {
    uint32_t  ip;                         /* IPv4 in network byte order */
    char      domain[DNS_DOMAIN_MAXLEN];  /* e.g. "rr1---sn-abc.googlevideo.com" */
    persona_t hint;                       /* persona derived from domain suffix */
    double    expire_time;                /* monotonic time when entry expires */
    int       active;                     /* 1 = occupied slot */
} dns_entry_t;

typedef struct {
    dns_entry_t     entries[DNS_CACHE_SIZE];
    pthread_mutex_t lock;
} dns_cache_t;

/* ── Cache operations ─────────────────────────────────────────── */

/* Initialize the cache (zero all entries, init mutex) */
void dns_cache_init(dns_cache_t *cache);

/* Destroy mutex resources */
void dns_cache_destroy(dns_cache_t *cache);

/* Look up an IP in the cache. Returns the hinted persona, or
 * PERSONA_UNKNOWN if the IP is not cached or the entry has expired.
 * Thread-safe (acquires lock). */
persona_t dns_cache_lookup(dns_cache_t *cache, uint32_t ip);

/* Insert or update a cache entry. Resolves domain→persona via suffix
 * matching. TTL is in seconds (from DNS response). Evicts LRU if full.
 * Thread-safe (acquires lock). */
void dns_cache_insert(dns_cache_t *cache, uint32_t ip,
                      const char *domain, uint32_t ttl_s);

/* ── Domain suffix → persona lookup ───────────────────────────── */

/* Match a domain name against the built-in suffix→persona table.
 * Returns PERSONA_UNKNOWN if no suffix matches.
 * E.g., "rr1---sn-abc.googlevideo.com" matches "googlevideo.com" → STREAMING */
persona_t dns_domain_to_hint(const char *domain);

/* ── DNS response parser ──────────────────────────────────────── */

/* Parse a raw DNS response packet and insert any A records into the cache.
 * Returns the number of A records successfully parsed, or -1 on error.
 * PARANOID: rejects malformed packets silently (no crash, no log spam). */
int dns_parse_response(dns_cache_t *cache, const uint8_t *pkt, size_t pkt_len);

/* ── Sniffer thread ───────────────────────────────────────────── */

/* Background thread function. Opens a raw socket on UDP port 53,
 * captures DNS responses, and populates the cache.
 * Arg: pointer to dns_cache_t. Checks g_stop each second.
 * Requires CAP_NET_RAW or root. */
void *dns_sniff_thread(void *arg);

#endif /* MYCO_DNS_H */
