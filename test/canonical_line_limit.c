// Does a long line with NO newline wedge the tty, and does that explain a pane
// that stops accepting input after a paste?
//
//   cc -I src -I src/include -o /tmp/pty_canon_test test/canonical_line_limit.c src/flutter_pty.c && /tmp/pty_canon_test
//
// The child stays in CANONICAL mode on purpose — that is the state a shell or
// `cat` leaves the terminal in, and canonical mode delivers whole LINES only,
// with a per-line cap (MAX_CANON, 1024 on macOS). Anything longer than that with
// no newline cannot be handed to the reader, so the tty stops taking input while
// the writer keeps queueing.
//
// Echo is off (not raw) so the line discipline still applies but the harness has
// no output to post to a Dart isolate it does not have.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "flutter_pty.h"

#define TEST_OUTPUT_FILE "/tmp/pty_canon_out.bin"

// Comfortably past MAX_CANON, with no newline anywhere in it.
#define TEST_LONG_LINE_BYTES 4096

// Written AFTER the long line, and terminated, so it could be delivered if the
// tty were still accepting input.
static const char *TEST_FOLLOW_UP = "FOLLOWUP\n";

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

    char childCommand[256];

    snprintf(childCommand, sizeof(childCommand),
             "stty -echo; cat > %s", TEST_OUTPUT_FILE);

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

    usleep(500000);

    char *longLine = malloc(TEST_LONG_LINE_BYTES);

    if (longLine == NULL)
    {
        printf("FAIL: allocation\n");

        return 1;
    }

    memset(longLine, 'x', TEST_LONG_LINE_BYTES);

    struct timeval start;

    gettimeofday(&start, NULL);

    printf("writing %d bytes with NO newline, bypassing the line discipline...\n",
           TEST_LONG_LINE_BYTES);

    PtyWriteOptions longWrite;

    memset(&longWrite, 0, sizeof(longWrite));

    longWrite.buffer = longLine;
    longWrite.length = TEST_LONG_LINE_BYTES;
    longWrite.bypassLineDiscipline = true;

    pty_write(handle, &longWrite);

    // Then a terminated line the ORDINARY way, which proves canonical mode came
    // back afterwards rather than being left switched off.
    PtyWriteOptions followUpWrite;

    memset(&followUpWrite, 0, sizeof(followUpWrite));

    followUpWrite.buffer = (char *)TEST_FOLLOW_UP;
    followUpWrite.length = (int)strlen(TEST_FOLLOW_UP);

    pty_write(handle, &followUpWrite);

    // Give both plenty of time to drain.
    usleep(2000000);

    const long received = file_size(TEST_OUTPUT_FILE);

    printf("after %.2fs the child has %ld bytes\n", seconds_since(start), received);

    free(longLine);

    const long expected = TEST_LONG_LINE_BYTES + (long)strlen(TEST_FOLLOW_UP);

    if (received < expected)
    {
        printf("FAIL: %ld of %ld bytes — a line longer than MAX_CANON was refused,\n",
               received, expected);
        printf("      which is what wedges a pane. Splitting the write does not\n");
        printf("      help: the limit is on the LINE.\n");

        return 1;
    }

    printf("PASS: the over-long line AND the ordinary line after it both arrived,\n");
    printf("      so canonical mode was stepped around and then restored.\n");

    unlink(TEST_OUTPUT_FILE);

    return 0;
}
