#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include <time.h>

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} LogLevel;

/* Initialise the logger; creates a timestamped log file under log_dir.
 * Returns 0 on success, -1 on failure. */
int  logger_init(const char *log_dir, LogLevel min_level);

/* Write a log message */
void logger_log(LogLevel level, const char *fmt, ...);

/* Close and flush the log file */
void logger_close(void);

/* Convenience macros */
#define LOG_DEBUG_MSG(fmt, ...) logger_log(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_INFO_MSG(fmt, ...)  logger_log(LOG_INFO,  fmt, ##__VA_ARGS__)
#define LOG_WARN_MSG(fmt, ...)  logger_log(LOG_WARN,  fmt, ##__VA_ARGS__)
#define LOG_ERROR_MSG(fmt, ...) logger_log(LOG_ERROR, fmt, ##__VA_ARGS__)

#endif /* LOGGER_H */
