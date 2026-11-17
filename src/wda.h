// wdx data layer (WAGO Device Access conventions): the HTTP interface to the KBUS
// data: GET/PATCH /wda/parameters[/{id}[/definition]], JSON:API 1.0.
#ifndef KBUS_GW_WDA_H
#define KBUS_GW_WDA_H

#include <stddef.h>

#include "config.h"

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} wbuf_t;

// Build the parameter/id tables once (call after gateway_init()).
void wda_init(void);

// Handle one request (any method / path: this is the only interface). Fills
// `body` (may stay empty, e.g. for 204) and returns the HTTP status. The body is
// always JSON:API (application/vnd.api+json).
int  wda_handle(const char *req, size_t len, const gw_config_t *cfg, wbuf_t *body);

void wbuf_free(wbuf_t *b);

#endif
