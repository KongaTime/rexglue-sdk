# PS5_ReXGlue's PS5 tests as payloads for the console (ps5/tests/README.md).
#
#   make PS5_PAYLOAD_SDK=<sdk>                      build memory_ps5_test.elf
#   make test PS5_PAYLOAD_SDK=<sdk> PS5_HOST=<console address>
#                                                   send it to the console's ELF loader
#
# <sdk> is the payload SDK fork as EdgeOfTimeRecomp's ps5/tools/build-rexglue.sh
# installs it (.deps/native/ps5-payload-sdk): the tests link its platform layer.
#
# SPDX-License-Identifier: BSD-3-Clause

PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ROOT := ../..
ELF := memory_ps5_test.elf

CXXFLAGS := -std=c++23 -O1 -g -Wall -Wextra -march=znver2 -fno-omit-frame-pointer \
	-fexperimental-library -DFMT_HEADER_ONLY -Istub -I$(ROOT)/include \
	-I$(ROOT)/thirdparty/fmt/include -I$(ROOT)/thirdparty/simde

all: $(ELF)

$(ELF): memory_ps5_test.cpp $(ROOT)/src/core/memory_ps5.cpp stub/rex/logging.h
	$(CXX) $(CXXFLAGS) -o $@ memory_ps5_test.cpp $(ROOT)/src/core/memory_ps5.cpp -lps5platform

clean:
	rm -f $(ELF)

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $(ELF)

.PHONY: all clean test
