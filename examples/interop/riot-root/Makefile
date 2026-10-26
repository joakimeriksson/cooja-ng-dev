# Contiki-NG RPL root for a stock RIOT gnrc_networking node (see riot-root.c).
# Build through tools/build-device-firmware.sh with --example
# examples/interop/riot-root and --make-args "CONTIKI=<contiki-ng checkout>".
CONTIKI_PROJECT = riot-root
all: $(CONTIKI_PROJECT)

MAKE_ROUTING = MAKE_ROUTING_RPL_CLASSIC

CONTIKI ?= ../../../../contiki-ng
include $(CONTIKI)/Makefile.include
