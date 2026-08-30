CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -g -Isrc
SRCDIR   := src
BUILDDIR := build
TARGET   := bin/cse

SOURCES := $(shell find $(SRCDIR) -name '*.cpp')
OBJECTS := $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(SOURCES))

all: $(TARGET)

$(TARGET): $(OBJECTS)
	@mkdir -p bin
	$(CXX) $(OBJECTS) -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILDDIR) $(TARGET)

.PHONY: all clean
