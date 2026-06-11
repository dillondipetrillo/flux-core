#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"
#include "engine.h"
#include "logger.h"

/**
 * Worker PID tracking.
 * The parent maintains this array to know which processes are its workers.
 * Required for graceful shutdown: when parent receives SIGTERM, it
 * forwards SIGTERM to all workers so they can broadcast TYPE_SYS_SHUTDOWN
 * to clients and exit cleanly before the parent exits.
 * 
 * MAX_WORKERS: hard upper bound on worker count.
 * In practice cfg->worker_count controls how many are started.
 */
#define MAX_WORKERS 256
static pid_t worker_pids[MAX_WORKERS];
static int worker_count_actual = 0;
static struct engine_config *g_config = NULL;

/**
 * parent_shutdown_flag: set by SIGTERM/SIGINT handler in the parent.
 * When set, the parent stops restarting dead workers and exits after all
 * remaining workers have exited.
 */
static volatile sig_atomic_t parent_shutdown = 0;

/**
 * parent_handle_signal - parent's SIGTERM/SIGINT handler.
 * 
 * Sets the shutdown flag and forwards SIGTERM to all known workers.
 * Workers receive SIGTERM, their own signal handler sets engine_running=0,
 * their event loop exits, perform_graceful_shudown runs, they exit(0).
 * The parent's waitpid loop sees them exit cleanly and does not restart them.
 * 
 * We send SIGTERM not SIGKILL because workers need to run graceful shutdown.
 * SIGKILL would skip perform_graceful_shutdown entirely.
 */
static void parent_handle_signal(int sig)
{
    if (sig == SIGTERM || sig == SIGINT) {
        parent_shutdown = 1;
        // Forward to all known workers
        for (int i = 0; i < worker_count_actual; i++) {
            if(worker_pids[i] > 0)
                kill(worker_pids[i], SIGTERM);
        }
    } else if (sig == SIGHUP) {
        /**
         * Forward SIGHUP to all workers.
         * Workers catch it in their own handle_sighup handler (in engine.c)
         * which sets reopen_log = 1. Their event loop then calls
         * logger_reopen() on the next iteration.
         */
        for (int i = 0; i < worker_count_actual; i++) {
            if (worker_pids[i] > 0)
                kill(worker_pids[i], SIGHUP);
        }
    }
}

/**
 * add_worker_pid - record a newly forked worker's PID.
 */
static void add_worker_pid(pid_t pid)
{
    for (int i = 0; i < MAX_WORKERS; i++) {
        if (worker_pids[i] == 0) {
            worker_pids[i] = pid;
            if (i >= worker_count_actual)
                worker_count_actual = i + 1;
            return;
        }
    }
    log_error("MAX_WORKERS (%d) exceeded - could not track worker pid=%d",
        MAX_WORKERS, (int)pid);
}

/**
 * remove_worker_pid - remove a dead worker's PID from tracking.
 */
static void remove_worker_pid(pid_t pid)
{
    for (int i = 0; i < MAX_WORKERS; i++) {
        if (worker_pids[i] == pid) {
            worker_pids[i] = 0;
            return;
        }
    }
}

/**
 * fork_worker - fork one worker process.
 * The child calls worker_run and never returns.
 * The parent records the new PID and returns.
 */
static void fork_worker(struct engine_config *config)
{
    pid_t pid = fork();

    if (pid == -1) {
        log_error("fork failed: %s", strerror(errno));
        return;
    }

    if (pid == 0) {
        /**
         * Child process.
         * 
         * Reset signal handlers: the child should NOT use the parent's
         * SIGTERM handler (parent_handle_signal). The child needs its own
         * handler (handle_shutdown in engine.c) which sets engine_running=0.
         * engine_init registers the correct handlers - we just need to clear
         * the parent's handler first.
         * 
         * SIGPIPE is still SIG_IGN from the parent - that is correct,
         * children inherit signal dispositions.
         */
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);

        if (logger_init(config->log_path) == -1) {
            fprintf(stderr, "FATAL: Worker pid=%d could not open "
                "log file: %s\n", (int)getpid(), config->log_path);
            exit(1);
        }

        if (engine_init(config) == -1) {
            log_error("Worker pid=%d: engine_init failed, exiting",
                (int)getpid());
            exit(1);
        }

        auth_hook_fn hook = select_auth_hook(config);
        engine_set_auth_hook(hook);

        engine_run(); // blocks until signal

        log_info("Worker pid=%d exiting cleanly", (int)getpid());
        exit(0);
    }

    // Parent: record the new worker
    log_info("Forked worker pid=%d", (int)pid);
    add_worker_pid(pid);
}

int main(void)
{
    /**
     * Ignore SIGPIPE globally - inheritted by all child processes.
     * Must be first, before any socket operations.
     */
    signal(SIGPIPE, SIG_IGN);

    struct engine_config config = config_load();
    g_config = &config;

    config_log(&config);

    /**
     * Pre-fork initialization - must happen before any fork().
     * Any library that uses global state and is not fork-safe must be
     * initialized here.
     */
    if (engine_prefork_init(&config) == -1) {
        log_error("FATAL: engine_prefork_init failed");
        logger_close();
        return 1;
    }

    /**
     * Determine worker count.
     * 
     * ENGINE_WORKER_COUNT=0: means auto-detect CPU count.
     * ENGINE_WORKER_COUNT=1: single worker (USE THIS DURING DEVELOPMENT).
     *      Single worker gives clean, sequential log output and makes
     *      debugging straightforward. 16 workers produces interleaved log
     *      output that is nearly impossible to read.
     * ENGINE_WORKER_COUNT=N: exactly N workers for production tuning.
     * 
     * Production recommendation: set to the number of CPU cores.
     * Development recommendation: ENGINE_WORKER_COUNT=1
     */
    int workers = config.worker_count;
    if (workers <= 0) {
        /**
         * ENGINE_WORKER_COUNT=0 is an explicit opt-in to auto-detect.
         * Default is 1. Operators who want auto-detect set
         * ENGINE_WORKER_COUNT=0 in production after verifying their machine
         * has sufficient memory for (cpu_count * max_clients) client_info
         * structs.
         * 
         * Memory estimate per worker:
         *      max_clients * (recv_buf + send_buf + overhead)
         *      = max_clients * 75KB
         *      = 10000 * 75KB = ~720MB per worker
         * 
         * Verify your available RAM before enabling auto-detect.
         */
        workers = (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (workers <= 0) workers = 1; // sysconf can return -1 on error
        log_info("ENGINE_WORKER_COUNT=0: auto-detect %d CPU core(s). "
            "Verify RAM >= %d workers x %d clients x ~75KB = ~%dMB",
            workers, workers, config.max_clients,
            (int)((long long)workers * config.max_clients * 75 / 1024));
    }

    /**
     * Hard cap: never exceed MAX_WORKERS.
     * Protects against misconfiguration.
     */
    if (workers > MAX_WORKERS) {
        log_error("Requested %d workers exceeds MAX_WORKERS=%d, capping",
            workers, MAX_WORKERS);
        workers = MAX_WORKERS;
    }

    log_info("Starting %d worker process(es) "
        "(tip: set ENGINE_WORKER_COUNT=1 during development)", workers);

    /**
     * Register parent's signal handler BEFORE forking.
     * When SIGTERM arrives at the parent, it forwards to all workers.
     * Children will reset these to SIG_DFL before calling engine_init,
     * which registers the correct per-worker handlers.
     */
    signal(SIGTERM, parent_handle_signal);
    signal(SIGINT, parent_handle_signal);
    signal(SIGHUP, parent_handle_signal);

    // Initialize PID tracking array
    memset(worker_pids, 0, sizeof(worker_pids));

    // Fork all initial workers
    for (int i = 0; i < workers; i++) {
        fork_worker(&config);
    }

    /**
     * Parent monitor loop.
     * 
     * The parent does nothing except:
     *      1. Wait for a worker to exit
     *      2. Log what happened
     *      3. If not shutting down and exit was abnormal: fork a replacement
     *      4. If shutting down: do not restart, just wait for all to exit
     * 
     * The loop exits when:
     *      - All children have exited (ECHILD from waitpid)
     *      - parent_shutdown is set AND no children remain
     */
    while (1) {
        int status;
        pid_t dead = waitpid(-1, &status, 0);

        if (dead == -1) {
            if (errno == EINTR) {
                /**
                 * Signal interrupted waitpid.
                 * Check if we are shutting down and have no more children.
                 */
                if (parent_shutdown) {
                    // Check if any workers are still alive
                    int alive = 0;
                    for (int i = 0; i < MAX_WORKERS; i++) {
                        if (worker_pids[i] > 0) {
                            alive = 1;
                            break;
                        }
                    }
                    if (!alive) {
                        log_info("All workers exited, parent shutting down");
                        break;
                    }
                }
                continue;
            }
            if (errno == ECHILD) {
                log_info("No more worker processes, parent exiting");
                break;
            }
            log_error("waitpid: %s", strerror(errno));
            break;
        }

        remove_worker_pid(dead);

        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            if (code == 0) {
                /**
                 * Clean exit (exit(0)) means graceful SIGTERM shutdown.
                 * Do not restart - the server is intentionally stopping.
                 */
                log_info("Worker pid=%d exited cleanly (code=0)", (int)dead);
            } else {
                /**
                 * Non-zero exit means initialization failure or crash.
                 * Restart unless parent is shutting down.
                 */
                log_error("Worker pid=%d exited with code=%d",
                    (int)dead, code);
                if (!parent_shutdown) {
                    log_info("Restarting worker...");
                    fork_worker(&config);
                }
            }
        } else if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            if (sig == SIGTERM || sig == SIGINT) {
                /**
                 * Worker was signaled by SIGTERM or SIGINT.
                 * This is expected during graceful shutdown - do not restart.
                 * (Workers should exit(0) after SIGTERM via their own handler,
                 * but if they were killed before their handler ran, this
                 * catches it.)
                 */
                log_info("Worker pid=%d stopped by signal %d", (int)dead, sig);
            } else {
                /**
                 * Unexpected signal - SIGSEGV, SIGBUS, SIGABRT, etc.
                 * This is a crash. Restart unless shutting down.
                 */
                log_error("Worker pid=%d killed by signal %d (%s)",
                    (int)dead, sig, strsignal(sig));
                if (!parent_shutdown) {
                    log_info("Restarting crashed worker...");
                    fork_worker(&config);
                }
            }
        }
    }

    logger_close();
    return 0;
}