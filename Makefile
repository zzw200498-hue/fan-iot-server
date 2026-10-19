CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -pthread -Iinclude
LDLIBS  = -lsqlite3 -pthread

SRC     = $(wildcard src/*.c)
OBJ     = $(SRC:.c=.o)
TARGET  = fan-iot-server

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all run clean
