CXX = g++
CC = gcc

CXXFLAGS = -std=c++17 -fPIC
GLIB_CFLAGS = $(shell pkg-config --cflags glib-2.0 gio-2.0)
GLIB_LIBS = $(shell pkg-config --libs glib-2.0 gio-2.0)
NOTIFY_CFLAGS = $(shell pkg-config --cflags libnotify)
NOTIFY_LIBS = $(shell pkg-config --libs libnotify)

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
               $(SRCDIR)/dbus.cpp \
               $(SRCDIR)/config.cpp

LMKD_USER_SERVER_SOURCES = $(SRCDIR)/user/lmkd-user-server.c

PSI_SOURCES = $(LIBPSI_DIR)/psi.cpp

LMKD_OBJECTS = $(LMKD_SOURCES:%.cpp=$(BUILDDIR)/%.o)
LMKD_USER_SERVER_OBJECTS = $(LMKD_USER_SERVER_SOURCES:%.c=$(BUILDDIR)/%.o)
PSI_OBJECTS = $(PSI_SOURCES:%.cpp=$(BUILDDIR)/%.o)
OBJECTS = $(LMKD_OBJECTS) $(PSI_OBJECTS)

TARGET_LMKD = lmkd
TARGET_LMKD_USER_SERVER = lmkd-user-server

PREFIX ?= /usr

all: $(TARGET_LMKD) $(TARGET_LMKD_USER_SERVER)

.PHONY: all clean install uninstall

$(BUILDDIR):
	mkdir -p $(BUILDDIR)/$(SRCDIR)
	mkdir -p $(BUILDDIR)/$(SRCDIR)/user
	mkdir -p $(BUILDDIR)/$(LIBPSI_DIR)

$(TARGET_LMKD): $(OBJECTS)
	$(CXX) -g $(OBJECTS) -o $@ $(GLIB_LIBS)

$(TARGET_LMKD_USER_SERVER): $(LMKD_USER_SERVER_OBJECTS)
	$(CC) $(LMKD_USER_SERVER_OBJECTS) -o $@ $(GLIB_LIBS) $(NOTIFY_LIBS)

$(BUILDDIR)/%.o: %.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: %.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(GLIB_CFLAGS) $(NOTIFY_CFLAGS) -c $< -o $@

install: $(TARGET_LMKD) $(TARGET_LMKD_USER_SERVER)
	install -d $(DESTDIR)$(PREFIX)/sbin
	install -d $(DESTDIR)$(PREFIX)/libexec
	install -d $(DESTDIR)$(PREFIX)/share/lmkd
	install -d $(DESTDIR)$(PREFIX)/lib/systemd/system
	install -d $(DESTDIR)$(PREFIX)/lib/systemd/user
	install -m 755 $(TARGET_LMKD) $(DESTDIR)$(PREFIX)/sbin/
	install -m 755 $(TARGET_LMKD_USER_SERVER) $(DESTDIR)$(PREFIX)/libexec/
	install -m 644 $(DATADIR)/lmkd.conf $(DESTDIR)$(PREFIX)/share/lmkd/
	install -m 644 $(DATADIR)/lmkd.service $(DESTDIR)$(PREFIX)/lib/systemd/system/
	install -d $(DESTDIR)$(PREFIX)/share/dbus-1/system-services
	install -m 0644 data/io.furios.Lmkd.service $(DESTDIR)$(PREFIX)/share/dbus-1/system-services/
	install -d $(DESTDIR)$(PREFIX)/share/dbus-1/system.d
	install -m 0644 data/io.furios.Lmkd.conf $(DESTDIR)$(PREFIX)/share/dbus-1/system.d/
	install -m 644 $(DATADIR)/lmkd-user-server.service $(DESTDIR)$(PREFIX)/lib/systemd/user/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/sbin/lmkd
	rm -f $(DESTDIR)$(PREFIX)/libexec/lmkd-user-server
	rm -rf $(DESTDIR)$(PREFIX)/share/lmkd
	rm -f $(DESTDIR)$(PREFIX)/lib/systemd/system/lmkd.service
	rm -f $(DESTDIR)$(PREFIX)/lib/systemd/user/lmkd-user-server.service
	rm -f $(DESTDIR)$(PREFIX)/share/dbus-1/system-services/io.furios.Lmkd.service
	rm -f $(DESTDIR)$(PREFIX)/share/dbus-1/system.d/io.furios.Lmkd.conf

clean:
	rm -rf $(BUILDDIR)
	rm -f $(TARGET_LMKD)
	rm -f $(TARGET_LMKD_USER_SERVER)
