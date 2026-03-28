/*
 * test_logger.c – Unit tests for the logger module
 */
#include "logger.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void test_logger_init_and_close(void)
{
    /* Use /tmp as the log directory so we don't pollute the source tree */
    int ret = logger_init("/tmp", LOG_DEBUG);
    assert(ret == 0);
    logger_close();
    printf("PASS  test_logger_init_and_close\n");
}

static void test_logger_write_messages(void)
{
    int ret = logger_init("/tmp", LOG_DEBUG);
    assert(ret == 0);

    /* These should not crash */
    LOG_DEBUG_MSG("debug message %d", 1);
    LOG_INFO_MSG("info message %s", "hello");
    LOG_WARN_MSG("warn message");
    LOG_ERROR_MSG("error message %f", 3.14);

    logger_close();
    printf("PASS  test_logger_write_messages\n");
}

static void test_logger_invalid_dir(void)
{
    /* Initialising with a non-existent directory should fail gracefully */
    int ret = logger_init("/nonexistent_dir_xyz/logs", LOG_DEBUG);
    assert(ret != 0);
    /* Logging after failed init should not crash */
    LOG_WARN_MSG("this should not crash");
    logger_close();
    printf("PASS  test_logger_invalid_dir\n");
}

int main(void)
{
    test_logger_init_and_close();
    test_logger_write_messages();
    test_logger_invalid_dir();

    printf("\nAll logger tests passed.\n");
    return EXIT_SUCCESS;
}
