/* js8_msg - what a JS8 frame says: JS8Call's Varicode, receiving side.
 *
 * A frame (js8_rx.h) carries twelve characters and three type bits: the
 * first and last frames of a message are flagged, and a data-only frame
 * too.  The twelve characters pack one of: a heartbeat or CQ (a callsign
 * and grid), a compound callsign, a directed message (from, to and a
 * command), or text, Huffman-coded or compressed against JSC's word
 * list (262144 words, dsp/js8_jsc_words.h).  This unpacks each as
 * JS8Call's DecodedText does, in its order, into the text JS8Call shows
 * and the callsigns it names; a frame that unpacks as none comes back as
 * the raw frame.
 *
 * Ported from JS8Call-improved's JS8_Main/Varicode.cpp,
 * JS8_Mode/DecodedText.cpp and JS8_JSC/JSC.cpp (GPL-3.0).
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef JS8_MSG_H
#define JS8_MSG_H

#include <stdbool.h>

#include "js8_frame.h"

/* The frame's type bits (JS8Call's TransmissionType). */
#define JS8_TX_FIRST   1
#define JS8_TX_LAST    2
#define JS8_TX_DATA    4

#define JS8_MSG_TEXT_MAX  96
#define JS8_MSG_CALL_MAX  16
#define JS8_MSG_GRID_MAX  8

typedef enum {
    JS8_MSG_RAW,              /* not unpacked */
    JS8_MSG_HEARTBEAT,        /* "CALL: @HB HEARTBEAT GRID" */
    JS8_MSG_CQ,               /* "CALL: @ALLCALL CQ CQ CQ GRID" */
    JS8_MSG_COMPOUND,         /* "CALL: " - a message's first frame */
    JS8_MSG_COMPOUND_DIRECTED,/* "CALL SNR +05" */
    JS8_MSG_DIRECTED,         /* "FROM: TO SNR -12" */
    JS8_MSG_DATA,             /* text */
} js8_msg_kind;

typedef struct {
    js8_msg_kind kind;
    char text[JS8_MSG_TEXT_MAX];   /* as JS8Call shows it */
    char from[JS8_MSG_CALL_MAX];   /* "" if the frame names none */
    char to[JS8_MSG_CALL_MAX];
    char grid[JS8_MSG_GRID_MAX];
    bool first, last;              /* the frame's place in its message */
} js8_msg;

/* What frame (twelve characters) with type bits `type` says. */
void js8_msg_unpack(const char *frame, int type, js8_msg *out);

#endif
