CC := gcc
CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror
SRC_DIR := src
TARGET := visual-window-app
CORE_SOURCES := $(SRC_DIR)/input/sha256.c $(SRC_DIR)/input/input_policy.c $(SRC_DIR)/input/input_logger.c $(SRC_DIR)/input/replay_runner.c
SOURCES := $(SRC_DIR)/main.c $(SRC_DIR)/window.c $(SRC_DIR)/renderer.c $(SRC_DIR)/policy_client.c $(CORE_SOURCES)
CORE_TEST := tests/test_input_core

SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null)
SDL_LIBS := $(shell sdl2-config --libs 2>/dev/null)
LDLIBS := $(SDL_LIBS) -lSDL2_image

.PHONY: all clean run core-test service-test test

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -I$(SRC_DIR) $(SDL_CFLAGS) $(SOURCES) -o $@ $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

$(CORE_TEST): tests/test_input_core.c $(CORE_SOURCES)
	$(CC) $(CFLAGS) -I$(SRC_DIR)/input tests/test_input_core.c $(CORE_SOURCES) -o $@

core-test: $(CORE_TEST)
	./$(CORE_TEST)

service-test:
	python3 -m unittest discover -s tests -v

test: core-test service-test

clean:
	rm -f $(TARGET) $(CORE_TEST)
