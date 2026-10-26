#ifndef PROJECT_CONF_H_
#define PROJECT_CONF_H_
/* Zephyr's echo_server runs no routing protocol, so the server is resolved
 * with standard Neighbor Discovery (the Makefile's nullrouting turns NS on).
 * Keep the console to the exchange: the IPv6 layer logs the multicast it
 * drops (Zephyr's MLD reports) and its neighbor-cache notes at error level. */
#define LOG_CONF_LEVEL_IPV6 LOG_LEVEL_NONE
#endif /* PROJECT_CONF_H_ */
