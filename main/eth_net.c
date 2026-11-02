#include "eth_net.h"

#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "sdkconfig.h"

static const char *TAG = "eth";

static volatile bool s_has_ip;

#if CONFIG_SCINTIX_ETHERNET

static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == ETH_EVENT && event_id == ETHERNET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "Link up");
    } else if (event_base == ETH_EVENT && event_id == ETHERNET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "Link down");
        s_has_ip = false;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        s_has_ip = true;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_LOST_IP) {
        s_has_ip = false;
    }
}

esp_err_t eth_net_start(const char *hostname)
{
    /* The ESP32-P4 EMAC defaults are the module's RMII wiring: MDC 31, MDIO 52,
     * 50 MHz REF_CLK from the PHY into GPIO50, TX_EN 49, TXD0/1 34/35,
     * CRS_DV 28, RXD0/1 29/30. */
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = CONFIG_SCINTIX_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_SCINTIX_ETH_PHY_RST_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_ksz80xx(&phy_config);
    if (mac == NULL || phy == NULL) {
        ESP_LOGE(TAG, "EMAC/PHY allocation failed");
        if (mac) {
            mac->del(mac);
        }
        if (phy) {
            phy->del(phy);
        }
        return ESP_ERR_NO_MEM;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = NULL;
    esp_err_t err = esp_eth_driver_install(&eth_config, &eth);
    if (err != ESP_OK) {
        /* No PHY answering on MDIO, or no REF_CLK from it. */
        ESP_LOGW(TAG, "Ethernet driver install failed (%s); continuing without Ethernet", esp_err_to_name(err));
        mac->del(mac);
        phy->del(phy);
        return err;
    }

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_config);
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth);
    if (netif == NULL || glue == NULL) {
        ESP_LOGE(TAG, "Ethernet netif allocation failed");
        return ESP_ERR_NO_MEM;
    }
    if (hostname != NULL) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_hostname(netif, hostname));
    }
    if ((err = esp_netif_attach(netif, glue)) != ESP_OK ||
        (err = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL)) != ESP_OK ||
        (err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, eth_event_handler, NULL)) != ESP_OK ||
        (err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, eth_event_handler, NULL)) != ESP_OK ||
        (err = esp_eth_start(eth)) != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Ethernet started (KSZ8091 PHY addr %d); waiting for link + DHCP", CONFIG_SCINTIX_ETH_PHY_ADDR);
    return ESP_OK;
}

#else

esp_err_t eth_net_start(const char *hostname)
{
    (void)hostname;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif

bool eth_net_has_ip(void)
{
    return s_has_ip;
}
