#ifndef LOGGER_H
#define LOGGER_H

#include <netinet/in.h>
#include <stdint.h>

int logger_init(const char *filepath);
void logger_close(void);

void log_info(const char *fmt, ...);
void log_error(const char *fmt, ...);

// Log rotation - called when SIGHUP fires
void logger_reopen(void);

// Billing log - structured JSON events for usage tracking
int billing_log_init(const char *filepath);
void billing_log_connect(int fd, struct sockaddr_in *addr);
void billing_log_auth(int fd, uint32_t user_id);
void billing_log_disconnect(int fd, uint32_t user_id, uint64_t bytes_sent,
    uint64_t bytes_recv);

#endif
