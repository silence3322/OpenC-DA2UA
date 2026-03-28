# OpenC-DA2UA Makefile
#
# Simple alternative to CMake.  Set WITH_SNAP7=1 and/or
# WITH_OPEN62541=1 to enable the real backends.

CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -Isrc -Ithird_party/cJSON
LDFLAGS = -lpthread

ifdef WITH_SNAP7
CFLAGS  += -DHAVE_SNAP7
LDFLAGS += -lsnap7
endif

ifdef WITH_OPEN62541
CFLAGS  += -DHAVE_OPEN62541
LDFLAGS += -lopen62541
endif

SRCS = src/main.c \
       src/config.c \
       src/logger.c \
       src/opcda_client.c \
       src/opcua_server.c \
       third_party/cJSON/cJSON.c

OBJS = $(SRCS:.c=.o)
TARGET = opcda2ua

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Tests
TEST_CONFIG_SRCS = tests/test_config.c src/config.c third_party/cJSON/cJSON.c
TEST_LOGGER_SRCS = tests/test_logger.c src/logger.c

test: test_config test_logger
	./test_config && echo "test_config PASSED"
	./test_logger && echo "test_logger PASSED"

test_config: $(TEST_CONFIG_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test_logger: $(TEST_LOGGER_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(OBJS) $(TARGET) test_config test_logger
