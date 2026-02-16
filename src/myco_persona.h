/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_persona.h — Persona inference heuristics
 */
#ifndef MYCO_PERSONA_H
#define MYCO_PERSONA_H

#include "myco_types.h"

void         persona_init(persona_state_t *state);
const char  *persona_name(persona_t persona);
persona_t    persona_update(persona_state_t *state, const metrics_t *metrics);

#endif /* MYCO_PERSONA_H */
