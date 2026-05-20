CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2 -g

SRCDIR := src
OBJDIR := obj
BINDIR := bin
TARGET := $(BINDIR)/netmon

# Auto-discover every .cpp under src/.
SRCS := $(shell find $(SRCDIR) -name '*.cpp')
OBJS := $(patsubst $(SRCDIR)/%.cpp,$(OBJDIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

INCLUDE := -I$(SRCDIR)
LIB     := -lSDL2 -lSDL2_image -lSDL2_ttf

# Platform-specific glue.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    BREW_PREFIX := $(shell brew --prefix)
    INCLUDE += -I$(BREW_PREFIX)/include -I$(BREW_PREFIX)/include/SDL2 -D_THREAD_SAFE
    LIB     := -L$(BREW_PREFIX)/lib $(LIB)
endif

CXXFLAGS += $(INCLUDE)

all: $(TARGET)

$(TARGET): $(OBJS) | $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIB)

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(BINDIR):
	@mkdir -p $(BINDIR)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(OBJDIR) $(BINDIR)

-include $(DEPS)

.PHONY: all clean run
