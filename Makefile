# Compiler settings
CXX ?= g++
CXXFLAGS = -std=c++17 -O3 -Wall -Wextra

# Linker flags (-static prevents missing DLL errors when running outside MSYS2 shell)
LDFLAGS = -static -static-libgcc -static-libstdc++

# Project files
TARGET = cachex.exe
SRCS = src/main.cpp
HEADERS = src/platform_win.h src/platform_linux.h src/platform_netbsd.h src/result.h src/scsi_status.h

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
