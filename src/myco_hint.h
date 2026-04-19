/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_hint.h — Port-based persona hint lookup
 *
 * Maps well-known destination ports to persona hints. These hints act as
 * tiebreakers when the behavioral decision tree is ambiguous — they do NOT
 * override strong behavioral signals.
 */
#ifndef MYCO_HINT_H
#define MYCO_HINT_H

#include "myco_types.h"
#include "myco_service.h"
#include <stdint.h>

/*
 * Look up a persona hint based on transport protocol and destination port.
 *
 * Returns the hinted persona, or PERSONA_UNKNOWN if the port is not
 * recognized (e.g., port 443 which is used by everything).
 *
 * Protocol: 6 = TCP, 17 = UDP.
 */
persona_t hint_from_port(uint8_t protocol, uint16_t dst_port);

/*
 * Finer-grained v3 port→service mapping. Returns SVC_UNKNOWN for ports
 * that don't carry a reliable signal (e.g., 443). Protocol: 6=TCP, 17=UDP.
 */
service_t service_from_port(uint8_t protocol, uint16_t dst_port);

#endif /* MYCO_HINT_H */
