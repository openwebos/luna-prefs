
REL := -O2
DEB := -g

DEBREL := $(DEB)

LUNA_STAGING?=$(HOME)/luna-desktop-binaries/staging
PKG_CONFIG_PATH+=$(LUNA_STAGING)/lib/pkgconfig

CFLAGS := $(CFLAGS) $(DEBREL) -Wall -Wno-deprecated-declarations -Werror \
		-I$(LUNA_STAGING)/include \
		$(shell pkg-config --libs sqlite3) \
		$(shell pkg-config --cflags glib-2.0) \

PN := luna-prefs

BUILD := $(TOP)/bin
LDFLAGS := -Wl,-rpath-link,$(BUILD)/lib -Wl,-rpath,$(BUILD)/lib $(LDFLAGS) \
		$(shell pkg-config --libs sqlite3) \
		$(shell pkg-config --cflags glib-2.0) \
		-llunaservice \
		-L$(LUNA_STAGING)/lib \
		$(LDFLAGS) \
#		-lmjson \

