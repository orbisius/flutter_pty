// Proves the write path never parks the CALLER, which is what froze the app.
//
// The child here reads nothing and prints nothing (`sleep`), so the pty's input
// buffer fills after a few kilobytes and stays full. Before the write queue, a
// large `pty_write` blocked there forever — on the app's UI thread — and the
// hang took the whole process down with it, quit included.
//
// Build and run from the repo root:
//
//   cc -I src -I src/include -o /tmp/pty_write_test test/write_does_not_block.c src/flutter_pty.c && /tmp/pty_write_test
//
// Exits non-zero and says which expectation failed, so it is usable as a gate.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "flutter_pty.h"

// Far larger than any pty buffer, so the write CANNOT complete inline and the
// old code had to block.
#define TEST_WRITE_BYTES (8 * 1024 * 1024)

// Generous: the queueing path is memcpy plus a list append, so it is microseconds
// in practice. Anything approaching a second means it went back to waiting on the
// child.
#define TEST_MAX_SECONDS 2.0

static double seconds_since(struct timeval start)
{
    struct timeval now;

    gettimeofday(&now, NULL);

    double elapsed = (double)(now.tv_sec - start.tv_sec);

    elapsed += (double)(now.tv_usec - start.tv_usec) / 1000000.0;

    return elapsed;
}

int main(void)
{
    // A child that never reads its input and never writes output: the pty fills
    // up and nothing drains it, which is the state a suspended or unresponsive
    // program leaves behind.
    //
    // `stty -echo raw` first, because a pty echoes what is written to it by
    // default — that echo would come back as OUTPUT, and this harness has no
    // Dart isolate for the read thread to post it to. Silencing the terminal
    // keeps the test measuring the write path instead of crashing in the
    // reader.
    char *arguments[] = {"sh", "-c", "stty -echo raw; exec sleep 30", NULL};

    PtyOptions options;

    memset(&options, 0, sizeof(options));

    options.rows = 24;
    options.cols = 80;
    options.executable = "/bin/sh";
    options.arguments = arguments;
    options.stdout_port = 0;
    options.exit_port = 0;
    options.ackRead = false;

    PtyHandle *handle = pty_create(&options);

    if (handle == NULL)
    {
        printf("FAIL: pty_create returned NULL (%s)\n", pty_error());

        return 1;
    }

    // Let the child reach its `stty` before anything is written, so no byte is
    // echoed while the terminal is still in its default mode.
    usleep(400000);

    char *payload = malloc(TEST_WRITE_BYTES);

    if (payload == NULL)
    {
        printf("FAIL: could not allocate the test payload\n");

        return 1;
    }

    memset(payload, 'x', TEST_WRITE_BYTES);

    struct timeval start;

    gettimeofday(&start, NULL);

    PtyWriteOptions write;

    memset(&write, 0, sizeof(write));

    write.buffer = payload;
    write.length = TEST_WRITE_BYTES;

    pty_write(handle, &write);

    double elapsed = seconds_since(start);

    free(payload);

    printf("pty_write of %d bytes returned in %.4fs\n", TEST_WRITE_BYTES, elapsed);

    if (elapsed > TEST_MAX_SECONDS)
    {
        printf("FAIL: the caller was blocked (over %.1fs) — the write is not queued\n", TEST_MAX_SECONDS);

        return 1;
    }

    printf("PASS: the caller returned immediately with the child not reading\n");

    return 0;
}
