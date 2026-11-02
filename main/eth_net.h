#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* On-module Ethernet: ESP32-P4 EMAC + Microchip KSZ8091RNA PHY over RMII, DHCP
 * client. Needs esp_netif_init() and the default event loop first; the caller
 * handles IP_EVENT_ETH_GOT_IP. Without a cable it just waits for a link. */
esp_err_t eth_net_start(const char *hostname);

/* True while the Ethernet interface holds a DHCP lease. */
bool eth_net_has_ip(void);
