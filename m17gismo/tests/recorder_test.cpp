// recorder_test.cpp - does the recorder write a WAV that a strict parser accepts?
//
//     g++ -O2 -I. tests/recorder_test.cpp recorder.cpp -o /tmp/recorder_test -lpthread
//     /tmp/recorder_test
//
// It records eight seconds of a 440 Hz tone through exactly the code the station
// uses, then reports what the header says.  Check the file it leaves behind with
// Python's wave module (or any player) - the point is that the header fields are
// right, not that the tone is pretty.
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "recorder.h"

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "/tmp/recorder_test.wav";
    int16_t block[160];
    unsigned long samples = 0;
    unsigned int seconds = 0;

    if (!rec_start(path)) {
        printf("start failed: %s\n", rec_error());
        return 1;
    }
    for (int i = 0; i < 50 * 8; i++) {           /* 8 seconds at 50 frames/s */
        for (int k = 0; k < 160; k++) {
            double t = (double)(i * 160 + k) / 8000.0;
            block[k] = (int16_t)(12000.0 * sin(2.0 * M_PI * 440.0 * t));
        }
        rec_push(block, 160);
    }
    rec_stats(&samples, &seconds);
    printf("recording: %s, %lu samples, %u s\n", rec_file(), samples, seconds);
    rec_stop();

    /* Starting again after a stop truncates and starts over.  Done on a second
       file, so the eight seconds above are left alone to be played. */
    {
        char second[300];
        snprintf(second, sizeof(second), "%s.2", path);
        if (!rec_start(second)) {
            printf("re-start failed: %s\n", rec_error());
            return 1;
        }
        rec_stats(&samples, &seconds);
        printf("re-started: %s, %lu samples (expected 0)\n", rec_file(), samples);
        rec_stop();
    }

    /* and a path that cannot be written must say so rather than pretend */
    if (rec_start("/nonexistent-directory/x.wav")) {
        printf("a start into a missing directory unexpectedly succeeded\n");
        return 1;
    }
    printf("start into a missing directory correctly failed: %s\n", rec_error());
    return 0;
}
