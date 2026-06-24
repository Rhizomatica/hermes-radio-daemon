/* shm_audio - POSIX-SHM audio bridge to mercury (-x shm). See shm_audio.h. */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shm_audio.h"
#include "radio_media.h"
#include "vendor/hermes_shm/ring_buffer_posix.h"

/* Must match mercury common/defines_modem.h exactly. */
#define SIGNAL_BUFFER_SIZE 12288000
#define SIGNAL_INPUT       "/signal-radio2modem"   /* daemon -> modem (RX) */
#define SIGNAL_OUTPUT      "/signal-modem2radio"   /* modem -> daemon (TX) */

#define TX_CHUNK_SAMPLES 320                        /* 40 ms @ 8 kHz */

static cbuf_handle_t s_rx_ring;     /* SIGNAL_INPUT  (we write)  */
static cbuf_handle_t s_tx_ring;     /* SIGNAL_OUTPUT (we read)   */
static pthread_t     s_tx_tid;
static volatile bool s_run;
static bool          s_started;
static radio        *s_radio;

/* Dedicated TX feeder: blocks on the SHM ring (cond_wait) until mercury writes
 * a chunk of TX audio, converts int32->int16, and hands it to the playback
 * path (tx_audio_ring -> resample to codec rate -> codec). */
static void *tx_thread(void *arg)
{
    (void) arg;
    int32_t in32[TX_CHUNK_SAMPLES];
    int16_t out16[TX_CHUNK_SAMPLES];

    while (s_run)
    {
        /* Blocking read of one chunk — this is the intended ring-buffer use:
         * a dedicated thread parking on the cond until data arrives. */
        if (hsr_read_buffer(s_tx_ring, (uint8_t *) in32, sizeof(in32)) != 0)
            continue;

        for (int i = 0; i < TX_CHUNK_SAMPLES; i++)
            out16[i] = (int16_t) (in32[i] >> 16);

        radio_media_push_tx_audio(s_radio, out16, TX_CHUNK_SAMPLES);
    }
    return NULL;
}

bool shm_audio_init(radio *radio_h)
{
    if (s_started)
        return true;

    s_radio = radio_h;

    s_rx_ring = circular_buf_init_shm(SIGNAL_BUFFER_SIZE, (char *) SIGNAL_INPUT);
    s_tx_ring = circular_buf_init_shm(SIGNAL_BUFFER_SIZE, (char *) SIGNAL_OUTPUT);
    if (!s_rx_ring || !s_tx_ring)
    {
        fprintf(stderr, "shm_audio: failed to create SHM rings\n");
        return false;
    }
    hsr_clear_buffer(s_rx_ring);
    hsr_clear_buffer(s_tx_ring);

    s_run = true;
    if (pthread_create(&s_tx_tid, NULL, tx_thread, NULL) != 0)
    {
        fprintf(stderr, "shm_audio: failed to start TX thread\n");
        s_run = false;
        return false;
    }

    s_started = true;
    fprintf(stderr, "shm_audio: bridge up (%s / %s, 8 kHz int32 mono)\n",
            SIGNAL_INPUT, SIGNAL_OUTPUT);
    return true;
}

void shm_audio_push_rx(const int16_t *samples, size_t nsamples)
{
    if (!s_started || !s_rx_ring || !samples || !nsamples)
        return;

    /* Convert int16 -> full-scale int32 in bounded chunks and write only what
     * fits, so the capture thread never blocks (drop on ring-full). */
    int32_t buf[1024];
    while (nsamples)
    {
        size_t chunk = nsamples > 1024 ? 1024 : nsamples;
        size_t bytes = chunk * sizeof(int32_t);

        if (circular_buf_free_size(s_rx_ring) >= bytes)
        {
            for (size_t i = 0; i < chunk; i++)
                buf[i] = ((int32_t) samples[i]) << 16;
            hsr_write_buffer(s_rx_ring, (uint8_t *) buf, bytes);  /* room checked => no block */
        }
        /* else: mercury isn't draining — drop this chunk rather than stall RX */
        samples   += chunk;
        nsamples  -= chunk;
    }
}

void shm_audio_shutdown(void)
{
    if (!s_started)
        return;
    s_run = false;
    /* tx_thread may be parked in read_buffer's cond_wait; cancel to unblock. */
    pthread_cancel(s_tx_tid);
    pthread_join(s_tx_tid, NULL);

    circular_buf_destroy_shm(s_rx_ring, SIGNAL_BUFFER_SIZE, (char *) SIGNAL_INPUT);
    circular_buf_destroy_shm(s_tx_ring, SIGNAL_BUFFER_SIZE, (char *) SIGNAL_OUTPUT);
    circular_buf_free_shm(s_rx_ring);
    circular_buf_free_shm(s_tx_ring);
    s_rx_ring = s_tx_ring = NULL;
    s_started = false;
}
