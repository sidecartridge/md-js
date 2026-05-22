LIB_DIR = ../../target/atarist/src
BUILD_DIR = ./build
DIST_DIR = ./dist
BIN = MDJSCODE.PRG

CC = m68k-atari-mint-gcc
CFLAGS = -m68000 -O2 -fomit-frame-pointer -I. -I$(LIB_DIR)

.PHONY: all clean

all: $(DIST_DIR)/$(BIN) $(DIST_DIR)/math.js

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(DIST_DIR):
	mkdir -p $(DIST_DIR)

$(BUILD_DIR)/sidecart_stubs.o: $(LIB_DIR)/sidecart_stubs.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/mdjs.o: $(LIB_DIR)/mdjs.c $(LIB_DIR)/mdjs.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/textedit.o: textedit.c textedit.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/main.o: main.c $(LIB_DIR)/mdjs.h textedit.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(DIST_DIR)/$(BIN): $(BUILD_DIR)/sidecart_stubs.o $(BUILD_DIR)/mdjs.o \
                    $(BUILD_DIR)/textedit.o $(BUILD_DIR)/main.o | $(DIST_DIR)
	$(CC) $(CFLAGS) $^ -lgem -o $@

$(DIST_DIR)/math.js: math.js | $(DIST_DIR)
	cp $< $@

clean:
	rm -rf $(BUILD_DIR) $(DIST_DIR)
