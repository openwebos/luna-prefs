# @@@LICENSE
#
#      Copyright (c) 2008 - 2012 Hewlett-Packard Development Company, L.P.
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

TOP := $(shell pwd)
include $(TOP)/config.mk
export # make submakes import these variables

LIBFILES := $(BUILD)/lib/libluna-prefs.so.0 
BINFILES := $(BUILD)/bin/luna-service $(BUILD)/bin/luna-prop
TESTS := $(BUILD)/bin/test/*
#EXECS := $(BUILD)/bin/memchute 
MACHINE ?= x86

# default: tests-noltp

all: init
	$(MAKE) -C libluna-prefs
	$(MAKE) -C luna-prefs-service
	$(MAKE) -C luna-prop

docs:
	echo "Processing doxygen..."
	(cd doc && doxygen ./Doxyfile &> /dev/null)

testall: all
	$(MAKE) -C tests testall

test: init all
	BUILD_LTP="yes" $(MAKE) -C tests

tests-noltp: init all
	$(MAKE) -C tests

clean:
	rm -fR $(BUILD) patches
	rm -fR doc/html
	$(MAKE) -C libluna-prefs clean
	$(MAKE) -C luna-prefs-service clean
	$(MAKE) -C luna-prop clean
	$(MAKE) -C tests clean

init:
	mkdir -p $(BUILD)/bin
	mkdir -p $(BUILD)/lib

install:
	mkdir -p $(LUNA_STAGING)/bin
	mkdir -p $(LUNA_STAGING)/lib
	mkdir -p $(LUNA_STAGING)/include
	install -m 0644 include/*.h    $(LUNA_STAGING)/include
	install -m 0755 $(BUILD)/lib/* $(LUNA_STAGING)/lib
	install -m 0755 $(BUILD)/bin/* $(LUNA_STAGING)/bin
