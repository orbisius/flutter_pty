
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#ifdef __APPLE__
// macOS has no execvpe, and a dylib cannot link the `environ` global directly —
// this is the documented way to reach it.
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#else
extern char **environ;
#endif

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

// The counting semaphore, built on a mutex + condition variable.
//
// NOT sem_t: unnamed POSIX semaphores are not implemented on Darwin. sem_init
// there returns -1/ENOSYS and every later sem_wait/sem_post fails with EBADF, so
// the reader never waited and the bound above silently did nothing on macOS while
// working on Linux. A mutex+condvar is plain pthreads, which both platforms
// implement, and it keeps the property the ack path needs: the thread that
// signals is not required to be the thread that waited.
typedef struct PtyCredits
{
    pthread_mutex_t mutex;

    pthread_cond_t available;

    unsigned int count;

} PtyCredits;

// Returns 0 on success, or the failing pthread error code — the CALLER MUST
// check it. A silent init failure is exactly what hid the Darwin breakage.
static int pty_credits_init(PtyCredits *credits, unsigned int initial)
{
    int mutexResult = pthread_mutex_init(&credits->mutex, NULL);

    if (mutexResult != 0)
    {
        return mutexResult;
    }

    int condResult = pthread_cond_init(&credits->available, NULL);

    if (condResult != 0)
    {
        pthread_mutex_destroy(&credits->mutex);

        return condResult;
    }

    credits->count = initial;

    return 0;
}

// Spends one credit, blocking until one is free.
//
// The wait is a LOOP, not an if: a condition variable may wake spuriously, and
// several waiters may race for one credit.
static void pty_credits_wait(PtyCredits *credits)
{
    pthread_mutex_lock(&credits->mutex);

    while (credits->count == 0)
    {
        pthread_cond_wait(&credits->available, &credits->mutex);
    }

    credits->count--;

    pthread_mutex_unlock(&credits->mutex);
}

// Hands one credit back. Safe from any thread — which is the whole reason this is
// a semaphore and not a mutex the reader holds.
static void pty_credits_post(PtyCredits *credits)
{
    pthread_mutex_lock(&credits->mutex);

    credits->count++;

    pthread_cond_signal(&credits->available);

    pthread_mutex_unlock(&credits->mutex);
}

// WRITING NEVER HAPPENS ON THE CALLER'S THREAD.
//
// A pty master is a blocking fd, and the caller here is Dart's UI thread. When
// the child is not draining its input the buffer fills, `write` parks, and the
// whole app stops — including the ack that would let the reader drain the other
// direction, so the two sides wait on each other forever. That deadlock was
// caught in a real hang (three threads: Dart in `write`, the reader in
// `pty_credits_wait`, the main thread in `Shell::~Shell()` waiting for Dart).
//
// So `pty_write` COPIES the bytes into this queue and returns immediately, and
// one thread per pty drains it with blocking writes. A slow child now costs
// memory instead of the UI thread, and a large paste — a 7 MB file into
// `cat > out.txt` — streams out at whatever rate the child reads while the
// terminal stays responsive.
typedef struct PtyWriteChunk
{
    char *bytes;

    size_t length;

    // Deliver this chunk with the line discipline's CANONICAL mode switched off,
    // then switch it back.
    //
    // Canonical mode hands the program whole lines and cannot hand over one
    // longer than MAX_CANON (1024). Past that the tty stops accepting input
    // entirely, so a paste containing one long line wedges the terminal — and
    // splitting the write does not help, because the limit is on the LINE, not
    // on the write. Measured: a 4096-byte line delivers 0 bytes at every chunk
    // size from 64 bytes up, and 4096 of 4096 with canonical mode off.
    //
    // The flip lives HERE, on the writing thread, because it has to be ordered
    // with the bytes: restoring from the caller would race the queue and wedge
    // the tty again.
    bool bypassLineDiscipline;

    struct PtyWriteChunk *next;

} PtyWriteChunk;

// A runaway guard, NOT a paste limit: normal pastes are megabytes and must all
// arrive, so this sits far above them and only catches a child that has stopped
// reading forever. Reaching it drops the newest write and says so, because the
// alternative — growing until the process is killed — loses the whole session.
#define PTY_WRITE_QUEUE_MAX_BYTES (256 * 1024 * 1024)

// How long a single wait for the pty to accept more may last. It only bounds one
// iteration — the write is retried until it completes — so this is a wake-up
// interval, not a deadline on the write.
#define PTY_WRITE_WAIT_MILLISECONDS 100

typedef struct PtyWriteQueue
{
    pthread_mutex_t mutex;

    pthread_cond_t pending;

    PtyWriteChunk *head;

    PtyWriteChunk *tail;

    size_t queuedBytes;

    int fd;

    // False when the queue could not be set up; `pty_write` then writes inline
    // rather than losing the data.
    bool running;

    // Set once the pty is finished. Without it the writer thread would park on
    // the condition variable forever after its pane closed — one leaked thread,
    // and its stack, per pty the user ever opened.
    bool closed;

} PtyWriteQueue;

typedef struct PtyHandle
{
    int ptm;

    int pid;

    PtyCredits read_credits;

    PtyWriteQueue write_queue;

    bool ackRead;

} PtyHandle;

// Writes every byte or reports how far it got.
//
// A pty write is allowed to be PARTIAL — it takes what fits and returns — so the
// single unchecked `write` this replaces silently dropped the tail of anything
// larger than the buffer, which is most of a paste. EINTR is a retry, not a
// failure: a signal arriving mid-write must not truncate the user's input.
static size_t write_all(int fd, const char *bytes, size_t length)
{
    size_t written = 0;

    while (written < length)
    {
        ssize_t result = write(fd, bytes + written, length - written);

        if (result < 0)
        {
            // A signal arriving mid-write is a retry, not a failure: truncating
            // there would lose the rest of what the user typed or pasted.
            if (errno == EINTR)
            {
                continue;
            }

            // The fd reports "not now" rather than an error. A pty master can do
            // this whenever the slave's input queue is full — so WAIT for room
            // instead of abandoning the tail, which is how a large paste used to
            // stop halfway with nothing said about it.
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                struct pollfd waiter;

                waiter.fd = fd;
                waiter.events = POLLOUT;
                waiter.revents = 0;

                poll(&waiter, 1, PTY_WRITE_WAIT_MILLISECONDS);

                continue;
            }

            // Anything else is a real failure, and the bytes are gone. SAY SO:
            // a silent short write is indistinguishable from a terminal that
            // simply stopped listening.
            fprintf(stderr, "flutter_pty: write failed after %zu of %zu bytes (errno %d)\n",
                    written, length, errno);

            break;
        }

        written += (size_t)result;
    }

    return written;
}

// Writes one chunk, taking the tty out of canonical mode first when the chunk
// asks for it and putting it back afterwards.
//
// The restore runs even when the write fails: leaving a terminal in a mode its
// program did not choose is worse than the failed write, because the user is
// then typing into a line discipline that no longer echoes or edits the way the
// program expects.
static void write_chunk(int fd, PtyWriteChunk *chunk)
{
    if (!chunk->bypassLineDiscipline)
    {
        write_all(fd, chunk->bytes, chunk->length);

        return;
    }

    struct termios original;

    // No termios means this is not a tty we can reshape — write it as it is
    // rather than refusing to write at all.
    if (tcgetattr(fd, &original) != 0)
    {
        write_all(fd, chunk->bytes, chunk->length);

        return;
    }

    struct termios delivery = original;

    delivery.c_lflag &= ~((tcflag_t)ICANON);

    if (tcsetattr(fd, TCSANOW, &delivery) != 0)
    {
        write_all(fd, chunk->bytes, chunk->length);

        return;
    }

    write_all(fd, chunk->bytes, chunk->length);

    // TCSADRAIN, not TCSANOW: canonical mode must not come back until the bytes
    // already handed to the tty have gone out, or the tail of the chunk meets
    // the very limit this avoided.
    tcsetattr(fd, TCSADRAIN, &original);
}

static void *write_loop(void *arg)
{
    PtyWriteQueue *queue = (PtyWriteQueue *)arg;

    while (1)
    {
        pthread_mutex_lock(&queue->mutex);

        while (queue->head == NULL && !queue->closed)
        {
            pthread_cond_wait(&queue->pending, &queue->mutex);
        }

        // Drain first, THEN exit: bytes already accepted from the caller are
        // still owed to the child, and the pty stays writable until its last
        // reader goes away.
        if (queue->head == NULL)
        {
            pthread_mutex_unlock(&queue->mutex);

            break;
        }

        PtyWriteChunk *chunk = queue->head;

        queue->head = chunk->next;

        if (queue->head == NULL)
        {
            queue->tail = NULL;
        }

        queue->queuedBytes -= chunk->length;

        // The blocking write happens OUTSIDE the lock, so a stalled child cannot
        // stop `pty_write` from queueing the next chunk.
        pthread_mutex_unlock(&queue->mutex);

        write_chunk(queue->fd, chunk);

        free(chunk->bytes);
        free(chunk);
    }

    return NULL;
}

// Returns 0 on success, or the failing pthread error code. The CALLER MUST check
// it: a queue that silently failed to start would send every keystroke down a
// path that does nothing.
static int pty_write_queue_init(PtyWriteQueue *queue, int fd)
{
    queue->head = NULL;
    queue->tail = NULL;
    queue->queuedBytes = 0;
    queue->fd = fd;
    queue->running = false;
    queue->closed = false;

    int mutexResult = pthread_mutex_init(&queue->mutex, NULL);

    if (mutexResult != 0)
    {
        return mutexResult;
    }

    int condResult = pthread_cond_init(&queue->pending, NULL);

    if (condResult != 0)
    {
        pthread_mutex_destroy(&queue->mutex);

        return condResult;
    }

    pthread_t thread;

    int threadResult = pthread_create(&thread, NULL, &write_loop, queue);

    if (threadResult != 0)
    {
        pthread_cond_destroy(&queue->pending);
        pthread_mutex_destroy(&queue->mutex);

        return threadResult;
    }

    pthread_detach(thread);

    queue->running = true;

    return 0;
}

// Tells the writer thread the pty is finished, so it drains what it still holds
// and then exits instead of parking forever. Called from the READER thread,
// which is the side that learns the pty ended (its read returns 0).
static void pty_write_queue_close(PtyWriteQueue *queue)
{
    if (!queue->running)
    {
        return;
    }

    pthread_mutex_lock(&queue->mutex);

    queue->closed = true;

    pthread_cond_signal(&queue->pending);

    pthread_mutex_unlock(&queue->mutex);
}

// The HANDLE, not a copy of its fields: the loop needs the fd, the credits, the
// write queue and the ack flag, and listing them one by one meant every new need
// changed this struct, the thread starter and the call site together.
//
// [port] stays separate because it is the one thing that does NOT live on the
// handle — it belongs to the create options.
typedef struct ReadLoopOptions
{
    PtyHandle *handle;

    Dart_Port port;

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
        if (options->handle->ackRead)
        {
            // Spend a credit to read. The app returns it once it has processed
            // the chunk, so at most PTY_READ_CREDITS reads can be in flight.
            pty_credits_wait(&options->handle->read_credits);
        }
        ssize_t n = read(options->handle->ptm, buffer, sizeof(buffer));

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

    pty_write_queue_close(&options->handle->write_queue);

    return NULL;
}

static void start_read_thread(PtyHandle *handle, Dart_Port port)
{
    ReadLoopOptions *options = malloc(sizeof(ReadLoopOptions));

    options->handle = handle;

    options->port = port;

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

// Applies `environment` to the child, ADDING to what it inherited.
//
// putenv can add a variable or change its value; it cannot remove one. Use
// replace_environment when the caller needs the environment to be exactly what
// it passed.
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

// Makes `environment` the child's WHOLE environment, dropping everything it
// inherited.
//
// Assigning `environ` rather than calling execvpe: execvpe is glibc-only, and
// this has to work on macOS too. exec* keeps whatever `environ` points at, so
// the assignment is what the child is left with — and it must happen in the
// child, after the fork, or the parent's own environment would be replaced.
static void replace_environment(char **environment)
{
    if (environment == NULL)
    {
        return;
    }

    environ = environment;
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
        if (options->replaceEnvironment)
        {
            replace_environment(options->environment);
        }
        else
        {
            set_environment(options->environment);
        }

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
    handle->ackRead = options->ackRead;

    int creditsResult = pty_credits_init(&handle->read_credits, PTY_READ_CREDITS);

    if (creditsResult != 0)
    {
        // Reading UNBOUNDED beats not reading at all — the terminal keeps working,
        // it just loses the flood bound. Said out loud rather than swallowed: the
        // previous silent failure is why this was broken on macOS for a release.
        fprintf(stderr, "flutter_pty: read credits unavailable (%d), reading unbounded\n", creditsResult);

        handle->ackRead = false;
    }

    int writeQueueResult = pty_write_queue_init(&handle->write_queue, ptm);

    if (writeQueueResult != 0)
    {
        // Inline writes are what this replaced, so the terminal still works — it
        // just regains the old risk of blocking the caller. Said out loud rather
        // than swallowed, for the same reason the credits failure is.
        fprintf(stderr, "flutter_pty: write queue unavailable (%d), writing inline\n", writeQueueResult);
    }

    start_read_thread(handle, options->stdout_port);

    start_wait_exit_thread(pid, options->exit_port);

    return handle;
}

FFI_PLUGIN_EXPORT void pty_write(PtyHandle *handle, PtyWriteOptions *options)
{
    if (options == NULL)
    {
        return;
    }

    char *buffer = options->buffer;
    const int length = options->length;
    const bool bypassLineDiscipline = options->bypassLineDiscipline;

    if (buffer == NULL || length <= 0)
    {
        return;
    }

    // No queue means the setup failed and said so at create time. Writing inline
    // keeps the terminal usable — it can block the caller, which is the old
    // behavior, and that beats dropping what the user typed.
    if (!handle->write_queue.running)
    {
        PtyWriteChunk inlineChunk;

        inlineChunk.bytes = buffer;
        inlineChunk.length = (size_t)length;
        inlineChunk.bypassLineDiscipline = bypassLineDiscipline;
        inlineChunk.next = NULL;

        write_chunk(handle->ptm, &inlineChunk);

        return;
    }

    // COPIED because the caller frees its buffer as soon as this returns, while
    // the writer thread reads it later.
    char *bytes = (char *)malloc((size_t)length);

    if (bytes == NULL)
    {
        fprintf(stderr, "flutter_pty: out of memory queueing %d bytes to write\n", length);

        return;
    }

    memcpy(bytes, buffer, (size_t)length);

    PtyWriteChunk *chunk = (PtyWriteChunk *)malloc(sizeof(PtyWriteChunk));

    if (chunk == NULL)
    {
        free(bytes);

        fprintf(stderr, "flutter_pty: out of memory queueing a write of %d bytes\n", length);

        return;
    }

    chunk->bytes = bytes;
    chunk->length = (size_t)length;
    chunk->bypassLineDiscipline = bypassLineDiscipline;
    chunk->next = NULL;

    pthread_mutex_lock(&handle->write_queue.mutex);

    // The child is gone, so there is nobody left to read this. Queueing it would
    // hand bytes to a thread that has already stopped taking work.
    if (handle->write_queue.closed)
    {
        pthread_mutex_unlock(&handle->write_queue.mutex);

        free(chunk->bytes);
        free(chunk);

        return;
    }

    if (handle->write_queue.queuedBytes + (size_t)length > PTY_WRITE_QUEUE_MAX_BYTES)
    {
        pthread_mutex_unlock(&handle->write_queue.mutex);

        free(chunk->bytes);
        free(chunk);

        fprintf(stderr, "flutter_pty: write queue full (%zu bytes unread by the child), dropped %d bytes\n",
                handle->write_queue.queuedBytes, length);

        return;
    }

    if (handle->write_queue.tail == NULL)
    {
        handle->write_queue.head = chunk;
        handle->write_queue.tail = chunk;
    }
    else
    {
        handle->write_queue.tail->next = chunk;
        handle->write_queue.tail = chunk;
    }

    handle->write_queue.queuedBytes += (size_t)length;

    pthread_cond_signal(&handle->write_queue.pending);

    pthread_mutex_unlock(&handle->write_queue.mutex);
}

FFI_PLUGIN_EXPORT void pty_ack_read(PtyHandle *handle)
{
    if (handle->ackRead)
    {
        // Hands the credit back so one more read may happen. Posting from a
        // different thread than the waiter is well defined, unlike a mutex unlock.
        pty_credits_post(&handle->read_credits);
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
