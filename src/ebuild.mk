HEADERDIR  := $(TOPDIR)/include

bins                   += $(PACKAGE)
$(PACKAGE)-objs        := main.o config_tools.o modules.o notifications.o operational.o rpc.o schema.o sessions.o
$(PACKAGE)-objs        += status.o transport.o utilities.o libconfig.o
$(PACKAGE)-cflags      := $(EXTRA_CFLAGS)
$(PACKAGE)-ldflags     := $(EXTRA_LDFRAGS) -ljson-c
$(PACKAGE)-pkgconf     := libyang sysrepo fcgi libstroll libelog libutils libconfig