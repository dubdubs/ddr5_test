CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Werror
LDFLAGS ?= -pthread -lrt

BOOST_HOME ?= /home/xylan/workspace/DDR5Bridge/QEMU/Hybrid/wisteria/thirdparty/boost/v1.80
CPPFLAGS ?= -I$(BOOST_HOME)/include

TARGETS := sw_memory_client mock_hw_server dispatcher_sw_client

.PHONY: all clean

all: $(TARGETS)

sw_memory_client: sw_memory_client.cpp protocol.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ sw_memory_client.cpp $(LDFLAGS)

mock_hw_server: mock_hw_server.cpp protocol.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ mock_hw_server.cpp $(LDFLAGS)

dispatcher_sw_client: dispatcher_sw_client.cpp protocol.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ dispatcher_sw_client.cpp $(LDFLAGS)

clean:
	rm -f $(TARGETS)
