CC ?= gcc
CXX ?= g++
CFLAGS ?= -O2 -Wall -Wextra
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++17
CPPFLAGS ?=
LDFLAGS ?=

TARGET := libfake_ddr5.so
SRC := fake_ddr5.c
TARGET_CC := libfake_ddr5_cc.so
SRC_CC := fake_ddr5.cc
TARGET_CC_BLOCKING := libfake_ddr5_cc_blocking.so
TARGET_CC_NONBLOCKING := libfake_ddr5_cc_nonblocking.so

# If svdpi.h is not in default include path, pass:
#   make SV_DPI_INC=/path/to/simulator/include
SV_DPI_INC ?=
INCLUDES := $(if $(SV_DPI_INC),-I$(SV_DPI_INC),)

.PHONY: all cc cc_blocking cc_nonblocking clean

all: $(TARGET)

cc: $(TARGET_CC)
cc_blocking: $(TARGET_CC_BLOCKING)
cc_nonblocking: $(TARGET_CC_NONBLOCKING)

$(TARGET): $(SRC)
	$(CC) -fPIC -shared $(CPPFLAGS) $(CFLAGS) $(INCLUDES) -o $@ $< $(LDFLAGS)

$(TARGET_CC): $(SRC_CC)
	$(CXX) -fPIC -shared $(CPPFLAGS) $(CXXFLAGS) $(INCLUDES) -o $@ $< $(LDFLAGS)

$(TARGET_CC_BLOCKING): $(SRC_CC)
	$(CXX) -fPIC -shared $(CPPFLAGS) $(CXXFLAGS) $(INCLUDES) -o $@ $< $(LDFLAGS)

$(TARGET_CC_NONBLOCKING): $(SRC_CC)
	$(CXX) -fPIC -shared $(CPPFLAGS) $(CXXFLAGS) -DMM_DDR5_SINGLE_STORAGE_NONBLOCKING $(INCLUDES) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET) $(TARGET_CC) $(TARGET_CC_BLOCKING) $(TARGET_CC_NONBLOCKING)
