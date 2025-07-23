CXX = g++
CXXFLAGS = -std=c++17 -fPIC

GLIB_CFLAGS = $(shell pkg-config --cflags glib-2.0 gio-2.0)
GLIB_LIBS = $(shell pkg-config --libs glib-2.0 gio-2.0)

SRCDIR = src
INCDIR = include
LIBPSI_DIR = libpsi
LIBPSI_INCDIR = $(LIBPSI_DIR)/include
BUILDDIR = build
DATADIR = data

INCLUDES = -I$(INCDIR) -I$(LIBPSI_INCDIR) $(GLIB_CFLAGS)

LMKD_SOURCES = $(SRCDIR)/lmkd.cpp \
               $(SRCDIR)/reaper.cpp \
               $(SRCDIR)/watchdog.cpp \
               $(SRCDIR)/processwatcher.cpp \
               $(SRCDIR)/dbus.cpp

PSI_SOURCES = $(LIBPSI_DIR)/psi.cpp

LMKD_OBJECTS = $(LMKD_SOURCES:%.cpp=$(BUILDDIR)/%.o)
PSI_OBJECTS = $(PSI_SOURCES:%.cpp=$(BUILDDIR)/%.o)

OBJECTS = $(LMKD_OBJECTS) $(PSI_OBJECTS)

TARGET = lmkd

PREFIX ?= /usr

all: $(TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)/$(SRCDIR)
	mkdir -p $(BUILDDIR)/$(LIBPSI_DIR)

$(TARGET): $(OBJECTS)
	$(CXX) -g $(OBJECTS) -o $@ $(GLIB_LIBS)

$(BUILDDIR)/%.o: %.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/sbin
	install -d $(DESTDIR)$(PREFIX)/share/lmkd
	install -d $(DESTDIR)$(PREFIX)/lib/systemd/system
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/sbin/
	install -m 644 $(DATADIR)/lmkd.conf $(DESTDIR)$(PREFIX)/share/lmkd/
	install -m 644 $(DATADIR)/lmkd.service $(DESTDIR)$(PREFIX)/lib/systemd/system/
	install -d $(DESTDIR)$(PREFIX)/share/dbus-1/system-services
	install -m 0644 data/io.furios.Lmkd.service $(DESTDIR)$(PREFIX)/share/dbus-1/system-services/
	install -d $(DESTDIR)$(PREFIX)/share/dbus-1/system.d
	install -m 0644 data/io.furios.Lmkd.conf $(DESTDIR)$(PREFIX)/share/dbus-1/system.d/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/sbin/lmkd
	rm -rf $(DESTDIR)$(PREFIX)/share/lmkd
	rm -f $(DESTDIR)$(PREFIX)/lib/systemd/system/lmkd.service
	rm -f $(DESTDIR)$(PREFIX)/share/dbus-1/system-services/io.furios.Lmkd.service
	rm -f $(DESTDIR)$(PREFIX)/share/dbus-1/system.d/io.furios.Lmkd.conf

clean:
	rm -rf $(BUILDDIR)
	rm -f $(TARGET)

.PHONY: all clean install uninstall

-include $(OBJECTS:.o=.d)

$(BUILDDIR)/%.d: %.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MM -MT $(BUILDDIR)/$*.o $< > $@
