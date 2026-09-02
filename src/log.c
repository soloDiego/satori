// Timestamped diagnostics. Every line satori writes goes through here.
//
// The timestamp is the whole point. A window manager bug is only ever reported
// as "I pressed the key and nothing happened", and without a clock there is no
// way to line that sentence up against the log -- an absent binding and a
// binding that fired an hour ago look identical. Wall clock rather than
// monotonic, so it also lines up with journald and with the app logs that share
// this stream.

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include "satori.h"

void satori_log(const char *fmt, ...) {
    struct timespec ts;
    struct tm tm;
    char stamp[16] = "--:--:--.---";

    // Built by hand rather than with strftime: strftime leaves the buffer
    // indeterminate when it fails, and there is nowhere useful to report that
    // from inside the logger.
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0 && localtime_r(&ts.tv_sec, &tm)) {
        // Milliseconds, because a whole manage sequence is sub-millisecond and
        // seconds alone collapse an entire burst onto one stamp. The % 1000 is
        // not redundant: it is what tells the compiler this is three digits, so
        // the fixed-size buffer is provably big enough.
        unsigned ms = (unsigned) (ts.tv_nsec / 1000000) % 1000u;
        snprintf(stamp, sizeof stamp, "%02d:%02d:%02d.%03u",
                 tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
    }
    fprintf(stderr, "%s ", stamp);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    // stderr is line buffered on a terminal but FULLY buffered when redirected
    // to a file, which is how satori always runs. Without this flush the log
    // lags behind the session and a hang shows an empty file -- which is
    // exactly the moment the log is wanted.
    fflush(stderr);
}
