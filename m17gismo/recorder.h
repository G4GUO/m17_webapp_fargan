// recorder.h
//
// Records what the receiver decodes - the same 8 kHz mono audio that goes to the
// speaker, whichever decoder produced it - so an example can be kept as a normal
// WAV file.  Switch FARGAN off for a classic example, on for a neural one, and
// the two files are directly comparable because both are captured here.
//
// Deliberately NOT a persisted setting: nothing writes a "record" line to
// config.txt, because a station that started recording itself after a reboot
// would fill its own disk.  It has to be asked for each time.
#ifndef RECORDER_H
#define RECORDER_H

#include <stdint.h>
#include <stdbool.h>

// Is a recording open?
bool rec_active(void);

// Start a recording.  path may be NULL, which names the file after the date and
// time (rx-YYYYMMDD-HHMMSS.wav) in the working directory.  Returns false on
// failure; rec_error() says why.
bool rec_start(const char *path);

// Stop and close it.  Safe to call when nothing is recording.
void rec_stop(void);

// The audio that is being played: mono, 8 kHz, signed 16-bit.
void rec_push(const int16_t *samples, int count);

// Current (or most recently used) file name, "" if there has never been one.
const char *rec_file(void);

// Why the last start failed, "" if it did not.
const char *rec_error(void);

// How much has gone into the current recording.
void rec_stats(unsigned long *samples, unsigned int *seconds);

#endif // RECORDER_H
