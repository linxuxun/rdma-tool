CC       = gcc
CFLAGS   = -Wall -Wextra -std=c11 -O2 -g
LDFLAGS  = -lrdmacm -libverbs -lpthread
SRC_DIR  = src
BUILD_DIR = build
TARGET   = $(BUILD_DIR)/rdma_tool

SOURCES  = $(wildcard $(SRC_DIR)/*.c)
HDRS     = $(wildcard $(SRC_DIR)/*.h)

all: $(TARGET)

$(TARGET): $(SOURCES) $(HDRS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -o $@ $(SOURCES) $(LDFLAGS)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
