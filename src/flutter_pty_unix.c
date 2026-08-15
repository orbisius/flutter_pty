
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include "forkpty.h"
#include "flutter_pty.h"

#include "include/dart_api.h"
#include "include/dart_api_dl.h"
#include "include/dart_native_api.h"

// How many reads may be outstanding before the reader has to wait for the app to
// acknowledge one.
//
// A COUNTING semaphore rather than a mutex, for two reasons. Correctness: the ack
// comes from a different thread than the one that took the lock, and unlocking a
// pthread mutex from a foreign thread is undefined — measured as a segfault.
// Throughput: a single permit would serialize reading against parsing and cost a
// whole core of pipelining (measured 10x slower), while a few permits let the
// reader run ahead a bounded amount. The bound is the point — it is what stops a
// flood queueing thousands of messages ahead of the user's next keystroke.
#define PTY_READ_CREDITS 4

typedef struct PtyHandle
{
    int ptm;

    int pid;

    sem_t read_credits;

    bool ackRead;

} PtyHandle;

typedef struct ReadLoopOptions
{
    int fd;

    sem_t *read_credits;

    Dart_Port port;

    bool waitForReadAck;

} ReadLoopOptions;

char *error_message = NULL;

// One read is one Dart port message, and each message costs a typed-data
// allocation, a stream event and the GC that follows. The read SIZE therefore
// decides throughput under heavy output far more than the byte count does: at
// 1 KB, 15 MB of output became ~15,000 messages and the isolate spent its time
// on message machinery instead of the terminal.
//
// read() returns as soon as any data is available and never waits to fill this,
// so an echoed keystroke still arrives in one small read — interactive latency
// is unaffected.
#define PTY_READ_BUFFER_SIZE (64 * 1024)

static void *read_loop(void *arg)
{
    ReadLoopOptions *options = (ReadLoopOptions *)arg;

    char buffer[PTY_READ_BUFFER_SIZE];

    while (1)
    {
        if (options->waitForReadAck)
        {
            // Spend a credit to read. The app returns it once it has processed
            // the chunk, so at most PTY_READ_CREDITS reads can be in flight.
            sem_wait(options->read_credits);
        }
        ssize_t n = read(options->fd, buffer, sizeof(buffer));

        if (n < 0)
        {
            // TODO: handle error
            break;
        }

        if (n == 0)
        {
            break;
        }

        Dart_CObject result;
        result.type = Dart_CObject_kTypedData;
        result.value.as_typed_data.type = Dart_TypedData_kUint8;
        result.value.as_typed_data.length = n;
        result.value.as_typed_data.values = (uint8_t *)buffer;

        Dart_PostCObject_DL(options->port, &result);
    }

    return NULL;
}

static void start_read_thread(int fd, Dart_Port port, sem_t *read_credits, bool waitForReadAck)
{
    ReadLoopOptions *options = malloc(sizeof(ReadLoopOptions));

    options->fd = fd;

    options->port = port;

    options->read_credits = read_credits;

    options->waitForReadAck = waitForReadAck;

    pthread_t _thread;

    pthread_create(&_thread, NULL, &read_loop, options);
}

typedef struct WaitExitOptions
{
    int pid;

    Dart_Port port;

} WaitExitOptions;

static void *wait_exit_thread(void *arg)
{
    WaitExitOptions *options = (WaitExitOptions *)arg;

    int status;

    waitpid(options->pid, &status, 0);

    if (WIFEXITED(status))
    {
        Dart_PostInteger_DL(options->port, WEXITSTATUS(status));
    }
    else if (WIFSIGNALED(status))
    {
        Dart_PostInteger_DL(options->port, -WTERMSIG(status));
    }

    return NULL;
}

static void start_wait_exit_thread(int pid, Dart_Port port)
{
    WaitExitOptions *options = malloc(sizeof(WaitExitOptions));

    options->pid = pid;

    options->port = port;

    pthread_t _thread;

    pthread_create(&_thread, NULL, &wait_exit_thread, options);
}

static void set_environment(char **environment)
{
    if (environment == NULL)
    {
        return;
    }

    while (*environment != NULL)
    {
        putenv(*environment);
        environment++;
    }
}

FFI_PLUGIN_EXPORT PtyHandle *pty_create(PtyOptions *options)
{
    struct winsize ws;

    ws.ws_row = options->rows;
    ws.ws_col = options->cols;

    int ptm;

    int pid = pty_forkpty(&ptm, NULL, NULL, &ws);

    if (pid < 0)
    {
        error_message = "pty_forkpty failed";
        perror("pty_forkpty");
        return NULL;
    }

    if (pid == 0)
    {
        set_environment(options->environment);

        if (options->working_directory != NULL && strlen(options->working_directory) > 0)
        {
            chdir(options->working_directory);
        }

        int ok = execvp(options->executable, options->arguments);

        if (ok < 0)
        {
            perror("execvp");
        }
    }

    PtyHandle *handle = (PtyHandle *)malloc(sizeof(PtyHandle));

    handle->ptm = ptm;
    handle->pid = pid;
    sem_init(&handle->read_credits, 0, PTY_READ_CREDITS);
    handle->ackRead = options->ackRead;

    start_read_thread(ptm, options->stdout_port, &handle->read_credits, options->ackRead);

    start_wait_exit_thread(pid, options->exit_port);

    return handle;
}

FFI_PLUGIN_EXPORT void pty_write(PtyHandle *handle, char *buffer, int length)
{
    write(handle->ptm, buffer, length);
}

FFI_PLUGIN_EXPORT void pty_ack_read(PtyHandle *handle)
{
    if (handle->ackRead)
    {
        // Hands the credit back so one more read may happen. sem_post from a
        // different thread than sem_wait is well defined, unlike a mutex unlock.
        sem_post(&handle->read_credits);
    }
}

FFI_PLUGIN_EXPORT int pty_resize(PtyHandle *handle, int rows, int cols)
{
    struct winsize ws;

    ws.ws_row = rows;
    ws.ws_col = cols;

    return ioctl(handle->ptm, TIOCSWINSZ, &ws);
}

FFI_PLUGIN_EXPORT int pty_getpid(PtyHandle *handle)
{
    return handle->pid;
}

FFI_PLUGIN_EXPORT char *pty_error(void)
{
    return NULL;
}
