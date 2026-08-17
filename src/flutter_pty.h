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

FFI_PLUGIN_EXPORT void pty_write(PtyHandle *handle, char *buffer, int length);

FFI_PLUGIN_EXPORT void pty_ack_read(PtyHandle *handle);

FFI_PLUGIN_EXPORT int pty_resize(PtyHandle *handle, int rows, int cols);

FFI_PLUGIN_EXPORT int pty_getpid(PtyHandle *handle);

FFI_PLUGIN_EXPORT char *pty_error(void);

#endif