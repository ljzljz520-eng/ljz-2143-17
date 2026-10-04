CC := gcc
CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror
SRC_DIR := src
CORE_DIR := $(SRC_DIR)/core
PLATFORM_DIR := $(SRC_DIR)/platform
UI_DIR := $(SRC_DIR)/ui
TARGET := visual-window-app
TEST_TARGET := test_core

CORE_SRCS := $(wildcard $(CORE_DIR)/*.c)
APP_SRCS := $(SRC_DIR)/main.c $(SRC_DIR)/window.c $(SRC_DIR)/renderer.c \
            $(PLATFORM_DIR)/net.c $(PLATFORM_DIR)/sync.c \
            $(PLATFORM_DIR)/sdl_input.c $(UI_DIR)/font.c $(UI_DIR)/ui.c
TEST_SRCS := tests/test_core.c

SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null)
SDL_LIBS := $(shell sdl2-config --libs 2>/dev/null)
LDLIBS := $(SDL_LIBS) -lSDL2_image

.PHONY: all clean run test test-service

all: $(TARGET) replay_tool

replay_tool: src/replay_main.c $(CORE_SRCS)
	$(CC) $(CFLAGS) -I$(SRC_DIR) src/replay_main.c $(CORE_SRCS) -o $@

$(TARGET): $(APP_SRCS) $(CORE_SRCS)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -I$(SRC_DIR) \
	  $(APP_SRCS) $(CORE_SRCS) -o $@ $(LDLIBS)

$(TEST_TARGET): $(TEST_SRCS) $(CORE_SRCS)
	$(CC) $(CFLAGS) -I$(CORE_DIR) $(TEST_SRCS) $(CORE_SRCS) -o tests/$(TEST_TARGET)

test: $(TEST_TARGET)
	./tests/$(TEST_TARGET)
	python3 -m unittest discover -s tests -p 'test_service.py' -v

clean:
	rm -f $(TARGET) tests/$(TEST_TARGET)

run: $(TARGET)
	mkdir -p data
	./$(TARGET)
