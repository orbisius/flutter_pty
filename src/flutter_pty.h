#ifndef FLUTTER_PTY_H_
#define FLUTTER_PTY_H_

#if _WIN32
#define FFI_PLUGIN_EXPORT __declspec(dllexport)
#else
#define FFI_PLUGIN_EXPORT
#endif

#if defined(__linux__) || defined(__GLIBC__) || defined(__GNU__)
#define _GNU_SOURCE /* GNU glibc grantpt() prototypes */
#endif

#include "include/dart_api_dl.h"

typedef struct PtyOptions
{
    int rows;

    int cols;

    char *executable;

    char **arguments;

    char **environment;

    char *working_directory;

    Dart_Port stdout_port;

    Dart_Port exit_port;

    bool ackRead;

    /// Whether `environment` REPLACES the child's environment instead of being
    /// added to the inherited one.
    ///
    /// The unix child applies `environment` with putenv, which can add a
    /// variable or change its value but cannot REMOVE one — so a caller has no
    /// way to stop something in the parent's environment reaching the child.
    /// That matters for a terminal: an editor, an IDE or an agent launching the
    /// app passes its own variables down to every shell the user opens, where
    /// they silently change how unrelated tools behave.
    ///
    /// Off by default, so existing callers keep the additive behaviour. When on,
    /// `environment` is the child's whole environment — which is already how the
    /// Windows implementation works, so this also makes the platforms agree.
    bool replaceEnvironment;

} PtyOptions;

typedef struct PtyHandle PtyHandle;

FFI_PLUGIN_EXPORT PtyHandle *pty_create(PtyOptions *options);

/// What to send to the pty, in the same shape as [PtyOptions] — a struct rather
/// than a growing argument list, so a new capability adds a FIELD instead of
/// changing the signature and every call site with it.
typedef struct PtyWriteOptions
{
    char *buffer;

    int length;

    /// Deliver this write with the terminal's CANONICAL mode switched off, then
    /// switch it back.
    ///
    /// Canonical mode hands the program whole lines and cannot hand over one
    /// longer than MAX_CANON (1024): past that the tty stops accepting input at
    /// all, so a paste holding one long line wedges the session until the program
    /// is interrupted. Splitting the write does NOT avoid it — the limit is on
    /// the LINE, not the write (measured at every chunk size from 64 bytes up).
    ///
    /// False for ordinary input: a terminal should not reshape a program's tty
    /// for a keystroke. Windows has no line discipline and ignores this.
    bool bypassLineDiscipline;

} PtyWriteOptions;

/// Sends [options] to the pty. The bytes are COPIED and queued, so this returns
/// immediately and never blocks the caller.
FFI_PLUGIN_EXPORT void pty_write(PtyHandle *handle, PtyWriteOptions *options);

FFI_PLUGIN_EXPORT void pty_ack_read(PtyHandle *handle);

FFI_PLUGIN_EXPORT int pty_resize(PtyHandle *handle, int rows, int cols);

FFI_PLUGIN_EXPORT int pty_getpid(PtyHandle *handle);

FFI_PLUGIN_EXPORT char *pty_error(void);

#endif