CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
TARGET = local_mw
SRC = local_mw.cpp
PREFIX = $(HOME)/.local
BINDIR = $(PREFIX)/bin

# Version from git tags with fallback
VERSION ?= $(shell git describe --tags 2>/dev/null || echo "dev-unknown")
CXXFLAGS += -DAPP_VERSION="\"$(VERSION)\""

all: $(TARGET)

$(TARGET): $(SRC)
	mkdir -p bin
	$(CXX) $(CXXFLAGS) -o bin/$(TARGET) $(SRC)

clean:
	rm -f bin/$(TARGET)

run: $(TARGET)
	./bin/$(TARGET)

install: $(TARGET)
	mkdir -p $(BINDIR)
	install -m 755 bin/$(TARGET) $(BINDIR)/$(TARGET)
	@echo "Installed $(TARGET) to $(BINDIR)/$(TARGET)"
	@echo "Make sure $(BINDIR) is in your PATH"

uninstall:
	rm -f $(BINDIR)/$(TARGET)
	@echo "Removed $(TARGET) from $(BINDIR)"

.PHONY: all clean run install uninstall