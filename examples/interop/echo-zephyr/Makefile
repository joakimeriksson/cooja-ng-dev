# Contiki-NG client of Zephyr's stock echo_server (see echo-zephyr.c).  Build
# through tools/build-device-firmware.sh with --example
# examples/interop/echo-zephyr and --make-args "CONTIKI=<contiki-ng checkout>".
CONTIKI_PROJECT = echo-zephyr
all: $(CONTIKI_PROJECT)

MAKE_ROUTING = MAKE_ROUTING_NULLROUTING

CONTIKI ?= ../../../../contiki-ng
include $(CONTIKI)/Makefile.include
