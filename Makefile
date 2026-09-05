CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -g -Isrc
SRCDIR   := src
BUILDDIR := build
BINDIR   := bin

# Library sources (everything except main.cpp)
LIB_SOURCES := $(filter-out $(SRCDIR)/main.cpp, $(shell find $(SRCDIR) -name '*.cpp'))
LIB_OBJECTS := $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(LIB_SOURCES))
PIC_OBJECTS := $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.pic.o,$(LIB_SOURCES))

# Targets
STATIC_LIB  := $(BINDIR)/libcse.a
DYNAMIC_LIB := $(BINDIR)/libcse.so
TARGET      := $(BINDIR)/cse

all: $(STATIC_LIB) $(DYNAMIC_LIB) $(TARGET)

# Static library
$(STATIC_LIB): $(LIB_OBJECTS)
	@mkdir -p $(BINDIR)
	ar rcs $@ $^

# Dynamic library
$(DYNAMIC_LIB): $(PIC_OBJECTS)
	@mkdir -p $(BINDIR)
	$(CXX) -shared -o $@ $^

# Executable (statically linked)
$(TARGET): $(BUILDDIR)/main.o $(STATIC_LIB)
	@mkdir -p $(BINDIR)
	$(CXX) $(BUILDDIR)/main.o $(STATIC_LIB) -o $@

# Normal objects (for static lib and main.o)
$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# PIC objects (for dynamic lib)
$(BUILDDIR)/%.pic.o: $(SRCDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -fPIC -c $< -o $@

clean:
	rm -rf $(BUILDDIR) $(BINDIR)

.PHONY: all clean
