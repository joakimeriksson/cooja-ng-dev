/*
 * riot-root — a Contiki-NG RPL root that a stock RIOT node joins.
 *
 * The RIOT side is the unmodified gnrc_networking example, driven from its
 * shell.  This node starts a DODAG for fd00::/64 in storing mode (RIOT's RPL
 * builds storing-mode DAOs only; Contiki-NG's default RPL Lite is
 * non-storing), prints its route table whenever it changes, and answers UDP
 * on port 5678 from any source port by replying to the sender's port 8765,
 * where RIOT's `udp server start 8765` listens.
 *
 * The traffic is link-local.  Contiki-NG adds the RPL Option (RFC 6553, type
 * 0x63) to packets for global addresses, and RIOT has no handler for it; the
 * type's top bits say "discard if unrecognised", so RIOT drops them.
 */
#include "contiki.h"
#include "net/routing/routing.h"
#include "net/ipv6/simple-udp.h"
#include "net/ipv6/uip-ds6-route.h"
#include "net/ipv6/uiplib.h"

#include <stdio.h>
#include <string.h>

#define SERVER_PORT 5678
#define RIOT_PORT   8765

static struct simple_udp_connection conn;

PROCESS(riot_root_process, "riot-root");
AUTOSTART_PROCESSES(&riot_root_process);

static void
print_addr(const uip_ipaddr_t *a)
{
  char buf[UIPLIB_IPV6_MAX_STR_LEN];
  uiplib_ipaddr_snprint(buf, sizeof(buf), a);
  printf("%s", buf);
}

static void
rx(struct simple_udp_connection *c, const uip_ipaddr_t *from,
   uint16_t from_port, const uip_ipaddr_t *to, uint16_t to_port,
   const uint8_t *data, uint16_t len)
{
  static char reply[96];
  printf("RIOT says '%.*s' (from ", len, (const char *)data);
  print_addr(from);
  printf(")\n");
  int n = snprintf(reply, sizeof(reply), "Contiki-NG heard '%.*s'",
                   len > 60 ? 60 : len, (const char *)data);
  simple_udp_sendto_port(&conn, reply, n, from, RIOT_PORT);
}

PROCESS_THREAD(riot_root_process, ev, data)
{
  static struct etimer et;
  static int last_routes = -1;

  PROCESS_BEGIN();

  NETSTACK_ROUTING.root_start();
  simple_udp_register(&conn, SERVER_PORT, NULL, 0, rx);
  printf("riot-root: DODAG root for fd00::/64 (RPL Classic, storing mode); "
         "UDP on port %u\n", SERVER_PORT);

  etimer_set(&et, CLOCK_SECOND);
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
    etimer_reset(&et);
    int routes = uip_ds6_route_num_routes();
    if(routes == last_routes) {
      continue;
    }
    last_routes = routes;
    printf("routes: %d\n", routes);
    for(uip_ds6_route_t *r = uip_ds6_route_head(); r != NULL;
        r = uip_ds6_route_next(r)) {
      printf("  ");
      print_addr(&r->ipaddr);
      printf(" via ");
      print_addr(uip_ds6_route_nexthop(r));
      printf("\n");
    }
  }

  PROCESS_END();
}
