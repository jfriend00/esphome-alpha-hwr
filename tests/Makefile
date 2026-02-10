# Makefile for GENI Protocol Unit Tests
# Compiles and runs host-based tests without ESP32 dependencies

CXX ?= g++
CXXFLAGS = -std=c++11 -Wall -Wextra -O2 -I.
LDFLAGS =

# Test executables
TESTS = test_protocol

# Default target
all: $(TESTS)

# Build test executable
test_protocol: test_protocol.cpp protocol.h
	$(CXX) $(CXXFLAGS) test_protocol.cpp -o test_protocol $(LDFLAGS)

# Run tests
test: $(TESTS)
	@echo "=========================================="
	@echo "Running Protocol Tests..."
	@echo "=========================================="
	@./test_protocol

# Alternative targets for specific compilers
test-gcc: CXX = g++
test-gcc: clean test

test-clang: CXX = clang++
test-clang: clean test

# Clean build artifacts
clean:
	rm -f $(TESTS) *.o

# Help target
help:
	@echo "GENI Protocol Unit Test Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all         - Build all test executables (default)"
	@echo "  test        - Build and run all tests"
	@echo "  test-gcc    - Build and run tests with GCC"
	@echo "  test-clang  - Build and run tests with Clang"
	@echo "  clean       - Remove build artifacts"
	@echo "  help        - Show this help message"
	@echo ""
	@echo "Environment Variables:"
	@echo "  CXX         - C++ compiler (default: g++)"
	@echo "  CXXFLAGS    - Compiler flags"
	@echo "  LDFLAGS     - Linker flags"

.PHONY: all test test-gcc test-clang clean help
