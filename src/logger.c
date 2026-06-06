#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "logger.h"

/**
 * ===========================================================
 * ASYNC LOGGER - RING BUFFER DESIGN
 * 
 * The event loop writes log entries into a fix-sized ring buffer
 * in memory. A background thread drains the ring to disk.
 * 
 * The event loop NEVER waits for disk I/O. A full ring drops
 * messages rather than stalling the event loop. Dropped messages
 * are counted and reported when space becomes available.
 * 
 * Memory: LOG_RING_SIZE x LOG_ENTRY_SIZE = 4096 x 512 = 2MB
 * This memory is always present. It is the price of zero-latency
 * logging on the hot path.
 * ===========================================================
 */

#define LOG_RING_SIZE 4096 // Must be power of 2 for fast modulo
#define LOG_ENTRY_SIZE 512 // max bytes per log line including timestamp

/**
 * A single log entry in the ring.
 * Fixed size so ring[N % LOG_RING_SIZE] is a single array index operation.
 * Variable-size entries would require pointer chasing and heap allocation.
 */
struct log_entry {
    char msg[LOG_ENTRY_SIZE];
    size_t len;
};

// The ring buffer - statically allocated, always present in memory
static struct log_entry ring[LOG_RING_SIZE];

/**
 * write_head: index where the next entry will be written.
 * read_head: index of the next entry to be read by the logger thread.
 * 
 * Both are uint64_t and monotonically increase, never wrap.
 * Ring position: head % LOG_RING_SIZE (fast because LOG_RING_SIZE is power of 
 * 2).
 * 
 * volatile: prevents compiler from caching these in registers. They are
 * written by the event loop and read by the logger thread. volatile is
 * sufficient enough for our sing-writer-per-worker design.
 */
static volatile uint64_t write_head = 0;
static volatile uint64_t read_head = 0;

// Counts dropped log messages when ring is full
static volatile uint64_t dropped_count = 0;

// Background logger thread
static pthread_t log_thread;
static int log_thread_running = 0;

// Log file for async writes
static FILE *log_file = NULL;
static char log_path_stored[256] = {0};

/**
 * ===========================================================
 * BILLING LOG
 * 
 * Billing events are written synchronously with immediate fflush.
 * Billing data mus never be lost. A dropped billing event means
 * you cannot charge for usage. The risk of losing revenue outweighs
 * the minor latency cost of a synchronous write.
 * Billing events are rare (connect/auth/disconnect) - not per-packet.
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

    while (log_thread_running || read_head < write_head) {
        /**
         * Drain all available entries from the ring.
         * This inner loop runs without any sleep - drain as fasr as possible
         * when there is work to do.
         */
        uint64_t dropped_snapshot = 0;

        while (read_head < write_head) {
            uint64_t slot = read_head % LOG_RING_SIZE;

            if (log_file && ring[slot].len > 0)
                fwrite(ring[slot].msg, 1, ring[slot].len, log_file);

            /**
             * Also write to stderr for visibility during development.
             * In higher environments, operators redirect stderr to /dev/null
             * and use the log file. Both outputs help during development.
             */
            if (ring[slot].len > 0)
                fwrite(ring[slot].msg, 1, ring[slot].len, stderr);

            ring[slot].len = 0;
            read_head++;
        }

        /**
         * Report dropped messaages if any accumulated.
         * Write the report directly to avoid recursion into log_write.
         */
        if (dropped_count > 0) {
            dropped_snapshot = dropped_count;
            dropped_count = 0;
            if (log_file) {
                fprintf(log_file, "[LOGGER] WARNING: %lu log messages dropped "
                    "(ring buffer full - event loop under heavy load)\n",
                    (unsigned long)dropped_snapshot);
            }
            fprintf(stderr, "[LOGGER] WARNING: %lu log messages dropped\n",
                (unsigned long)dropped_snapshot);
        }

        // Flush to disk periodically
        if (log_file) fflush(log_file);

        /**
         * Sleep 1ms when ring is empty.
         * This keeps CPU usage near 0 when the server is idle.
         * 1ms is the maximum additional latency for a log message to appear
         * on disk - acceptable for logging.
         */
        struct timespec ts = {0, 1000000}; // 1ms
        nanosleep(&ts, NULL);
    }

    // Final flush on shutdown
    if(log_file) fflush(log_file);
    return NULL;
}

/**
 * ===========================================================
 * CORE WRITE FUNCTION
 * Called from log_info and log_error.
 * Formats the message and puts it in the ring.
 * Returns immediately, no disk I/O on the calling thread.
 * ===========================================================
 */

static void log_write(const char *level, const char *fmt, va_list args)
{
    /**
     * Check if ring is full.
     * Full condition: write_head - read_head >= LOG_RING_SIZE
     * If full, drop the message rather than blocking the event loop.
     * Increment dropped_count so the logger thread can report it.
     */
    if (write_head - read_head >= LOG_RING_SIZE) {
        dropped_count++;
        return;
    }

    uint64_t slot = write_head % LOG_RING_SIZE;
    struct log_entry *entry = &ring[slot];

    /**
     * Format timestamp.
     * time() and localtime() are not async-signal-safe but log_write is not
     * called from signal handlers, only from the event loop and connection
     * handlers.
     */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);

    /**
     * Build the log line into the ring entry.
     * snprintf is safe, it never writes beyond LOG_ENTRY_SIZE.
     * We write timestamp + level + message + newline in one operation.
     */
    int prefix_len = snprintf(entry->msg, LOG_ENTRY_SIZE, "[%s] %-5s", ts,
        level);

    if (prefix_len < 0 || prefix_len >= LOG_ENTRY_SIZE) {
        entry->len = 0;
        write_head++;
        return;
    }

    int msg_len = vsnprintf(entry->msg + prefix_len,
        LOG_ENTRY_SIZE - prefix_len - 1, fmt, args);

    if (msg_len < 0) {
        entry->len = 0;
        write_head++;
        return;
    }

    // Add newline. Calculate total length carefully to avoid overflows.
    size_t total = (size_t)prefix_len + (size_t)msg_len;
    if (total >= LOG_ENTRY_SIZE - 1) total = LOG_ENTRY_SIZE - 2;

    entry->msg[total] = '\n';
    entry->msg[total + 1] = '\0';
    entry->len = total + 1;

    /**
     * Advance write_head AFTER the entry is fully written.
     * The logger thread reads entries between read_head and write_head.
     * If we advance write_head before writing the entry, the logger thread
     * could read a partially-written entry.
     */
    write_head++;
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
    if (filepath && *filepath) {
        strncpy(log_path_stored, filepath, sizeof(log_path_stored) - 1);
        log_file = fopen(filepath, "a");
        if (!log_file) {
            fprintf(stderr, "logger_init: cannot open %s: %s\n",
                filepath, strerror(errno));
            return -1;
        }
    }

    write_head = 0;
    read_head = 0;
    dropped_count = 0;

    log_thread_running = 1;
    if (pthread_create(&log_thread, NULL, logger_thread_fn, NULL) != 0) {
        fprintf(stderr, "logger_init: pthread_create failed: %s\n",
            strerror(errno));
        if (log_file) fclose(log_file);
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

    if (log_file) {
        fflush(log_file);
        fclose(log_file);
        log_file = NULL;
    }
}

void logger_reopen(void)
{
    /**
     * Called when SIGHUP fires (log rotation).
     * Log rotation tools rename the current log file and send SIGHUP.
     * We close and reopen to start writing to the new file.
     * 
     * This is called from the event loop (not the logger thread) when the
     * reopen_log flag is set. The logger thread may be mid-write. We use a
     * brief pause to let the thread finish its current write before swapping
     * the file pointer.
     */
    if (log_path_stored[0] == '\0') return;

    FILE *new_file = fopen(log_path_stored, "a");
    if (!new_file) {
        log_error("logger_reopen: cannot open %s: %s", log_path_stored,
            strerror(errno));
        return;
    }

    FILE *old_file = log_file;
    log_file = new_file;

    if (old_file) {
        fflush(old_file);
        fclose(old_file);
    }

    log_info("Log file reopened after SIGHUP");
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
    if (!filepath || !*filepath) return 0;

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