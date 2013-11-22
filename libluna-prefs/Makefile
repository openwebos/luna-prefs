# @@@LICENSE
#
#      Copyright (c) 2008-2013 LG Electronics, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# LICENSE@@@

TOP ?= ..
include $(TOP)/config.mk

LIBS=sqlite3 glib-2.0
# PmIpcLib  mjson

OBJDIR=objs

# Make sure PN is defined for desktop-binaries (non-OE) build
PN ?= luna-prefs

REAL_LIB=libluna-prefs.so
OUT_LIB=lib$(PN).so.0
# What we claim to be internally: should match current pmpiclib's soname
SONAME=libluna-prefs.so

cflags := $(CFLAGS) -fPIC -I../include -I../include/private/

ldflags := -L$(BUILD)/lib -L$(STAGING_LIBDIR) $(LDFLAGS)

libs := $(addprefix -l, $(LIBS))

srcs := $(wildcard *.c)

objs := $(srcs)
objs := $(addprefix $(OBJDIR)/, $(objs))
objs := $(objs:.c=.o)

pcs  := $(srcs:.c=.pc)

#example-service-glue.h: example-service.xml
#    echo $(LIBTOOL) --mode=execute /usr/bin/dbus-binding-tool --prefix=main_object --mode=glib-server --output=example-service-glue.h example-service.xml
#    $(LIBTOOL) --mode=execute /usr/bin/dbus-binding-tool --prefix=main_object --mode=glib-server --output=example-service-glue.h example-service.xml

all: init $(OBJDIR)/$(OUT_LIB)

# Main binary
$(OBJDIR)/$(OUT_LIB): $(objs)
# -v -Wl,--verbose 
	$(CC) $(ldflags) -shared -o $@ $^ $(libs) $(GCC_COMPILEFlAGS) -Wl,-soname,$(SONAME) #$(OUT_LIB)
	cp $@ $(BUILD)/lib
	cd $(BUILD)/lib && ln -sf $(OUT_LIB) lib$(PN).so
#	cd $(BUILD)/lib && ln -sf $(OUT_LIB) $(REAL_LIB)

# Generic rules
.c.pc:
	$(CC) $(cflags) -o $*.pc -E $*.c

$(OBJDIR)/%.o: %.c
	$(CC) $(cflags) -o $@ -c $<

# Post-processed results
pc: $(pcs)


init:
	mkdir -p $(OBJDIR)
	mkdir -p $(BUILD)/lib

.PHONY: clean
clean:
	rm -fR $(EXE) $(objs) $(OBJDIR)
