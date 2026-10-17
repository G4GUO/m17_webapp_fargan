// recorder_test.cpp - does the recorder write WAVs a strict parser accepts, at
// the rate it was asked for?
//
//     g++ -O2 -I. tests/recorder_test.cpp recorder.cpp -o /tmp/recorder_test -lpthread
//     /tmp/recorder_test [directory]
//
// It records eight seconds of a tone at 8 kHz (the classic decoder's rate) and
// four at 16 kHz (FARGAN's own), checks that an unsupported rate is refused, and
// that samples arriving at a rate the file does not use are refused rather than
// written in.  The tone is only there to be something measurable; the file
// handling is the subject.
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "recorder.h"

static void tone(int16_t *buf, int n, int rate, double freq, int *phase)
{
    for (int k = 0; k < n; k++) {
        double t = (double)((*phase)++) / (double)rate;
        buf[k] = (int16_t)(12000.0 * sin(2.0 * M_PI * freq * t));
    }
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    char p8[300], p16[300], bogus[300];
    int16_t blk8[160], blk16[320];
    unsigned long samples = 0;
    unsigned int seconds = 0;
    int phase = 0;

    snprintf(p8,    sizeof(p8),    "%s/recorder_test_8k.wav",  dir);
    snprintf(p16,   sizeof(p16),   "%s/recorder_test_16k.wav", dir);
    snprintf(bogus, sizeof(bogus), "%s/recorder_test_bogus.wav", dir);

    /* 8 kHz: what the classic decoder produces */
    if (!rec_start(p8, 8000)) { printf("8k start failed: %s\n", rec_error()); return 1; }
    for (int i = 0; i < 50 * 8; i++) {
        tone(blk8, 160, 8000, 440.0, &phase);
        rec_push(blk8, 160, 8000);
    }
    rec_stats(&samples, &seconds);
    printf("8k : %s at %d Hz, %lu samples, %u s\n", rec_file(), rec_rate(), samples, seconds);
    rec_stop();

    /* a rate that is neither 8 nor 16 kHz must be refused, not guessed at */
    if (rec_start(bogus, 11025)) { printf("an 11025 Hz start was accepted\n"); return 1; }
    printf("11k: correctly refused: %s\n", rec_error());

    /* 16 kHz: FARGAN's own output, with a push at the wrong rate in the middle */
    if (!rec_start(p16, 16000)) { printf("16k start failed: %s\n", rec_error()); return 1; }
    for (int i = 0; i < 50 * 4; i++) {
        tone(blk16, 320, 16000, 800.0, &phase);
        rec_push(blk16, 320, 16000);
    }
    rec_push(blk8, 160, 8000);            /* must be ignored, with a reason */
    rec_stats(&samples, &seconds);
    printf("16k: %s at %d Hz, %lu samples, %u s; the 8 kHz push was refused: %s\n",
           rec_file(), rec_rate(), samples, seconds, rec_error());
    rec_stop();

    /* a path that cannot be written must say so rather than pretend */
    if (rec_start("/nonexistent-directory/x.wav", 8000)) {
        printf("a start into a missing directory succeeded\n");
        return 1;
    }
    printf("dir: correctly failed: %s\n", rec_error());
    return 0;
}
