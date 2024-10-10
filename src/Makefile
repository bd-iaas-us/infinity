CXX = g++
CXXFLAGS = -std=c++11 -Wall -g


INCLUDES = -I/usr/local/cuda/include
LDFLAGS = -L/usr/local/cuda/lib64
LIBS = -lcudart -luv -libverbs


SITE_PACKAGES = $(shell python3.11 -c "import site; print(site.getsitepackages()[0])")
#only triton has complete cuda include files
INCLUDES += -I$(SITE_PACKAGES)/triton/backends/nvidia/include/
#find cuda runtime lib
LDFLAGS += -L$(SITE_PACKAGES)/nvidia/cuda_runtime/lib/

PYBIND11_INCLUDES = $(shell python3.11 -m pybind11 --includes)
PYTHON_EXTENSION_SUFFIX = $(shell python3.11-config --extension-suffix)

PYBIND_TARGET= _infinistore$(PYTHON_EXTENSION_SUFFIX)

SOURCES := $(wildcard *.cpp)
OBJECTS = $(SOURCES:.cpp=.o)

all:$(PYBIND_TARGET)

-include $(OBJECTS:.o=.d)
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -fPIC -c $< -o $@

$(PYBIND_TARGET): pybind.cpp libinfinistore.o utils.o protocol.o infinistore.o log.o ibv_helper.o mempool.o
	$(CXX) $(CXXFLAGS) $(INCLUDES) --shared -fPIC $(PYBIND11_INCLUDES) $^ \
	-o $(PYBIND_TARGET) $(LDFLAGS) $(LIBS)
	cp $(PYBIND_TARGET) ../infinistore/


.PHONY: clean test
clean:
	rm *.so *.o *.d
	make -C test clean
