/*
 * test_persona.c - Unit tests for persona heuristics
 */
#include <stdio.h>
#include <string.h>
#include "../minunit.h"
#include "../myco_types.h"
#include "../myco_persona.h"

int tests_run = 0;

static char *test_persona_voting() {
    persona_state_t state;
    persona_init(&state);

    metrics_t m_interactive = { .rtt_ms = 50.0, .jitter_ms = 1.0, .cpu_pct = 5.0, .tx_bps = 50000.0, .rx_bps = 50000.0 };
    metrics_t m_bulk = { .rtt_ms = 30.0, .jitter_ms = 5.0, .cpu_pct = 10.0, .tx_bps = 5000000.0, .rx_bps = 1000000.0 };

    // Initially unknown/default
    mu_assert("error, initial persona should be unknown", state.current == PERSONA_UNKNOWN);

    // Feed 1 interactive sample -> not enough
    persona_update(&state, &m_interactive);
    // (We can't easily check internal vote count without exposing it, but we can check result)
    // Assuming k=3 of m=5
    
    // Feed 3 consecutive interactive samples
    persona_update(&state, &m_interactive);
    persona_update(&state, &m_interactive);
    
    // Should switch to interactive
    mu_assert("error, should switch to interactive after votes", state.current == PERSONA_INTERACTIVE);

    // Feed mixed signals
    persona_update(&state, &m_bulk); // 1 bulk
    mu_assert("error, should stay interactive (hysteresis)", state.current == PERSONA_INTERACTIVE);
    
    persona_update(&state, &m_bulk); // 2 bulk
    persona_update(&state, &m_bulk); // 3 bulk
    
    // Should switch to bulk
    mu_assert("error, should switch to bulk after votes", state.current == PERSONA_BULK);

    return 0;
}

static char *all_tests() {
    mu_run_test(test_persona_voting);
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
