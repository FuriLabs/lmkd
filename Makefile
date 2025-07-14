CXX = g++
CXXFLAGS = -std=c++17 -fPIC

GLIB_CFLAGS = $(shell pkg-config --cflags glib-2.0)
GLIB_LIBS = $(shell pkg-config --libs glib-2.0)

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
               $(SRCDIR)/processwatcher.cpp

PSI_SOURCES = $(LIBPSI_DIR)/psi.cpp

LMKD_OBJECTS = $(LMKD_SOURCES:%.cpp=$(BUILDDIR)/%.o)
PSI_OBJECTS = $(PSI_SOURCES:%.cpp=$(BUILDDIR)/%.o)

OBJECTS = $(LMKD_OBJECTS) $(PSI_OBJECTS)

TARGET = lmkd

all: $(TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)/$(SRCDIR)
	mkdir -p $(BUILDDIR)/$(LIBPSI_DIR)

$(TARGET): $(OBJECTS)
	$(CXX) -g $(OBJECTS) -o $@ $(GLIB_LIBS)

$(BUILDDIR)/%.o: %.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

install: $(TARGET)
	install -d $(DESTDIR)/usr/sbin
	install -d $(DESTDIR)/usr/share/lmkd
	install -d $(DESTDIR)/lib/systemd/system
	install -m 755 $(TARGET) $(DESTDIR)/usr/sbin/
	install -m 644 $(DATADIR)/lmkd.conf $(DESTDIR)/usr/share/lmkd/
	install -m 644 $(DATADIR)/lmkd.service $(DESTDIR)/lib/systemd/system/

uninstall:
	rm -f $(DESTDIR)/usr/sbin/lmkd
	rm -rf $(DESTDIR)/usr/share/lmkd
	rm -f $(DESTDIR)/lib/systemd/system/lmkd.service

clean:
	rm -rf $(BUILDDIR)
	rm -f $(TARGET)

.PHONY: all clean install uninstall

-include $(OBJECTS:.o=.d)

$(BUILDDIR)/%.d: %.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MM -MT $(BUILDDIR)/$*.o $< > $@
