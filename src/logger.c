#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "logger.h"

#define ERROR "ERROR"
#define INFO "INFO"

static FILE *log_file = NULL;
static FILE *billing_file = NULL;

int logger_init(const char *filepath)
{
    log_file = fopen(filepath, "a");
    if (log_file == NULL) {
        perror("logger_init: fopen");
        return -1;
    }
    log_info("=== Server started ===\n");
    return 0;
}

void logger_close(void)
{
    if (log_file != NULL) {
        log_info("=== Server stopped ===\n");
        fclose(log_file);
        log_file = NULL;
    }
}

static void log_write(const char *level, const char *fmt, va_list args)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", t);

    FILE *out = strcmp(level, ERROR) == 0 ? stderr : stdout;
    va_list args_copy;
    va_copy(args_copy, args);

    fprintf(out, "[%s] %-5s ", timestamp, level);
    vfprintf(out, fmt, args);
    fprintf(out, "\n");

    if (log_file != NULL) {
        fprintf(log_file, "[%s] %-5s ", timestamp, level);
        vfprintf(log_file, fmt, args_copy);
        fprintf(log_file, "\n");
        fflush(log_file);
    }

    va_end(args_copy);
}

void log_info(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_write(INFO, fmt, args);
    va_end(args);
}

void log_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_write(ERROR, fmt, args);
    va_end(args);
}

void logger_reopen(void)
{
    // Placeholder for actual file close/reopen for log rotation
    log_info("logger_reopen called (placeholder implementation)");
}

int billing_log_init(const char *filepath)
{
    billing_file = fopen(filepath, "a");
    if (!billing_file) {
        log_error("billing_log_init: could not open %s", filepath);
        return -1;
    }
    return 0;
}

void billing_log_connect(int fd, struct sockaddr_in *addr)
{
    if (!billing_file) return;
    fprintf(billing_file,
        "{\"ts\":%lu,\"event\":\"connect\",\"fd\":%d,\"ip\":\"%s\"}\n",
        (unsigned long)time(NULL), fd,
        addr ? inet_ntoa(addr->sin_addr) : "unknown");
    fflush(billing_file);
}

void billing_log_auth(int fd, uint32_t user_id)
{
    if (!billing_file) return;
    fprintf(billing_file,
        "{\"ts\":%lu,\"event\":\"auth\",\"fd\":%d,\"user_id\":%u}\n",
        (unsigned long)time(NULL), fd, user_id);
    fflush(billing_file);
}

void billing_log_disconnect(int fd, uint32_t user_id, uint64_t bytes_sent,
    uint64_t bytes_recv)
{
    if (!billing_file) return;
    fprintf(billing_file,
        "{\"ts\":%lu,\"event\":\"disconnect\",\"fd\":%d,"
        "\"user_id\":%u,\"bytes_sent\":%lu,\"bytes_recv\":%lu}\n",
        (unsigned long)time(NULL), fd, user_id,
        (unsigned long)bytes_sent, (unsigned long)bytes_recv);
    fflush(billing_file);
}