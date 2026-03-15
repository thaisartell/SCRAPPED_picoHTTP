/* Mental Model:
CYW43 config : Wi-Fi network exists
DHCP server : clients obtain IP addresses
lwIP TCP/IP : handles packet transport
HTTP server : handles browser requests
Code : generates page behavior */

/* Standard library imports */
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

/* lwIP library imports */
#include "lwip/apps/httpd.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/tcp.h"

/* Custom imports */
#include "lwipopts.h"
#include "dhcpserver.h"

static const char WIFI_SSID[] = "nasPico";
static const char WIFI_PASSWORD[] = "LuBrRaJaTh";

#define WIFI_CHANNEL 1
#define CYW43_AUTH_WPA2_AES_PSK (0x00400004)

int main() {
    stdio_init_all();

    /* Initialize CYW43 architecture */
    if (cyw43_arch_init()) {
        printf("failed to initialise\n");
        return 1;
    }

    /* Enable CYW43 architecture AP mode */
    cyw43_arch_enable_ap_mode(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);
    
    /* Set CYW43 access point communication channel --  */
    cyw43_wifi_ap_set_channel(&cyw43_state, WIFI_CHANNEL);
    
    /* Configuring AP-side IP/DHCP and starting HTTP server */

    dhcp_server_t dhcp; // DHCP state object
    ip_addr_t ip;
    ip_addr_t netmask;

    IP4_ADDR(&ip, 192,168,4,1); // Host (Pico) address
    IP4_ADDR(&netmask, 255,255,255,0);  // Final portion (y) of address (xxx.xxx.x.yyy) becomes device specifier

    dhcp_server_init(&dhcp, &ip, &netmask);

    /* Initialize HTTP server */
    httpd_init();

    /* Infinite driving loop */
    while(1) {
        cyw43_arch_poll();
    }
    
    dhcp_server_deinit(&dhcp);

    return 1;
}
