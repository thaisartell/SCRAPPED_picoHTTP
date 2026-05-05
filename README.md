# picoHTTP Backup Controller

Firmware for a Raspberry Pi Pico 2 W that powers a downstream Raspberry Pi,
hosts a small Wi-Fi/web UI, and tracks a backup/sync run using pulse-width
status signals from the downstream Pi.

The Pico does not run the backup itself. Its job is to:

- expose a local access point and web UI,
- drive the downstream Pi power-enable line,
- decode status pulses from the downstream Pi,
- enforce ready/backup timeouts,
- optionally schedule daily backup starts,
- report current state and last result over HTTP.

## Hardware Interface

The firmware uses Pico GPIO numbers, not physical header pin numbers.

| Signal | Pico GPIO | Pico physical pin | Direction | Purpose |
| --- | ---: | ---: | --- | --- |
| Pi power enable | GPIO26 / GP26 | pin 31 | output | Drives high to enable power to the downstream Pi. |
| Pi status input | GPIO27 / GP27 | pin 32 | input | Receives active-high status pulses from the downstream Pi. |

The boards must share ground. The status signal must be 3.3 V logic.

GPIO27 is configured as an input with pull-down and interrupts on both rising
and falling edges. Pulse width is measured from rising edge to falling edge.

## Status Pulse Protocol

The downstream Pi is expected to pulse GPIO27 high using these widths:

| Status | High width accepted by firmware |
| --- | ---: |
| `ALIVE` | 8-12 ms |
| `COMPLETE` | 13-17 ms |
| `SYNCTHING_ERROR` | 28-32 ms |
| `SYSTEM_ERROR` | 33-37 ms |

The comments describe a 25 ms low gap between pulses, but the current firmware
does not enforce the inter-pulse low time. It only decodes high-pulse width.

The interrupt handler stores one decoded pulse snapshot at a time. The main
loop consumes that snapshot in `sync_process_pi_status()`. If the downstream Pi
sends multiple valid pulses faster than the main loop consumes them, the latest
decoded pulse can overwrite the previous one.

## Normal Backup Flow

The intended run flow is:

1. User starts a backup from the web UI, `/gpio.cgi?state=on`, `/sync.cgi`, or a
   scheduled backup fires.
2. Pico drives GPIO26 high.
3. Firmware immediately enters `TRANSFERRING`.
4. Downstream Pi boots and starts its own sync process.
5. Downstream Pi may send `ALIVE` pulses on GPIO27 while work is in progress.
6. Downstream Pi sends `COMPLETE` when sync succeeds.
7. Firmware records success.
8. If automatic power-off is enabled, GPIO26 is driven low immediately. If it is
    disabled, GPIO26 remains high until manual web UI power-off or the backup
    window expires.

Power-on is treated as sufficient evidence that a transfer has begun. `ALIVE`
pulses are useful status evidence, but they are not required before `COMPLETE`
can be accepted.

## Sync State Machine

The firmware state is stored in `sync_state` and exposed through `/status`.

| State | Meaning | Exit conditions |
| --- | --- | --- |
| `IDLE` | No active backup run. Message may still report GPIO26 state or that a completed backup is being held powered. | Start request, schedule trigger, or valid `ALIVE` while GPIO26 is high and not in completed-power hold. |
| `WAITING_READY` | Legacy state retained in code, but normal backup starts no longer enter it. | If reached unexpectedly, `ALIVE` -> `TRANSFERRING`; ready timeout -> `TIMEOUT`; manual power-off -> `ERROR`. |
| `TRANSFERRING` | GPIO26 has been driven high for a backup run, or an `ALIVE` pulse was seen while GPIO26 was already high. | `COMPLETE` -> `SUCCESS`; error pulse -> `ERROR`; 30-minute backup window -> `TIMEOUT`; manual power-off -> `ERROR`. |
| `SUCCESS` | A `COMPLETE` pulse was accepted after transfer had started. | After 2 seconds, returns to `IDLE` message state. |
| `ERROR` | Manual cancel or downstream error pulse. | After 2 seconds, returns to `IDLE` message state. |
| `TIMEOUT` | Ready or transfer timeout expired. | After 2 seconds, returns to `IDLE` message state. |

Important details:

- `ALIVE` pulses while already `TRANSFERRING` mark the Pi as ready but do not
  extend the 30-minute backup window.
- `SYNCTHING_ERROR` and `SYSTEM_ERROR` are accepted while GPIO26 is high and the
  state is `IDLE`, `WAITING_READY`, or `TRANSFERRING`.
- `COMPLETE` is ignored unless state is `TRANSFERRING`.
- If automatic power-off is disabled, `SUCCESS` leaves GPIO26 high, starts a
  completed-power hold, and then returns to `IDLE` after the result display
  delay. During completed-power hold, later `ALIVE` and `COMPLETE` pulses do not
  start or finish another run.

## Timeouts

| Timeout | Value | Starts when | Power behavior |
| --- | ---: | --- | --- |
| Ready timeout | Legacy only | Normal backup starts no longer enter `WAITING_READY`. | If `WAITING_READY` is reached unexpectedly and expires, state becomes `TIMEOUT` and GPIO26 is driven low. |
| Backup window | 30 minutes | Backup start enters `TRANSFERRING`. | On expiry, state becomes `TIMEOUT` and GPIO26 is driven low. |
| Result display delay | 2 seconds | `SUCCESS`, `ERROR`, or `TIMEOUT` is set. | State message returns to `IDLE` after delay. |
| Completed-power hold | Until the active backup window deadline, or 30 minutes fallback | `COMPLETE` is accepted while automatic power-off is disabled. | GPIO26 stays high until manual power-off or hold expiry. |

With the current design, there is no separate "wait for first GPIO27 activity"
timeout in the normal start path. GPIO26 can remain high with no `ALIVE` pulses
for the 30-minute backup window. A valid `COMPLETE` pulse during that window is
accepted as success.

## Web UI And HTTP API

The Pico creates an access point:

- SSID: `nasPico`
- Password: currently hard-coded in `server.c`
- Address: `http://192.168.4.1/`
- Channel: 1

The UI in `html_files/index.shtml` polls `/status` once per second and sends the
browser's local time to `/time.cgi` on load and every 60 seconds.

CGI endpoints:

| Endpoint | Purpose |
| --- | --- |
| `/sync.cgi` | Starts a backup run. Kept as a direct start endpoint. |
| `/gpio.cgi?state=on` | Starts a backup run and drives GPIO26 high. |
| `/gpio.cgi?state=off` | Cancels an active run or manually powers off GPIO26. |
| `/gpio.cgi?state=toggle` | Toggles based on current GPIO26 requested state. |
| `/auto-power-off.cgi?enabled=0|1` | Sets whether `COMPLETE` powers off immediately. Not persisted across reboot. |
| `/time.cgi?localEpoch=<seconds>` | Sets the scheduler clock from browser local time. |
| `/schedule.cgi?enabled=0|1&hour=0-23&minute=0-59` | Saves the daily schedule. |

JSON endpoints:

| Endpoint | Purpose |
| --- | --- |
| `/status` | Reports sync state, message, GPIO status, auto-power-off setting, schedule, clock, and last backup result. |
| `/schedule` | Reports saved schedule only. |

Captive portal probe paths `/hotspot-detect.html`, `/site.shtml`, and
`/library/test/success.html` redirect to `/index.shtml`.

## Scheduling And Clock

The daily schedule is saved in the final flash sector when the firmware image
does not overlap that sector. The stored structure includes magic, version,
enabled flag, hour, minute, and checksum. Defaults are disabled at 02:00.

The clock is not battery-backed and is not loaded from network time. The browser
sets a local epoch through `/time.cgi`; the Pico advances that time using uptime.
Scheduled backups do not run until the clock has been set.

The scheduler checks once per main loop iteration. It triggers when current
local minute equals the saved scheduled minute, and it suppresses duplicate
triggers within the same local day/minute.

Scheduled backup starts are skipped if:

- the schedule is disabled,
- the clock has not been set,
- another sync state is active,
- GPIO26 is already high,
- `sync_start_backup_run()` refuses the start.

## Build And Generated HTTP Content

The project targets `pico2_w` and is built with CMake/Ninja through the Pico SDK.

```sh
cmake --build build
```

`html_files/*` is converted into `htmldata.c` by `makefsdata.py`. The CMake
build regenerates `htmldata.c` when HTML content changes. If editing the UI by
hand, keep `html_files/index.shtml` as the source of truth and rebuild before
flashing.

## Main Loop

After initialization, the firmware repeatedly:

1. processes any pending GPIO27 status pulse,
2. handles sync state timeouts and completed-power hold expiry,
3. polls the daily schedule,
4. lets the CYW43/lwIP background stack wait for work for up to 100 ms.

This means GPIO pulse capture is interrupt-driven, but state transitions happen
in the main loop.

## Review Notes For Firmware Corrections

These are current design assumptions worth checking against hardware behavior:

- A single pulse snapshot is stored. Closely spaced valid pulses may overwrite
  each other before the main loop reads them.
- The declared 25 ms inter-pulse low interval is not validated.
- `ALIVE` is optional and does not refresh the 30-minute backup window.
- `COMPLETE` and error pulses are accepted before the first `ALIVE` once a
  backup start has placed the firmware in `TRANSFERRING`.
- Automatic power-off preference is volatile and resets to enabled on reboot.
- Schedule persistence uses the final flash sector and erases/programs it while
  interrupts are disabled.
- Browser-provided local time is trusted for scheduled backups.
- Wi-Fi credentials are hard-coded in `server.c`.
