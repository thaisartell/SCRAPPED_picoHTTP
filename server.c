/* Mental Model:
CYW43 config : Wi-Fi network exists
DHCP server : clients obtain IP addresses
lwIP TCP/IP : handles packet transport
HTTP server : handles browser requests
Code : generates page behavior */

/* Standard library imports */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/regs/addressmap.h"
#include "hardware/sync.h"

/* lwIP library imports */
#include "lwip/apps/fs.h"
#include "lwip/apps/httpd.h"
#include "lwip/def.h"
#include "lwip/ip4_addr.h"
#include "lwip/mem.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"

/* Custom imports */
#include "lwipopts.h"
#include "dhcpserver.h"
#include "dnserver.h"

extern char __flash_binary_end;

static const char WIFI_SSID[] = "nasPico";
static const char WIFI_PASSWORD[] = "LuBrRaJaTh";

/* ---------------- Network ---------------- */
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

/* ---------------- GPIO ---------------- */
#define PI_STATUS_INPUT_GPIO 18
#define PI_POWER_ENABLE_GPIO 26

/* ---------------- Pi Status ----------------
Tolerance: 2ms
Downstream Pi leaves the line low for 25ms between pulses.
Alive: 10ms
Complete: 15ms
SyncThing error: 30ms
System error: 35ms */
#define PI_STATUS_INTER_PULSE_LOW_US 25000
#define PI_STATUS_ALIVE_MIN_US 8000
#define PI_STATUS_ALIVE_MAX_US 12000
#define PI_STATUS_COMPLETE_MIN_US 13000
#define PI_STATUS_COMPLETE_MAX_US 17000
#define PI_STATUS_SYNCTHING_ERROR_MIN_US 28000
#define PI_STATUS_SYNCTHING_ERROR_MAX_US 32000
#define PI_STATUS_SYSTEM_ERROR_MIN_US 33000
#define PI_STATUS_SYSTEM_ERROR_MAX_US 37000
#define SYNC_RESULT_DISPLAY_DELAY_MS 2000
#define PI_READY_TIMEOUT_MS (10 * 60 * 1000)

/* ---------------- General ---------------- */
#define STATUS_RESPONSE_BUFFER_SIZE 2048
#define SCHEDULE_RESPONSE_BUFFER_SIZE 256
#define LOCAL_TIME_BUFFER_SIZE 16
#define RESULT_TEXT_BUFFER_SIZE 24
#define RESULT_MESSAGE_BUFFER_SIZE 96
#define STDIO_USB_WAIT_TIMEOUT_MS 3000
#define BACKUP_WINDOW_TIMEOUT_MS (30 * 60 * 1000)
#define SCHEDULE_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define SCHEDULE_STORAGE_MAGIC 0x53434844u
#define SCHEDULE_STORAGE_VERSION 1u

typedef enum {
    SYNC_STATE_IDLE,
    SYNC_STATE_WAITING_READY,
    SYNC_STATE_TRANSFERRING,
    SYNC_STATE_SUCCESS,
    SYNC_STATE_ERROR,
    SYNC_STATE_TIMEOUT,
} sync_state_t;

typedef enum {
    PI_STATUS_NONE,
    PI_STATUS_ALIVE,
    PI_STATUS_COMPLETE,
    PI_STATUS_SYNCTHING_ERROR,
    PI_STATUS_SYSTEM_ERROR,
} pi_status_t;

typedef struct {
    uint8_t enabled;
    uint8_t hour;
    uint8_t minute;
} backup_schedule_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint8_t enabled;
    uint8_t hour;
    uint8_t minute;
    uint8_t reserved[FLASH_PAGE_SIZE - sizeof(uint32_t) - sizeof(uint16_t) - (3 * sizeof(uint8_t)) - sizeof(uint32_t)];
    uint32_t checksum;
} persisted_schedule_t;

_Static_assert(sizeof(persisted_schedule_t) == FLASH_PAGE_SIZE, "schedule persistence must fit one flash page");

static sync_state_t sync_state = SYNC_STATE_IDLE;
static char sync_status_message[64];
static volatile uint64_t pi_status_rise_us = 0;
static volatile uint32_t pi_status_width_us = 0;
static volatile int pi_status_ready = 0;
static volatile pi_status_t pi_decoded_status = PI_STATUS_NONE;
static volatile uint32_t pi_status_irq_count = 0;
static volatile uint32_t pi_status_rise_count = 0;
static volatile uint32_t pi_status_fall_count = 0;
static volatile uint32_t pi_status_decoded_count = 0;
static volatile uint32_t pi_status_ignored_width_count = 0;
static volatile uint32_t pi_status_no_rise_count = 0;
static volatile uint32_t pi_status_last_width_us = 0;
static volatile pi_status_t pi_status_last_decoded = PI_STATUS_NONE;
static volatile int pi_power_output_enabled = 0;
static volatile int automatic_power_off_enabled = 1;
static volatile int pi_ready_for_sync = 0;
static volatile int sync_run_active = 0;
static volatile int completed_power_hold_active = 0;

static backup_schedule_t backup_schedule = {0, 2, 0};
static int schedule_persistence_available = 0;
static int scheduler_clock_valid = 0;
static int64_t scheduler_clock_local_epoch_sec = 0;
static uint64_t scheduler_clock_uptime_sec = 0;
static int32_t last_scheduled_day = INT32_MIN;
static int last_scheduled_minute = -1;
static char last_backup_result[RESULT_TEXT_BUFFER_SIZE] = "NEVER";
static char last_backup_message[RESULT_MESSAGE_BUFFER_SIZE] = "No scheduled backup has completed yet";

/* Timestamp (ms) until sync state transition */
static absolute_time_t sync_next_transition_at;
static absolute_time_t completed_power_hold_until;

static const char HTTP_SERVER_HEADER[] = "Server: lwIP/pre-0.6 (http://www.sics.se/~adam/lwip/)\r\n";
static const char JSON_RESPONSE_HEADER[] =
    "HTTP/1.0 200 OK\r\n"
    "Server: lwIP/pre-0.6 (http://www.sics.se/~adam/lwip/)\r\n"
    "Content-Type: application/json\r\n"
    "Cache-Control: no-store\r\n"
    "Pragma: no-cache\r\n"
    "Expires: 0\r\n"
    "\r\n";
static const char PAGE_REDIRECT_RESPONSE[] =
    "HTTP/1.0 302 Found\r\n"
    "Server: lwIP/pre-0.6 (http://www.sics.se/~adam/lwip/)\r\n"
    "Location: /index.shtml\r\n"
    "Cache-Control: no-store\r\n"
    "Pragma: no-cache\r\n"
    "Expires: 0\r\n"
    "Content-Length: 0\r\n"
    "\r\n";
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

static const char *pi_status_name(pi_status_t status) {
    switch (status) {
        case PI_STATUS_ALIVE:
            return "ALIVE";
        case PI_STATUS_COMPLETE:
            return "COMPLETE";
        case PI_STATUS_SYNCTHING_ERROR:
            return "SYNCTHING_ERROR";
        case PI_STATUS_SYSTEM_ERROR:
            return "SYSTEM_ERROR";
        case PI_STATUS_NONE:
        default:
            return "NONE";
    }
}

static const char *sync_state_name(sync_state_t state) {
    switch (state) {
        case SYNC_STATE_IDLE:
            return "IDLE";
        case SYNC_STATE_WAITING_READY:
            return "WAITING_READY";
        case SYNC_STATE_TRANSFERRING:
            return "TRANSFERRING";
        case SYNC_STATE_SUCCESS:
            return "SUCCESS";
        case SYNC_STATE_ERROR:
            return "ERROR";
        case SYNC_STATE_TIMEOUT:
            return "TIMEOUT";
        default:
            return "ERROR";
    }
}

static const char *gpio_output_state_name(int enabled) {
    if (enabled) {
        return "ON";
    }

    return "OFF";
}

static const char *gpio_level_name(int level) {
    if (level) {
        return "HIGH";
    }

    return "LOW";
}

static const char *json_bool_name(int enabled) {
    if (enabled) {
        return "true";
    }

    return "false";
}

static void format_schedule_time(int hour, int minute, char *buffer, size_t buffer_size) {
    snprintf(buffer, buffer_size, "%02d:%02d", hour, minute);
}

static int32_t local_day_number_from_epoch(int64_t local_epoch_sec) {
    int64_t day_number = local_epoch_sec / 86400;

    if ((local_epoch_sec < 0) && ((local_epoch_sec % 86400) != 0)) {
        day_number -= 1;
    }

    if (day_number < INT32_MIN) {
        return INT32_MIN;
    }
    if (day_number > INT32_MAX) {
        return INT32_MAX;
    }

    return (int32_t)day_number;
}

static int local_minute_of_day_from_epoch(int64_t local_epoch_sec) {
    int64_t second_of_day = local_epoch_sec % 86400;

    if (second_of_day < 0) {
        second_of_day += 86400;
    }

    return (int)(second_of_day / 60);
}

static void format_local_time_hhmm(int64_t local_epoch_sec, char *buffer, size_t buffer_size) {
    int minute_of_day = local_minute_of_day_from_epoch(local_epoch_sec);
    int hour = minute_of_day / 60;
    int minute = minute_of_day % 60;

    format_schedule_time(hour, minute, buffer, buffer_size);
}

static uint32_t schedule_storage_checksum(const persisted_schedule_t *storage) {
    const uint8_t *bytes = (const uint8_t *)storage;
    uint32_t checksum = 2166136261u;
    size_t index;

    for (index = 0; index < offsetof(persisted_schedule_t, checksum); ++index) {
        checksum ^= bytes[index];
        checksum *= 16777619u;
    }

    return checksum;
}

static void backup_schedule_set_defaults(backup_schedule_t *schedule) {
    schedule->enabled = 0;
    schedule->hour = 2;
    schedule->minute = 0;
}

static int schedule_storage_is_valid(const persisted_schedule_t *storage) {
    if (storage->magic != SCHEDULE_STORAGE_MAGIC) {
        return 0;
    }
    if (storage->version != SCHEDULE_STORAGE_VERSION) {
        return 0;
    }
    if (storage->enabled > 1) {
        return 0;
    }
    if (storage->hour > 23) {
        return 0;
    }
    if (storage->minute > 59) {
        return 0;
    }

    return storage->checksum == schedule_storage_checksum(storage);
}

static void last_backup_set(const char *result, const char *message) {
    uint32_t interrupt_state = save_and_disable_interrupts();

    snprintf(last_backup_result, sizeof(last_backup_result), "%s", result);
    snprintf(last_backup_message, sizeof(last_backup_message), "%s", message);

    restore_interrupts(interrupt_state);
}

static void last_backup_get_snapshot(char *result, size_t result_size, char *message, size_t message_size) {
    uint32_t interrupt_state = save_and_disable_interrupts();

    snprintf(result, result_size, "%s", last_backup_result);
    snprintf(message, message_size, "%s", last_backup_message);

    restore_interrupts(interrupt_state);
}

static void scheduler_clock_initialize(void) {
    uint32_t interrupt_state = save_and_disable_interrupts();

    scheduler_clock_valid = 0;
    scheduler_clock_local_epoch_sec = 0;
    scheduler_clock_uptime_sec = 0;

    restore_interrupts(interrupt_state);
}

static void scheduler_clock_set_local_epoch(int64_t local_epoch_sec) {
    char current_time_text[LOCAL_TIME_BUFFER_SIZE];
    uint32_t interrupt_state = save_and_disable_interrupts();

    scheduler_clock_valid = 1;
    scheduler_clock_local_epoch_sec = local_epoch_sec;
    scheduler_clock_uptime_sec = time_us_64() / 1000000u;

    restore_interrupts(interrupt_state);

    format_local_time_hhmm(local_epoch_sec, current_time_text, sizeof(current_time_text));
    printf("scheduler clock set -> %s local time\n", current_time_text);
}

static int scheduler_clock_get_local_epoch(int64_t *local_epoch_sec) {
    int valid;
    int64_t base_epoch_sec;
    uint64_t base_uptime_sec;
    uint64_t current_uptime_sec = time_us_64() / 1000000u;
    uint32_t interrupt_state = save_and_disable_interrupts();

    valid = scheduler_clock_valid;
    base_epoch_sec = scheduler_clock_local_epoch_sec;
    base_uptime_sec = scheduler_clock_uptime_sec;

    restore_interrupts(interrupt_state);

    if (!valid) {
        return 0;
    }

    *local_epoch_sec = base_epoch_sec + (int64_t)(current_uptime_sec - base_uptime_sec);
    return 1;
}

static void schedule_reset_last_trigger_marker(void) {
    last_scheduled_day = INT32_MIN;
    last_scheduled_minute = -1;
}

static void backup_schedule_get_snapshot(int *enabled, int *hour, int *minute) {
    uint32_t interrupt_state = save_and_disable_interrupts();

    *enabled = backup_schedule.enabled ? 1 : 0;
    *hour = backup_schedule.hour;
    *minute = backup_schedule.minute;

    restore_interrupts(interrupt_state);
}

static void backup_schedule_configure_storage(void) {
    uintptr_t binary_end_offset = (uintptr_t)&__flash_binary_end - XIP_BASE;

    schedule_persistence_available = binary_end_offset < SCHEDULE_FLASH_OFFSET;
    if (schedule_persistence_available) {
        printf("schedule persistence enabled at flash offset 0x%08x\n", SCHEDULE_FLASH_OFFSET);
    } else {
        printf("schedule persistence unavailable: binary end overlaps storage sector\n");
    }
}

static void backup_schedule_load(void) {
    backup_schedule_t loaded_schedule;
    const persisted_schedule_t *storage;
    char schedule_text[LOCAL_TIME_BUFFER_SIZE];

    backup_schedule_set_defaults(&loaded_schedule);
    if (!schedule_persistence_available) {
        backup_schedule = loaded_schedule;
        return;
    }

    storage = (const persisted_schedule_t *)(XIP_BASE + SCHEDULE_FLASH_OFFSET);
    if (schedule_storage_is_valid(storage)) {
        loaded_schedule.enabled = storage->enabled;
        loaded_schedule.hour = storage->hour;
        loaded_schedule.minute = storage->minute;
        format_schedule_time(loaded_schedule.hour, loaded_schedule.minute, schedule_text, sizeof(schedule_text));
        printf(
            "loaded persisted schedule -> %s at %s\n",
            loaded_schedule.enabled ? "enabled" : "disabled",
            schedule_text
        );
    } else {
        printf("no valid persisted schedule found; using defaults\n");
    }

    backup_schedule = loaded_schedule;
}

static void backup_schedule_persist(void) {
    persisted_schedule_t storage;
    uint32_t interrupt_state;

    if (!schedule_persistence_available) {
        return;
    }

    memset(&storage, 0xff, sizeof(storage));
    storage.magic = SCHEDULE_STORAGE_MAGIC;
    storage.version = SCHEDULE_STORAGE_VERSION;
    storage.enabled = backup_schedule.enabled ? 1 : 0;
    storage.hour = backup_schedule.hour;
    storage.minute = backup_schedule.minute;
    storage.checksum = schedule_storage_checksum(&storage);

    interrupt_state = save_and_disable_interrupts();
    flash_range_erase(SCHEDULE_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SCHEDULE_FLASH_OFFSET, (const uint8_t *)&storage, FLASH_PAGE_SIZE);
    restore_interrupts(interrupt_state);
}

static void backup_schedule_save(int enabled, int hour, int minute) {
    char schedule_text[LOCAL_TIME_BUFFER_SIZE];
    uint32_t interrupt_state = save_and_disable_interrupts();

    backup_schedule.enabled = enabled ? 1 : 0;
    backup_schedule.hour = (uint8_t)hour;
    backup_schedule.minute = (uint8_t)minute;

    restore_interrupts(interrupt_state);

    schedule_reset_last_trigger_marker();
    backup_schedule_persist();
    format_schedule_time(hour, minute, schedule_text, sizeof(schedule_text));
    printf("schedule saved -> %s at %s\n", enabled ? "enabled" : "disabled", schedule_text);
}

static int pi_power_output_get_level(void) {
    return gpio_get(PI_POWER_ENABLE_GPIO) ? 1 : 0;
}

static void pi_power_output_initialize(void) {
    gpio_init(PI_POWER_ENABLE_GPIO);
    gpio_disable_pulls(PI_POWER_ENABLE_GPIO);
    gpio_set_dir(PI_POWER_ENABLE_GPIO, GPIO_OUT);
    gpio_put(PI_POWER_ENABLE_GPIO, 0);
}

static void pi_status_reset_snapshot(void) {
    pi_status_rise_us = 0;
    pi_status_width_us = 0;
    pi_status_ready = 0;
    pi_decoded_status = PI_STATUS_NONE;
}

static void pi_power_output_set_enabled(int enabled) {
    uint32_t interrupt_state = save_and_disable_interrupts();
    int output_level;

    pi_status_reset_snapshot();
    pi_ready_for_sync = 0;
    if (!enabled) {
        completed_power_hold_active = 0;
        completed_power_hold_until = at_the_end_of_time;
    }
    gpio_put(PI_POWER_ENABLE_GPIO, enabled ? 1 : 0);
    pi_power_output_enabled = enabled ? 1 : 0;

    restore_interrupts(interrupt_state);
    output_level = pi_power_output_get_level();

    printf(
        "GPIO %d power output request -> %s, pad readback -> %s\n",
        PI_POWER_ENABLE_GPIO,
        enabled ? "ON (driving 3V3)" : "OFF",
        gpio_level_name(output_level)
    );
}

static int pi_power_output_get_enabled(void) {
    int enabled;
    uint32_t interrupt_state = save_and_disable_interrupts();

    enabled = pi_power_output_enabled;

    restore_interrupts(interrupt_state);
    return enabled;
}

static void automatic_power_off_set_enabled(int enabled) {
    uint32_t interrupt_state = save_and_disable_interrupts();

    automatic_power_off_enabled = enabled ? 1 : 0;

    restore_interrupts(interrupt_state);
    printf("automatic power off after COMPLETE -> %s\n", enabled ? "enabled" : "disabled");
}

static int automatic_power_off_get_enabled(void) {
    int enabled;
    uint32_t interrupt_state = save_and_disable_interrupts();

    enabled = automatic_power_off_enabled;

    restore_interrupts(interrupt_state);
    return enabled;
}

static void pi_ready_for_sync_set(int enabled) {
    uint32_t interrupt_state = save_and_disable_interrupts();

    pi_ready_for_sync = enabled ? 1 : 0;

    restore_interrupts(interrupt_state);
}

static int pi_ready_for_sync_get(void) {
    int enabled;
    uint32_t interrupt_state = save_and_disable_interrupts();

    enabled = pi_ready_for_sync;

    restore_interrupts(interrupt_state);
    return enabled;
}

/* Decide Pi pulse statuses by width */
static pi_status_t decode_pi_status_from_pulse_width(uint64_t width_us) {
    if ((width_us >= PI_STATUS_ALIVE_MIN_US) && (width_us <= PI_STATUS_ALIVE_MAX_US)) {
        return PI_STATUS_ALIVE;
    }
    if ((width_us >= PI_STATUS_COMPLETE_MIN_US) && (width_us <= PI_STATUS_COMPLETE_MAX_US)) {
        return PI_STATUS_COMPLETE;
    }
    if ((width_us >= PI_STATUS_SYNCTHING_ERROR_MIN_US) && (width_us <= PI_STATUS_SYNCTHING_ERROR_MAX_US)) {
        return PI_STATUS_SYNCTHING_ERROR;
    }
    if ((width_us >= PI_STATUS_SYSTEM_ERROR_MIN_US) && (width_us <= PI_STATUS_SYSTEM_ERROR_MAX_US)) {
        return PI_STATUS_SYSTEM_ERROR;
    }

    return PI_STATUS_NONE;
}

/*
Occurs on GPIO27 interrupt.
Pulse RISE: captures rising edge time.
Pulse FALL: compares falling edge time against old rising edge time. Sends captured pulse length to be decoded.
*/
static void pi_status_gpio_irq_handler(uint gpio, uint32_t events) {
    uint64_t now = time_us_64();

    pi_status_irq_count++;

    if (gpio != PI_STATUS_INPUT_GPIO) {
        return;
    }

    if (events & GPIO_IRQ_EDGE_RISE) {
        pi_status_rise_count++;
        pi_status_rise_us = now;
    }

    if (events & GPIO_IRQ_EDGE_FALL) {
        uint64_t width_us;
        pi_status_t decoded_status;

        pi_status_fall_count++;
        if (pi_status_rise_us == 0) {
            pi_status_no_rise_count++;
            return;
        }

        width_us = now - pi_status_rise_us;
        pi_status_last_width_us = (uint32_t)width_us;
        decoded_status = decode_pi_status_from_pulse_width(width_us);
        pi_status_last_decoded = decoded_status;
        if (decoded_status == PI_STATUS_NONE) {
            pi_status_ignored_width_count++;
            return;
        }

        pi_status_width_us = (uint32_t)width_us;
        pi_decoded_status = decoded_status;
        pi_status_decoded_count++;
        pi_status_ready = 1;
    }
}

static void sync_initialize(void) {
    uint32_t interrupt_state = save_and_disable_interrupts();

    sync_state = SYNC_STATE_IDLE;
    snprintf(sync_status_message, sizeof(sync_status_message), "%s", "Ready");
    sync_next_transition_at = at_the_end_of_time;
    completed_power_hold_until = at_the_end_of_time;
    pi_status_reset_snapshot();
    pi_power_output_enabled = 0;
    automatic_power_off_enabled = 1;
    pi_ready_for_sync = 0;
    sync_run_active = 0;
    completed_power_hold_active = 0;

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

static int completed_power_hold_get_active(void);

static void sync_set_idle_wait_message(void) {
    if (!pi_power_output_get_enabled()) {
        sync_set_state(SYNC_STATE_IDLE, "Power control output is OFF", at_the_end_of_time);
        return;
    }

    if (completed_power_hold_get_active()) {
        sync_set_state(SYNC_STATE_IDLE, "Backup complete; power left on", at_the_end_of_time);
        return;
    }

    if (!pi_ready_for_sync_get()) {
        sync_set_state(SYNC_STATE_IDLE, "Waiting for Raspberry Pi ready status", at_the_end_of_time);
        return;
    }

    sync_set_state(SYNC_STATE_IDLE, "Raspberry Pi is powered and ready", at_the_end_of_time);
}

static void sync_get_snapshot(sync_state_t *state, char *message, size_t message_size) {
    uint32_t interrupt_state = save_and_disable_interrupts();

    *state = sync_state;
    snprintf(message, message_size, "%s", sync_status_message);

    restore_interrupts(interrupt_state);
}

static int sync_get_state_value(void) {
    int state_value;
    uint32_t interrupt_state = save_and_disable_interrupts();

    state_value = (int)sync_state;

    restore_interrupts(interrupt_state);
    return state_value;
}

static void sync_set_run_active(int active) {
    uint32_t interrupt_state = save_and_disable_interrupts();

    sync_run_active = active ? 1 : 0;

    restore_interrupts(interrupt_state);
}

static int sync_is_run_active(void) {
    int active;
    uint32_t interrupt_state = save_and_disable_interrupts();

    active = sync_run_active;

    restore_interrupts(interrupt_state);
    return active;
}

static void completed_power_hold_start(absolute_time_t hold_until) {
    if (is_at_the_end_of_time(hold_until) || time_reached(hold_until)) {
        hold_until = make_timeout_time_ms(BACKUP_WINDOW_TIMEOUT_MS);
    }

    uint32_t interrupt_state = save_and_disable_interrupts();

    completed_power_hold_active = 1;
    completed_power_hold_until = hold_until;

    restore_interrupts(interrupt_state);
}

static void completed_power_hold_clear(void) {
    uint32_t interrupt_state = save_and_disable_interrupts();

    completed_power_hold_active = 0;
    completed_power_hold_until = at_the_end_of_time;

    restore_interrupts(interrupt_state);
}

static int completed_power_hold_get_active(void) {
    int active;
    uint32_t interrupt_state = save_and_disable_interrupts();

    active = completed_power_hold_active;

    restore_interrupts(interrupt_state);
    return active;
}

static int sync_start_backup_run(const char *message) {
    sync_state_t current_state;
    uint32_t interrupt_state = save_and_disable_interrupts();

    current_state = sync_state;
    restore_interrupts(interrupt_state);

    if (current_state != SYNC_STATE_IDLE) {
        printf("backup start ignored: %s\n", sync_state_name(current_state));
        return 0;
    }

    completed_power_hold_clear();
    sync_set_run_active(1);

    if (!pi_power_output_get_enabled()) {
        pi_power_output_set_enabled(1);
    }

    sync_set_state(SYNC_STATE_TRANSFERRING, message, make_timeout_time_ms(BACKUP_WINDOW_TIMEOUT_MS));
    return 1;
}

static void sync_finish_run(sync_state_t final_state, const char *state_message, const char *result, const char *result_message, int power_off_after_run) {
    absolute_time_t power_hold_until;
    uint32_t interrupt_state = save_and_disable_interrupts();

    power_hold_until = sync_next_transition_at;
    restore_interrupts(interrupt_state);

    sync_set_run_active(0);
    if (power_off_after_run) {
        pi_power_output_set_enabled(0);
    } else {
        completed_power_hold_start(power_hold_until);
    }

    last_backup_set(result, result_message);
    sync_set_state(final_state, state_message, make_timeout_time_ms(SYNC_RESULT_DISPLAY_DELAY_MS));
}

static void sync_cancel_run(const char *reason) {
    if (!sync_is_run_active()) {
        return;
    }

    sync_finish_run(SYNC_STATE_ERROR, reason, "CANCELLED", reason, 1);
}

/* Pi communication initialization:
    Configures GPIO pin selected by PI_STATUS_INPUT_GPIO
    Pin configuration: input, active high (pull-down),
        assigns handler function for pin interrupt events
*/
static void pi_status_input_initialize(void) {
    gpio_set_irq_enabled(PI_STATUS_INPUT_GPIO, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);
    gpio_init(PI_STATUS_INPUT_GPIO);
    gpio_set_dir(PI_STATUS_INPUT_GPIO, GPIO_IN);
    gpio_pull_down(PI_STATUS_INPUT_GPIO);
    gpio_set_irq_enabled_with_callback(
        PI_STATUS_INPUT_GPIO,
        GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL,
        true,
        &pi_status_gpio_irq_handler
    );
}

static int pi_status_take_snapshot(pi_status_t *status, uint32_t *width_us) {
    uint32_t interrupt_state = save_and_disable_interrupts();

    if (!pi_status_ready) {
        restore_interrupts(interrupt_state);
        return 0;
    }

    *status = pi_decoded_status;
    *width_us = pi_status_width_us;
    pi_decoded_status = PI_STATUS_NONE;
    pi_status_width_us = 0;
    pi_status_ready = 0;

    restore_interrupts(interrupt_state);
    return 1;
}

static void sync_request(void) {
    sync_start_backup_run("Manual backup started");
}

static void sync_process_pi_status(void) {
    pi_status_t pi_status;
    sync_state_t current_state;
    uint32_t pulse_width_us;
    uint32_t interrupt_state;
    int power_output_enabled;
    int power_hold_active;

    if (!pi_status_take_snapshot(&pi_status, &pulse_width_us)) {
        return;
    }

    interrupt_state = save_and_disable_interrupts();
    current_state = sync_state;
    restore_interrupts(interrupt_state);
    power_output_enabled = pi_power_output_get_enabled();
    power_hold_active = completed_power_hold_get_active();

    printf("pi status -> %s (%u us)\n", pi_status_name(pi_status), pulse_width_us);

    switch (pi_status) {
        case PI_STATUS_ALIVE:
            if (!power_output_enabled) {
                pi_ready_for_sync_set(0);
                if (current_state == SYNC_STATE_IDLE) {
                    sync_set_idle_wait_message();
                }
                break;
            }
            pi_ready_for_sync_set(1);
            if (current_state == SYNC_STATE_IDLE) {
                if (power_hold_active) {
                    sync_set_idle_wait_message();
                    break;
                }
                sync_set_run_active(1);
                sync_set_state(
                    SYNC_STATE_TRANSFERRING,
                    "Backup in progress",
                    make_timeout_time_ms(BACKUP_WINDOW_TIMEOUT_MS)
                );
                break;
            }
            if (current_state != SYNC_STATE_WAITING_READY) {
                break;
            }
            sync_set_state(
                SYNC_STATE_TRANSFERRING,
                "Backup in progress",
                make_timeout_time_ms(BACKUP_WINDOW_TIMEOUT_MS)
            );
            break;
        case PI_STATUS_COMPLETE:
        {
            int power_off_after_complete;

            if (!power_output_enabled) {
                break;
            }
            if (power_hold_active) {
                break;
            }
            if (current_state != SYNC_STATE_TRANSFERRING) {
                printf(
                    "pi status COMPLETE ignored while sync state is %s; no active transfer\n",
                    sync_state_name(current_state)
                );
                break;
            }

            power_off_after_complete = automatic_power_off_get_enabled();

            sync_finish_run(
                SYNC_STATE_SUCCESS,
                power_off_after_complete ? "Backup completed successfully; power removed" : "Backup complete; power left on",
                "SUCCESS",
                power_off_after_complete
                    ? "Raspberry Pi reported backup complete"
                    : "Raspberry Pi reported backup complete; automatic power-off disabled",
                power_off_after_complete
            );
            break;
        }
        case PI_STATUS_SYNCTHING_ERROR:
            pi_ready_for_sync_set(0);
            if (!power_output_enabled) {
                break;
            }
            if ((current_state != SYNC_STATE_IDLE) &&
                (current_state != SYNC_STATE_WAITING_READY) &&
                (current_state != SYNC_STATE_TRANSFERRING)) {
                break;
            }
            sync_finish_run(
                SYNC_STATE_ERROR,
                "Backup failed: SyncThing error",
                "SYNCTHING_ERROR",
                "Raspberry Pi reported a SyncThing error",
                1
            );
            break;
        case PI_STATUS_SYSTEM_ERROR:
            pi_ready_for_sync_set(0);
            if (!power_output_enabled) {
                break;
            }
            if ((current_state != SYNC_STATE_IDLE) &&
                (current_state != SYNC_STATE_WAITING_READY) &&
                (current_state != SYNC_STATE_TRANSFERRING)) {
                break;
            }
            sync_finish_run(
                SYNC_STATE_ERROR,
                "Backup failed: system error",
                "SYSTEM_ERROR",
                "Raspberry Pi reported a system error",
                1
            );
            break;
        case PI_STATUS_NONE:
        default:
            break;
    }
}

static void scheduled_backup_poll(void) {
    int enabled;
    int hour;
    int minute;
    int64_t local_epoch_sec;
    int32_t current_day;
    int current_minute;
    int scheduled_minute;

    backup_schedule_get_snapshot(&enabled, &hour, &minute);
    if (!enabled) {
        return;
    }
    if (!scheduler_clock_get_local_epoch(&local_epoch_sec)) {
        return;
    }

    current_day = local_day_number_from_epoch(local_epoch_sec);
    current_minute = local_minute_of_day_from_epoch(local_epoch_sec);
    scheduled_minute = (hour * 60) + minute;

    if ((current_day == last_scheduled_day) && (current_minute == last_scheduled_minute)) {
        return;
    }
    if (current_minute != scheduled_minute) {
        return;
    }

    last_scheduled_day = current_day;
    last_scheduled_minute = current_minute;

    if (sync_get_state_value() != SYNC_STATE_IDLE) {
        last_backup_set("SKIPPED", "Scheduled minute arrived while another backup state was active");
        return;
    }
    if (pi_power_output_get_enabled()) {
        last_backup_set("SKIPPED", "Scheduled minute arrived while manual GPIO power was already ON");
        return;
    }

    if (!sync_start_backup_run("Scheduled backup started")) {
        last_backup_set("SKIPPED", "Scheduled backup could not start");
    }
}

static void completed_power_hold_poll(void) {
    int hold_active;
    absolute_time_t hold_until;
    uint32_t interrupt_state = save_and_disable_interrupts();

    hold_active = completed_power_hold_active;
    hold_until = completed_power_hold_until;

    restore_interrupts(interrupt_state);

    if (!hold_active || !time_reached(hold_until)) {
        return;
    }

    pi_power_output_set_enabled(0);
    sync_set_state(SYNC_STATE_IDLE, "Completed backup power hold expired; power removed", at_the_end_of_time);
}

static void sync_poll_state_machine(void) {
    sync_state_t current_state;
    absolute_time_t next_transition_at;
    const char *timeout_message;

    sync_process_pi_status();

    uint32_t interrupt_state = save_and_disable_interrupts();
    current_state = sync_state;
    next_transition_at = sync_next_transition_at;
    restore_interrupts(interrupt_state);

    if (current_state == SYNC_STATE_IDLE) {
        completed_power_hold_poll();
        return;
    }

    if (!time_reached(next_transition_at)) {
        completed_power_hold_poll();
        return;
    }

    if (current_state == SYNC_STATE_WAITING_READY) {
        timeout_message = "Timed out waiting for Raspberry Pi ready";
        sync_finish_run(SYNC_STATE_TIMEOUT, timeout_message, "TIMEOUT", timeout_message, 1);
        return;
    }

    if (current_state == SYNC_STATE_TRANSFERRING) {
        timeout_message = "Backup window timed out";
        sync_finish_run(SYNC_STATE_TIMEOUT, timeout_message, "TIMEOUT", timeout_message, 1);
        return;
    }

    if ((current_state == SYNC_STATE_SUCCESS) || (current_state == SYNC_STATE_ERROR) || (current_state == SYNC_STATE_TIMEOUT)) {
        sync_set_idle_wait_message();
    }

    completed_power_hold_poll();
}

static const char *find_query_value(const char *name, int iNumParams, char *pcParam[], char *pcValue[]) {
    int index;

    for (index = 0; index < iNumParams; ++index) {
        if (strcmp(pcParam[index], name) == 0) {
            return pcValue[index];
        }
    }

    return NULL;
}

static int parse_int_query_value(const char *value, int min_value, int max_value, int *parsed_value) {
    char *end_ptr;
    long parsed_long;

    if (value == NULL) {
        return 0;
    }

    parsed_long = strtol(value, &end_ptr, 10);
    if ((*value == '\0') || (*end_ptr != '\0')) {
        return 0;
    }
    if ((parsed_long < min_value) || (parsed_long > max_value)) {
        return 0;
    }

    *parsed_value = (int)parsed_long;
    return 1;
}

static int parse_int64_query_value(const char *value, int64_t *parsed_value) {
    char *end_ptr;
    long long parsed_long_long;

    if (value == NULL) {
        return 0;
    }

    parsed_long_long = strtoll(value, &end_ptr, 10);
    if ((*value == '\0') || (*end_ptr != '\0')) {
        return 0;
    }

    *parsed_value = (int64_t)parsed_long_long;
    return 1;
}

static const char *sync_request_cgi_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]) {
    (void)iIndex;
    (void)iNumParams;
    (void)pcParam;
    (void)pcValue;

    sync_request();
    return "/success.txt";
}

static const char *gpio_request_cgi_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]) {
    int next_output_enabled = -1;
    const char *requested_state = NULL;

    (void)iIndex;

    requested_state = find_query_value("state", iNumParams, pcParam, pcValue);
    if (requested_state != NULL) {
        if (strcmp(requested_state, "on") == 0) {
            next_output_enabled = 1;
        } else if (strcmp(requested_state, "off") == 0) {
            next_output_enabled = 0;
        } else if (strcmp(requested_state, "toggle") == 0) {
            next_output_enabled = !pi_power_output_get_enabled();
        }
    }

    if (next_output_enabled < 0) {
        next_output_enabled = !pi_power_output_get_enabled();
    }

    printf(
        "GPIO CGI request: state=%s -> %s\n",
        requested_state != NULL ? requested_state : "(default toggle)",
        gpio_output_state_name(next_output_enabled)
    );

    if (next_output_enabled) {
        sync_start_backup_run("Manual backup started");
    } else if (sync_is_run_active()) {
        sync_cancel_run("Backup cancelled by manual GPIO power-off");
    } else {
        pi_power_output_set_enabled(0);
        sync_set_idle_wait_message();
    }

    return "/success.txt";
}

static const char *auto_power_off_request_cgi_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]) {
    const char *enabled_value;
    int enabled;

    (void)iIndex;

    enabled_value = find_query_value("enabled", iNumParams, pcParam, pcValue);
    if (!parse_int_query_value(enabled_value, 0, 1, &enabled)) {
        printf("auto power-off CGI request ignored: invalid params\n");
        return "/success.txt";
    }

    automatic_power_off_set_enabled(enabled);
    return "/success.txt";
}

static const char *time_request_cgi_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]) {
    const char *local_epoch_value;
    int64_t local_epoch_sec;

    (void)iIndex;

    local_epoch_value = find_query_value("localEpoch", iNumParams, pcParam, pcValue);
    if (parse_int64_query_value(local_epoch_value, &local_epoch_sec)) {
        scheduler_clock_set_local_epoch(local_epoch_sec);
    } else {
        printf("time CGI request ignored: invalid localEpoch value\n");
    }

    return "/success.txt";
}

static const char *schedule_request_cgi_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]) {
    const char *enabled_value;
    const char *hour_value;
    const char *minute_value;
    int enabled;
    int hour;
    int minute;

    (void)iIndex;

    enabled_value = find_query_value("enabled", iNumParams, pcParam, pcValue);
    hour_value = find_query_value("hour", iNumParams, pcParam, pcValue);
    minute_value = find_query_value("minute", iNumParams, pcParam, pcValue);

    if (!parse_int_query_value(enabled_value, 0, 1, &enabled) ||
        !parse_int_query_value(hour_value, 0, 23, &hour) ||
        !parse_int_query_value(minute_value, 0, 59, &minute)) {
        printf("schedule CGI request ignored: invalid params\n");
        return "/success.txt";
    }

    backup_schedule_save(enabled, hour, minute);
    return "/success.txt";
}

static const tCGI cgi_handlers[] = {
    {"/sync.cgi", sync_request_cgi_handler},
    {"/gpio.cgi", gpio_request_cgi_handler},
    {"/auto-power-off.cgi", auto_power_off_request_cgi_handler},
    {"/time.cgi", time_request_cgi_handler},
    {"/schedule.cgi", schedule_request_cgi_handler},
};

static int fs_open_json_response(struct fs_file *file, const char *json_body) {
    char *response;
    size_t header_length = strlen(JSON_RESPONSE_HEADER);
    size_t body_length = strlen(json_body);
    size_t response_length = header_length + body_length;

    response = mem_malloc(response_length + 1);
    if (response == NULL) {
        memset(file, 0, sizeof(struct fs_file));
        file->data = STATUS_ERROR_RESPONSE;
        file->len = (int)strlen(STATUS_ERROR_RESPONSE);
        file->index = file->len;
        file->flags = FS_FILE_FLAGS_HEADER_INCLUDED | FS_FILE_FLAGS_HEADER_PERSISTENT;
        return 1;
    }

    memcpy(response, JSON_RESPONSE_HEADER, header_length);
    memcpy(response + header_length, json_body, body_length);
    response[response_length] = '\0';

    memset(file, 0, sizeof(struct fs_file));
    file->pextension = response;
    file->data = response;
    file->len = (int)response_length;
    file->index = file->len;
    file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;
    return 1;
}

static int build_status_json(char *json_body, size_t json_body_size) {
    sync_state_t current_state;
    char current_message[sizeof(sync_status_message)];
    int gpio_output_enabled;
    int gpio_output_level;
    int status_input_level;
    int auto_power_off_enabled;
    int ready_for_sync;
    uint32_t status_irq_count;
    uint32_t status_rise_count;
    uint32_t status_fall_count;
    uint32_t status_decoded_count;
    uint32_t status_ignored_width_count;
    uint32_t status_no_rise_count;
    uint32_t status_last_width_us;
    pi_status_t status_last_decoded;
    int schedule_enabled;
    int schedule_hour;
    int schedule_minute;
    char schedule_time[LOCAL_TIME_BUFFER_SIZE];
    char current_time[LOCAL_TIME_BUFFER_SIZE];
    int clock_valid;
    int64_t current_local_epoch_sec;
    char last_result[RESULT_TEXT_BUFFER_SIZE];
    char last_message[RESULT_MESSAGE_BUFFER_SIZE];

    sync_get_snapshot(&current_state, current_message, sizeof(current_message));
    gpio_output_enabled = pi_power_output_get_enabled();
    gpio_output_level = pi_power_output_get_level();
    status_input_level = gpio_get(PI_STATUS_INPUT_GPIO) ? 1 : 0;
    auto_power_off_enabled = automatic_power_off_get_enabled();
    ready_for_sync = pi_ready_for_sync_get();
    uint32_t interrupt_state = save_and_disable_interrupts();
    status_irq_count = pi_status_irq_count;
    status_rise_count = pi_status_rise_count;
    status_fall_count = pi_status_fall_count;
    status_decoded_count = pi_status_decoded_count;
    status_ignored_width_count = pi_status_ignored_width_count;
    status_no_rise_count = pi_status_no_rise_count;
    status_last_width_us = pi_status_last_width_us;
    status_last_decoded = pi_status_last_decoded;
    restore_interrupts(interrupt_state);
    backup_schedule_get_snapshot(&schedule_enabled, &schedule_hour, &schedule_minute);
    format_schedule_time(schedule_hour, schedule_minute, schedule_time, sizeof(schedule_time));
    clock_valid = scheduler_clock_get_local_epoch(&current_local_epoch_sec);
    if (clock_valid) {
        format_local_time_hhmm(current_local_epoch_sec, current_time, sizeof(current_time));
    } else {
        snprintf(current_time, sizeof(current_time), "%s", "UNSET");
    }
    last_backup_get_snapshot(last_result, sizeof(last_result), last_message, sizeof(last_message));

    return snprintf(
        json_body,
        json_body_size,
        "{\"state\":\"%s\",\"message\":\"%s\",\"statusInputPin\":%d,\"statusInputLevel\":\"%s\",\"statusInputLevelValue\":%d,\"statusIrqCount\":%u,\"statusRiseCount\":%u,\"statusFallCount\":%u,\"statusDecodedCount\":%u,\"statusIgnoredWidthCount\":%u,\"statusNoRiseCount\":%u,\"statusLastPulseWidthUs\":%u,\"statusLastDecoded\":\"%s\",\"powerOutputPin\":%d,\"powerOutput\":\"%s\",\"powerOutputLevel\":\"%s\",\"powerOutputLevelValue\":%d,\"autoPowerOff\":%s,\"piReadyForSync\":%s,\"scheduleEnabled\":%s,\"scheduleHour\":%d,\"scheduleMinute\":%d,\"scheduleTime\":\"%s\",\"clockValid\":%s,\"currentLocalTime\":\"%s\",\"lastBackupResult\":\"%s\",\"lastBackupMessage\":\"%s\"}\n",
        sync_state_name(current_state),
        current_message,
        PI_STATUS_INPUT_GPIO,
        gpio_level_name(status_input_level),
        status_input_level,
        status_irq_count,
        status_rise_count,
        status_fall_count,
        status_decoded_count,
        status_ignored_width_count,
        status_no_rise_count,
        status_last_width_us,
        pi_status_name(status_last_decoded),
        PI_POWER_ENABLE_GPIO,
        gpio_output_state_name(gpio_output_enabled),
        gpio_level_name(gpio_output_level),
        gpio_output_level,
        json_bool_name(auto_power_off_enabled),
        json_bool_name(ready_for_sync),
        json_bool_name(schedule_enabled),
        schedule_hour,
        schedule_minute,
        schedule_time,
        json_bool_name(clock_valid),
        current_time,
        last_result,
        last_message
    );
}

static int build_schedule_json(char *json_body, size_t json_body_size) {
    int schedule_enabled;
    int schedule_hour;
    int schedule_minute;
    char schedule_time[LOCAL_TIME_BUFFER_SIZE];

    backup_schedule_get_snapshot(&schedule_enabled, &schedule_hour, &schedule_minute);
    format_schedule_time(schedule_hour, schedule_minute, schedule_time, sizeof(schedule_time));

    return snprintf(
        json_body,
        json_body_size,
        "{\"enabled\":%s,\"hour\":%d,\"minute\":%d,\"time\":\"%s\"}\n",
        json_bool_name(schedule_enabled),
        schedule_hour,
        schedule_minute,
        schedule_time
    );
}

/* Recognized by lwIP as hook via defining macro LWIP_HTTPD_CUSTOM_FILES
* Serves HTTP responses that are not stored in htmldata.c
*/
int fs_open_custom(struct fs_file *file, const char *name) {
    char json_body[STATUS_RESPONSE_BUFFER_SIZE];
    int json_length;

    if ((strcmp(name, "/hotspot-detect.html") == 0) ||
        (strcmp(name, "/site.shtml") == 0) ||
        (strcmp(name, "/library/test/success.html") == 0)) {
        memset(file, 0, sizeof(struct fs_file));
        file->data = PAGE_REDIRECT_RESPONSE;
        file->len = (int)strlen(PAGE_REDIRECT_RESPONSE);
        file->index = file->len;
        file->flags = FS_FILE_FLAGS_HEADER_INCLUDED | FS_FILE_FLAGS_HEADER_PERSISTENT;
        return 1;
    }

    if (strcmp(name, "/status") == 0) {
        json_length = build_status_json(json_body, sizeof(json_body));
        if ((json_length < 0) || ((size_t)json_length >= sizeof(json_body))) {
            return fs_open_json_response(file, "{\"state\":\"ERROR\",\"message\":\"Status unavailable\"}\n");
        }
        return fs_open_json_response(file, json_body);
    }

    if (strcmp(name, "/schedule") == 0) {
        json_length = build_schedule_json(json_body, SCHEDULE_RESPONSE_BUFFER_SIZE);
        if ((json_length < 0) || ((size_t)json_length >= SCHEDULE_RESPONSE_BUFFER_SIZE)) {
            return fs_open_json_response(file, "{\"enabled\":false,\"hour\":0,\"minute\":0,\"time\":\"00:00\"}\n");
        }
        return fs_open_json_response(file, json_body);
    }

    return 0;
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
    backup_schedule_configure_storage();
    backup_schedule_load();
    scheduler_clock_initialize();
    sync_initialize();
    pi_status_input_initialize();
    pi_power_output_initialize();
    schedule_reset_last_trigger_marker();
    sync_set_idle_wait_message();
    printf("=== Raspberry Pi status input listening on GPIO %d\n", PI_STATUS_INPUT_GPIO);
    printf("=== Raspberry Pi power control output ready on GPIO %d (default OFF)\n", PI_POWER_ENABLE_GPIO);

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
    dhcp_server_t dhcp; /* DHCP state object */
    ip_addr_t ip;
    ip_addr_t netmask;
    err_t dns_err;

    IP4_ADDR(&ip, AP_ADDR_0, AP_ADDR_1, AP_ADDR_2, AP_ADDR_3); /* Host (Pico) address */
    IP4_ADDR(&netmask, AP_MASK_0, AP_MASK_1, AP_MASK_2, AP_MASK_3);

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
    printf(
        "=== Connect to Wi-Fi '%s' and open http://%d.%d.%d.%d/\n",
        WIFI_SSID,
        AP_ADDR_0,
        AP_ADDR_1,
        AP_ADDR_2,
        AP_ADDR_3
    );

    /* Infinite driving loop */
    while (1) {
        sync_poll_state_machine();
        scheduled_backup_poll();
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(100));
    }

    dnserv_free();
    dhcp_server_deinit(&dhcp);

    return 1;
}
