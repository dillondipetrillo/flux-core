#ifndef LOGGER_H
#define LOGGER_H

#include <netinet/in.h>
#include <stdint.h>

// Core logging
int logger_init(const char *filepath);
void logger_close(void);
void logger_reopen(void);
void log_info(const char *fmt, ...);
void log_error(const char *fmt, ...);

// Parent process logging
int logger_init_parent(const char *filepath);
void logger_close_parent(void);

// Billing log - structured JSON events for usage tracking
int billing_log_init(const char *filepath);
void billing_log_connect(int fd, struct sockaddr_in *addr);
void billing_log_auth(int fd, uint32_t user_id);
void billing_log_disconnect(int fd, uint32_t user_id, uint64_t bytes_sent,
    uint64_t bytes_recv);

#endif
