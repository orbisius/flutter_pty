// Proves a MULTI-MEGABYTE write arrives COMPLETE — the paste case.
//
// Build and run from the repo root:
//
//   cc -I src -I src/include -o /tmp/pty_large_test test/write_large_payload.c src/flutter_pty.c && /tmp/pty_large_test
//
// Two failures this catches, both of which look identical to a user ("it just
// stopped taking the paste"):
//
//   * the caller blocking forever, which is what the write queue fixed;
//   * a SHORT write being abandoned — the old code ignored write()'s return
//     value, and even the queued version dropped the tail on any errno it did
//     not expect.
//
// The child runs `stty raw -echo` so the line discipline neither echoes the
// payload back (this harness has no Dart isolate for the reader to post to) nor
// interprets any byte of it, and `head -c` so it exits on its own byte count
// instead of needing an EOF.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "flutter_pty.h"

// The size that failed by hand: a 7 MB crash log pasted into `cat > file`.
#define TEST_PAYLOAD_BYTES 7337871

#define TEST_OUTPUT_FILE "/tmp/pty_large_payload_out.bin"

#define TEST_TIMEOUT_SECONDS 60

static double seconds_since(struct timeval start)
{
    struct timeval now;

    gettimeofday(&now, NULL);

    double elapsed = (double)(now.tv_sec - start.tv_sec);

    elapsed += (double)(now.tv_usec - start.tv_usec) / 1000000.0;

    return elapsed;
}

static long file_size(const char *file)
{
    struct stat info;

    if (stat(file, &info) != 0)
    {
        return -1;
    }

    return (long)info.st_size;
}

int main(void)
{
    unlink(TEST_OUTPUT_FILE);

    char childCommand[512];

    snprintf(childCommand, sizeof(childCommand),
             "stty raw -echo; head -c %d > %s",
             TEST_PAYLOAD_BYTES, TEST_OUTPUT_FILE);

    char *arguments[] = {"sh", "-c", childCommand, NULL};

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

    // Give the child time to reach its stty before a byte is written.
    usleep(500000);

    char *payload = malloc(TEST_PAYLOAD_BYTES);

    if (payload == NULL)
    {
        printf("FAIL: could not allocate %d bytes\n", TEST_PAYLOAD_BYTES);

        return 1;
    }

    // A recognisable pattern rather than zeroes, so a truncation shows up as a
    // byte count AND the tail can be checked.
    for (int index = 0; index < TEST_PAYLOAD_BYTES; index++)
    {
        payload[index] = (char)('a' + (index % 26));
    }

    struct timeval start;

    gettimeofday(&start, NULL);

    PtyWriteOptions write;

    memset(&write, 0, sizeof(write));

    write.buffer = payload;
    write.length = TEST_PAYLOAD_BYTES;

    pty_write(handle, &write);

    double queuedAfter = seconds_since(start);

    printf("pty_write returned in %.4fs (queued, not yet drained)\n", queuedAfter);

    // Wait for the child to collect every byte.
    long written = 0;

    while (seconds_since(start) < TEST_TIMEOUT_SECONDS)
    {
        written = file_size(TEST_OUTPUT_FILE);

        if (written >= TEST_PAYLOAD_BYTES)
        {
            break;
        }

        usleep(100000);
    }

    double drainedAfter = seconds_since(start);

    free(payload);

    printf("child received %ld of %d bytes after %.2fs\n",
           written, TEST_PAYLOAD_BYTES, drainedAfter);

    if (written != TEST_PAYLOAD_BYTES)
    {
        printf("FAIL: %ld bytes short — the tail was dropped or the write stalled\n",
               (long)TEST_PAYLOAD_BYTES - written);

        return 1;
    }

    printf("PASS: every byte of a %d-byte write arrived\n", TEST_PAYLOAD_BYTES);

    unlink(TEST_OUTPUT_FILE);

    return 0;
}
