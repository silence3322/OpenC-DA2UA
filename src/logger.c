/*
 * logger.c – Simple file-based logger for OpenC-DA2UA
 */
#include "logger.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static FILE    *g_log_fp    = NULL;
static LogLevel g_min_level = LOG_DEBUG;

static const char *level_str(LogLevel l)
{
    switch (l) {
        case LOG_DEBUG: return "DEBUG";
        case LOG_INFO:  return "INFO ";
        case LOG_WARN:  return "WARN ";
        case LOG_ERROR: return "ERROR";
        default:        return "?????";
    }
}

int logger_init(const char *log_dir, LogLevel min_level)
{
    g_min_level = min_level;

    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    char path[512];
    snprintf(path, sizeof(path), "%s/%04d%02d%02d%02d%02d.log",
             log_dir,
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min);

    g_log_fp = fopen(path, "w");
    if (!g_log_fp) {
        fprintf(stderr, "logger: cannot open log file '%s'\n", path);
        return -1;
    }
    return 0;
}

void logger_log(LogLevel level, const char *fmt, ...)
{
    if (level < g_min_level) return;

    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    char time_buf[64];
    snprintf(time_buf, sizeof(time_buf), "%04d-%02d-%02d %02d:%02d:%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);

    va_list ap;

    /* Always write to stderr */
    va_start(ap, fmt);
    fprintf(stderr, "[%s] [%s] ", time_buf, level_str(level));
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);

    /* Write to file if open */
    if (g_log_fp) {
        va_start(ap, fmt);
        fprintf(g_log_fp, "[%s] [%s] ", time_buf, level_str(level));
        vfprintf(g_log_fp, fmt, ap);
        fprintf(g_log_fp, "\n");
        fflush(g_log_fp);
        va_end(ap);
    }
}

void logger_close(void)
{
    if (g_log_fp) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
}
