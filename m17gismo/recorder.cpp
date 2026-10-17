// recorder.cpp - write the decoded audio out as a WAV file.
//
// The station's own audio path is mono 8 kHz signed 16-bit, which is what a
// Codec2 3200 frame is, so a recording needs no conversion at all: the samples
// that were handed to the speaker are the samples in the file.
//
// Two things matter more than they look:
//
//  * The header is kept honest while recording.  A Pi has no UPS; if the power
//    goes off mid-QSO the file would otherwise say it holds nothing and be
//    unplayable.  Every few seconds the sizes are rewritten and the file is
//    flushed, so a recording that was interrupted still plays.
//  * The header fields are written by offset through small helpers rather than
//    by hand.  Laying a WAV header out by hand got the fmt fields one position
//    wrong once already, which produced files of the right length that Windows
//    refused to play.
//
// Audio reaches this from either receive thread (radio and reflector), so the
// file is guarded by a mutex; the critical section is a single fwrite.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "recorder.h"

/* The rate is fixed when a recording starts: a WAV file has one rate, and
   writing samples of another rate into it would only mis-pitch the result.  The
   classic decoder gives 8 kHz; FARGAN's own output is 16 kHz. */
static int s_rate = 8000;
#define REC_MAX_SECONDS (30 * 60)        /* half an hour */
#define REC_FIXUP_SEC   5

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *s_f;
static char s_path[256];
static char s_err[128];
static unsigned long s_samples;
static time_t s_next_fixup;

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

static void put16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
}

/*
 * The canonical 44-byte header.  Offsets, so they are not guessed at again:
 *
 *   0  "RIFF"        12  "fmt "        22  channels      32  block align
 *   4  36 + data     16  16 (fmt size) 24  sample rate   34  bits per sample
 *   8  "WAVE"        20  1 (PCM)       28  byte rate     36  "data"
 *                                                        40  data bytes
 */
static void write_header(FILE *f, uint32_t data_bytes)
{
    unsigned char h[44];

    memcpy(h + 0,  "RIFF", 4);  put32(h + 4,  36 + data_bytes);
    memcpy(h + 8,  "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);  put32(h + 16, 16);
    put16(h + 20, 1);                           /* PCM */
    put16(h + 22, 1);                           /* mono */
    put32(h + 24, (uint32_t)s_rate);
    put32(h + 28, (uint32_t)s_rate * 2);        /* byte rate = rate * channels * 2 */
    put16(h + 32, 2);                           /* block align */
    put16(h + 34, 16);                          /* bits per sample */
    memcpy(h + 36, "data", 4);  put32(h + 40, data_bytes);

    rewind(f);
    fwrite(h, 1, sizeof(h), f);
    fflush(f);
    fseek(f, 0, SEEK_END);
}

bool rec_active(void)
{
    bool on;
    pthread_mutex_lock(&s_lock);
    on = (s_f != NULL);
    pthread_mutex_unlock(&s_lock);
    return on;
}

const char *rec_file(void)  { return s_path; }
const char *rec_error(void) { return s_err; }

void rec_stats(unsigned long *samples, unsigned int *seconds)
{
    pthread_mutex_lock(&s_lock);
    if (samples) *samples = s_samples;
    if (seconds) *seconds = (unsigned int)(s_samples / (s_rate > 0 ? s_rate : 8000));
    pthread_mutex_unlock(&s_lock);
}

int rec_rate(void) { return s_rate; }

bool rec_start(const char *path, int rate)
{
    char name[256];
    FILE *f;

    if (rate != 8000 && rate != 16000) {
        snprintf(s_err, sizeof(s_err), "unsupported rate %d (8000 or 16000)", rate);
        return false;
    }

    if (path && *path) {
        snprintf(name, sizeof(name), "%s", path);
    } else {
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        if (!tm || strftime(name, sizeof(name), "rx-%Y%m%d-%H%M%S.wav", tm) == 0)
            snprintf(name, sizeof(name), "rx-recording.wav");
    }

    pthread_mutex_lock(&s_lock);
    if (s_f) {
        snprintf(s_err, sizeof(s_err), "already recording to %s", s_path);
        pthread_mutex_unlock(&s_lock);
        return false;
    }
    f = fopen(name, "wb+");
    if (!f) {
        snprintf(s_err, sizeof(s_err), "cannot write %s", name);
        pthread_mutex_unlock(&s_lock);
        return false;
    }
    s_rate = rate;
    write_header(f, 0);
    s_f = f;
    snprintf(s_path, sizeof(s_path), "%s", name);
    s_err[0] = 0;
    s_samples = 0;
    s_next_fixup = time(NULL) + REC_FIXUP_SEC;
    pthread_mutex_unlock(&s_lock);
    return true;
}

void rec_stop(void)
{
    pthread_mutex_lock(&s_lock);
    if (s_f) {
        write_header(s_f, (uint32_t)(s_samples * sizeof(int16_t)));
        fclose(s_f);
        s_f = NULL;
    }
    pthread_mutex_unlock(&s_lock);
}

void rec_push(const int16_t *samples, int count, int rate)
{
    if (count <= 0) return;

    pthread_mutex_lock(&s_lock);
    if (s_f) {
        if (rate != s_rate) {
            /* FARGAN was switched while recording.  Say so rather than writing
               samples into a file whose header claims something else. */
            snprintf(s_err, sizeof(s_err),
                     "samples at %d Hz ignored: this recording is %d Hz", rate, s_rate);
            pthread_mutex_unlock(&s_lock);
            return;
        }
        if (s_samples + (unsigned long)count >
            (unsigned long)REC_MAX_SECONDS * (unsigned long)s_rate) {
            write_header(s_f, (uint32_t)(s_samples * sizeof(int16_t)));
            fclose(s_f);
            s_f = NULL;
            snprintf(s_err, sizeof(s_err), "stopped at the %d minute limit", REC_MAX_SECONDS / 60);
            pthread_mutex_unlock(&s_lock);
            return;
        }
        if (fwrite(samples, sizeof(int16_t), (size_t)count, s_f) == (size_t)count) {
            s_samples += (unsigned long)count;
            if (time(NULL) >= s_next_fixup) {
                write_header(s_f, (uint32_t)(s_samples * sizeof(int16_t)));
                s_next_fixup = time(NULL) + REC_FIXUP_SEC;
            }
        }
    }
    pthread_mutex_unlock(&s_lock);
}
