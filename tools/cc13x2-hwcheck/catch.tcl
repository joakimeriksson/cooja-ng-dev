# openocd script: get a CC13x2 LaunchPad's CPU halted whatever its image does.
#
# An image that sleeps in standby (any Contiki build with the radio off,
# e.g. hello-world-nullradio) powers the JTAG domain down, and openocd's
# plain "init; halt" then reads an all-zero scan chain.  Pulse nRESET
# through the XDS110 and enable the CPU tap in the boot window, before the
# image reaches standby.  The CPU halts in the boot ROM; it stays parked
# there after openocd exits until a reset through the core ("reset run").
reset_config srst_only srst_nogate
init
adapter assert srst
sleep 20
adapter deassert srst
set ok 0
for {set i 0} {$i < 40} {incr i} {
    if {[catch {jtag arp_init; dap init; cc13x2.cpu arp_examine; halt 100} err] == 0 &&
        [cc13x2.cpu curstate] eq "halted"} {
        set ok 1
        break
    }
    sleep 5
}
if {!$ok} { error "cc13x2: CPU not caught after reset" }
