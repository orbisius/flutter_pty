#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Windows.h>

#include "flutter_pty.h"

#include "include/dart_api.h"
#include "include/dart_api_dl.h"
#include "include/dart_native_api.h"

// WRITING NEVER HAPPENS ON THE CALLER'S THREAD.
//
// `WriteFile` on a pipe blocks once the pipe is full and the child is not
// reading, and the caller here is Dart's UI thread — so a paste into a program
// that has stopped consuming stdin froze the whole app. Worse, the previous code
// followed every write with `FlushFileBuffers`, which waits for the READER to
// consume, so it blocked even when the pipe had room.
//
// The Unix side hit the same wall and is fixed the same way, so the two
// platforms behave alike: `pty_write` copies the bytes and returns, and one
// thread per pty drains the queue. A large paste — a 7 MB file into
// `cat > out.txt` — streams out while the terminal stays responsive.
typedef struct PtyWriteChunk
{
    char *bytes;

    size_t length;

    struct PtyWriteChunk *next;

} PtyWriteChunk;

// A runaway guard, NOT a paste limit: normal pastes are megabytes and must all
// arrive, so this sits far above them and only catches a child that has stopped
// reading forever.
#define PTY_WRITE_QUEUE_MAX_BYTES (256 * 1024 * 1024)

typedef struct PtyWriteQueue
{
    CRITICAL_SECTION lock;

    CONDITION_VARIABLE pending;

    PtyWriteChunk *head;

    PtyWriteChunk *tail;

    size_t queuedBytes;

    HANDLE fileHandle;

    // False when the queue could not be set up; `pty_write` then writes inline
    // rather than losing the data.
    BOOL running;

    // Set once the pty is finished, so the writer thread stops waiting for work
    // instead of parking forever — one leaked thread per pty otherwise.
    BOOL closed;

} PtyWriteQueue;

// Defined here, above the read loop, because that loop takes the HANDLE and
// reads its fields — a forward declaration would not be enough.
typedef struct PtyHandle
{
    PHANDLE inputWriteSide;

    PHANDLE outputReadSide;

    HPCON hPty;

    DWORD dwProcessId;

    BOOL ackRead;

    HANDLE hMutex;

    PtyWriteQueue write_queue;

} PtyHandle;

// The read loop closes the queue when the pty ends, so it needs this before the
// definition further down.
static void pty_write_queue_close(PtyWriteQueue *queue);

static LPWSTR build_command(char *executable, char **arguments)
{
    int command_length = 0;

    if (executable != NULL)
    {
        command_length += (int)strlen(executable);
    }

    if (arguments != NULL)
    {
        int i = 0;

        while (arguments[i] != NULL)
        {
            command_length += (int)strlen(arguments[i]) + 1;
            i++;
        }
    }

    LPWSTR command = malloc((command_length + 1) * sizeof(WCHAR));

    if (command != NULL)
    {
        int i = 0;

        if (executable != NULL)
        {
            int j = 0;

            while (executable[j] != 0)
            {
                command[i] = (WCHAR)executable[j];
                i++;
                j++;
            }
        }

        if (arguments != NULL)
        {
            int j = 0;

            while (arguments[j] != NULL)
            {
                command[i++] = ' ';

                int k = 0;

                while (arguments[j][k] != 0)
                {
                    command[i] = (WCHAR)arguments[j][k];
                    i++;
                    k++;
                }

                j++;
            }
        }

        command[i] = 0;
    }

    return command;
}

static LPWSTR build_environment(char **environment)
{
    LPWSTR environment_block = NULL;
    int environment_block_length = 0;

    if (environment != NULL)
    {
        int i = 0;

        while (environment[i] != NULL)
        {
            environment_block_length += (int)strlen(environment[i]) + 1;
            i++;
        }
    }

    environment_block = malloc((environment_block_length + 1) * sizeof(WCHAR));

    if (environment_block != NULL)
    {
        int i = 0;

        if (environment != NULL)
        {
            int j = 0;

            while (environment[j] != NULL)
            {
                int k = 0;

                while (environment[j][k] != 0)
                {
                    environment_block[i] = (WCHAR)environment[j][k];
                    i++;
                    k++;
                }

                environment_block[i++] = 0;

                j++;
            }
        }

        environment_block[i] = 0;
    }

    return environment_block;
}

static LPWSTR build_working_directory(char *working_directory)
{
    if (working_directory == NULL)
    {
        return NULL;
    }

    int working_directory_length = (int)strlen(working_directory);

    LPWSTR working_directory_block = malloc((working_directory_length + 1) * sizeof(WCHAR));

    if (working_directory_block == NULL)
    {
        return NULL;
    }

    int i = 0;

    while (working_directory[i] != 0)
    {
        working_directory_block[i] = (WCHAR)working_directory[i++];
    }

    working_directory_block[i] = 0;

    return working_directory_block;
}

// The HANDLE, not a copy of its fields: the loop needs the pipe, the read
// semaphore, the ack flag and the write queue, and listing them one by one meant
// every new need changed this struct, the thread starter and the call site
// together.
//
// [port] stays separate because it is the one thing that does NOT live on the
// handle — it belongs to the create options.
typedef struct ReadLoopOptions
{
    PtyHandle *handle;

    Dart_Port port;

} ReadLoopOptions;

// See the note in flutter_pty_unix.c — one read is one Dart port message, so the
// read size is what decides throughput under heavy output.
#define PTY_READ_BUFFER_SIZE (64 * 1024)

// How many reads may be outstanding before the reader waits for the app to
// acknowledge one. A single permit lets no reading happen while the app parses,
// which costs most of the throughput; a few permits bound the read-ahead without
// giving that up. The bound is the point — it is what keeps a flood from queueing
// thousands of messages ahead of the user's next keystroke.
#define PTY_READ_CREDITS 4

static DWORD WINAPI read_loop(LPVOID arg)
{
    ReadLoopOptions *options = (ReadLoopOptions *)arg;

    char buffer[PTY_READ_BUFFER_SIZE];

    while (1)
    {
        DWORD readlen = 0;

        if (options->handle->ackRead)
        {
            WaitForSingleObject(options->handle->hMutex, INFINITE);
        }

        BOOL ok = ReadFile((HANDLE)options->handle->outputReadSide, buffer, sizeof(buffer), &readlen, NULL);

        if (!ok)
        {
            break;
        }

        if (readlen <= 0)
        {
            break;
        }

        Dart_CObject result;
        result.type = Dart_CObject_kTypedData;
        result.value.as_typed_data.type = Dart_TypedData_kUint8;
        result.value.as_typed_data.length = readlen;
        result.value.as_typed_data.values = (uint8_t *)buffer;

        Dart_PostCObject_DL(options->port, &result);
    }

    pty_write_queue_close(&options->handle->write_queue);

    return 0;
}

static void start_read_thread(PtyHandle *handle, Dart_Port port)
{
    ReadLoopOptions *options = malloc(sizeof(ReadLoopOptions));

    options->handle = handle;
    options->port = port;

    DWORD thread_id;

    HANDLE thread = CreateThread(NULL, 0, read_loop, options, 0, &thread_id);

    if (thread == NULL)
    {
        free(options);
    }
}

typedef struct WaitExitOptions
{
    HANDLE pid;

    Dart_Port port;

    HANDLE hMutex;
} WaitExitOptions;

static DWORD WINAPI wait_exit_thread(LPVOID arg)
{
    WaitExitOptions *options = (WaitExitOptions *)arg;

    DWORD exit_code = 0;

    WaitForSingleObject(options->pid, INFINITE);

    GetExitCodeProcess(options->pid, &exit_code);

    CloseHandle(options->pid);
    CloseHandle(options->hMutex);

    Dart_PostInteger_DL(options->port, exit_code);

    return 0;
}

static void start_wait_exit_thread(HANDLE pid, Dart_Port port, HANDLE mutex)
{
    WaitExitOptions *options = malloc(sizeof(WaitExitOptions));

    options->pid = pid;
    options->port = port;
    options->hMutex = mutex;

    DWORD thread_id;

    HANDLE thread = CreateThread(NULL, 0, wait_exit_thread, options, 0, &thread_id);

    if (thread == NULL)
    {
        free(options);
    }
}

// Writes every byte or reports how far it got.
//
// `WriteFile` on a pipe may report fewer bytes than asked for, so the single
// unchecked call this replaces could drop the tail of a large paste.
static size_t write_all(HANDLE fileHandle, const char *bytes, size_t length)
{
    size_t written = 0;

    while (written < length)
    {
        DWORD chunkWritten = 0;

        // DWORD is 32-bit while the queue counts in size_t, so a single write is
        // capped rather than truncated by the conversion.
        size_t remaining = length - written;
        DWORD toWrite = remaining > MAXDWORD ? MAXDWORD : (DWORD)remaining;

        if (!WriteFile(fileHandle, bytes + written, toWrite, &chunkWritten, NULL))
        {
            break;
        }

        if (chunkWritten == 0)
        {
            break;
        }

        written += chunkWritten;
    }

    return written;
}

static DWORD WINAPI write_loop(LPVOID arg)
{
    PtyWriteQueue *queue = (PtyWriteQueue *)arg;

    while (1)
    {
        EnterCriticalSection(&queue->lock);

        while (queue->head == NULL && !queue->closed)
        {
            SleepConditionVariableCS(&queue->pending, &queue->lock, INFINITE);
        }

        // Drain first, THEN exit: bytes already accepted from the caller are
        // still owed to the child.
        if (queue->head == NULL)
        {
            LeaveCriticalSection(&queue->lock);

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
        LeaveCriticalSection(&queue->lock);

        write_all(queue->fileHandle, chunk->bytes, chunk->length);

        free(chunk->bytes);
        free(chunk);
    }

    return 0;
}

// Returns TRUE on success. The CALLER MUST check it: a queue that silently
// failed to start would send every keystroke down a path that does nothing.
static BOOL pty_write_queue_init(PtyWriteQueue *queue, HANDLE fileHandle)
{
    queue->head = NULL;
    queue->tail = NULL;
    queue->queuedBytes = 0;
    queue->fileHandle = fileHandle;
    queue->running = FALSE;
    queue->closed = FALSE;

    InitializeCriticalSection(&queue->lock);
    InitializeConditionVariable(&queue->pending);

    DWORD threadId = 0;

    HANDLE thread = CreateThread(NULL, 0, write_loop, queue, 0, &threadId);

    if (thread == NULL)
    {
        DeleteCriticalSection(&queue->lock);

        return FALSE;
    }

    CloseHandle(thread);

    queue->running = TRUE;

    return TRUE;
}

// Tells the writer thread the pty is finished, so it drains what it still holds
// and then exits.
static void pty_write_queue_close(PtyWriteQueue *queue)
{
    if (!queue->running)
    {
        return;
    }

    EnterCriticalSection(&queue->lock);

    queue->closed = TRUE;

    WakeConditionVariable(&queue->pending);

    LeaveCriticalSection(&queue->lock);
}

char *error_message = NULL;

FFI_PLUGIN_EXPORT PtyHandle *pty_create(PtyOptions *options)
{
    HANDLE inputReadSide = NULL;
    HANDLE inputWriteSide = NULL;

    HANDLE outputReadSide = NULL;
    HANDLE outputWriteSide = NULL;

    if (!CreatePipe(&inputReadSide, &inputWriteSide, NULL, 0))
    {
        error_message = "Failed to create input pipe";
        return NULL;
    }

    if (!CreatePipe(&outputReadSide, &outputWriteSide, NULL, 0))
    {
        error_message = "Failed to create output pipe";
        return NULL;
    }

    COORD size;

    size.X = options->cols;
    size.Y = options->rows;

    HPCON hPty;

    HRESULT result = CreatePseudoConsole(size, inputReadSide, outputWriteSide, 0, &hPty);

    if (FAILED(result))
    {
        error_message = "Failed to create pseudo console";
        return NULL;
    }

    STARTUPINFOEX startupInfo;

    ZeroMemory(&startupInfo, sizeof(startupInfo));
    startupInfo.StartupInfo.cb = sizeof(startupInfo);

    startupInfo.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.StartupInfo.hStdInput = NULL;
    startupInfo.StartupInfo.hStdOutput = NULL;
    startupInfo.StartupInfo.hStdError = NULL;

    SIZE_T bytesRequired;
    InitializeProcThreadAttributeList(NULL, 1, 0, &bytesRequired);
    startupInfo.lpAttributeList = (PPROC_THREAD_ATTRIBUTE_LIST)malloc(bytesRequired);

    BOOL ok = InitializeProcThreadAttributeList(startupInfo.lpAttributeList, 1, 0, &bytesRequired);

    if (!ok)
    {
        error_message = "Failed to initialize proc thread attribute list";
        return NULL;
    }

    ok = UpdateProcThreadAttribute(startupInfo.lpAttributeList,
                                   0,
                                   PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   hPty,
                                   sizeof(hPty),
                                   NULL,
                                   NULL);

    if (!ok)
    {
        error_message = "Failed to update proc thread attribute list";
        return NULL;
    }

    LPWSTR command = build_command(options->executable, options->arguments);

    LPWSTR environment_block = build_environment(options->environment);

    LPWSTR working_directory = build_working_directory(options->working_directory);

    PROCESS_INFORMATION processInfo;
    ZeroMemory(&processInfo, sizeof(processInfo));

    Sleep(1000);

    ok = CreateProcessW(NULL,
                        command,
                        NULL,
                        NULL,
                        FALSE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                        environment_block,
                        working_directory,
                        &startupInfo.StartupInfo,
                        &processInfo);

    if (command != NULL)
    {
        free(command);
    }

    if (environment_block != NULL)
    {
        free(environment_block);
    }

    if (working_directory != NULL)
    {
        free(working_directory);
    }

    if (!ok)
    {
        error_message = "Failed to create process";
        DWORD error = GetLastError();
        printf("error no: %d\n", error);
        return NULL;
    }

    // free(startupInfo.lpAttributeList);

    // CloseHandle(processInfo.hThread);

    HANDLE mutex = CreateSemaphore(
        NULL,               // default security attributes
        PTY_READ_CREDITS,   // initial count
        PTY_READ_CREDITS,   // maximum count
        NULL);

    // The handle is built BEFORE the threads start, because the read loop now
    // takes it — and because the write queue must exist before anything can be
    // written to the pty.
    PtyHandle *pty = malloc(sizeof(PtyHandle));

    if (pty == NULL)
    {
        error_message = "Failed to allocate pty handle";
        return NULL;
    }

    pty->inputWriteSide = inputWriteSide;
    pty->outputReadSide = outputReadSide;
    pty->hPty = hPty;
    pty->dwProcessId = processInfo.dwProcessId;
    pty->ackRead = options->ackRead;
    pty->hMutex = mutex;

    if (!pty_write_queue_init(&pty->write_queue, (HANDLE)inputWriteSide))
    {
        // Inline writes are what this replaced, so the terminal still works — it
        // just regains the old risk of blocking the caller. Said out loud rather
        // than swallowed.
        fprintf(stderr, "flutter_pty: write queue unavailable (%lu), writing inline\n", GetLastError());
    }

    start_read_thread(pty, options->stdout_port);

    start_wait_exit_thread(processInfo.hProcess, options->exit_port, mutex);

    return pty;
}

// `bypassLineDiscipline` is read and ignored: a Windows pty is a PIPE with no
// line discipline, so there is no canonical mode to step around and no per-line
// limit to hit. The field exists so both platforms present one signature.
FFI_PLUGIN_EXPORT void pty_write(PtyHandle *handle, PtyWriteOptions *options)
{
    if (options == NULL)
    {
        return;
    }

    char *buffer = options->buffer;
    const int length = options->length;

    if (buffer == NULL || length <= 0)
    {
        return;
    }

    // No queue means the setup failed and said so at create time. Writing inline
    // keeps the terminal usable — it can block the caller, which is the old
    // behavior, and that beats dropping what the user typed.
    if (!handle->write_queue.running)
    {
        write_all((HANDLE)handle->inputWriteSide, buffer, (size_t)length);

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
    chunk->next = NULL;

    EnterCriticalSection(&handle->write_queue.lock);

    // The child is gone, so there is nobody left to read this.
    if (handle->write_queue.closed)
    {
        LeaveCriticalSection(&handle->write_queue.lock);

        free(chunk->bytes);
        free(chunk);

        return;
    }

    if (handle->write_queue.queuedBytes + (size_t)length > PTY_WRITE_QUEUE_MAX_BYTES)
    {
        LeaveCriticalSection(&handle->write_queue.lock);

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

    WakeConditionVariable(&handle->write_queue.pending);

    LeaveCriticalSection(&handle->write_queue.lock);
}

FFI_PLUGIN_EXPORT void pty_ack_read(PtyHandle *handle)
{
    if (handle->ackRead)
    {
        ReleaseSemaphore(handle->hMutex, 1, NULL);
    }
}

FFI_PLUGIN_EXPORT int pty_resize(PtyHandle *handle, int rows, int cols)
{
    COORD size;

    size.X = cols;
    size.Y = rows;

    return ResizePseudoConsole(handle->hPty, size);
}

FFI_PLUGIN_EXPORT int pty_getpid(PtyHandle *handle)
{
    return (int)handle->dwProcessId;
}

FFI_PLUGIN_EXPORT char *pty_error()
{
    return error_message;
}
