/* sBitx D-STAR DV modem (GMSK 4800 baud at 24 kHz)
 *
 * Port of the MMDVM firmware D-Star modulator/demodulator (DStarTX.cpp /
 * DStarRX.cpp, Copyright (C) 2009-2020 Jonathan Naylor G4KLX, GPLv2) to
 * plain C for the hermes-radio-daemon DSP.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sbitx_dstar.h"

#define DSTAR_RADIO_SYMBOL_LENGTH        5U    /* samples per symbol at 24 kHz */
#define DSTAR_FEC_SECTION_LENGTH_BYTES   83U
#define DSTAR_FEC_SECTION_LENGTH_SYMBOLS 660U
#define DSTAR_FEC_SECTION_LENGTH_SAMPLES 3300U
#define DSTAR_DATA_LENGTH_BYTES          12U
#define DSTAR_DATA_LENGTH_SYMBOLS        96U
#define DSTAR_DATA_LENGTH_SAMPLES        480U
#define DSTAR_FRAME_SYNC_LENGTH_SYMBOLS  24U
#define DSTAR_FRAME_SYNC_LENGTH_SAMPLES  120U
#define DSTAR_DATA_SYNC_LENGTH_SYMBOLS   24U
/* One superframe: the data sync recurs every 21 frames. */
#define DSTAR_SUPERFRAME_SAMPLES         (21U * DSTAR_DATA_LENGTH_SAMPLES)
#define DSTAR_DATA_SYNC_LENGTH_SAMPLES   120U
#define DSTAR_END_SYNC_LENGTH_BYTES      6U

#define FRAME_SYNC_DATA  0x0000000000557650ULL
#define FRAME_SYNC_MASK  0x0000000000FFFFFFULL
#define FRAME_SYNC_ERRS  2U
#define DATA_SYNC_DATA   0x0000000000AAB468ULL
#define DATA_SYNC_MASK   0x0000000000FFFFFFULL
#define DATA_SYNC_ERRS   2U
#define END_SYNC_DATA    0x0000AAAAAAAA135EULL
#define END_SYNC_MASK    0x0000FFFFFFFFFFFFULL
#define END_SYNC_ERRS    1U

#define NOENDPTR 9999U
#define MAX_FRAMES 150U

#define TX_TYPE_HEADER 0U
#define TX_TYPE_DATA   1U
#define TX_TYPE_EOT    2U

/* ── RX tables (from MMDVM DStarRX.cpp) ─────────────────────────── */

static const uint8_t BIT_MASK_TABLE0[] = {0x7FU, 0xBFU, 0xDFU, 0xEFU, 0xF7U, 0xFBU, 0xFDU, 0xFEU};
static const uint8_t BIT_MASK_TABLE1[] = {0x80U, 0x40U, 0x20U, 0x10U, 0x08U, 0x04U, 0x02U, 0x01U};
static const uint8_t BIT_MASK_TABLE2[] = {0xFEU, 0xFDU, 0xFBU, 0xF7U, 0xEFU, 0xDFU, 0xBFU, 0x7FU};
static const uint8_t BIT_MASK_TABLE3[] = {0x01U, 0x02U, 0x04U, 0x08U, 0x10U, 0x20U, 0x40U, 0x80U};

#define WRITE_BIT1(p, i, b) p[(i) >> 3] = (b) ? (p[(i) >> 3] | BIT_MASK_TABLE1[(i) & 7]) : (p[(i) >> 3] & BIT_MASK_TABLE0[(i) & 7])
#define READ_BIT1(p, i)     (p[(i) >> 3] & BIT_MASK_TABLE1[(i) & 7])
#define WRITE_BIT2(p, i, b) p[(i) >> 3] = (b) ? (p[(i) >> 3] | BIT_MASK_TABLE3[(i) & 7]) : (p[(i) >> 3] & BIT_MASK_TABLE2[(i) & 7])

static const uint8_t INTERLEAVE_TABLE_RX[] = {

    0x00U, 0x00U, 0x03U, 0x00U, 0x06U, 0x00U, 0x09U, 0x00U, 0x0CU, 0x00U, 0x0FU, 0x00U,
    0x12U, 0x00U, 0x15U, 0x00U, 0x18U, 0x00U, 0x1BU, 0x00U, 0x1EU, 0x00U, 0x21U, 0x00U,
    0x24U, 0x00U, 0x27U, 0x00U, 0x2AU, 0x00U, 0x2DU, 0x00U, 0x30U, 0x00U, 0x33U, 0x00U,
    0x36U, 0x00U, 0x39U, 0x00U, 0x3CU, 0x00U, 0x3FU, 0x00U, 0x42U, 0x00U, 0x45U, 0x00U,
    0x48U, 0x00U, 0x4BU, 0x00U, 0x4EU, 0x00U, 0x51U, 0x00U, 0x00U, 0x01U, 0x03U, 0x01U,
    0x06U, 0x01U, 0x09U, 0x01U, 0x0CU, 0x01U, 0x0FU, 0x01U, 0x12U, 0x01U, 0x15U, 0x01U,
    0x18U, 0x01U, 0x1BU, 0x01U, 0x1EU, 0x01U, 0x21U, 0x01U, 0x24U, 0x01U, 0x27U, 0x01U,
    0x2AU, 0x01U, 0x2DU, 0x01U, 0x30U, 0x01U, 0x33U, 0x01U, 0x36U, 0x01U, 0x39U, 0x01U,
    0x3CU, 0x01U, 0x3FU, 0x01U, 0x42U, 0x01U, 0x45U, 0x01U, 0x48U, 0x01U, 0x4BU, 0x01U,
    0x4EU, 0x01U, 0x51U, 0x01U, 0x00U, 0x02U, 0x03U, 0x02U, 0x06U, 0x02U, 0x09U, 0x02U,
    0x0CU, 0x02U, 0x0FU, 0x02U, 0x12U, 0x02U, 0x15U, 0x02U, 0x18U, 0x02U, 0x1BU, 0x02U,
    0x1EU, 0x02U, 0x21U, 0x02U, 0x24U, 0x02U, 0x27U, 0x02U, 0x2AU, 0x02U, 0x2DU, 0x02U,
    0x30U, 0x02U, 0x33U, 0x02U, 0x36U, 0x02U, 0x39U, 0x02U, 0x3CU, 0x02U, 0x3FU, 0x02U,
    0x42U, 0x02U, 0x45U, 0x02U, 0x48U, 0x02U, 0x4BU, 0x02U, 0x4EU, 0x02U, 0x51U, 0x02U,
    0x00U, 0x03U, 0x03U, 0x03U, 0x06U, 0x03U, 0x09U, 0x03U, 0x0CU, 0x03U, 0x0FU, 0x03U,
    0x12U, 0x03U, 0x15U, 0x03U, 0x18U, 0x03U, 0x1BU, 0x03U, 0x1EU, 0x03U, 0x21U, 0x03U,
    0x24U, 0x03U, 0x27U, 0x03U, 0x2AU, 0x03U, 0x2DU, 0x03U, 0x30U, 0x03U, 0x33U, 0x03U,
    0x36U, 0x03U, 0x39U, 0x03U, 0x3CU, 0x03U, 0x3FU, 0x03U, 0x42U, 0x03U, 0x45U, 0x03U,
    0x48U, 0x03U, 0x4BU, 0x03U, 0x4EU, 0x03U, 0x51U, 0x03U, 0x00U, 0x04U, 0x03U, 0x04U,
    0x06U, 0x04U, 0x09U, 0x04U, 0x0CU, 0x04U, 0x0FU, 0x04U, 0x12U, 0x04U, 0x15U, 0x04U,
    0x18U, 0x04U, 0x1BU, 0x04U, 0x1EU, 0x04U, 0x21U, 0x04U, 0x24U, 0x04U, 0x27U, 0x04U,
    0x2AU, 0x04U, 0x2DU, 0x04U, 0x30U, 0x04U, 0x33U, 0x04U, 0x36U, 0x04U, 0x39U, 0x04U,
    0x3CU, 0x04U, 0x3FU, 0x04U, 0x42U, 0x04U, 0x45U, 0x04U, 0x48U, 0x04U, 0x4BU, 0x04U,
    0x4EU, 0x04U, 0x51U, 0x04U, 0x00U, 0x05U, 0x03U, 0x05U, 0x06U, 0x05U, 0x09U, 0x05U,
    0x0CU, 0x05U, 0x0FU, 0x05U, 0x12U, 0x05U, 0x15U, 0x05U, 0x18U, 0x05U, 0x1BU, 0x05U,
    0x1EU, 0x05U, 0x21U, 0x05U, 0x24U, 0x05U, 0x27U, 0x05U, 0x2AU, 0x05U, 0x2DU, 0x05U,
    0x30U, 0x05U, 0x33U, 0x05U, 0x36U, 0x05U, 0x39U, 0x05U, 0x3CU, 0x05U, 0x3FU, 0x05U,
    0x42U, 0x05U, 0x45U, 0x05U, 0x48U, 0x05U, 0x4BU, 0x05U, 0x4EU, 0x05U, 0x51U, 0x05U,
    0x00U, 0x06U, 0x03U, 0x06U, 0x06U, 0x06U, 0x09U, 0x06U, 0x0CU, 0x06U, 0x0FU, 0x06U,
    0x12U, 0x06U, 0x15U, 0x06U, 0x18U, 0x06U, 0x1BU, 0x06U, 0x1EU, 0x06U, 0x21U, 0x06U,
    0x24U, 0x06U, 0x27U, 0x06U, 0x2AU, 0x06U, 0x2DU, 0x06U, 0x30U, 0x06U, 0x33U, 0x06U,
    0x36U, 0x06U, 0x39U, 0x06U, 0x3CU, 0x06U, 0x3FU, 0x06U, 0x42U, 0x06U, 0x45U, 0x06U,
    0x48U, 0x06U, 0x4BU, 0x06U, 0x4EU, 0x06U, 0x51U, 0x06U, 0x00U, 0x07U, 0x03U, 0x07U,
    0x06U, 0x07U, 0x09U, 0x07U, 0x0CU, 0x07U, 0x0FU, 0x07U, 0x12U, 0x07U, 0x15U, 0x07U,
    0x18U, 0x07U, 0x1BU, 0x07U, 0x1EU, 0x07U, 0x21U, 0x07U, 0x24U, 0x07U, 0x27U, 0x07U,
    0x2AU, 0x07U, 0x2DU, 0x07U, 0x30U, 0x07U, 0x33U, 0x07U, 0x36U, 0x07U, 0x39U, 0x07U,
    0x3CU, 0x07U, 0x3FU, 0x07U, 0x42U, 0x07U, 0x45U, 0x07U, 0x48U, 0x07U, 0x4BU, 0x07U,
    0x4EU, 0x07U, 0x51U, 0x07U, 0x01U, 0x00U, 0x04U, 0x00U, 0x07U, 0x00U, 0x0AU, 0x00U,
    0x0DU, 0x00U, 0x10U, 0x00U, 0x13U, 0x00U, 0x16U, 0x00U, 0x19U, 0x00U, 0x1CU, 0x00U,
    0x1FU, 0x00U, 0x22U, 0x00U, 0x25U, 0x00U, 0x28U, 0x00U, 0x2BU, 0x00U, 0x2EU, 0x00U,
    0x31U, 0x00U, 0x34U, 0x00U, 0x37U, 0x00U, 0x3AU, 0x00U, 0x3DU, 0x00U, 0x40U, 0x00U,
    0x43U, 0x00U, 0x46U, 0x00U, 0x49U, 0x00U, 0x4CU, 0x00U, 0x4FU, 0x00U, 0x52U, 0x00U,
    0x01U, 0x01U, 0x04U, 0x01U, 0x07U, 0x01U, 0x0AU, 0x01U, 0x0DU, 0x01U, 0x10U, 0x01U,
    0x13U, 0x01U, 0x16U, 0x01U, 0x19U, 0x01U, 0x1CU, 0x01U, 0x1FU, 0x01U, 0x22U, 0x01U,
    0x25U, 0x01U, 0x28U, 0x01U, 0x2BU, 0x01U, 0x2EU, 0x01U, 0x31U, 0x01U, 0x34U, 0x01U,
    0x37U, 0x01U, 0x3AU, 0x01U, 0x3DU, 0x01U, 0x40U, 0x01U, 0x43U, 0x01U, 0x46U, 0x01U,
    0x49U, 0x01U, 0x4CU, 0x01U, 0x4FU, 0x01U, 0x52U, 0x01U, 0x01U, 0x02U, 0x04U, 0x02U,
    0x07U, 0x02U, 0x0AU, 0x02U, 0x0DU, 0x02U, 0x10U, 0x02U, 0x13U, 0x02U, 0x16U, 0x02U,
    0x19U, 0x02U, 0x1CU, 0x02U, 0x1FU, 0x02U, 0x22U, 0x02U, 0x25U, 0x02U, 0x28U, 0x02U,
    0x2BU, 0x02U, 0x2EU, 0x02U, 0x31U, 0x02U, 0x34U, 0x02U, 0x37U, 0x02U, 0x3AU, 0x02U,
    0x3DU, 0x02U, 0x40U, 0x02U, 0x43U, 0x02U, 0x46U, 0x02U, 0x49U, 0x02U, 0x4CU, 0x02U,
    0x4FU, 0x02U, 0x52U, 0x02U, 0x01U, 0x03U, 0x04U, 0x03U, 0x07U, 0x03U, 0x0AU, 0x03U,
    0x0DU, 0x03U, 0x10U, 0x03U, 0x13U, 0x03U, 0x16U, 0x03U, 0x19U, 0x03U, 0x1CU, 0x03U,
    0x1FU, 0x03U, 0x22U, 0x03U, 0x25U, 0x03U, 0x28U, 0x03U, 0x2BU, 0x03U, 0x2EU, 0x03U,
    0x31U, 0x03U, 0x34U, 0x03U, 0x37U, 0x03U, 0x3AU, 0x03U, 0x3DU, 0x03U, 0x40U, 0x03U,
    0x43U, 0x03U, 0x46U, 0x03U, 0x49U, 0x03U, 0x4CU, 0x03U, 0x4FU, 0x03U, 0x52U, 0x03U,
    0x01U, 0x04U, 0x04U, 0x04U, 0x07U, 0x04U, 0x0AU, 0x04U, 0x0DU, 0x04U, 0x10U, 0x04U,
    0x13U, 0x04U, 0x16U, 0x04U, 0x19U, 0x04U, 0x1CU, 0x04U, 0x1FU, 0x04U, 0x22U, 0x04U,
    0x25U, 0x04U, 0x28U, 0x04U, 0x2BU, 0x04U, 0x2EU, 0x04U, 0x31U, 0x04U, 0x34U, 0x04U,
    0x37U, 0x04U, 0x3AU, 0x04U, 0x3DU, 0x04U, 0x40U, 0x04U, 0x43U, 0x04U, 0x46U, 0x04U,
    0x49U, 0x04U, 0x4CU, 0x04U, 0x4FU, 0x04U, 0x01U, 0x05U, 0x04U, 0x05U, 0x07U, 0x05U,
    0x0AU, 0x05U, 0x0DU, 0x05U, 0x10U, 0x05U, 0x13U, 0x05U, 0x16U, 0x05U, 0x19U, 0x05U,
    0x1CU, 0x05U, 0x1FU, 0x05U, 0x22U, 0x05U, 0x25U, 0x05U, 0x28U, 0x05U, 0x2BU, 0x05U,
    0x2EU, 0x05U, 0x31U, 0x05U, 0x34U, 0x05U, 0x37U, 0x05U, 0x3AU, 0x05U, 0x3DU, 0x05U,
    0x40U, 0x05U, 0x43U, 0x05U, 0x46U, 0x05U, 0x49U, 0x05U, 0x4CU, 0x05U, 0x4FU, 0x05U,
    0x01U, 0x06U, 0x04U, 0x06U, 0x07U, 0x06U, 0x0AU, 0x06U, 0x0DU, 0x06U, 0x10U, 0x06U,
    0x13U, 0x06U, 0x16U, 0x06U, 0x19U, 0x06U, 0x1CU, 0x06U, 0x1FU, 0x06U, 0x22U, 0x06U,
    0x25U, 0x06U, 0x28U, 0x06U, 0x2BU, 0x06U, 0x2EU, 0x06U, 0x31U, 0x06U, 0x34U, 0x06U,
    0x37U, 0x06U, 0x3AU, 0x06U, 0x3DU, 0x06U, 0x40U, 0x06U, 0x43U, 0x06U, 0x46U, 0x06U,
    0x49U, 0x06U, 0x4CU, 0x06U, 0x4FU, 0x06U, 0x01U, 0x07U, 0x04U, 0x07U, 0x07U, 0x07U,
    0x0AU, 0x07U, 0x0DU, 0x07U, 0x10U, 0x07U, 0x13U, 0x07U, 0x16U, 0x07U, 0x19U, 0x07U,
    0x1CU, 0x07U, 0x1FU, 0x07U, 0x22U, 0x07U, 0x25U, 0x07U, 0x28U, 0x07U, 0x2BU, 0x07U,
    0x2EU, 0x07U, 0x31U, 0x07U, 0x34U, 0x07U, 0x37U, 0x07U, 0x3AU, 0x07U, 0x3DU, 0x07U,
    0x40U, 0x07U, 0x43U, 0x07U, 0x46U, 0x07U, 0x49U, 0x07U, 0x4CU, 0x07U, 0x4FU, 0x07U,
    0x02U, 0x00U, 0x05U, 0x00U, 0x08U, 0x00U, 0x0BU, 0x00U, 0x0EU, 0x00U, 0x11U, 0x00U,
    0x14U, 0x00U, 0x17U, 0x00U, 0x1AU, 0x00U, 0x1DU, 0x00U, 0x20U, 0x00U, 0x23U, 0x00U,
    0x26U, 0x00U, 0x29U, 0x00U, 0x2CU, 0x00U, 0x2FU, 0x00U, 0x32U, 0x00U, 0x35U, 0x00U,
    0x38U, 0x00U, 0x3BU, 0x00U, 0x3EU, 0x00U, 0x41U, 0x00U, 0x44U, 0x00U, 0x47U, 0x00U,
    0x4AU, 0x00U, 0x4DU, 0x00U, 0x50U, 0x00U, 0x02U, 0x01U, 0x05U, 0x01U, 0x08U, 0x01U,
    0x0BU, 0x01U, 0x0EU, 0x01U, 0x11U, 0x01U, 0x14U, 0x01U, 0x17U, 0x01U, 0x1AU, 0x01U,
    0x1DU, 0x01U, 0x20U, 0x01U, 0x23U, 0x01U, 0x26U, 0x01U, 0x29U, 0x01U, 0x2CU, 0x01U,
    0x2FU, 0x01U, 0x32U, 0x01U, 0x35U, 0x01U, 0x38U, 0x01U, 0x3BU, 0x01U, 0x3EU, 0x01U,
    0x41U, 0x01U, 0x44U, 0x01U, 0x47U, 0x01U, 0x4AU, 0x01U, 0x4DU, 0x01U, 0x50U, 0x01U,
    0x02U, 0x02U, 0x05U, 0x02U, 0x08U, 0x02U, 0x0BU, 0x02U, 0x0EU, 0x02U, 0x11U, 0x02U,
    0x14U, 0x02U, 0x17U, 0x02U, 0x1AU, 0x02U, 0x1DU, 0x02U, 0x20U, 0x02U, 0x23U, 0x02U,
    0x26U, 0x02U, 0x29U, 0x02U, 0x2CU, 0x02U, 0x2FU, 0x02U, 0x32U, 0x02U, 0x35U, 0x02U,
    0x38U, 0x02U, 0x3BU, 0x02U, 0x3EU, 0x02U, 0x41U, 0x02U, 0x44U, 0x02U, 0x47U, 0x02U,
    0x4AU, 0x02U, 0x4DU, 0x02U, 0x50U, 0x02U, 0x02U, 0x03U, 0x05U, 0x03U, 0x08U, 0x03U,
    0x0BU, 0x03U, 0x0EU, 0x03U, 0x11U, 0x03U, 0x14U, 0x03U, 0x17U, 0x03U, 0x1AU, 0x03U,
    0x1DU, 0x03U, 0x20U, 0x03U, 0x23U, 0x03U, 0x26U, 0x03U, 0x29U, 0x03U, 0x2CU, 0x03U,
    0x2FU, 0x03U, 0x32U, 0x03U, 0x35U, 0x03U, 0x38U, 0x03U, 0x3BU, 0x03U, 0x3EU, 0x03U,
    0x41U, 0x03U, 0x44U, 0x03U, 0x47U, 0x03U, 0x4AU, 0x03U, 0x4DU, 0x03U, 0x50U, 0x03U,
    0x02U, 0x04U, 0x05U, 0x04U, 0x08U, 0x04U, 0x0BU, 0x04U, 0x0EU, 0x04U, 0x11U, 0x04U,
    0x14U, 0x04U, 0x17U, 0x04U, 0x1AU, 0x04U, 0x1DU, 0x04U, 0x20U, 0x04U, 0x23U, 0x04U,
    0x26U, 0x04U, 0x29U, 0x04U, 0x2CU, 0x04U, 0x2FU, 0x04U, 0x32U, 0x04U, 0x35U, 0x04U,
    0x38U, 0x04U, 0x3BU, 0x04U, 0x3EU, 0x04U, 0x41U, 0x04U, 0x44U, 0x04U, 0x47U, 0x04U,
    0x4AU, 0x04U, 0x4DU, 0x04U, 0x50U, 0x04U, 0x02U, 0x05U, 0x05U, 0x05U, 0x08U, 0x05U,
    0x0BU, 0x05U, 0x0EU, 0x05U, 0x11U, 0x05U, 0x14U, 0x05U, 0x17U, 0x05U, 0x1AU, 0x05U,
    0x1DU, 0x05U, 0x20U, 0x05U, 0x23U, 0x05U, 0x26U, 0x05U, 0x29U, 0x05U, 0x2CU, 0x05U,
    0x2FU, 0x05U, 0x32U, 0x05U, 0x35U, 0x05U, 0x38U, 0x05U, 0x3BU, 0x05U, 0x3EU, 0x05U,
    0x41U, 0x05U, 0x44U, 0x05U, 0x47U, 0x05U, 0x4AU, 0x05U, 0x4DU, 0x05U, 0x50U, 0x05U,
    0x02U, 0x06U, 0x05U, 0x06U, 0x08U, 0x06U, 0x0BU, 0x06U, 0x0EU, 0x06U, 0x11U, 0x06U,
    0x14U, 0x06U, 0x17U, 0x06U, 0x1AU, 0x06U, 0x1DU, 0x06U, 0x20U, 0x06U, 0x23U, 0x06U,
    0x26U, 0x06U, 0x29U, 0x06U, 0x2CU, 0x06U, 0x2FU, 0x06U, 0x32U, 0x06U, 0x35U, 0x06U,
    0x38U, 0x06U, 0x3BU, 0x06U, 0x3EU, 0x06U, 0x41U, 0x06U, 0x44U, 0x06U, 0x47U, 0x06U,
    0x4AU, 0x06U, 0x4DU, 0x06U, 0x50U, 0x06U, 0x02U, 0x07U, 0x05U, 0x07U, 0x08U, 0x07U,
    0x0BU, 0x07U, 0x0EU, 0x07U, 0x11U, 0x07U, 0x14U, 0x07U, 0x17U, 0x07U, 0x1AU, 0x07U,
    0x1DU, 0x07U, 0x20U, 0x07U, 0x23U, 0x07U, 0x26U, 0x07U, 0x29U, 0x07U, 0x2CU, 0x07U,
    0x2FU, 0x07U, 0x32U, 0x07U, 0x35U, 0x07U, 0x38U, 0x07U, 0x3BU, 0x07U, 0x3EU, 0x07U,
    0x41U, 0x07U, 0x44U, 0x07U, 0x47U, 0x07U, 0x4AU, 0x07U, 0x4DU, 0x07U, 0x50U, 0x07U,
};

static const uint8_t SCRAMBLE_TABLE_RX[] = {

    0x70U, 0x4FU, 0x93U, 0x40U, 0x64U, 0x74U, 0x6DU, 0x30U, 0x2BU, 0xE7U, 0x2DU, 0x54U,
    0x5FU, 0x8AU, 0x1DU, 0x7FU, 0xB8U, 0xA7U, 0x49U, 0x20U, 0x32U, 0xBAU, 0x36U, 0x98U,
    0x95U, 0xF3U, 0x16U, 0xAAU, 0x2FU, 0xC5U, 0x8EU, 0x3FU, 0xDCU, 0xD3U, 0x24U, 0x10U,
    0x19U, 0x5DU, 0x1BU, 0xCCU, 0xCAU, 0x79U, 0x0BU, 0xD5U, 0x97U, 0x62U, 0xC7U, 0x1FU,
    0xEEU, 0x69U, 0x12U, 0x88U, 0x8CU, 0xAEU, 0x0DU, 0x66U, 0xE5U, 0xBCU, 0x85U, 0xEAU,
    0x4BU, 0xB1U, 0xE3U, 0x0FU, 0xF7U, 0x34U, 0x09U, 0x44U, 0x46U, 0xD7U, 0x06U, 0xB3U,
    0x72U, 0xDEU, 0x42U, 0xF5U, 0xA5U, 0xD8U, 0xF1U, 0x87U, 0x7BU, 0x9AU, 0x04U, 0x22U,
    0xA3U, 0x6BU, 0x83U, 0x59U, 0x39U, 0x6FU, 0x00U,
};

static const uint16_t CCITT_TABLE[] = {

    0x0000U, 0x1189U, 0x2312U, 0x329BU, 0x4624U, 0x57ADU, 0x6536U, 0x74BFU,
    0x8C48U, 0x9DC1U, 0xAF5AU, 0xBED3U, 0xCA6CU, 0xDBE5U, 0xE97EU, 0xF8F7U,
    0x1081U, 0x0108U, 0x3393U, 0x221AU, 0x56A5U, 0x472CU, 0x75B7U, 0x643EU,
    0x9CC9U, 0x8D40U, 0xBFDBU, 0xAE52U, 0xDAEDU, 0xCB64U, 0xF9FFU, 0xE876U,
    0x2102U, 0x308BU, 0x0210U, 0x1399U, 0x6726U, 0x76AFU, 0x4434U, 0x55BDU,
    0xAD4AU, 0xBCC3U, 0x8E58U, 0x9FD1U, 0xEB6EU, 0xFAE7U, 0xC87CU, 0xD9F5U,
    0x3183U, 0x200AU, 0x1291U, 0x0318U, 0x77A7U, 0x662EU, 0x54B5U, 0x453CU,
    0xBDCBU, 0xAC42U, 0x9ED9U, 0x8F50U, 0xFBEFU, 0xEA66U, 0xD8FDU, 0xC974U,
    0x4204U, 0x538DU, 0x6116U, 0x709FU, 0x0420U, 0x15A9U, 0x2732U, 0x36BBU,
    0xCE4CU, 0xDFC5U, 0xED5EU, 0xFCD7U, 0x8868U, 0x99E1U, 0xAB7AU, 0xBAF3U,
    0x5285U, 0x430CU, 0x7197U, 0x601EU, 0x14A1U, 0x0528U, 0x37B3U, 0x263AU,
    0xDECDU, 0xCF44U, 0xFDDFU, 0xEC56U, 0x98E9U, 0x8960U, 0xBBFBU, 0xAA72U,
    0x6306U, 0x728FU, 0x4014U, 0x519DU, 0x2522U, 0x34ABU, 0x0630U, 0x17B9U,
    0xEF4EU, 0xFEC7U, 0xCC5CU, 0xDDD5U, 0xA96AU, 0xB8E3U, 0x8A78U, 0x9BF1U,
    0x7387U, 0x620EU, 0x5095U, 0x411CU, 0x35A3U, 0x242AU, 0x16B1U, 0x0738U,
    0xFFCFU, 0xEE46U, 0xDCDDU, 0xCD54U, 0xB9EBU, 0xA862U, 0x9AF9U, 0x8B70U,
    0x8408U, 0x9581U, 0xA71AU, 0xB693U, 0xC22CU, 0xD3A5U, 0xE13EU, 0xF0B7U,
    0x0840U, 0x19C9U, 0x2B52U, 0x3ADBU, 0x4E64U, 0x5FEDU, 0x6D76U, 0x7CFFU,
    0x9489U, 0x8500U, 0xB79BU, 0xA612U, 0xD2ADU, 0xC324U, 0xF1BFU, 0xE036U,
    0x18C1U, 0x0948U, 0x3BD3U, 0x2A5AU, 0x5EE5U, 0x4F6CU, 0x7DF7U, 0x6C7EU,
    0xA50AU, 0xB483U, 0x8618U, 0x9791U, 0xE32EU, 0xF2A7U, 0xC03CU, 0xD1B5U,
    0x2942U, 0x38CBU, 0x0A50U, 0x1BD9U, 0x6F66U, 0x7EEFU, 0x4C74U, 0x5DFDU,
    0xB58BU, 0xA402U, 0x9699U, 0x8710U, 0xF3AFU, 0xE226U, 0xD0BDU, 0xC134U,
    0x39C3U, 0x284AU, 0x1AD1U, 0x0B58U, 0x7FE7U, 0x6E6EU, 0x5CF5U, 0x4D7CU,
    0xC60CU, 0xD785U, 0xE51EU, 0xF497U, 0x8028U, 0x91A1U, 0xA33AU, 0xB2B3U,
    0x4A44U, 0x5BCDU, 0x6956U, 0x78DFU, 0x0C60U, 0x1DE9U, 0x2F72U, 0x3EFBU,
    0xD68DU, 0xC704U, 0xF59FU, 0xE416U, 0x90A9U, 0x8120U, 0xB3BBU, 0xA232U,
    0x5AC5U, 0x4B4CU, 0x79D7U, 0x685EU, 0x1CE1U, 0x0D68U, 0x3FF3U, 0x2E7AU,
    0xE70EU, 0xF687U, 0xC41CU, 0xD595U, 0xA12AU, 0xB0A3U, 0x8238U, 0x93B1U,
    0x6B46U, 0x7ACFU, 0x4854U, 0x59DDU, 0x2D62U, 0x3CEBU, 0x0E70U, 0x1FF9U,
    0xF78FU, 0xE606U, 0xD49DU, 0xC514U, 0xB1ABU, 0xA022U, 0x92B9U, 0x8330U,
    0x7BC7U, 0x6A4EU, 0x58D5U, 0x495CU, 0x3DE3U, 0x2C6AU, 0x1EF1U, 0x0F78U,
};

/* ── TX tables (from MMDVM DStarTX.cpp) ─────────────────────────── */

static const uint8_t BIT_MASK_TABLE[] = {0x80U, 0x40U, 0x20U, 0x10U, 0x08U, 0x04U, 0x02U, 0x01U};

static const uint8_t INTERLEAVE_TABLE_TX[] = {

    0x00U, 0x04U, 0x04U, 0x00U, 0x07U, 0x04U, 0x0BU, 0x00U, 0x0EU, 0x04U, 0x12U, 0x00U,
    0x15U, 0x04U, 0x19U, 0x00U, 0x1CU, 0x04U, 0x20U, 0x00U, 0x23U, 0x04U, 0x27U, 0x00U,
    0x2AU, 0x04U, 0x2DU, 0x07U, 0x31U, 0x02U, 0x34U, 0x05U, 0x38U, 0x00U, 0x3BU, 0x03U,
    0x3EU, 0x06U, 0x42U, 0x01U, 0x45U, 0x04U, 0x48U, 0x07U, 0x4CU, 0x02U, 0x4FU, 0x05U,
    0x00U, 0x05U, 0x04U, 0x01U, 0x07U, 0x05U, 0x0BU, 0x01U, 0x0EU, 0x05U, 0x12U, 0x01U,
    0x15U, 0x05U, 0x19U, 0x01U, 0x1CU, 0x05U, 0x20U, 0x01U, 0x23U, 0x05U, 0x27U, 0x01U,
    0x2AU, 0x05U, 0x2EU, 0x00U, 0x31U, 0x03U, 0x34U, 0x06U, 0x38U, 0x01U, 0x3BU, 0x04U,
    0x3EU, 0x07U, 0x42U, 0x02U, 0x45U, 0x05U, 0x49U, 0x00U, 0x4CU, 0x03U, 0x4FU, 0x06U,
    0x00U, 0x06U, 0x04U, 0x02U, 0x07U, 0x06U, 0x0BU, 0x02U, 0x0EU, 0x06U, 0x12U, 0x02U,
    0x15U, 0x06U, 0x19U, 0x02U, 0x1CU, 0x06U, 0x20U, 0x02U, 0x23U, 0x06U, 0x27U, 0x02U,
    0x2AU, 0x06U, 0x2EU, 0x01U, 0x31U, 0x04U, 0x34U, 0x07U, 0x38U, 0x02U, 0x3BU, 0x05U,
    0x3FU, 0x00U, 0x42U, 0x03U, 0x45U, 0x06U, 0x49U, 0x01U, 0x4CU, 0x04U, 0x4FU, 0x07U,
    0x00U, 0x07U, 0x04U, 0x03U, 0x07U, 0x07U, 0x0BU, 0x03U, 0x0EU, 0x07U, 0x12U, 0x03U,
    0x15U, 0x07U, 0x19U, 0x03U, 0x1CU, 0x07U, 0x20U, 0x03U, 0x23U, 0x07U, 0x27U, 0x03U,
    0x2AU, 0x07U, 0x2EU, 0x02U, 0x31U, 0x05U, 0x35U, 0x00U, 0x38U, 0x03U, 0x3BU, 0x06U,
    0x3FU, 0x01U, 0x42U, 0x04U, 0x45U, 0x07U, 0x49U, 0x02U, 0x4CU, 0x05U, 0x50U, 0x00U,
    0x01U, 0x00U, 0x04U, 0x04U, 0x08U, 0x00U, 0x0BU, 0x04U, 0x0FU, 0x00U, 0x12U, 0x04U,
    0x16U, 0x00U, 0x19U, 0x04U, 0x1DU, 0x00U, 0x20U, 0x04U, 0x24U, 0x00U, 0x27U, 0x04U,
    0x2BU, 0x00U, 0x2EU, 0x03U, 0x31U, 0x06U, 0x35U, 0x01U, 0x38U, 0x04U, 0x3BU, 0x07U,
    0x3FU, 0x02U, 0x42U, 0x05U, 0x46U, 0x00U, 0x49U, 0x03U, 0x4CU, 0x06U, 0x50U, 0x01U,
    0x01U, 0x01U, 0x04U, 0x05U, 0x08U, 0x01U, 0x0BU, 0x05U, 0x0FU, 0x01U, 0x12U, 0x05U,
    0x16U, 0x01U, 0x19U, 0x05U, 0x1DU, 0x01U, 0x20U, 0x05U, 0x24U, 0x01U, 0x27U, 0x05U,
    0x2BU, 0x01U, 0x2EU, 0x04U, 0x31U, 0x07U, 0x35U, 0x02U, 0x38U, 0x05U, 0x3CU, 0x00U,
    0x3FU, 0x03U, 0x42U, 0x06U, 0x46U, 0x01U, 0x49U, 0x04U, 0x4CU, 0x07U, 0x50U, 0x02U,
    0x01U, 0x02U, 0x04U, 0x06U, 0x08U, 0x02U, 0x0BU, 0x06U, 0x0FU, 0x02U, 0x12U, 0x06U,
    0x16U, 0x02U, 0x19U, 0x06U, 0x1DU, 0x02U, 0x20U, 0x06U, 0x24U, 0x02U, 0x27U, 0x06U,
    0x2BU, 0x02U, 0x2EU, 0x05U, 0x32U, 0x00U, 0x35U, 0x03U, 0x38U, 0x06U, 0x3CU, 0x01U,
    0x3FU, 0x04U, 0x42U, 0x07U, 0x46U, 0x02U, 0x49U, 0x05U, 0x4DU, 0x00U, 0x50U, 0x03U,
    0x01U, 0x03U, 0x04U, 0x07U, 0x08U, 0x03U, 0x0BU, 0x07U, 0x0FU, 0x03U, 0x12U, 0x07U,
    0x16U, 0x03U, 0x19U, 0x07U, 0x1DU, 0x03U, 0x20U, 0x07U, 0x24U, 0x03U, 0x27U, 0x07U,
    0x2BU, 0x03U, 0x2EU, 0x06U, 0x32U, 0x01U, 0x35U, 0x04U, 0x38U, 0x07U, 0x3CU, 0x02U,
    0x3FU, 0x05U, 0x43U, 0x00U, 0x46U, 0x03U, 0x49U, 0x06U, 0x4DU, 0x01U, 0x50U, 0x04U,
    0x01U, 0x04U, 0x05U, 0x00U, 0x08U, 0x04U, 0x0CU, 0x00U, 0x0FU, 0x04U, 0x13U, 0x00U,
    0x16U, 0x04U, 0x1AU, 0x00U, 0x1DU, 0x04U, 0x21U, 0x00U, 0x24U, 0x04U, 0x28U, 0x00U,
    0x2BU, 0x04U, 0x2EU, 0x07U, 0x32U, 0x02U, 0x35U, 0x05U, 0x39U, 0x00U, 0x3CU, 0x03U,
    0x3FU, 0x06U, 0x43U, 0x01U, 0x46U, 0x04U, 0x49U, 0x07U, 0x4DU, 0x02U, 0x50U, 0x05U,
    0x01U, 0x05U, 0x05U, 0x01U, 0x08U, 0x05U, 0x0CU, 0x01U, 0x0FU, 0x05U, 0x13U, 0x01U,
    0x16U, 0x05U, 0x1AU, 0x01U, 0x1DU, 0x05U, 0x21U, 0x01U, 0x24U, 0x05U, 0x28U, 0x01U,
    0x2BU, 0x05U, 0x2FU, 0x00U, 0x32U, 0x03U, 0x35U, 0x06U, 0x39U, 0x01U, 0x3CU, 0x04U,
    0x3FU, 0x07U, 0x43U, 0x02U, 0x46U, 0x05U, 0x4AU, 0x00U, 0x4DU, 0x03U, 0x50U, 0x06U,
    0x01U, 0x06U, 0x05U, 0x02U, 0x08U, 0x06U, 0x0CU, 0x02U, 0x0FU, 0x06U, 0x13U, 0x02U,
    0x16U, 0x06U, 0x1AU, 0x02U, 0x1DU, 0x06U, 0x21U, 0x02U, 0x24U, 0x06U, 0x28U, 0x02U,
    0x2BU, 0x06U, 0x2FU, 0x01U, 0x32U, 0x04U, 0x35U, 0x07U, 0x39U, 0x02U, 0x3CU, 0x05U,
    0x40U, 0x00U, 0x43U, 0x03U, 0x46U, 0x06U, 0x4AU, 0x01U, 0x4DU, 0x04U, 0x50U, 0x07U,
    0x01U, 0x07U, 0x05U, 0x03U, 0x08U, 0x07U, 0x0CU, 0x03U, 0x0FU, 0x07U, 0x13U, 0x03U,
    0x16U, 0x07U, 0x1AU, 0x03U, 0x1DU, 0x07U, 0x21U, 0x03U, 0x24U, 0x07U, 0x28U, 0x03U,
    0x2BU, 0x07U, 0x2FU, 0x02U, 0x32U, 0x05U, 0x36U, 0x00U, 0x39U, 0x03U, 0x3CU, 0x06U,
    0x40U, 0x01U, 0x43U, 0x04U, 0x46U, 0x07U, 0x4AU, 0x02U, 0x4DU, 0x05U, 0x51U, 0x00U,
    0x02U, 0x00U, 0x05U, 0x04U, 0x09U, 0x00U, 0x0CU, 0x04U, 0x10U, 0x00U, 0x13U, 0x04U,
    0x17U, 0x00U, 0x1AU, 0x04U, 0x1EU, 0x00U, 0x21U, 0x04U, 0x25U, 0x00U, 0x28U, 0x04U,
    0x2CU, 0x00U, 0x2FU, 0x03U, 0x32U, 0x06U, 0x36U, 0x01U, 0x39U, 0x04U, 0x3CU, 0x07U,
    0x40U, 0x02U, 0x43U, 0x05U, 0x47U, 0x00U, 0x4AU, 0x03U, 0x4DU, 0x06U, 0x51U, 0x01U,
    0x02U, 0x01U, 0x05U, 0x05U, 0x09U, 0x01U, 0x0CU, 0x05U, 0x10U, 0x01U, 0x13U, 0x05U,
    0x17U, 0x01U, 0x1AU, 0x05U, 0x1EU, 0x01U, 0x21U, 0x05U, 0x25U, 0x01U, 0x28U, 0x05U,
    0x2CU, 0x01U, 0x2FU, 0x04U, 0x32U, 0x07U, 0x36U, 0x02U, 0x39U, 0x05U, 0x3DU, 0x00U,
    0x40U, 0x03U, 0x43U, 0x06U, 0x47U, 0x01U, 0x4AU, 0x04U, 0x4DU, 0x07U, 0x51U, 0x02U,
    0x02U, 0x02U, 0x05U, 0x06U, 0x09U, 0x02U, 0x0CU, 0x06U, 0x10U, 0x02U, 0x13U, 0x06U,
    0x17U, 0x02U, 0x1AU, 0x06U, 0x1EU, 0x02U, 0x21U, 0x06U, 0x25U, 0x02U, 0x28U, 0x06U,
    0x2CU, 0x02U, 0x2FU, 0x05U, 0x33U, 0x00U, 0x36U, 0x03U, 0x39U, 0x06U, 0x3DU, 0x01U,
    0x40U, 0x04U, 0x43U, 0x07U, 0x47U, 0x02U, 0x4AU, 0x05U, 0x4EU, 0x00U, 0x51U, 0x03U,
    0x02U, 0x03U, 0x05U, 0x07U, 0x09U, 0x03U, 0x0CU, 0x07U, 0x10U, 0x03U, 0x13U, 0x07U,
    0x17U, 0x03U, 0x1AU, 0x07U, 0x1EU, 0x03U, 0x21U, 0x07U, 0x25U, 0x03U, 0x28U, 0x07U,
    0x2CU, 0x03U, 0x2FU, 0x06U, 0x33U, 0x01U, 0x36U, 0x04U, 0x39U, 0x07U, 0x3DU, 0x02U,
    0x40U, 0x05U, 0x44U, 0x00U, 0x47U, 0x03U, 0x4AU, 0x06U, 0x4EU, 0x01U, 0x51U, 0x04U,
    0x02U, 0x04U, 0x06U, 0x00U, 0x09U, 0x04U, 0x0DU, 0x00U, 0x10U, 0x04U, 0x14U, 0x00U,
    0x17U, 0x04U, 0x1BU, 0x00U, 0x1EU, 0x04U, 0x22U, 0x00U, 0x25U, 0x04U, 0x29U, 0x00U,
    0x2CU, 0x04U, 0x2FU, 0x07U, 0x33U, 0x02U, 0x36U, 0x05U, 0x3AU, 0x00U, 0x3DU, 0x03U,
    0x40U, 0x06U, 0x44U, 0x01U, 0x47U, 0x04U, 0x4AU, 0x07U, 0x4EU, 0x02U, 0x51U, 0x05U,
    0x02U, 0x05U, 0x06U, 0x01U, 0x09U, 0x05U, 0x0DU, 0x01U, 0x10U, 0x05U, 0x14U, 0x01U,
    0x17U, 0x05U, 0x1BU, 0x01U, 0x1EU, 0x05U, 0x22U, 0x01U, 0x25U, 0x05U, 0x29U, 0x01U,
    0x2CU, 0x05U, 0x30U, 0x00U, 0x33U, 0x03U, 0x36U, 0x06U, 0x3AU, 0x01U, 0x3DU, 0x04U,
    0x40U, 0x07U, 0x44U, 0x02U, 0x47U, 0x05U, 0x4BU, 0x00U, 0x4EU, 0x03U, 0x51U, 0x06U,
    0x02U, 0x06U, 0x06U, 0x02U, 0x09U, 0x06U, 0x0DU, 0x02U, 0x10U, 0x06U, 0x14U, 0x02U,
    0x17U, 0x06U, 0x1BU, 0x02U, 0x1EU, 0x06U, 0x22U, 0x02U, 0x25U, 0x06U, 0x29U, 0x02U,
    0x2CU, 0x06U, 0x30U, 0x01U, 0x33U, 0x04U, 0x36U, 0x07U, 0x3AU, 0x02U, 0x3DU, 0x05U,
    0x41U, 0x00U, 0x44U, 0x03U, 0x47U, 0x06U, 0x4BU, 0x01U, 0x4EU, 0x04U, 0x51U, 0x07U,
    0x02U, 0x07U, 0x06U, 0x03U, 0x09U, 0x07U, 0x0DU, 0x03U, 0x10U, 0x07U, 0x14U, 0x03U,
    0x17U, 0x07U, 0x1BU, 0x03U, 0x1EU, 0x07U, 0x22U, 0x03U, 0x25U, 0x07U, 0x29U, 0x03U,
    0x2CU, 0x07U, 0x30U, 0x02U, 0x33U, 0x05U, 0x37U, 0x00U, 0x3AU, 0x03U, 0x3DU, 0x06U,
    0x41U, 0x01U, 0x44U, 0x04U, 0x47U, 0x07U, 0x4BU, 0x02U, 0x4EU, 0x05U, 0x52U, 0x00U,
    0x03U, 0x00U, 0x06U, 0x04U, 0x0AU, 0x00U, 0x0DU, 0x04U, 0x11U, 0x00U, 0x14U, 0x04U,
    0x18U, 0x00U, 0x1BU, 0x04U, 0x1FU, 0x00U, 0x22U, 0x04U, 0x26U, 0x00U, 0x29U, 0x04U,
    0x2DU, 0x00U, 0x30U, 0x03U, 0x33U, 0x06U, 0x37U, 0x01U, 0x3AU, 0x04U, 0x3DU, 0x07U,
    0x41U, 0x02U, 0x44U, 0x05U, 0x48U, 0x00U, 0x4BU, 0x03U, 0x4EU, 0x06U, 0x52U, 0x01U,
    0x03U, 0x01U, 0x06U, 0x05U, 0x0AU, 0x01U, 0x0DU, 0x05U, 0x11U, 0x01U, 0x14U, 0x05U,
    0x18U, 0x01U, 0x1BU, 0x05U, 0x1FU, 0x01U, 0x22U, 0x05U, 0x26U, 0x01U, 0x29U, 0x05U,
    0x2DU, 0x01U, 0x30U, 0x04U, 0x33U, 0x07U, 0x37U, 0x02U, 0x3AU, 0x05U, 0x3EU, 0x00U,
    0x41U, 0x03U, 0x44U, 0x06U, 0x48U, 0x01U, 0x4BU, 0x04U, 0x4EU, 0x07U, 0x52U, 0x02U,
    0x03U, 0x02U, 0x06U, 0x06U, 0x0AU, 0x02U, 0x0DU, 0x06U, 0x11U, 0x02U, 0x14U, 0x06U,
    0x18U, 0x02U, 0x1BU, 0x06U, 0x1FU, 0x02U, 0x22U, 0x06U, 0x26U, 0x02U, 0x29U, 0x06U,
    0x2DU, 0x02U, 0x30U, 0x05U, 0x34U, 0x00U, 0x37U, 0x03U, 0x3AU, 0x06U, 0x3EU, 0x01U,
    0x41U, 0x04U, 0x44U, 0x07U, 0x48U, 0x02U, 0x4BU, 0x05U, 0x4FU, 0x00U, 0x52U, 0x03U,
    0x03U, 0x03U, 0x06U, 0x07U, 0x0AU, 0x03U, 0x0DU, 0x07U, 0x11U, 0x03U, 0x14U, 0x07U,
    0x18U, 0x03U, 0x1BU, 0x07U, 0x1FU, 0x03U, 0x22U, 0x07U, 0x26U, 0x03U, 0x29U, 0x07U,
    0x2DU, 0x03U, 0x30U, 0x06U, 0x34U, 0x01U, 0x37U, 0x04U, 0x3AU, 0x07U, 0x3EU, 0x02U,
    0x41U, 0x05U, 0x45U, 0x00U, 0x48U, 0x03U, 0x4BU, 0x06U, 0x4FU, 0x01U, 0x52U, 0x04U,
    0x03U, 0x04U, 0x07U, 0x00U, 0x0AU, 0x04U, 0x0EU, 0x00U, 0x11U, 0x04U, 0x15U, 0x00U,
    0x18U, 0x04U, 0x1CU, 0x00U, 0x1FU, 0x04U, 0x23U, 0x00U, 0x26U, 0x04U, 0x2AU, 0x00U,
    0x2DU, 0x04U, 0x30U, 0x07U, 0x34U, 0x02U, 0x37U, 0x05U, 0x3BU, 0x00U, 0x3EU, 0x03U,
    0x41U, 0x06U, 0x45U, 0x01U, 0x48U, 0x04U, 0x4BU, 0x07U, 0x4FU, 0x02U, 0x52U, 0x05U,
    0x03U, 0x05U, 0x07U, 0x01U, 0x0AU, 0x05U, 0x0EU, 0x01U, 0x11U, 0x05U, 0x15U, 0x01U,
    0x18U, 0x05U, 0x1CU, 0x01U, 0x1FU, 0x05U, 0x23U, 0x01U, 0x26U, 0x05U, 0x2AU, 0x01U,
    0x2DU, 0x05U, 0x31U, 0x00U, 0x34U, 0x03U, 0x37U, 0x06U, 0x3BU, 0x01U, 0x3EU, 0x04U,
    0x41U, 0x07U, 0x45U, 0x02U, 0x48U, 0x05U, 0x4CU, 0x00U, 0x4FU, 0x03U, 0x52U, 0x06U,
    0x03U, 0x06U, 0x07U, 0x02U, 0x0AU, 0x06U, 0x0EU, 0x02U, 0x11U, 0x06U, 0x15U, 0x02U,
    0x18U, 0x06U, 0x1CU, 0x02U, 0x1FU, 0x06U, 0x23U, 0x02U, 0x26U, 0x06U, 0x2AU, 0x02U,
    0x2DU, 0x06U, 0x31U, 0x01U, 0x34U, 0x04U, 0x37U, 0x07U, 0x3BU, 0x02U, 0x3EU, 0x05U,
    0x42U, 0x00U, 0x45U, 0x03U, 0x48U, 0x06U, 0x4CU, 0x01U, 0x4FU, 0x04U, 0x52U, 0x07U,
    0x03U, 0x07U, 0x07U, 0x03U, 0x0AU, 0x07U, 0x0EU, 0x03U, 0x11U, 0x07U, 0x15U, 0x03U,
    0x18U, 0x07U, 0x1CU, 0x03U, 0x1FU, 0x07U, 0x23U, 0x03U, 0x26U, 0x07U, 0x2AU, 0x03U,
};

static const uint8_t SCRAMBLE_TABLE_TX[] = {

    0x00U, 0xF7U, 0x34U, 0x09U, 0x44U, 0x46U, 0xD7U, 0x06U, 0xB3U, 0x72U, 0xDEU, 0x42U,
    0xF5U, 0xA5U, 0xD8U, 0xF1U, 0x87U, 0x7BU, 0x9AU, 0x04U, 0x22U, 0xA3U, 0x6BU, 0x83U,
    0x59U, 0x39U, 0x6FU, 0xA1U, 0xFAU, 0x52U, 0xECU, 0xF8U, 0xC3U, 0x3DU, 0x4DU, 0x02U,
    0x91U, 0xD1U, 0xB5U, 0xC1U, 0xACU, 0x9CU, 0xB7U, 0x50U, 0x7DU, 0x29U, 0x76U, 0xFCU,
    0xE1U, 0x9EU, 0x26U, 0x81U, 0xC8U, 0xE8U, 0xDAU, 0x60U, 0x56U, 0xCEU, 0x5BU, 0xA8U,
    0xBEU, 0x14U, 0x3BU, 0xFEU, 0x70U, 0x4FU, 0x93U, 0x40U, 0x64U, 0x74U, 0x6DU, 0x30U,
    0x2BU, 0xE7U, 0x2DU, 0x54U, 0x5FU, 0x8AU, 0x1DU, 0x7FU, 0xB8U, 0xA7U, 0x49U, 0x20U,
    0x32U, 0xBAU, 0x36U, 0x98U, 0x95U, 0xF3U, 0x06U,
};

/* gaussfir(0.35, 1, 5) — time-reversed per the CMSIS convention. */
static const float GAUSSIAN_0_35_FILTER[15] = {
    0.0f, 0.0f, 0.0f, 0.0f, 1001.0f / 32768.0f, 3514.0f / 32768.0f, 9333.0f / 32768.0f,
    18751.0f / 32768.0f, 28499.0f / 32768.0f, 32767.0f / 32768.0f, 28499.0f / 32768.0f,
    18751.0f / 32768.0f, 9333.0f / 32768.0f, 3514.0f / 32768.0f, 1001.0f / 32768.0f
};

#define DSTAR_LEVEL0 (-841.0f / 32768.0f)
#define DSTAR_LEVEL1 (841.0f / 32768.0f)

/* Gaussian BT=0.5 matched filter (MMDVM's GAUSSIAN_0_5_FILTER), normalised
 * to unit sum. Applied to the discriminator at 24 kHz before slicing. */
static const float GAUSSIAN_0_5_FILTER[11] = {
    8.0f / 39760.0f, 104.0f / 39760.0f, 760.0f / 39760.0f, 3158.0f / 39760.0f,
    7421.0f / 39760.0f, 9866.0f / 39760.0f, 7421.0f / 39760.0f, 3158.0f / 39760.0f,
    760.0f / 39760.0f, 104.0f / 39760.0f, 8.0f / 39760.0f
};

static const uint8_t DSTAR_DATA_SYNC_BYTES[] = {0x9E, 0x8D, 0x32, 0x88, 0x26, 0x1A, 0x3F, 0x61, 0xE8, 0x55, 0x2D, 0x16};
static const uint8_t DSTAR_END_SYNC_BYTES[] = {0x55, 0x55, 0x55, 0x55, 0xC8, 0x7A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static const bool DSTAR_FRAME_SYNC_SYMBOLS[] = {
    false, true, false, true, false, true, false, true, false, true, true, true,
    false, true, true, false, false, true, false, true, false, false, false, false
};

static const bool DSTAR_DATA_SYNC_SYMBOLS[] = {
    true, false, true, false, true, false, true, false, true, false, true, true,
    false, true, false, false, false, true, true, false, true, false, false, false
};

static unsigned int
countBits64(uint64_t value)
{
    return (unsigned int)__builtin_popcountll(value);
}

/* ── RX state machine ───────────────────────────────────────────── */

struct sbitx_dstar_rx {
    int state;                          /* 0 none, 1 header, 2 data */
    uint64_t bit_buffer[DSTAR_RADIO_SYMBOL_LENGTH];
    float    header_buffer[DSTAR_FEC_SECTION_LENGTH_SAMPLES + 2U * DSTAR_RADIO_SYMBOL_LENGTH];
    float    data_buffer[DSTAR_DATA_LENGTH_SAMPLES];
    uint16_t bit_ptr;

    /* Sampling-clock observation.
     *
     * At lock, consecutive data syncs are exactly one superframe apart --
     * 21 frames of 480 samples. Any surplus or shortfall in OUR samples is
     * the difference between the transmitter's symbol clock and our sampling
     * clock, which is what dstar_clock_ppm corrects. The modem can only
     * re-align its own sampling pointer by +/-1 sample per superframe
     * (max_sync_ptr = sync_ptr + 1), i.e. about 99 ppm of tracking range, so
     * anything larger has to be fixed upstream in the resampler. */
    /* Diagnostics: where header recovery succeeds or fails. */
    uint32_t stat_frame_sync;    /* header preamble correlations accepted */
    uint32_t stat_header_ok;     /* header decoded and CRC verified */
    uint32_t stat_header_bad;    /* header collected but CRC rejected */
    uint32_t stat_header_soft_ok;/* of the good ones, how many the soft path got */
    uint32_t stat_data_sync;     /* data syncs accepted (lock without a header) */
    uint32_t stat_header_slow;   /* headers recovered from the slow-data stream */

    /* Slow-data header assembly.
     *
     * Besides the header burst at the start of an over, D-STAR repeats the
     * whole 41-byte header in the 3 slow-data bytes of every voice frame:
     * 6-byte units spanning 2 frames, each starting with a type byte (0x5n
     * = header, n data bytes), restarting at every data sync. That gives a
     * fresh header roughly every 420 ms, so a receiver tuning in mid-over --
     * or one that simply missed the single header burst, which is most of
     * them -- still gets the callsigns. The stream carries no FEC, so the
     * assembled header is only trusted once its CRC16 verifies. */
    void (*slow_debug_cb)(void *user, const uint8_t *hdr41, bool crc_ok);
    void (*burst_debug_cb)(void *user, const uint8_t *hdr41, bool crc_ok, bool soft, int32_t corr);
    uint8_t  slow_unit[6];
    uint8_t  slow_unit_n;
    uint8_t  slow_header[SBITX_DSTAR_HEADER_BYTES];
    uint8_t  slow_header_n;
    /* Per-bit vote counters across superframes. The header repeats
     * identically about 2.4 times a second and the slow-data stream has no
     * FEC, so a single stray bit fails the CRC for that superframe -- but
     * the errors land in different places each time, and voting bit by bit
     * across a few repeats recovers a clean header in ~2 s. */
    uint8_t  slow_vote[SBITX_DSTAR_HEADER_BYTES][8];
    uint8_t  slow_vote_n;
    uint8_t  last_header[SBITX_DSTAR_HEADER_BYTES];
    bool     last_header_valid;

    uint64_t sample_count;
    uint64_t last_sync_sample;
    bool     last_sync_valid;
    int64_t  drift_samples;   /* accumulated surplus samples */
    uint64_t drift_span;      /* samples the accumulation covers */
    uint16_t header_ptr;
    uint16_t data_ptr;
    uint16_t start_ptr;
    uint16_t sync_ptr;
    uint16_t min_sync_ptr;
    uint16_t max_sync_ptr;
    int32_t  max_frame_corr;
    int32_t  max_data_corr;
    uint16_t frame_count;
    uint8_t  countdown;
    unsigned int mar;
    int     path_metric[4];
    uint32_t path_memory0[42];
    uint32_t path_memory1[42];
    uint32_t path_memory2[42];
    uint32_t path_memory3[42];
    uint8_t  fec_output[42];

    /* Discriminator front-end (MMDVM-style): slow DC tracker, Gaussian
     * BT=0.5 matched filter, and an optional polarity flip. Without the
     * matched filter the zero-crossing slicer below is dominated by noise
     * and real D-Star signals never sync. */
    float    mf_state[11];
    float    dc_est;
    float    polarity;

    sbitx_dstar_header_cb header_cb;
    sbitx_dstar_data_cb   data_cb;
    sbitx_dstar_lost_cb   lost_cb;
    sbitx_dstar_eot_cb    eot_cb;
    void *user;
};

static void
dstar_samples_to_bits(const float *in, uint16_t start, uint16_t count, uint8_t *out, uint16_t limit)
{
    for (uint16_t i = 0; i < count; i++) {
        float sample = in[start];
        WRITE_BIT2(out, i, sample < 0.0f);
        start += DSTAR_RADIO_SYMBOL_LENGTH;
        if (start >= limit)
            start -= limit;
    }
}

static bool
dstar_correlate_frame_sync(sbitx_dstar_rx *rx)
{
    if (countBits64((rx->bit_buffer[rx->bit_ptr] & FRAME_SYNC_MASK) ^ FRAME_SYNC_DATA) <= FRAME_SYNC_ERRS) {
        uint16_t ptr = rx->data_ptr + DSTAR_DATA_LENGTH_SAMPLES - DSTAR_FRAME_SYNC_LENGTH_SAMPLES
                       + DSTAR_RADIO_SYMBOL_LENGTH;
        if (ptr >= DSTAR_DATA_LENGTH_SAMPLES)
            ptr -= DSTAR_DATA_LENGTH_SAMPLES;

        int32_t corr = 0;
        for (uint8_t i = 0; i < DSTAR_FRAME_SYNC_LENGTH_SYMBOLS; i++) {
            float val = rx->data_buffer[ptr];
            if (DSTAR_FRAME_SYNC_SYMBOLS[i])
                corr -= (int32_t)(val * 32768.0f);
            else
                corr += (int32_t)(val * 32768.0f);
            ptr += DSTAR_RADIO_SYMBOL_LENGTH;
            if (ptr >= DSTAR_DATA_LENGTH_SAMPLES)
                ptr -= DSTAR_DATA_LENGTH_SAMPLES;
        }

        if (corr > rx->max_frame_corr) {
            rx->max_frame_corr = corr;
            rx->header_ptr = 0U;
            rx->stat_frame_sync++;
            return true;
        }
    }

    return false;
}

static bool
dstar_correlate_data_sync(sbitx_dstar_rx *rx)
{
    uint8_t max_errs = 0U;
    if (rx->state == 2)
        max_errs = DATA_SYNC_ERRS;

    if (countBits64((rx->bit_buffer[rx->bit_ptr] & DATA_SYNC_MASK) ^ DATA_SYNC_DATA) <= max_errs) {
        uint16_t ptr = rx->data_ptr + DSTAR_DATA_LENGTH_SAMPLES - DSTAR_DATA_SYNC_LENGTH_SAMPLES
                       + DSTAR_RADIO_SYMBOL_LENGTH;
        if (ptr >= DSTAR_DATA_LENGTH_SAMPLES)
            ptr -= DSTAR_DATA_LENGTH_SAMPLES;

        int32_t corr = 0;
        for (uint8_t i = 0; i < DSTAR_DATA_SYNC_LENGTH_SYMBOLS; i++) {
            float val = rx->data_buffer[ptr];
            if (DSTAR_DATA_SYNC_SYMBOLS[i])
                corr -= (int32_t)(val * 32768.0f);
            else
                corr += (int32_t)(val * 32768.0f);
            ptr += DSTAR_RADIO_SYMBOL_LENGTH;
            if (ptr >= DSTAR_DATA_LENGTH_SAMPLES)
                ptr -= DSTAR_DATA_LENGTH_SAMPLES;
        }

        if (corr > rx->max_data_corr) {
            rx->max_data_corr = corr;

            /* Time this sync against the last one. round() absorbs a missed
             * superframe; anything further out is a false correlation and is
             * dropped rather than poisoning the estimate. */
            if (rx->last_sync_valid) {
                int64_t elapsed = (int64_t) (rx->sample_count - rx->last_sync_sample);
                int64_t frames  = (elapsed + DSTAR_SUPERFRAME_SAMPLES / 2) / DSTAR_SUPERFRAME_SAMPLES;
                if (frames >= 1 && frames <= 4) {
                    int64_t expected = frames * DSTAR_SUPERFRAME_SAMPLES;
                    int64_t err = elapsed - expected;
                    if (err > -(int64_t) DSTAR_DATA_LENGTH_SAMPLES &&
                        err <  (int64_t) DSTAR_DATA_LENGTH_SAMPLES) {
                        rx->drift_samples += err;
                        rx->drift_span    += (uint64_t) expected;
                    }
                }
            }
            rx->last_sync_sample = rx->sample_count;
            rx->last_sync_valid  = true;
            rx->stat_data_sync++;

            rx->frame_count = 0U;
            rx->sync_ptr = rx->data_ptr;

            rx->start_ptr = rx->data_ptr + DSTAR_RADIO_SYMBOL_LENGTH;
            if (rx->start_ptr >= DSTAR_DATA_LENGTH_SAMPLES)
                rx->start_ptr -= DSTAR_DATA_LENGTH_SAMPLES;

            rx->max_sync_ptr = rx->sync_ptr + 1U;
            if (rx->max_sync_ptr >= DSTAR_DATA_LENGTH_SAMPLES)
                rx->max_sync_ptr -= DSTAR_DATA_LENGTH_SAMPLES;

            rx->min_sync_ptr = rx->sync_ptr + DSTAR_DATA_LENGTH_SAMPLES - 1U;
            if (rx->min_sync_ptr >= DSTAR_DATA_LENGTH_SAMPLES)
                rx->min_sync_ptr -= DSTAR_DATA_LENGTH_SAMPLES;

            return true;
        }
    }

    return false;
}

static void
dstar_acs(sbitx_dstar_rx *rx, int *metric)
{
    int temp_metric[4];

    unsigned int j = rx->mar >> 3;
    unsigned int k = rx->mar & 7;

    int m1 = metric[0] + rx->path_metric[0];
    int m2 = metric[4] + rx->path_metric[2];
    temp_metric[0] = m1 < m2 ? m1 : m2;
    if (m1 < m2)
        rx->path_memory0[j] &= BIT_MASK_TABLE0[k];
    else
        rx->path_memory0[j] |= BIT_MASK_TABLE1[k];

    m1 = metric[1] + rx->path_metric[0];
    m2 = metric[5] + rx->path_metric[2];
    temp_metric[1] = m1 < m2 ? m1 : m2;
    if (m1 < m2)
        rx->path_memory1[j] &= BIT_MASK_TABLE0[k];
    else
        rx->path_memory1[j] |= BIT_MASK_TABLE1[k];

    m1 = metric[2] + rx->path_metric[1];
    m2 = metric[6] + rx->path_metric[3];
    temp_metric[2] = m1 < m2 ? m1 : m2;
    if (m1 < m2)
        rx->path_memory2[j] &= BIT_MASK_TABLE0[k];
    else
        rx->path_memory2[j] |= BIT_MASK_TABLE1[k];

    m1 = metric[3] + rx->path_metric[1];
    m2 = metric[7] + rx->path_metric[3];
    temp_metric[3] = m1 < m2 ? m1 : m2;
    if (m1 < m2)
        rx->path_memory3[j] &= BIT_MASK_TABLE0[k];
    else
        rx->path_memory3[j] |= BIT_MASK_TABLE1[k];

    for (unsigned int i = 0U; i < 4U; i++)
        rx->path_metric[i] = temp_metric[i];

    rx->mar++;
}

static void
dstar_viterbi_decode(sbitx_dstar_rx *rx, int *data)
{
    int metric[8];

    metric[0] = (data[1] ^ 0) + (data[0] ^ 0);
    metric[1] = (data[1] ^ 1) + (data[0] ^ 1);
    metric[2] = (data[1] ^ 1) + (data[0] ^ 0);
    metric[3] = (data[1] ^ 0) + (data[0] ^ 1);
    metric[4] = (data[1] ^ 1) + (data[0] ^ 1);
    metric[5] = (data[1] ^ 0) + (data[0] ^ 0);
    metric[6] = (data[1] ^ 0) + (data[0] ^ 1);
    metric[7] = (data[1] ^ 1) + (data[0] ^ 0);

    dstar_acs(rx, metric);
}

/* Soft-decision variant of the Viterbi: the data are the matched-filtered
 * samples (positive = bit 0, negative = bit 1). The branch metric is the
 * correlation cost (the MIN still wins), matching the hard metric
 * (data[1]^b1) + (data[0]^b0): a mismatching 1-bit costs +data, a
 * mismatching 0-bit costs -data.
 *
 * The samples arrive normalised to about +/-1 by dstar_rx_header_soft, and
 * DSTAR_SOFT_SCALE then sets how much of that confidence survives into the
 * integer metric the ACS accumulates. That scaling is the whole point: an
 * earlier version fed raw samples (~0.06 amplitude on this receiver) through
 * (int)(... + 2.0f), where every branch truncated to the same integer, so the
 * soft information was thrown away and it behaved as a coarse hard decision.
 *
 * Worst-case path metric is DSTAR_FEC_SECTION_LENGTH_SYMBOLS/2 steps times
 * 2*DSTAR_SOFT_SCALE, i.e. ~21k with scale 64 -- comfortably inside int, and
 * dstar_acs needs no non-negative metrics (a per-step constant cannot change
 * which path wins). */
#define DSTAR_SOFT_SCALE 64.0f

static void
dstar_viterbi_decode_soft(sbitx_dstar_rx *rx, const float *data)
{
    int metric[8];

    const float s1 = data[1] * DSTAR_SOFT_SCALE;
    const float s0 = data[0] * DSTAR_SOFT_SCALE;

    metric[0] = (int) lrintf(-s1 - s0);   /* b1=0 b0=0 */
    metric[1] = (int) lrintf( s1 + s0);   /* b1=1 b0=1 */
    metric[2] = (int) lrintf( s1 - s0);   /* b1=1 b0=0 */
    metric[3] = (int) lrintf(-s1 + s0);   /* b1=0 b0=1 */
    metric[4] = metric[1];
    metric[5] = metric[0];
    metric[6] = metric[3];
    metric[7] = metric[2];

    dstar_acs(rx, metric);
}

static void
dstar_trace_back(sbitx_dstar_rx *rx)
{
    unsigned int j = 0U;
    unsigned int k = 0U;
    for (int i = 329; i >= 0; i--) {
        switch (j) {
        case 0U:
            if (!READ_BIT1(rx->path_memory0, i))
                j = 0U;
            else
                j = 2U;
            WRITE_BIT1(rx->fec_output, k, false);
            k++;
            break;

        case 1U:
            if (!READ_BIT1(rx->path_memory1, i))
                j = 0U;
            else
                j = 2U;
            WRITE_BIT1(rx->fec_output, k, true);
            k++;
            break;

        case 2U:
            if (!READ_BIT1(rx->path_memory2, i))
                j = 1U;
            else
                j = 3U;
            WRITE_BIT1(rx->fec_output, k, false);
            k++;
            break;

        default:
            if (!READ_BIT1(rx->path_memory3, i))
                j = 1U;
            else
                j = 3U;
            WRITE_BIT1(rx->fec_output, k, true);
            k++;
            break;
        }
    }
}

static bool
dstar_checksum(const uint8_t *header)
{
    union {
        uint16_t crc16;
        uint8_t crc8[2];
    } crc;

    crc.crc16 = 0xFFFFU;
    for (uint8_t i = 0U; i < (SBITX_DSTAR_HEADER_BYTES - 2U); i++)
        crc.crc16 = (uint16_t)(crc.crc8[1] ^ CCITT_TABLE[crc.crc8[0] ^ header[i]]);

    crc.crc16 = (uint16_t)~crc.crc16;

    return crc.crc8[0] == header[SBITX_DSTAR_HEADER_BYTES - 2U]
           && crc.crc8[1] == header[SBITX_DSTAR_HEADER_BYTES - 1U];
}

/* Soft-decision header decode: the matched-filtered samples flow through the
 * descramble (sign flip) and deinterleave (reorder) into the soft Viterbi,
 * keeping the amplitude as the bit confidence. */
static bool
dstar_rx_header_soft(sbitx_dstar_rx *rx, const float *samples, uint16_t start, uint8_t *out)
{
    float soft[DSTAR_FEC_SECTION_LENGTH_SYMBOLS];
    uint16_t p = start;
    for (int i = 0; i < DSTAR_FEC_SECTION_LENGTH_SYMBOLS; i++) {
        float v = samples[p];
        soft[i] = (SCRAMBLE_TABLE_RX[i >> 3] & (0x01U << (i & 7))) ? -v : v;
        p += DSTAR_RADIO_SYMBOL_LENGTH;
        if (p >= DSTAR_FEC_SECTION_LENGTH_SAMPLES + 2U * DSTAR_RADIO_SYMBOL_LENGTH)
            p -= DSTAR_FEC_SECTION_LENGTH_SAMPLES + 2U * DSTAR_RADIO_SYMBOL_LENGTH;
    }

    float inter[DSTAR_FEC_SECTION_LENGTH_SYMBOLS];
    memset(inter, 0, sizeof(inter));
    for (int i = 0; i < DSTAR_FEC_SECTION_LENGTH_SYMBOLS; i++)
        inter[INTERLEAVE_TABLE_RX[i * 2U] * 8U + INTERLEAVE_TABLE_RX[i * 2U + 1U]] = soft[i];

    for (int i = 0; i < 4; i++)
        rx->path_metric[i] = 0;

    /* Normalise by the mean symbol magnitude so the metric scale does not
     * depend on the receiver's gain chain -- only relative confidence
     * matters to the Viterbi. */
    float mag = 0.0f;
    for (int i = 0; i < DSTAR_FEC_SECTION_LENGTH_SYMBOLS; i++)
        mag += fabsf(inter[i]);
    mag /= (float) DSTAR_FEC_SECTION_LENGTH_SYMBOLS;
    const float norm = (mag > 1e-9f) ? (1.0f / mag) : 0.0f;

    float decode_data[2];

    rx->mar = 0U;
    for (int i = 0; i < DSTAR_FEC_SECTION_LENGTH_SYMBOLS; i += 2) {
        decode_data[1] = inter[i] * norm;
        decode_data[0] = inter[i + 1] * norm;
        dstar_viterbi_decode_soft(rx, decode_data);
    }

    dstar_trace_back(rx);

    for (int i = 0; i < SBITX_DSTAR_HEADER_BYTES; i++)
        out[i] = 0x00U;

    unsigned int j = 0;
    for (int i = 329; i >= 0; i--) {
        if (READ_BIT1(rx->fec_output, i))
            out[j >> 3] |= (0x01U << (j & 7));
        j++;
    }

    return dstar_checksum(out);
}

static bool
dstar_rx_header(sbitx_dstar_rx *rx, uint8_t *in, uint8_t *out)
{
    /* Descramble */
    for (int i = 0; i < DSTAR_FEC_SECTION_LENGTH_BYTES; i++)
        in[i] ^= SCRAMBLE_TABLE_RX[i];

    uint8_t intermediate[84];
    memset(intermediate, 0x00U, 84U);

    /* Deinterleave */
    int i = 0;
    while (i < 660) {
        uint8_t d = in[i / 8];

        if (d & 0x01U)
            intermediate[INTERLEAVE_TABLE_RX[i * 2U]] |= (0x80U >> INTERLEAVE_TABLE_RX[i * 2U + 1U]);
        i++;

        if (d & 0x02U)
            intermediate[INTERLEAVE_TABLE_RX[i * 2U]] |= (0x80U >> INTERLEAVE_TABLE_RX[i * 2U + 1U]);
        i++;

        if (d & 0x04U)
            intermediate[INTERLEAVE_TABLE_RX[i * 2U]] |= (0x80U >> INTERLEAVE_TABLE_RX[i * 2U + 1U]);
        i++;

        if (d & 0x08U)
            intermediate[INTERLEAVE_TABLE_RX[i * 2U]] |= (0x80U >> INTERLEAVE_TABLE_RX[i * 2U + 1U]);
        i++;

        if (i < 660) {
            if (d & 0x10U)
                intermediate[INTERLEAVE_TABLE_RX[i * 2U]] |= (0x80U >> INTERLEAVE_TABLE_RX[i * 2U + 1U]);
            i++;

            if (d & 0x20U)
                intermediate[INTERLEAVE_TABLE_RX[i * 2U]] |= (0x80U >> INTERLEAVE_TABLE_RX[i * 2U + 1U]);
            i++;

            if (d & 0x40U)
                intermediate[INTERLEAVE_TABLE_RX[i * 2U]] |= (0x80U >> INTERLEAVE_TABLE_RX[i * 2U + 1U]);
            i++;

            if (d & 0x80U)
                intermediate[INTERLEAVE_TABLE_RX[i * 2U]] |= (0x80U >> INTERLEAVE_TABLE_RX[i * 2U + 1U]);
            i++;
        }
    }

    for (i = 0; i < 4; i++)
        rx->path_metric[i] = 0;

    int decode_data[2];

    rx->mar = 0U;
    for (i = 0; i < 660; i += 2) {
        if (intermediate[i >> 3] & (0x80U >> (i & 7)))
            decode_data[1] = 1;
        else
            decode_data[1] = 0;

        if (intermediate[i >> 3] & (0x40U >> (i & 7)))
            decode_data[0] = 1;
        else
            decode_data[0] = 0;

        dstar_viterbi_decode(rx, decode_data);
    }

    dstar_trace_back(rx);

    for (i = 0; i < 41; i++)
        out[i] = 0x00U;

    unsigned int j = 0;
    for (i = 329; i >= 0; i--) {
        if (READ_BIT1(rx->fec_output, i))
            out[j >> 3] |= (0x01U << (j & 7));
        j++;
    }

    return dstar_checksum(out);
}

static void
dstar_process_none(sbitx_dstar_rx *rx, float sample)
{
    (void)sample;

    if (dstar_correlate_frame_sync(rx)) {
        rx->countdown = 5U;

        rx->header_buffer[rx->header_ptr] = sample;
        rx->header_ptr++;

        rx->state = 1;
        return;
    }

    if (dstar_correlate_data_sync(rx)) {
        rx->state = 2;
    }
}

static void
dstar_process_header(sbitx_dstar_rx *rx, float sample)
{
    if (rx->countdown > 0U) {
        dstar_correlate_frame_sync(rx);
        rx->countdown--;
    }

    rx->header_buffer[rx->header_ptr] = sample;
    rx->header_ptr++;

    if (rx->header_ptr == (DSTAR_FEC_SECTION_LENGTH_SAMPLES + DSTAR_RADIO_SYMBOL_LENGTH)) {
        uint8_t buffer[DSTAR_FEC_SECTION_LENGTH_BYTES];
        dstar_samples_to_bits(rx->header_buffer, DSTAR_RADIO_SYMBOL_LENGTH, DSTAR_FEC_SECTION_LENGTH_SYMBOLS,
                              buffer, DSTAR_FEC_SECTION_LENGTH_SAMPLES);

        uint8_t header[41];
        const uint32_t soft_before = rx->stat_header_soft_ok;
        bool ok = dstar_rx_header_soft(rx, rx->header_buffer, DSTAR_RADIO_SYMBOL_LENGTH, header);
        if (ok)
            rx->stat_header_soft_ok++;
        else
            ok = dstar_rx_header(rx, buffer, header);
        if (ok) {
            rx->stat_header_ok++;
            memcpy(rx->last_header, header, SBITX_DSTAR_HEADER_BYTES);
            rx->last_header_valid = true;
        } else {
            rx->stat_header_bad++;
        }
        if (rx->burst_debug_cb != NULL)
            rx->burst_debug_cb(rx->user, header, ok, rx->stat_header_soft_ok != soft_before,
                               rx->max_frame_corr);
        if (!ok) {
            rx->state = 0;
            rx->max_frame_corr = 0;
            rx->max_data_corr = 0;
        } else if (rx->header_cb != NULL) {
            rx->header_cb(rx->user, header);
        }
    }

    if (rx->header_ptr == (DSTAR_FEC_SECTION_LENGTH_SAMPLES + 2U * DSTAR_RADIO_SYMBOL_LENGTH)) {
        rx->frame_count = 0U;
        rx->data_ptr = 0U;

        rx->start_ptr = 476U;
        rx->sync_ptr = 471U;
        rx->max_sync_ptr = 472U;
        rx->min_sync_ptr = 470U;

        rx->state = 2;
    }
}

/* The fixed pattern D-STAR scrambles every frame's slow-data bytes with. */
static const uint8_t DSTAR_SLOW_SCRAMBLE[3] = {0x70U, 0x4FU, 0x93U};

static void
dstar_slow_reset(sbitx_dstar_rx *rx)
{
    rx->slow_unit_n = 0U;
    rx->slow_header_n = 0U;
}

/* One completed 6-byte slow-data unit. */
static void
dstar_slow_unit(sbitx_dstar_rx *rx)
{
    /* The type byte is NOT trusted to gate assembly. It is unprotected --
     * a single bit error turns 0x55 into 0x15, which happens in practice
     * (slot 3 of the very first superframe in the reference capture) -- and
     * the byte's position in the superframe already tells us where its data
     * belongs. So take the five data bytes positionally and let the header
     * CRC16 be the judge: filler units (0x66) simply produce a header that
     * fails the check and is discarded. */
    for (uint8_t i = 0U; i < 5U; i++) {
        if (rx->slow_header_n < SBITX_DSTAR_HEADER_BYTES)
            rx->slow_header[rx->slow_header_n++] = rx->slow_unit[1U + i];
    }

    if (rx->slow_header_n < SBITX_DSTAR_HEADER_BYTES)
        return;

    rx->slow_header_n = 0U;

    if (rx->slow_debug_cb != NULL)
        rx->slow_debug_cb(rx->user, rx->slow_header, dstar_checksum(rx->slow_header));

    uint8_t candidate[SBITX_DSTAR_HEADER_BYTES];
    bool ok = dstar_checksum(rx->slow_header);

    if (ok) {
        memcpy(candidate, rx->slow_header, sizeof(candidate));
    } else {
        /* Fold this repeat into the running vote and test the majority. */
        if (rx->slow_vote_n >= 32U) {
            /* Age the counts so a new station's header can take over. */
            for (int b = 0; b < SBITX_DSTAR_HEADER_BYTES; b++)
                for (int k = 0; k < 8; k++)
                    rx->slow_vote[b][k] = (uint8_t) (rx->slow_vote[b][k] / 2U);
            rx->slow_vote_n /= 2U;
        }

        for (int b = 0; b < SBITX_DSTAR_HEADER_BYTES; b++)
            for (int k = 0; k < 8; k++)
                if (rx->slow_header[b] & (1U << k))
                    rx->slow_vote[b][k]++;
        rx->slow_vote_n++;

        if (rx->slow_vote_n < 3U)
            return;   /* need a few repeats before a vote means anything */

        for (int b = 0; b < SBITX_DSTAR_HEADER_BYTES; b++) {
            uint8_t v = 0U;
            for (int k = 0; k < 8; k++)
                if (rx->slow_vote[b][k] * 2U > rx->slow_vote_n)
                    v |= (uint8_t) (1U << k);
            candidate[b] = v;
        }

        if (!dstar_checksum(candidate))
            return;
    }

    /* A header that verified: start the next vote from scratch. */
    memset(rx->slow_vote, 0, sizeof(rx->slow_vote));
    rx->slow_vote_n = 0U;
    memcpy(rx->slow_header, candidate, sizeof(candidate));

    rx->stat_header_slow++;

    /* Only announce a header the caller has not already been given, so a
     * steady over does not repeat it 2.4 times a second. */
    if (rx->last_header_valid &&
        memcmp(rx->last_header, rx->slow_header, SBITX_DSTAR_HEADER_BYTES) == 0)
        return;

    memcpy(rx->last_header, rx->slow_header, SBITX_DSTAR_HEADER_BYTES);
    rx->last_header_valid = true;
    rx->stat_header_ok++;

    if (rx->header_cb != NULL)
        rx->header_cb(rx->user, rx->slow_header);
}

/* Feed one delivered frame's slow-data field. slot is the frame's index
 * within the superframe; slot 0 carries the sync, not slow data. */
static void
dstar_slow_feed(sbitx_dstar_rx *rx, const uint8_t *frame, uint8_t slot)
{
    if (slot == 0U) {
        dstar_slow_reset(rx);
        return;
    }

    for (uint8_t i = 0U; i < 3U; i++) {
        if (rx->slow_unit_n < sizeof(rx->slow_unit))
            rx->slow_unit[rx->slow_unit_n++] = frame[9U + i] ^ DSTAR_SLOW_SCRAMBLE[i];
    }

    if (rx->slow_unit_n >= sizeof(rx->slow_unit)) {
        dstar_slow_unit(rx);
        rx->slow_unit_n = 0U;
    }
}

uint16_t
sbitx_dstar_rx_frame_index(const sbitx_dstar_rx *rx)
{
    return rx->frame_count;
}

static void
dstar_process_data(sbitx_dstar_rx *rx)
{
    if (countBits64((rx->bit_buffer[rx->bit_ptr] & END_SYNC_MASK) ^ END_SYNC_DATA) <= END_SYNC_ERRS) {
        if (rx->eot_cb != NULL)
            rx->eot_cb(rx->user);

        rx->max_frame_corr = 0;
        rx->max_data_corr = 0;

        rx->state = 0;
        return;
    }

    if (rx->min_sync_ptr < rx->max_sync_ptr) {
        if (rx->data_ptr >= rx->min_sync_ptr && rx->data_ptr <= rx->max_sync_ptr)
            dstar_correlate_data_sync(rx);
    } else {
        if (rx->data_ptr >= rx->min_sync_ptr || rx->data_ptr <= rx->max_sync_ptr)
            dstar_correlate_data_sync(rx);
    }

    if (rx->frame_count >= MAX_FRAMES) {
        if (rx->lost_cb != NULL)
            rx->lost_cb(rx->user);

        rx->max_frame_corr = 0;
        rx->max_data_corr = 0;

        rx->state = 0;
        return;
    }

    if (rx->data_ptr == rx->max_sync_ptr) {
        uint8_t buffer[DSTAR_DATA_LENGTH_BYTES];
        dstar_samples_to_bits(rx->data_buffer, rx->start_ptr, DSTAR_DATA_LENGTH_SYMBOLS, buffer,
                              DSTAR_DATA_LENGTH_SAMPLES);

        if (rx->frame_count == 0U) {
            buffer[9U] = DSTAR_DATA_SYNC_BYTES[9U];
            buffer[10U] = DSTAR_DATA_SYNC_BYTES[10U];
            buffer[11U] = DSTAR_DATA_SYNC_BYTES[11U];
        }

        if (rx->data_cb != NULL)
            rx->data_cb(rx->user, buffer);

        dstar_slow_feed(rx, buffer, (uint8_t) (rx->frame_count & 0xFFU));

        rx->frame_count++;

        rx->max_frame_corr = 0;
        rx->max_data_corr = 0;
    }
}

sbitx_dstar_rx *
sbitx_dstar_rx_new(void)
{
    sbitx_dstar_rx *rx = calloc(1, sizeof(sbitx_dstar_rx));
    if (rx != NULL) {
        rx->start_ptr = NOENDPTR;
        rx->sync_ptr = NOENDPTR;
        rx->min_sync_ptr = NOENDPTR;
        rx->max_sync_ptr = NOENDPTR;
        rx->polarity = 1.0f;
    }
    return rx;
}

void
sbitx_dstar_rx_set_polarity(sbitx_dstar_rx *rx, float polarity)
{
    if (rx == NULL)
        return;
    rx->polarity = (polarity < 0.0f) ? -1.0f : 1.0f;
}

void
sbitx_dstar_rx_free(sbitx_dstar_rx *rx)
{
    free(rx);
}

void
sbitx_dstar_rx_set_cbs(sbitx_dstar_rx *rx,
                       sbitx_dstar_header_cb hdr_cb,
                       sbitx_dstar_data_cb data_cb,
                       sbitx_dstar_lost_cb lost_cb,
                       sbitx_dstar_eot_cb eot_cb,
                       void *user)
{
    if (rx == NULL)
        return;
    rx->header_cb = hdr_cb;
    rx->data_cb = data_cb;
    rx->lost_cb = lost_cb;
    rx->eot_cb = eot_cb;
    rx->user = user;
}

void
sbitx_dstar_rx_reset(sbitx_dstar_rx *rx)
{
    if (rx == NULL)
        return;
    rx->state = 0;
    rx->header_ptr = 0U;
    rx->data_ptr = 0U;
    rx->bit_ptr = 0U;
    rx->max_frame_corr = 0;
    rx->max_data_corr = 0;
    rx->start_ptr = NOENDPTR;
    rx->sync_ptr = NOENDPTR;
    rx->min_sync_ptr = NOENDPTR;
    rx->max_sync_ptr = NOENDPTR;
    rx->frame_count = 0U;
    rx->countdown = 0U;
    rx->last_sync_valid = false;
    rx->drift_samples = 0;
    rx->drift_span = 0;
    rx->slow_unit_n = 0U;
    rx->slow_header_n = 0U;
    rx->slow_vote_n = 0U;
    memset(rx->slow_vote, 0, sizeof(rx->slow_vote));
    rx->last_header_valid = false;
}

void
sbitx_dstar_rx_set_slow_debug(sbitx_dstar_rx *rx,
                              void (*cb)(void *user, const uint8_t *hdr41, bool crc_ok))
{
    if (rx != NULL)
        rx->slow_debug_cb = cb;
}

void
sbitx_dstar_rx_set_burst_debug(sbitx_dstar_rx *rx,
                               void (*cb)(void *user, const uint8_t *hdr41, bool crc_ok,
                                          bool soft, int32_t corr))
{
    if (rx != NULL)
        rx->burst_debug_cb = cb;
}

void
sbitx_dstar_rx_get_stats(const sbitx_dstar_rx *rx, sbitx_dstar_rx_stats *out)
{
    if (rx == NULL || out == NULL)
        return;

    out->frame_sync     = rx->stat_frame_sync;
    out->header_ok      = rx->stat_header_ok;
    out->header_bad     = rx->stat_header_bad;
    out->header_soft_ok = rx->stat_header_soft_ok;
    out->data_sync      = rx->stat_data_sync;
    out->header_slow    = rx->stat_header_slow;
}

bool
sbitx_dstar_rx_take_clock_error(sbitx_dstar_rx *rx, double *ppm)
{
    if (rx == NULL || ppm == NULL)
        return false;

    /* Need a few superframes before the estimate means anything: one
     * superframe is 420 ms and a single +/-1 sample slip is ~99 ppm. */
    if (rx->drift_span < 3U * DSTAR_SUPERFRAME_SAMPLES)
        return false;

    *ppm = (double) rx->drift_samples / (double) rx->drift_span * 1e6;
    rx->drift_samples = 0;
    rx->drift_span = 0;
    return true;
}

void
sbitx_dstar_rx_process(sbitx_dstar_rx *rx, const float *audio, int n)
{
    if (rx == NULL || audio == NULL)
        return;

    rx->sample_count += (uint64_t) n;

    for (int i = 0; i < n; i++) {
        /* Slow DC tracker (carrier-offset drift), then the Gaussian BT=0.5
         * matched filter, then the polarity flip. The slicer and the sync
         * correlation operate on this matched-filtered signal. */
        float raw = audio[i];
        rx->dc_est += 0.001f * (raw - rx->dc_est);
        float s = raw - rx->dc_est;

        for (int k = 0; k < 10; k++)
            rx->mf_state[k] = rx->mf_state[k + 1];
        rx->mf_state[10] = s;
        float mf = 0.0f;
        for (int k = 0; k < 11; k++)
            mf += rx->mf_state[k] * GAUSSIAN_0_5_FILTER[k];
        float sample = mf * rx->polarity;

        rx->bit_buffer[rx->bit_ptr] <<= 1;
        if (sample < 0.0f)
            rx->bit_buffer[rx->bit_ptr] |= 0x01U;

        rx->data_buffer[rx->data_ptr] = sample;

        switch (rx->state) {
        case 1:
            dstar_process_header(rx, sample);
            break;
        case 2:
            dstar_process_data(rx);
            break;
        default:
            dstar_process_none(rx, sample);
            break;
        }

        rx->data_ptr++;
        if (rx->data_ptr >= DSTAR_DATA_LENGTH_SAMPLES)
            rx->data_ptr = 0U;

        rx->bit_ptr++;
        if (rx->bit_ptr >= DSTAR_RADIO_SYMBOL_LENGTH)
            rx->bit_ptr = 0U;
    }
}

/* ── TX ─────────────────────────────────────────────────────────── */

struct sbitx_dstar_tx {
    uint8_t queue[SBITX_DSTAR_TX_BUF_BYTES];
    uint16_t q_head;
    uint16_t q_tail;
    uint16_t q_len;

    uint8_t po_buffer[600];
    uint16_t po_len;
    uint16_t po_ptr;

    float mod_state[3];
    int   mod_state_n;      /* samples stored (0..2) */

    bool   first_header;    /* preamble still to emit */
    int    preamble_left;
};

static int
dstar_tx_queue_space(const sbitx_dstar_tx *tx)
{
    return SBITX_DSTAR_TX_BUF_BYTES - tx->q_len - 1;
}

static int
dstar_tx_queue_put(sbitx_dstar_tx *tx, const uint8_t *data, int len)
{
    if (len > dstar_tx_queue_space(tx))
        return -1;
    for (int i = 0; i < len; i++) {
        tx->queue[tx->q_head] = data[i];
        tx->q_head = (uint16_t)((tx->q_head + 1) % SBITX_DSTAR_TX_BUF_BYTES);
    }
    tx->q_len = (uint16_t)(tx->q_len + len);
    return 0;
}

static uint8_t
dstar_tx_queue_get(sbitx_dstar_tx *tx)
{
    uint8_t v = tx->queue[tx->q_tail];
    tx->q_tail = (uint16_t)((tx->q_tail + 1) % SBITX_DSTAR_TX_BUF_BYTES);
    tx->q_len--;
    return v;
}

static void
dstar_tx_header_fec(const uint8_t *in, uint8_t *out)
{
    uint8_t intermediate[84];
    memset(intermediate, 0x00U, 84U);
    for (uint32_t i = 0U; i < 83U; i++)
        out[i] = 0x00U;

    /* Convolve */
    uint8_t d, d1 = 0U, d2 = 0U, g0, g1;
    uint32_t k = 0U;
    for (uint32_t i = 0U; i < 42U; i++) {
        for (uint8_t j = 0U; j < 8U; j++) {
            uint8_t mask = (uint8_t)(0x01U << j);
            d = 0U;
            if (in[i] & mask)
                d = 1U;

            g0 = (uint8_t)((d + d2) & 1U);
            g1 = (uint8_t)((d + d1 + d2) & 1U);
            d2 = d1;
            d1 = d;

            if (g1)
                intermediate[k >> 3] |= BIT_MASK_TABLE[k & 7];
            k++;
            if (g0)
                intermediate[k >> 3] |= BIT_MASK_TABLE[k & 7];
            k++;
        }
    }

    /* Interleave */
    uint32_t i = 0U;
    while (i < 660U) {
        uint8_t d = intermediate[i >> 3];

        if (d & 0x80U)
            out[INTERLEAVE_TABLE_TX[i * 2U]] |= (uint8_t)(0x01U << INTERLEAVE_TABLE_TX[i * 2U + 1U]);
        i++;

        if (d & 0x40U)
            out[INTERLEAVE_TABLE_TX[i * 2U]] |= (uint8_t)(0x01U << INTERLEAVE_TABLE_TX[i * 2U + 1U]);
        i++;

        if (d & 0x20U)
            out[INTERLEAVE_TABLE_TX[i * 2U]] |= (uint8_t)(0x01U << INTERLEAVE_TABLE_TX[i * 2U + 1U]);
        i++;

        if (d & 0x10U)
            out[INTERLEAVE_TABLE_TX[i * 2U]] |= (uint8_t)(0x01U << INTERLEAVE_TABLE_TX[i * 2U + 1U]);
        i++;

        if (i < 660U) {
            if (d & 0x08U)
                out[INTERLEAVE_TABLE_TX[i * 2U]] |= (uint8_t)(0x01U << INTERLEAVE_TABLE_TX[i * 2U + 1U]);
            i++;

            if (d & 0x04U)
                out[INTERLEAVE_TABLE_TX[i * 2U]] |= (uint8_t)(0x01U << INTERLEAVE_TABLE_TX[i * 2U + 1U]);
            i++;

            if (d & 0x02U)
                out[INTERLEAVE_TABLE_TX[i * 2U]] |= (uint8_t)(0x01U << INTERLEAVE_TABLE_TX[i * 2U + 1U]);
            i++;

            if (d & 0x01U)
                out[INTERLEAVE_TABLE_TX[i * 2U]] |= (uint8_t)(0x01U << INTERLEAVE_TABLE_TX[i * 2U + 1U]);
            i++;
        }
    }

    /* Scramble */
    for (i = 0U; i < 83U; i++)
        out[i] ^= SCRAMBLE_TABLE_TX[i];
}

/* Modulate one byte (LSB first) into 40 samples at 24 kHz. */
static void
dstar_tx_modulate_byte(sbitx_dstar_tx *tx, uint8_t c, float *out)
{
    float symbols[8];

    uint8_t mask = 0x01U;
    for (uint8_t i = 0U; i < 8U; i++) {
        /* Bit 1 modulates POSITIVE. Measured against the IC-7100 over the
         * air through an independent receiver: with the mapping the other
         * way round our signal decoded only with the polarity inverted,
         * i.e. every bit was flipped relative to a real D-STAR rig, so no
         * standard receiver could ever decode it. The receive slicer's
         * "sample < 0 means bit 1" is the matching convention once the
         * discriminator inversion of the rig's audio path is accounted for
         * by dstar_polarity. */
        symbols[i] = (c & mask) ? DSTAR_LEVEL1 : DSTAR_LEVEL0;
        mask <<= 1;
    }

    for (uint8_t i = 0U; i < 8U; i++) {
        /* Shift the symbol history */
        tx->mod_state[0] = tx->mod_state[1];
        tx->mod_state[1] = tx->mod_state[2];
        tx->mod_state[2] = symbols[i];
        tx->mod_state_n++;

        /* 5 output samples per symbol (polyphase Gaussian, CMSIS layout) */
        for (int p = 0; p < 5; p++) {
            float acc = tx->mod_state[0] * GAUSSIAN_0_35_FILTER[4 - p]
                        + tx->mod_state[1] * GAUSSIAN_0_35_FILTER[9 - p]
                        + tx->mod_state[2] * GAUSSIAN_0_35_FILTER[14 - p];
            *out++ = acc;
        }
    }
}

sbitx_dstar_tx *
sbitx_dstar_tx_new(void)
{
    sbitx_dstar_tx *tx = calloc(1, sizeof(sbitx_dstar_tx));
    if (tx != NULL)
        tx->first_header = true;
    return tx;
}

void
sbitx_dstar_tx_free(sbitx_dstar_tx *tx)
{
    free(tx);
}

void
sbitx_dstar_tx_reset(sbitx_dstar_tx *tx)
{
    if (tx == NULL)
        return;
    tx->q_head = 0U;
    tx->q_tail = 0U;
    tx->q_len = 0U;
    tx->po_len = 0U;
    tx->po_ptr = 0U;
    memset(tx->mod_state, 0, sizeof(tx->mod_state));
    tx->mod_state_n = 0;
    tx->first_header = true;
    tx->preamble_left = 0;
}

int
sbitx_dstar_tx_header(sbitx_dstar_tx *tx, const uint8_t *header41)
{
    if (tx == NULL || header41 == NULL)
        return -1;

    /* Preamble only at the start of a transmission (idle queue). */
    if (tx->q_len == 0 && tx->po_len == 0 && tx->first_header) {
        tx->preamble_left = SBITX_DSTAR_TX_PREAMBLE_BYTES;
        tx->first_header = false;
    }

    uint8_t burst[1 + SBITX_DSTAR_HEADER_BYTES];
    burst[0] = TX_TYPE_HEADER;
    for (int i = 0; i < SBITX_DSTAR_HEADER_BYTES; i++)
        burst[1 + i] = header41[i];

    if (dstar_tx_queue_put(tx, burst, (int)sizeof(burst)) < 0)
        return -1;
    return 0;
}

int
sbitx_dstar_tx_frame(sbitx_dstar_tx *tx, const uint8_t *frame12)
{
    if (tx == NULL || frame12 == NULL)
        return -1;
    if (dstar_tx_queue_space(tx) < (SBITX_DSTAR_FRAME_BYTES + 1))
        return -1;

    uint8_t marker = TX_TYPE_DATA;
    if (dstar_tx_queue_put(tx, &marker, 1) < 0)
        return -1;
    if (dstar_tx_queue_put(tx, frame12, SBITX_DSTAR_FRAME_BYTES) < 0)
        return -1;
    return 0;
}

int
sbitx_dstar_tx_eot(sbitx_dstar_tx *tx)
{
    if (tx == NULL)
        return -1;
    if (dstar_tx_queue_space(tx) < (1 + 3 * DSTAR_END_SYNC_LENGTH_BYTES))
        return -1;

    uint8_t marker = TX_TYPE_EOT;
    if (dstar_tx_queue_put(tx, &marker, 1) < 0)
        return -1;

    uint8_t eot[3 * DSTAR_END_SYNC_LENGTH_BYTES];
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < DSTAR_END_SYNC_LENGTH_BYTES; i++)
            eot[j * DSTAR_END_SYNC_LENGTH_BYTES + i] = DSTAR_END_SYNC_BYTES[i];
    if (dstar_tx_queue_put(tx, eot, (int)sizeof(eot)) < 0)
        return -1;
    return 0;
}

int
sbitx_dstar_tx_pending(const sbitx_dstar_tx *tx)
{
    if (tx == NULL)
        return 0;
    return (tx->q_len > 0 || tx->po_len > 0);
}

static void
dstar_tx_refill_po(sbitx_dstar_tx *tx)
{
    if (tx->po_len > 0)
        return;

    /* Preamble bytes go straight to the modulator. */
    if (tx->preamble_left > 0) {
        while (tx->preamble_left > 0 && tx->po_len < 256) {
            tx->po_buffer[tx->po_len++] = 0xAAU;
            tx->preamble_left--;
        }
        tx->po_ptr = 0U;
        return;
    }

    if (tx->q_len == 0)
        return;

    uint8_t type = tx->queue[tx->q_tail]; /* peek */

    if (type == TX_TYPE_HEADER) {
        dstar_tx_queue_get(tx); /* marker */

        uint8_t header[42];
        for (int i = 0; i < SBITX_DSTAR_HEADER_BYTES; i++)
            header[i] = dstar_tx_queue_get(tx);
        header[41] = 0x00U; /* the 42nd convolution byte is padding */

        uint8_t buffer[86];
        dstar_tx_header_fec(header, buffer + 2U);

        buffer[0U] = 0xEAU;
        buffer[1U] = 0xA6U;
        buffer[2U] |= 0x00U;

        for (int i = 0U; i < 85U; i++)
            tx->po_buffer[tx->po_len++] = buffer[i];
    } else if (type == TX_TYPE_DATA) {
        dstar_tx_queue_get(tx); /* marker */
        for (int i = 0U; i < DSTAR_DATA_LENGTH_BYTES; i++)
            tx->po_buffer[tx->po_len++] = dstar_tx_queue_get(tx);
    } else if (type == TX_TYPE_EOT) {
        /* sbitx_dstar_tx_eot() queues the marker AND the three end-sync
         * patterns: take them from the queue. Writing them from the table
         * instead left those 18 bytes at the head of the queue, where no
         * type matched, so the modem never reported empty again. */
        dstar_tx_queue_get(tx); /* marker */
        for (int i = 0U; i < 3U * DSTAR_END_SYNC_LENGTH_BYTES; i++)
            tx->po_buffer[tx->po_len++] = dstar_tx_queue_get(tx);
    }

    tx->po_ptr = 0U;
}

int
sbitx_dstar_tx_generate(sbitx_dstar_tx *tx, float *out24k, int n)
{
    if (tx == NULL || out24k == NULL)
        return 0;

    int written = 0;

    while (written + 40 <= n) {
        dstar_tx_refill_po(tx);
        if (tx->po_len == 0)
            break;

        uint8_t c = tx->po_buffer[tx->po_ptr++];
        dstar_tx_modulate_byte(tx, c, out24k + written);
        written += 40;

        if (tx->po_ptr >= tx->po_len) {
            tx->po_ptr = 0U;
            tx->po_len = 0U;
        }
    }

    return written;
}

uint16_t
sbitx_dstar_crc16(const uint8_t *data, int len)
{
    uint16_t crc = 0xFFFFU;
    for (int i = 0; i < len; i++)
        crc = (uint16_t)((crc >> 8) ^ CCITT_TABLE[(crc ^ data[i]) & 0xFFU]);
    return (uint16_t)~crc;
}

void
sbitx_dstar_header_finalize(uint8_t *header41)
{
    uint16_t crc = sbitx_dstar_crc16(header41, SBITX_DSTAR_HEADER_BYTES - 2U);
    header41[SBITX_DSTAR_HEADER_BYTES - 2U] = (uint8_t)(crc & 0xFFU);
    header41[SBITX_DSTAR_HEADER_BYTES - 1U] = (uint8_t)(crc >> 8);
}
