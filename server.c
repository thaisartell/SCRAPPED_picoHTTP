/* Mental Model:
CYW43 config : Wi-Fi network exists
DHCP server : clients obtain IP addresses
lwIP TCP/IP : handles packet transport
HTTP server : handles browser requests
Code : generates page behavior */

/* Standard library imports */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/sync.h"

/* lwIP library imports */
#include "lwip/apps/fs.h"
#include "lwip/apps/httpd.h"
#include "lwip/def.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/mem.h"
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
#define STATUS_RESPONSE_BUFFER_SIZE 256
#define STDIO_USB_WAIT_TIMEOUT_MS 3000
#define SYNC_REQUESTED_DELAY_MS 1000
#define SYNC_TRANSFERRING_DELAY_MS 2000
#define SYNC_SUCCESS_DELAY_MS 1000

typedef enum {
    SYNC_STATE_IDLE,
    SYNC_STATE_REQUESTED,
    SYNC_STATE_TRANSFERRING,
    SYNC_STATE_SUCCESS,
    SYNC_STATE_ERROR,
} sync_state_t;

static sync_state_t sync_state = SYNC_STATE_IDLE;
static char sync_status_message[64];

/* Timestamp (ms) until sync state transition */
static absolute_time_t sync_next_transition_at;

static const char HTTP_SERVER_HEADER[] = "Server: lwIP/pre-0.6 (http://www.sics.se/~adam/lwip/)\r\n";
static const char STATUS_ERROR_RESPONSE[] =
    "HTTP/1.0 503 Service Unavailable\r\n"
    "Server: lwIP/pre-0.6 (http://www.sics.se/~adam/lwip/)\r\n"
    "Content-Type: application/json\r\n"
    "Cache-Control: no-store\r\n"
    "Pragma: no-cache\r\n"
    "Expires: 0\r\n"
    "\r\n"
    "{\"state\":\"ERROR\",\"message\":\"Status unavailable\"}\n";

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

/* Return string corresponding to state */
static const char *sync_state_name(sync_state_t state) {
    switch (state) {
        case SYNC_STATE_IDLE:
            return "IDLE";
        case SYNC_STATE_REQUESTED:
            return "REQUESTED";
        case SYNC_STATE_TRANSFERRING:
            return "TRANSFERRING";
        case SYNC_STATE_SUCCESS:
            return "SUCCESS";
        case SYNC_STATE_ERROR:
            return "ERROR";
        default:
            return "ERROR";
    }
}

/* Sync entry. Initialize state machine to IDLE. Set timestamp for next state transition. */
static void sync_initialize(void) {
    uint32_t interrupt_state = save_and_disable_interrupts();

    sync_state = SYNC_STATE_IDLE;
    snprintf(sync_status_message, sizeof(sync_status_message), "%s", "Ready");
    sync_next_transition_at = at_the_end_of_time;

    restore_interrupts(interrupt_state);
}

static void sync_set_state(sync_state_t new_state, const char *message, absolute_time_t next_transition_at) {
    char message_copy[sizeof(sync_status_message)];
    uint32_t interrupt_state = save_and_disable_interrupts();

    sync_state = new_state;
    snprintf(sync_status_message, sizeof(sync_status_message), "%s", message);
    sync_next_transition_at = next_transition_at;
    snprintf(message_copy, sizeof(message_copy), "%s", sync_status_message);

    restore_interrupts(interrupt_state);

    printf("sync state -> %s: %s\n", sync_state_name(new_state), message_copy);
}

static void sync_get_snapshot(sync_state_t *state, char *message, size_t message_size) {
    uint32_t interrupt_state = save_and_disable_interrupts();

    *state = sync_state;
    snprintf(message, message_size, "%s", sync_status_message);

    restore_interrupts(interrupt_state);
}

static void sync_request(void) {
    sync_state_t current_state;
    uint32_t interrupt_state = save_and_disable_interrupts();

    current_state = sync_state;
    restore_interrupts(interrupt_state);
    if (current_state != SYNC_STATE_IDLE) { /* If sync requested in non-IDLE state, ignore while busy */
        printf("sync request ignored: %s\n", sync_state_name(current_state));
        return;
    }

    /* Otherwise, verified IDLE state and can move to SYNC_STATE_REQUESTED */
    sync_set_state(SYNC_STATE_REQUESTED, "Sync requested", make_timeout_time_ms(SYNC_REQUESTED_DELAY_MS));
}

/*
* Runs in driving loop. 
*/
static void sync_poll_state_machine(void) {
    sync_state_t current_state;
    absolute_time_t next_transition_at;
    uint32_t interrupt_state = save_and_disable_interrupts();

    current_state = sync_state;
    next_transition_at = sync_next_transition_at;

    restore_interrupts(interrupt_state);

    /* Do nothing */
    if ((current_state == SYNC_STATE_IDLE) || (current_state == SYNC_STATE_ERROR)) {
        return;
    }

    /* If past current state transition time, break */
    if (!time_reached(next_transition_at)) {
        return;
    }


    if (current_state == SYNC_STATE_REQUESTED) {    /* If sync requested, move to transfer state */
        sync_set_state(
            SYNC_STATE_TRANSFERRING,
            "Transfer in progress",
            make_timeout_time_ms(SYNC_TRANSFERRING_DELAY_MS)
        );
    } else if (current_state == SYNC_STATE_TRANSFERRING) {  /* If transferring, signal transfer success */
        sync_set_state(
            SYNC_STATE_SUCCESS,
            "Transfer complete",
            make_timeout_time_ms(SYNC_SUCCESS_DELAY_MS)
        );
    } else if (current_state == SYNC_STATE_SUCCESS) {   /* If post-transfer success, default back to IDLE */
        sync_set_state(SYNC_STATE_IDLE, "Ready", at_the_end_of_time);
    }
}

static const char *sync_request_cgi_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]) {
    (void)iIndex;
    (void)iNumParams;
    (void)pcParam;
    (void)pcValue;

    sync_request();
    return "/index.shtml";
}

static const tCGI cgi_handlers[] = {
    {"/sync.cgi", sync_request_cgi_handler},
};

/* Recognized by lwIP as hook via defining macro LWIP_HTTPD_CUSTOM_FILES 
* Serves HTTP responses that are not stored in htmldata.c
*/
int fs_open_custom(struct fs_file *file, const char *name) {
    sync_state_t current_state;
    char current_message[sizeof(sync_status_message)];
    char *response;
    int response_len;

    if (strcmp(name, "/status") != 0) {
        return 0;
    }

    memset(file, 0, sizeof(struct fs_file));
    sync_get_snapshot(&current_state, current_message, sizeof(current_message));

    response = mem_malloc(STATUS_RESPONSE_BUFFER_SIZE);
    if (response == NULL) {
        file->data = STATUS_ERROR_RESPONSE;
        file->len = (int)strlen(STATUS_ERROR_RESPONSE);
        file->index = file->len;
        file->flags = FS_FILE_FLAGS_HEADER_INCLUDED | FS_FILE_FLAGS_HEADER_PERSISTENT;
        return 1;
    }

    response_len = snprintf(
        response,
        STATUS_RESPONSE_BUFFER_SIZE,
        "HTTP/1.0 200 OK\r\n"
        "%s"
        "Content-Type: application/json\r\n"
        "Cache-Control: no-store\r\n"
        "Pragma: no-cache\r\n"
        "Expires: 0\r\n"
        "\r\n"
        "{\"state\":\"%s\",\"message\":\"%s\"}\n",
        HTTP_SERVER_HEADER,
        sync_state_name(current_state),
        current_message
    );
    if ((response_len < 0) || (response_len >= STATUS_RESPONSE_BUFFER_SIZE)) {
        mem_free(response);
        file->data = STATUS_ERROR_RESPONSE;
        file->len = (int)strlen(STATUS_ERROR_RESPONSE);
        file->index = file->len;
        file->flags = FS_FILE_FLAGS_HEADER_INCLUDED | FS_FILE_FLAGS_HEADER_PERSISTENT;
        return 1;
    }

    file->pextension = response;
    file->data = response;
    file->len = response_len;
    file->index = file->len;
    file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;

    return 1;
}

void fs_close_custom(struct fs_file *file) {
    if ((file != NULL) && (file->pextension != NULL)) {
        mem_free(file->pextension);
        file->pextension = NULL;
    }
}

int main() {
    stdio_init_all();
    wait_for_optional_usb_serial();
    printf("=== STDIO boot okay\n");
    sync_initialize();

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
    http_set_cgi_handlers(cgi_handlers, LWIP_ARRAYSIZE(cgi_handlers)); /* Set CGI request handlers */
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
        sync_poll_state_machine();
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(100));
    }
    
    dnserv_free();
    dhcp_server_deinit(&dhcp);

    return 1;
}
