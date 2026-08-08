CC = gcc
CFLAGS = -Wall -Wextra -O2 -Iinclude -D_GNU_SOURCE -pthread
LDFLAGS = -lssl -lcrypto -lcurl -lpthread -lsqlite3 -lmosquitto

SRC = src/board_main.c \
      src/board_state.c \
      src/sysinfo.c \
      src/sensor_thread.c \
      src/stream_proxy.c \
      src/https_server.c \
      src/api_handlers.c \
      src/json_min.c \
      src/emailer.c \
      src/mqtt_client.c \
      src/blackbox.c \
      src/watchdog.c

OBJ = $(SRC:.c=.o)
TARGET = security-board

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)
