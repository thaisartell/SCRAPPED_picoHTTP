#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/apps/httpd.h"
#include "lwipopts.h"

#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/tcp.h"

static const char WIFI_SSID[] = "CenturyLink2405";
static const char WIFI_PASSWORD[] = "a7df7b3f6eeed6";


int main() {
    stdio_init_all();
    if (cyw43_arch_init()) {
        printf("failed to initialise\n");
        return 1;
    }
    // if (cyw43_arch_init_with_country(CYW43_COUNTRY_USA)) {
    //     printf("failed to initialise\n");
    //     return 1;
    // }
    cyw43_arch_enable_sta_mode();

    // Connect to the WiFI network - loop until connected
    while(cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000) != 0){
        printf("Attempting to connect...\n");
    }
    // Print a success message once connected
    printf("Connected! \n");
    
    // Initialise web server
    httpd_init();
    printf("Http server initialised\n");

    printf("connected\n");
    
    while(1);

    return 1;
}


/*
extern struct netif *netif_default;

static err_t on_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    tcp_close(tpcb);
    return ERR_OK;
}

static void heartbeat(void) {
    static bool on = false;
    on = !on;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
}

static err_t on_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    if (!p) { tcp_close(tpcb); return ERR_OK; }   // client closed
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);

    static const char resp[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "\r\n"
        "Hello from Pico\r\n";

    tcp_write(tpcb, resp, sizeof(resp) - 1, TCP_WRITE_FLAG_COPY);
    tcp_output(tpcb);
    return ERR_OK;
}

static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    tcp_recv(newpcb, on_recv);
    tcp_sent(newpcb, on_sent);
    return ERR_OK;
}

static void httpd_raw_init(void) {
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    tcp_bind(pcb, IP4_ADDR_ANY, 80);
    pcb = tcp_listen_with_backlog(pcb, 4);
    tcp_accept(pcb, on_accept);
}

int main() {
    stdio_init_all();
    sleep_ms(2000);

    if (cyw43_arch_init_with_country(CYW43_COUNTRY_USA)) {
        printf("cyw43 init failed\n");
        return 1;
    }
    cyw43_arch_enable_sta_mode();

    while (cyw43_arch_wifi_connect_timeout_ms(
               WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000) != 0) {
        printf("Attempting to connect...\n");
    }

    if (netif_default) {
        printf("IP: %s\n", ip4addr_ntoa(netif_ip4_addr(netif_default)));
    }

    cyw43_arch_lwip_begin();
    httpd_raw_init();
    cyw43_arch_lwip_end();

    while (true) {
        heartbeat();
        tight_loop_contents();
        sleep_ms(250);
    }
}
*/
