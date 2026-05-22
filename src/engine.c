#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/resource.h>
#include <time.h>

#include "auth_hook.h"
#include "config.h"
#include "conn_map.h"
#include "logger.h"
#include "protocol.h"
#include "scope_map.h"

/**
 * ===========================================================
 * SIGNAL FLAGS
 * ===========================================================
 */

static volatile sig_atomic_t engine_running = 1;
static volatile sig_atomic_t reopen_log = 0;

/**
 * ===========================================================
 * ENGINE STATE
 * Private to this file.
 * ===========================================================
 */
static int epfd = -1;
static int server_fd = -1;
static int health_fd = -1;
static struct engine_config *cfg = NULL;
static auth_hook_fn current_hook = NULL;
static time_t start_time = 0;

static struct scope_map scope_map;
static struct conn_map conn_map;

// Memory pool - pre-allocated client_info structs.
#define POOL_MAX 10000
static struct client_info pool[POOL_MAX];
static struct client_info *free_list[POOL_MAX];
static int free_count = 0;

/**
 * ===========================================================
 * FORWARD DECLARATIONS
 * All internal functions are static. Declaring them here
 * to be able to call each other in any order below.
 * ===========================================================
 */

 /**
  * ===========================================================
  * SIGNAL HANDLERS
  * Must be minimal - set a flag and return
  * ===========================================================
  */

static void handle_shutdown(int sig)
{
    (void)sig;
    engine_running = 0;
}

static void handle_sighup(int sig)
{
    (void)sig;
    reopen_log = 1;
}

/**
 * ===========================================================
 * PUBLIC API
 * ===========================================================
 */

void engine_set_auth_hook(auth_hook_fn hook)
{
    if (!hook) {
        log_error("engine_set_auth_hook: NULL hook ignored");
        return;
    }
    current_hook = hook;
    log_info("Auth hook registered");
}

void engine_stop(void)
{
    engine_running = 0;
}

int engine_init (struct engine_config *config)
{
    cfg = config;
    start_time = time(NULL);

    // Default to dev hook until engine_set_auth_hook() is called
    current_hook = default_auth_hook;

    /**
     * SIGPIPE: ignore it.
     * When send() is called on a closed socket, the OS sends SIGPIPE.
     * Default behavior is to kill the process immediately.
     * SIG_IGN makes send() return -1 with errno=EPIPE instead, which our
     * error handling already handles correctly.
     */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, handle_shutdown);
    signal(SIGINT, handle_shutdown);
    signal(SIGHUP, handle_sighup);
    log_info("Signal handlers registered");

    
    // Raise soft file descriptor to the hard limit maximum
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = rl.rlim_max;
        if (setrlimit(RLIMIT_NOFILE, &rl) == -1) {
            int e = errno;
            log_error("setrlimit failed: %s - fd limit may be low",
                strerror(e));
        }
        // Read back actual result, OS may cap it
        getrlimit(RLIMIT_NOFILE, &rl);
    }
    log_info("File descriptor limit: %lu", (unsigned long)rl.rlim_cur);

    return 0;
}

void engine_run(void)
{
    // TODO: implement event loop
    log_info("engine_run: event loop not yet implemented, "
        "this is a placeholder");
}