# Compiler settings
CXX ?= g++
CXXFLAGS = -std=c++17 -O3 -Wall -Wextra

# Linker flags (-static prevents missing DLL errors when running outside MSYS2 shell)
LDFLAGS = -static -static-libgcc -static-libstdc++

# Project files
TARGET = cachex.exe
SRCS = cachex.cpp
HEADERS = cachex_win.h cachex_linux.h cachex_netbsd.h result.h scsi_status.h

# Object files
OBJS = $(SRCS:.cpp=.o)

# Default rule
all: $(TARGET)

# Link the executable
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# Compile source files
%.o: %.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Clean up build artifacts
clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
