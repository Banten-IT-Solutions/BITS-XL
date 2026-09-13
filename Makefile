CC ?= cc
JSONC_CFLAGS := $(shell pkg-config --cflags json-c 2>/dev/null)
JSONC_LIBS := $(shell pkg-config --libs json-c 2>/dev/null)
MBEDCRYPTO_CFLAGS := $(shell pkg-config --cflags mbedcrypto 2>/dev/null)
MBEDCRYPTO_LIBS := $(shell pkg-config --libs mbedcrypto 2>/dev/null)
CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
CURL_LIBS := $(shell pkg-config --libs libcurl 2>/dev/null)
ifeq ($(strip $(MBEDCRYPTO_LIBS)),)
MBEDCRYPTO_LIBS := -lmbedcrypto
endif
ifeq ($(strip $(CURL_LIBS)),)
CURL_LIBS := -lcurl
endif
CFLAGS ?= -Os -std=c99 -Wall -Wextra -Wno-unused-parameter -ffunction-sections -fdata-sections
override CFLAGS += $(JSONC_CFLAGS) $(MBEDCRYPTO_CFLAGS) $(CURL_CFLAGS)
LDFLAGS ?= -Wl,--gc-sections
LDLIBS ?=
BITSXL_LIBS := $(JSONC_LIBS) $(MBEDCRYPTO_LIBS) $(CURL_LIBS)
ifneq ($(strip $(JSONC_LIBS)),)
override CFLAGS += -DBITSXL_HAVE_JSONC
endif
BIN := bitsxl
SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) $(LDFLAGS) $(LDLIBS) $(BITSXL_LIBS) -o $@
	strip $@ 2>/dev/null || true

clean:
	rm -f $(OBJ) $(BIN)

install: $(BIN)
	install -d $(DESTDIR)/usr/bin
	install -m 0755 $(BIN) $(DESTDIR)/usr/bin/bitsxl

.PHONY: all clean install
