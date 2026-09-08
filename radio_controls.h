/* hermes-radio-daemon - generic radio control surface
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One control vocabulary for the whole daemon. Controls are named with the
 * Hamlib token names ("RFPOWER", "MICGAIN", "NB", "VOX", ...) so the same
 * string reaches the rig from every direction: the websocket API, the
 * rigctld-compatible network server (rig_server) and the web UI all speak
 * these names, and the backend translates them once.
 *
 * A control is one of three Hamlib kinds:
 *   LEVEL - continuous or stepped value (RFPOWER, AF, MICGAIN, ATT, AGC, ...)
 *   FUNC  - on/off switch (NB, COMP, VOX, TUNER, RIT, XIT, ...)
 *   PARM  - rig-global parameter (ANN, BACKLIGHT, TIME, ...)
 *
 * The set of controls a rig actually has is discovered from the backend, not
 * hardcoded: the hamlib backend walks the open rig's capability masks, so an
 * IC-7300 exposes its controls and an FT-710 exposes its own.
 */

#ifndef RADIO_CONTROLS_H_
#define RADIO_CONTROLS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "radio.h"

#define RADIO_CTRL_NAME_MAX 24
/* Hamlib has 64 settings per kind; a rig never has them all, but size for
 * the worst case so enumeration never truncates. */
#define RADIO_CTRL_MAX 192

typedef enum {
    RADIO_CTRL_LEVEL = 0,
    RADIO_CTRL_FUNC  = 1,
    RADIO_CTRL_PARM  = 2,
} radio_ctrl_kind;

typedef struct {
    char            name[RADIO_CTRL_NAME_MAX];
    radio_ctrl_kind kind;
    /* Hamlib carries levels/parms either as float (typically 0..1) or as
     * int (dB, index, Hz). Clients need to know which to render a sane
     * widget and to round correctly. FUNCs are always 0/1. */
    bool            is_float;
    bool            can_get;
    bool            can_set;
    /* Value range and step as reported by the rig backend. When the rig
     * gives no granularity, min=0 max=1 step=0 for floats (Hamlib's
     * convention) and min=max=step=0 for ints, meaning "unknown". */
    double          min;
    double          max;
    double          step;
} radio_ctrl_info;

/* Return codes. Kept small and backend-neutral; rig_server maps them onto
 * rigctld RPRT numbers and the websocket onto error strings. */
#define RADIO_CTRL_OK       0
#define RADIO_CTRL_ENOTSUP (-1)   /* rig/backend does not have this control */
#define RADIO_CTRL_EINVAL  (-2)   /* unknown name or out-of-range value */
#define RADIO_CTRL_EIO     (-3)   /* rig refused / CAT error */

/* Enumerate the controls the active backend really supports. Returns the
 * number written. Cheap: capability masks only, no CAT traffic. */
size_t radio_controls_enumerate(radio *radio_h, radio_ctrl_info *out, size_t max);

/* Capability report as JSON: {"cmd":"get_controls","controls":[{...},...]}.
 * Capabilities only — no rig reads. */
int radio_controls_caps_json(radio *radio_h, char *out, size_t out_len);

/* Current values of gettable controls, as JSON. This DOES hit the rig — one
 * CAT read per control — so it is an on-demand snapshot, never something to
 * poll. `names` narrows it to a comma-separated subset ("AF,RF,NB"), which is
 * what a UI refreshing a few widgets should use; NULL or empty reads every
 * gettable control, which on a slow CAT link takes seconds. */
int radio_controls_values_json(radio *radio_h, const char *names,
                               char *out, size_t out_len);

const char *radio_controls_kind_name(radio_ctrl_kind kind);
bool radio_controls_kind_from_name(const char *name, radio_ctrl_kind *out);

/* Look up one enumerated control by name (case-sensitive, as Hamlib spells
 * it). Returns false when the active rig does not have it. */
bool radio_controls_find(radio *radio_h, const char *name, radio_ctrl_info *out);

/* Clamp a value to a control's advertised range/step. */
double radio_controls_clamp(const radio_ctrl_info *info, double value);

/* Map a RADIO_CTRL_* code onto the RPRT number rigctld clients expect
 * (0 = OK, -1 = invalid parameter, -11 = feature not available). */
int radio_controls_rprt(int rc);

#endif /* RADIO_CONTROLS_H_ */
