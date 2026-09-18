#include "system.h"
#include "alignment.h"
#include "hatch.h"
#include "relay.h"
#include <stdio.h>
#include <stdbool.h>

static bool faulted = false;

void system_fault(const char *msg) {
    alignment_emergency_stop();
    hatch_emergency_stop();
    light_off();
    faulted = true;
    printf("FAULT: %s\r\n", msg);
}

bool system_in_fault(void) {
    return faulted;
}

void system_clear_fault(void) {
    faulted = false;
}
