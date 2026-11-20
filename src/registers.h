// Registers: named values that containers share through the wdx data layer.
//
// A second data provider next to the KBUS (gateway.c). Registers never touch
// the KBUS: they have their own lock and are not part of the process image.
//
// Declared in REGISTER_FILE (default /etc/kbus-gateway-registers.conf), one per line:
//     <Name>  <type>  [default]  [persistent]
//   Name        segments [A-Za-z][A-Za-z0-9_]* separated by '/', e.g. Pump1/Setpoint
//   type        boolean | int32 | uint32 | float64 | string
//   default     JSON value (strings in double quotes); omitted = false / 0 / ""
//   persistent  "persistent" = the value survives a restart (REGISTER_STORE)
// wdx path: Registers/<Name>/Value. Registers/ChangeCounter counts the writes
// that changed at least one register.
#ifndef KBUS_GW_REGISTERS_H
#define KBUS_GW_REGISTERS_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

#define REG_MAX         256
#define REG_NAME_MAX    40     // characters of <Name>
#define REG_STRING_MAX  128    // bytes of a string value, including the terminating NUL

typedef enum { REG_BOOL = 0, REG_INT32, REG_UINT32, REG_FLOAT64, REG_STRING } reg_type_t;

typedef struct {
    int64_t i;                  // boolean (0/1), int32, uint32
    double  f;                  // float64
    char    s[REG_STRING_MAX];  // string (UTF-8, NUL terminated)
} reg_value_t;

typedef struct {
    char       name[REG_NAME_MAX + 1];
    reg_type_t type;
    int        persistent;
} reg_def_t;

// Read the declarations and the stored persistent values. A missing file means
// no registers; invalid lines are logged and skipped.
void registers_load(const gw_config_t *cfg);

size_t           registers_count(void);
const reg_def_t *registers_def(size_t index);          // 0-based
const char      *reg_type_name(reg_type_t type);

// Copy all values under one lock (consistent snapshot).
void registers_snapshot(reg_value_t *out, uint64_t *change_counter);

// Parse a JSON value for a register of this type.
// Returns 0, 400 (wrong JSON type / malformed) or 409 (out of range / too long).
int registers_parse(reg_type_t type, const char *json, size_t len, reg_value_t *out);

// Format a value as JSON into buf (always NUL terminated). Returns the length.
// 6 * REG_STRING_MAX + 3 bytes are always enough.
#define REG_JSON_MAX (6 * REG_STRING_MAX + 3)
size_t registers_format(reg_type_t type, const reg_value_t *v, char *buf, size_t len);

// Write n registers (idx[] 0-based, already parsed) under one lock.
// *changed = 1 when at least one value changed.
void registers_write(const size_t *idx, const reg_value_t *vals, size_t n, int *changed);

// Store the persistent registers when they changed: at most every
// REGISTER_SAVE_S seconds, or now when force != 0 (shutdown).
void registers_save(int force);

#endif
