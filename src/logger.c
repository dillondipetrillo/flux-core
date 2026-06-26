#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "logger.h"

/**
 * ===========================================================
 * ASYNC LOGGER - RING BUFFER DESIGN
 * 
 * The event loop writes log entries into a fix-sized ring buffer.
 * A background pthread drains the ring to disk at its own pace.
 * The event loop never waits for disk I/O.
 * 
 * Memory cost: LOG_RING_SIZE x LOG_ENTRY_SIZE = 2MB per worker.
 * This is constant. It is the cost of zero-latency logging on the
 * hot event loop path.
 * 
 * Correctness guarantee: _Atomic indices with explicit memory ordering.
 * mempry_order_release on write_head ensures the ring entry is fully
 * written before the reader sees the advanced index. memory_order_acquire
 * on read ensures the reader sees the write. This is zero-cost on x86
 * and correct on all architectures.
 * 
 * Single-writer: each worker process has its own ring buffer.
 * Workers do not share memory - they share nothing after fork().
 * ===========================================================
 */

#define LOG_RING_SIZE 4096 // Must be power of 2 for fast modulo
#define LOG_ENTRY_SIZE 256 // max bytes per log line including timestamp

/**
 * Set to 1 by logger_init_parent - enables synchronous direct writes.
 * Set to 0 by default - logger_init (worker mode) uses async ring buffer.
 * This flag is set once before fork() and never changes in the parent.
 * Each worker process gets its own copy after fork() with value 0, then calls
 * logger_init() which confirms async mode.
 */
static int parent_mode = 0;
static pid_t cached_pid = 0;

struct log_entry {
    char msg[LOG_ENTRY_SIZE];
    size_t len;
};

// The ring buffer - statically allocated, always present in memory
static struct log_entry ring[LOG_RING_SIZE];

/**
 * Atmoic indicies. Monotonically increasing - never wrap.
 * Ring position: index % LOG_RING_SIZE (fast: power-of-2-mask).
 * 
 * write_head: written by event loop, read by logger thread.
 * read_head: written by logger thread, read by event loop (fullness check).
 */
static _Atomic uint64_t write_head = 0;
static _Atomic uint64_t read_head = 0;

// Dropped message counter - incremented when ring is full
static _Atomic uint64_t dropped_count = 0;

// Background logger thread
static pthread_t log_thread;
static int log_thread_running = 0;

// Log file state - protected by log_file_mutex for logger_reopen
static FILE *log_file = NULL;
static char log_path_stored[256] = {0};
static pthread_mutex_t log_file_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * ===========================================================
 * BILLING LOG
 * 
 * Synchronous writes with immediate fflush.
 * Billing events must never be lost - revenue depends on it.
 * Billing events are rare (connect/auth/disconnect), not per-packet,
 * so synchronous writes do not affect hot path performance.
 * ===========================================================
 */
static FILE *billing_file = NULL;

/**
 * ===========================================================
 * BACKGROUND LOGGER THREAD
 * ===========================================================
 */

static void *logger_thread_fn(void *arg)
{
    (void)arg;

    /**
     * Set a modest stack for this thread - it only does string formatting
     * and file writes, does not need the default 8MB stack.
     * Note: stack size is set via pthread_attr at thread creation.
     * This comment documents the intent; see logger_init for the
     * implementation.
     */
    while (log_thread_running ||
        atomic_load_explicit(&read_head, memory_order_relaxed) !=
        atomic_load_explicit(&write_head, memory_order_relaxed))
    {
        uint64_t wh = atomic_load_explicit(&write_head, memory_order_acquire);
        uint64_t rh = atomic_load_explicit(&read_head, memory_order_relaxed);

        while (rh < wh) {
            uint64_t slot = rh & (LOG_RING_SIZE - 1);

            if (ring[slot].len > 0) {
                pthread_mutex_lock(&log_file_mutex);
                if (log_file) {
                    fwrite(ring[slot].msg, 1, ring[slot].len, log_file);
                }
                pthread_mutex_unlock(&log_file_mutex);
            }

            ring[slot].len = 0;
            rh++;
            atomic_store_explicit(&read_head, rh, memory_order_release);
        }

        // Report and reset dropped count
        uint64_t dropped = atomic_load_explicit(&dropped_count,
            memory_order_relaxed);
        if (dropped > 0) {
            atomic_fetch_sub_explicit(&dropped_count, dropped,
                memory_order_relaxed);
            
            char warn[128];
            int n = snprintf(warn, sizeof(warn),
                "[LOGGER] WARNING: %lu log entries dropped "
                "(ring full under heavy load)\n",
                (unsigned long)dropped);
            
            pthread_mutex_lock(&log_file_mutex);
            if (log_file && n > 0)
                fwrite(warn, 1, (size_t)n, log_file);
            pthread_mutex_unlock(&log_file_mutex);
        }

        // Flush periodically
        pthread_mutex_lock(&log_file_mutex);
        if (log_file) fflush(log_file);
        pthread_mutex_unlock(&log_file_mutex);

        // Sleep 1ms when ring is drained - keeps CPU at zero when idle
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }

    pthread_mutex_lock(&log_file_mutex);
    if (log_file) fflush(log_file);
    pthread_mutex_unlock(&log_file_mutex);

    return NULL;
}

/**
 * ===========================================================
 * CORE WRITE FUNCTION
 * 
 * Called by the log_info and log_error
 * Formats the entry into the ring and advances write_head.
 * Returns immediately - no disk I/O on the calling thread.
 * 
 * Parent mode: writes directly and synchronously, no ring buffer.
 * 
 * Memory ordering:
 *      1. Write ring entry completely.
 *      2. Advance write_head with memory_order_release.
 *          This ensures step 1 is visible to the reader before
 *          the reader sees the advanced index.
 * ===========================================================
 */

static void log_write(const char *level, const char *fmt, va_list args)
{
    /**
     * Parent mode: direct, synchronous writes.
     * No ring buffer, no background thread.
     * The parent logs rarely, startup config and worker lifecycle events.
     * Direct writes are fine here and safer around fork().
     */
    if (parent_mode) {
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);

        char msg[LOG_ENTRY_SIZE];
        int prefix = snprintf(msg, LOG_ENTRY_SIZE, "[%s] %-5s pid=%d ",
            ts, level, (int)cached_pid);
        if (prefix > 0 && prefix < LOG_ENTRY_SIZE)
            vsnprintf(msg + prefix, LOG_ENTRY_SIZE - prefix - 1, fmt, args);
        // Ensure newline
        size_t len = strlen(msg);
        if (len < LOG_ENTRY_SIZE - 1) {
            msg[len] = '\n';
            msg[len + 1] = '\0';
            len++;
        }

        pthread_mutex_lock(&log_file_mutex);
        if (log_file)
            fwrite(msg, 1, len, log_file);
        else
            fwrite(msg, 1, len, stderr);
        pthread_mutex_unlock(&log_file_mutex);
        return;
    }

    // Worker mode: async ring buffer path
    uint64_t wh = atomic_load_explicit(&write_head, memory_order_relaxed);
    uint64_t rh = atomic_load_explicit(&read_head, memory_order_acquire);

    if (wh - rh >= LOG_RING_SIZE) {
        atomic_fetch_add_explicit(&dropped_count, 1, memory_order_relaxed);
        return;
    }

    uint64_t slot = wh & (LOG_RING_SIZE - 1);
    struct log_entry *e = &ring[slot];
    
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);

    int prefix = snprintf(e->msg, LOG_ENTRY_SIZE, "[%s] %-5s pid=%d ",
        ts, level, (int)cached_pid);
    if (prefix < 0 || prefix >= LOG_ENTRY_SIZE) {
        e->len = 0;
        atomic_store_explicit(&write_head, wh + 1, memory_order_release);
        return;
    }

    int body = vsnprintf(e->msg + prefix,
        LOG_ENTRY_SIZE - prefix - 1, fmt, args);
    if (body < 0) body = 0;

    size_t total = (size_t)prefix + (size_t)body;
    if (total >= LOG_ENTRY_SIZE - 1) total = LOG_ENTRY_SIZE - 2;

    e->msg[total] = '\n';
    e->msg[total + 1] = '\0';
    e->len = total + 1;

    /**
     * memory_order_release: all writes to ring[slot] above are visible to
     * any thread that reads write_head with memory_order_acquire.
     */
    atomic_store_explicit(&write_head, wh + 1, memory_order_release);
}

/**
 * ===========================================================
 * PUBLIC API
 * ===========================================================
 */

void log_info(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_write("INFO", fmt, args);
    va_end(args);
}

void log_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_write("ERROR", fmt, args);
    va_end(args);
}

int logger_init(const char *filepath)
{
    parent_mode = 0; // worker mode, use async ring buffer
    cached_pid = getpid();

    if (filepath && *filepath) {
        strncpy(log_path_stored, filepath, sizeof(log_path_stored) - 1);
        log_file = fopen(filepath, "a");
        if (!log_file) {
            fprintf(stderr, "logger_init: cannot open log %s: %s\n",
                filepath, strerror(errno));
            return -1;
        }
        setvbuf(log_file, NULL, _IONBF, 0);
    } else {
        /**
         * No log file path - log to stderr only.
         * Production deployments must set ENGINE_LOG_PATH.
         * Log a warning to stderr so operators notice.
         */
        fprintf(stderr, "[LOGGER] WARNING: ENGINE_LOG_PATH not set - "
            "logging to stderr only. Set ENGINE_LOG_PATH for production.\n");
    }
    
    atomic_store_explicit(&write_head, 0, memory_order_relaxed);
    atomic_store_explicit(&read_head, 0, memory_order_relaxed);
    atomic_store_explicit(&dropped_count, 0, memory_order_relaxed);

    /**
     * Configure thread attributes - reduce stack to 256KB.
     * The logger thread only formats strings and calls fwrite.
     * The default 8MB stack is wasteful for this workload.
     * 256KB is generous for a logging thread.
     */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024); // 256KB

    log_thread_running = 1;
    int rc = pthread_create(&log_thread, &attr, logger_thread_fn, NULL);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        fprintf(stderr, "logger_init: pthread_create failed: %s\n",
            strerror(errno));
        if (log_file) {
            fclose(log_file);
            log_file = NULL;
        }
        log_file = NULL;
        return -1;
    }

    return 0;
}

void logger_close(void)
{
    /**
     * Signal the logger thread to stop after draining remaining entries.
     * Set running=0 first, then wait for the thread.
     * The thread condition is: while (running || read_head < write_head)
     * So it drains all remaining entries before exiting.
     */
    log_thread_running = 0;
    pthread_join(log_thread, NULL);

    pthread_mutex_lock(&log_file_mutex);
    if (log_file) {
        fflush(log_file);
        fclose(log_file);
        log_file = NULL;
    }
    pthread_mutex_unlock(&log_file_mutex);
}

void logger_reopen(void)
{
    /**
     * Called when SIGHUP fires (log rotation).
     * Log rotation tools rename the current log file and send SIGHUP.
     * We open the new file, swap the pointer under the mutex, then close
     * the old file.
     * 
     * The mutex prevents the logger thread from writing to log_file while
     * we are swapping it.
     */
    if (log_path_stored[0] == '\0') return;

    FILE *new_file = fopen(log_path_stored, "a");
    if (!new_file) {
        log_error("logger_reopen: cannot open %s: %s", log_path_stored,
            strerror(errno));
        return;
    }
    setvbuf(new_file, NULL, _IONBF, 0);

    pthread_mutex_lock(&log_file_mutex);
    FILE *old_file = log_file;
    log_file = new_file;
    pthread_mutex_unlock(&log_file_mutex);

    if (old_file) {
        fflush(old_file);
        fclose(old_file);
    }

    /**
     * Write confirmation directly, bypasses the async ring buffer.
     * The ring buffer has up to 1ms drain latency. Writing directly here
     * guarantees the message appears in the new file immediately.
     * This is the only direct write in the worker, all other log calls use
     * the async ring buffer path.
     */
    pthread_mutex_lock(&log_file_mutex);
    if (log_file) {
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
        fprintf(log_file, "[%s] INFO [Worker pid=%d] Log file reopened "
            "after SIGHUP (log rotation)\n", ts, (int)getpid());
    }
    pthread_mutex_unlock(&log_file_mutex);
}

/**
 * logger_init_parent - lightweight synchronous logger for the parent process.
 * 
 * The parent process does not have a hot event loop and logs rarely.
 * It does not need the async ring buffer or a background thread.
 * This function opens the log file and enables direct synchronous writes.
 * log_info and log_error called after this write directly to the file.
 * 
 * Must be called before fork(). Workers call logger_init() after fork().
 * The parent calls logger_close_parent() before exiting.
 */
int logger_init_parent(const char *filepath)
{
    parent_mode = 1;
    cached_pid = getpid();

    if (filepath && *filepath) {
        strncpy(log_path_stored, filepath, sizeof(log_path_stored) - 1);
        log_file = fopen(filepath, "a");
        if (!log_file) {
            fprintf(stderr, "logger_init_parent: cannot open %s: %s\n",
                filepath, strerror(errno));
            return -1;
        }
        /**
         * _IONBF: disable stdio buffering.
         * Parent writes are rare and synchronous.
         * Multiple processes (parent + workers) write to this file.
         */
        setvbuf(log_file, NULL, _IONBF, 0);
    } else {
        fprintf(stderr, "[PARENT] WARNING: ENGINE_LOG_PATH not set - "
            "logging to stderr only.\n");
    }

    return 0;
}

void logger_close_parent(void)
{
    if (log_file) {
        fflush(log_file);
        fclose(log_file);
        log_file = NULL;
    }
}

/**
 * ===========================================================
 * BILLING LOG
 * Synchronous writes with immediate fflush.
 * Billing data must never be lost, revenue depends on it.
 * ===========================================================
 */

int billing_log_init(const char *filepath)
{
    if (!filepath || !*filepath) {
        /**
         * No billing log path configured. Log a warning - billling events
         * will be silently lost, which means usage cannot be tracked. This
         * is a configuration error in production.
         */
        fprintf(stderr, "[LOGGER] WARNING: ENGINE_BILLING_LOG_PATH not set - "
            "billing events will not be recorded. "
            "Set ENGINE_BILLING_LOG_PATH for production.\n");
        return 0;
    }

    billing_file = fopen(filepath, "a");
    if (!billing_file) {
        log_error("billing_log_init: cannot open %s: %s", filepath,
            strerror(errno));
        return -1;
    }
    return 0;
}

void billing_log_connect(int fd, struct sockaddr_in *addr)
{
    if (!billing_file) return;
    fprintf(billing_file, "{\"ts\":%lu,\"event\":\"connect\","
        "\"fd\":%d,\"ip\":\"%s\"}\n", (unsigned long)time(NULL), fd,
        addr ? inet_ntoa(addr->sin_addr) : "unknown");
    fflush(billing_file);
}

void billing_log_auth(int fd, uint32_t user_id)
{
    if (!billing_file) return;
    fprintf(billing_file, "{\"ts\":%lu,\"event\":\"auth\","
        "\"fd\":%d,\"user_id\":%u}\n", (unsigned long)time(NULL), fd,
        user_id);
    fflush(billing_file);
}

void billing_log_disconnect(int fd, uint32_t user_id, uint64_t bytes_sent,
    uint64_t bytes_recv)
{
    if (!billing_file) return;
    fprintf(billing_file, "{\"ts\":%lu,\"event\":\"disconnect\","
        "\"fd\":%d,\"user_id\":%u,\"bytes_sent\":%lu,"
        "\"bytes_recv\":%lu}\n", (unsigned long)time(NULL), fd, user_id,
        (unsigned long)bytes_sent, (unsigned long)bytes_recv);
    fflush(billing_file);
}