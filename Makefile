CONTIKI_PROJECT = leader-election
all: $(CONTIKI_PROJECT)

# TelosB / Tmote Sky
TARGET ?= sky

# No network layer: single-hop group, NullNet on top of CSMA (default MAC)
MAKE_NET = MAKE_NET_NULLNET

PROJECT_SOURCEFILES += le-election.c le-link.c le-energy.c le-stats.c le-sensor.c

CONTIKI = ../..
include $(CONTIKI)/Makefile.include

# Static memory usage (text = flash, data+bss = RAM)
.PHONY: size
size: $(CONTIKI_PROJECT).$(TARGET)
	$(SIZE) $<
