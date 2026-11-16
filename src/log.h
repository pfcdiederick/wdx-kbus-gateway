// Minimal logging to stderr (Docker adds timestamps via `docker logs -t`).
#ifndef KBUS_GW_LOG_H
#define KBUS_GW_LOG_H

#include <stdio.h>

#define LOG_INFO(fmt, ...)  fprintf(stderr, "[info]  " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, "[warn]  " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[error] " fmt "\n", ##__VA_ARGS__)

#endif
