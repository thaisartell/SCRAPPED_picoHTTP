#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/apps/httpd.h"
#include "lwipopts.h"

#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/tcp.h"

static const char WIFI_SSID[] = "nasPico";
static const char WIFI_PASSWORD[] = "LuBrRaJaTh";

#define WIFI_CHANNEL 1
#define CYW43_AUTH_WPA2_AES_PSK (0x00400004)

cyw43_t cyw43_state;


int main() {
    stdio_init_all();
    if (cyw43_arch_init()) {
        printf("failed to initialise\n");
        return 1;
    }

    /* Enable CYW43 architecture AP mode */
    cyw43_arch_enable_ap_mode(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);
    
    /* Set CYW43 access point communication channel */
    cyw43_wifi_ap_set_channel(&cyw43_state, WIFI_CHANNEL);

    lwip_init();

    // Print a success message once connected
    printf("Connected! \n");
    
    // Initialise web server
    httpd_init();
    printf("Http server initialised\n");

    printf("connected\n");
    
    pico_cyw43_arch_lwip_poll();
    while(1) {
        cyw43_arch_poll();
    }

    return 1;
}
