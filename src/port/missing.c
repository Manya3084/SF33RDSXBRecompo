#include "port/port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SF_MISSING_MAX 1024

static const char *s_seen[SF_MISSING_MAX];
static int s_seen_n;

void sf_port_missing(const char *name)
{
    int i;
    for (i = 0; i < s_seen_n; i++) {
        if (s_seen[i] == name) {
            return;
        }
    }
    if (s_seen_n < SF_MISSING_MAX) {
        s_seen[s_seen_n++] = name;
    }
    fprintf(stderr, "port: no shim for %s\n", name);
    if (getenv("SF_PORT_STRICT")) {
        abort();
    }
}
