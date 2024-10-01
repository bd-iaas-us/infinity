CXX = g++
CXXFLAGS = -std=c++11 -Wall
DEBUG_FLAGS = -g -fsanitize=address -O0
RELEASE_FLAGS = -O3


INCLUDES = -I/usr/local/cuda/include
LDFLAGS = -L/usr/local/cuda/lib64
LIBS = -lcudart -luv -libverbs
PYBIND11_INCLUDES = $(shell python3 -m pybind11 --includes)
PYTHON_EXTENSION_SUFFIX = $(shell python3-config --extension-suffix)

PYBIND_TARGET= _infinity$(PYTHON_EXTENSION_SUFFIX)

SOURCES := $(wildcard *.cpp)
OBJECTS = $(SOURCES:.cpp=.o)

all: debug

debug: CXXFLAGS += $(DEBUG_FLAGS)
debug: $(PYBIND_TARGET) infinity_server

release: CXXFLAGS += $(RELEASE_FLAGS)
release: $(PYBIND_TARGET) infinity_server


-include $(OBJECTS:.o=.d)
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -fPIC -c $< -o $@

$(PYBIND_TARGET): pybind.cpp libinfinity.o utils.o protocol.o infinity.o log.o ibv_helper.o
	$(CXX) $(CXXFLAGS) $(INCLUDES) --shared -fPIC $(PYBIND11_INCLUDES) $^ \
	-o $(PYBIND_TARGET) $(LDFLAGS) $(LIBS)
	cp $(PYBIND_TARGET) ../infinity/
	
infinity_server: infinity.o utils.o protocol.o ibv_helper.o
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ $(LDFLAGS) $(LIBS)

.PHONY: clean test
clean:
	rm -f infinity_server *.so *.o *.d
	make -C test clean
