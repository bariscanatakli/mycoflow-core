/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_ebpf.h — eBPF load/attach/read/shutdown
 */
#ifndef MYCO_EBPF_H
#define MYCO_EBPF_H

#include "myco_types.h"

int  ebpf_init(const myco_config_t *cfg);
int  ebpf_attach_tc(const myco_config_t *cfg);
void ebpf_tick(const myco_config_t *cfg);
void ebpf_shutdown(void);

#endif /* MYCO_EBPF_H */
