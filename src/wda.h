// wdx data layer (WAGO Device Access conventions): the HTTP interface to the KBUS
// data: a subset of the WDA REST-API 1.11.0 (/wda, /wda/parameters, /wda/parameter-definitions).
#ifndef KBUS_GW_WDA_H
#define KBUS_GW_WDA_H

#include <stddef.h>

#include "config.h"

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
    char   location[256];   // Location header of a 301 answer ("" = none)
} wbuf_t;

// Initial state from the configuration (KBus/LocalControl); once, before wda_init().
void wda_config(const gw_config_t *cfg);

// Build the parameter/id tables once (call after gateway_init()).
void wda_init(void);

// Handle one request (any method / path: this is the only interface). Fills
// `body` (may stay empty, e.g. for 204) and returns the HTTP status. The body is
// always JSON:API (application/vnd.api+json).
// control = 1: the request came through the control socket (the dashboard).
int  wda_handle(const char *req, size_t len, const gw_config_t *cfg, int control, wbuf_t *body);

void wbuf_free(wbuf_t *b);

#endif
