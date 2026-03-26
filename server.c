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
#include "lwip/def.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/tcp.h"

/* Custom imports */
#include "lwipopts.h"
#include "dhcpserver.h"
#include "dnserver.h"

static const char WIFI_SSID[] = "nasPico";
static const char WIFI_PASSWORD[] = "LuBrRaJaTh";

#define AP_ADDR_0 192
#define AP_ADDR_1 168
#define AP_ADDR_2 4
#define AP_ADDR_3 1
#define AP_MASK_0 255
#define AP_MASK_1 255
#define AP_MASK_2 255
#define AP_MASK_3 0
#define WIFI_CHANNEL 1
#define CYW43_AUTH_WPA2_AES_PSK (0x00400004)
#define DNS_SERVER_PORT 53
#define STDIO_USB_WAIT_TIMEOUT_MS 3000

static void wait_for_optional_usb_serial(void) {
    absolute_time_t deadline = make_timeout_time_ms(STDIO_USB_WAIT_TIMEOUT_MS);

    while (!stdio_usb_connected() && !time_reached(deadline)) {
        sleep_ms(10);
    }
}

static bool captive_dns_query_proc(const char *name, ip4_addr_t *addr) {
    printf("DNS query: %s\n", name);
    IP4_ADDR(addr, AP_ADDR_0, AP_ADDR_1, AP_ADDR_2, AP_ADDR_3);
    return true;
}

static const char *sync_request_cgi_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]) {
    (void)iIndex;
    (void)iNumParams;
    (void)pcParam;
    (void)pcValue;

    printf("sync requested\n");
    return "/index.shtml";
}

static const tCGI cgi_handlers[] = {
    {"/sync.cgi", sync_request_cgi_handler},
};

int main() {
    stdio_init_all();
    wait_for_optional_usb_serial();
    printf("=== STDIO boot okay\n");

    /* Initialize CYW43 architecture */
    if (cyw43_arch_init()) {
        printf("failed to initialise\n");
        return 1;
    }
    printf("=== cyw43_arch initialized\n");

    /* Enable CYW43 architecture AP mode */
    cyw43_arch_enable_ap_mode(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);
    printf("=== cyw43_arch AP enabled\n");
    
    /* Set CYW43 access point communication channel --  */
    cyw43_wifi_ap_set_channel(&cyw43_state, WIFI_CHANNEL);
    printf("=== cyw43 AP channel set\n");
    
    /* Configuring AP-side IP/DHCP and starting HTTP server */

    dhcp_server_t dhcp; // DHCP state object
    ip_addr_t ip;
    ip_addr_t netmask;
    err_t dns_err;

    IP4_ADDR(&ip, AP_ADDR_0, AP_ADDR_1, AP_ADDR_2, AP_ADDR_3); // Host (Pico) address
    IP4_ADDR(&netmask, AP_MASK_0, AP_MASK_1, AP_MASK_2, AP_MASK_3);  // Final portion (y) of address (xxx.xxx.x.yyy) becomes device specifier

    cyw43_arch_lwip_begin();
    dhcp_server_init(&dhcp, &ip, &netmask);
    dns_err = dnserv_init(&ip, DNS_SERVER_PORT, captive_dns_query_proc);
    http_set_cgi_handlers(cgi_handlers, LWIP_ARRAYSIZE(cgi_handlers));
    httpd_init();
    cyw43_arch_lwip_end();

    printf("=== DHCP server initialized:\n");
    printf("IP Address: %d.%d.%d.%d\n", AP_ADDR_0, AP_ADDR_1, AP_ADDR_2, AP_ADDR_3);
    printf("Netmask: %d.%d.%d.%d\n", AP_MASK_0, AP_MASK_1, AP_MASK_2, AP_MASK_3);
    if (dns_err == ERR_OK) {
        printf("=== DNS server initialized\n");
    } else {
        printf("=== DNS server failed to initialize: %d\n", dns_err);
    }
    printf("=== HTTP server initialized\n");
    printf("=== Connect to Wi-Fi '%s' and open http://%d.%d.%d.%d/\n",
           WIFI_SSID, AP_ADDR_0, AP_ADDR_1, AP_ADDR_2, AP_ADDR_3);

    /* Infinite driving loop */
    while(1) {
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(1000));
    }
    
    dnserv_free();
    dhcp_server_deinit(&dhcp);

    return 1;
}
