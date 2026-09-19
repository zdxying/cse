CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -g -O0 -Isrc -MMD -MP
SRCDIR   := src
PLUGDIR  := plugins
BUILDDIR := build
BINDIR   := bin

# Library sources (everything except the two program entry points)
LIB_SOURCES := $(filter-out $(SRCDIR)/main.cpp $(PLUGDIR)/freelb/ur_emit_main.cpp, $(shell find $(SRCDIR) $(PLUGDIR) -name '*.cpp'))

# Map source paths to object paths: src/foo/bar.o -> build/foo/bar.o, plugins/freelb/baz.o -> build/plugins/freelb/baz.o
LIB_OBJECTS := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(LIB_SOURCES))
PIC_OBJECTS := $(patsubst %.cpp,$(BUILDDIR)/%.pic.o,$(LIB_SOURCES))

# Auto-generated header dependencies (so header edits trigger rebuilds)
DEPS := $(LIB_OBJECTS:.o=.d) $(PIC_OBJECTS:.o=.d) $(BUILDDIR)/$(SRCDIR)/main.d \
        $(BUILDDIR)/$(PLUGDIR)/freelb/ur_emit_main.d

# Targets
STATIC_LIB  := $(BINDIR)/libcse.a
DYNAMIC_LIB := $(BINDIR)/libcse.so
TARGET      := $(BINDIR)/cse
CSEGEN      := $(BINDIR)/csegen

all: $(STATIC_LIB) $(DYNAMIC_LIB) $(TARGET) $(CSEGEN)

# Static library
$(STATIC_LIB): $(LIB_OBJECTS)
	@mkdir -p $(BINDIR)
	ar rcs $@ $^

# Dynamic library
$(DYNAMIC_LIB): $(PIC_OBJECTS)
	@mkdir -p $(BINDIR)
	$(CXX) -shared -o $@ $^

# Executable (statically linked)
$(TARGET): $(BUILDDIR)/$(SRCDIR)/main.o $(STATIC_LIB)
	@mkdir -p $(BINDIR)
	$(CXX) $(BUILDDIR)/$(SRCDIR)/main.o $(STATIC_LIB) -o $@

# FreeLB .ur.h generator (statically linked)
$(CSEGEN): $(BUILDDIR)/$(PLUGDIR)/freelb/ur_emit_main.o $(STATIC_LIB)
	@mkdir -p $(BINDIR)
	$(CXX) $(BUILDDIR)/$(PLUGDIR)/freelb/ur_emit_main.o $(STATIC_LIB) -o $@

# Normal objects (for static lib and main.o)
$(BUILDDIR)/$(SRCDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILDDIR)/$(PLUGDIR)/%.o: $(PLUGDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# PIC objects (for dynamic lib)
$(BUILDDIR)/$(SRCDIR)/%.pic.o: $(SRCDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -fPIC -c $< -o $@

$(BUILDDIR)/$(PLUGDIR)/%.pic.o: $(PLUGDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -fPIC -c $< -o $@

clean:
	rm -rf $(BUILDDIR) $(BINDIR)

# Include header dependencies last so the default goal stays `all`.
-include $(DEPS)

.PHONY: all clean
