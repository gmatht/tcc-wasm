/* sys/wait.h — the wasm sandbox has NO child processes: there is
   nothing to wait for.  The wait family is declared here (so code that
   includes sys/wait.h compiles) and resolves to the fork's internal
   ECHILD stubs at link time — wait()/waitpid()/wait4() return -1 with
   errno = ECHILD (no children), the truthful sandbox answer.  The
   wasi sysroot deliberately omits this header for the same reason.
   The rusage pointer is opaque (void*) — the wasi sys/resource.h has
   an #error gate, and the stub never fills a rusage anyway. */
#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* wait status macros (POSIX) — the stubs never set a real status, but
   the macros keep code that inspects wait() results compiling. */
#define WNOHANG    1
#define WUNTRACED  2
#define WCONTINUED 8
#define WEXITED    4
#define WNOWAIT    0x01000000
#define WSTOPPED   2

#define WIFEXITED(s)   (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WTERMSIG(s)    ((s) & 0x7f)
#define WIFSIGNALED(s) (((((s) & 0x7f) + 1) >> 1) > 0)
#define WIFSTOPPED(s)  (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s)    WEXITSTATUS(s)
#define WIFCONTINUED(s) ((s) == 0xffff)

pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);
int waitid(int idtype, pid_t id, void *infop, int options);
pid_t wait4(pid_t pid, int *status, int options, void *rusage);

#ifdef __cplusplus
}
#endif

#endif
