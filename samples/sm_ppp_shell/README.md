# Periodic HTTPS baseline on the nRF54

This sample now brings up PPP automatically, synchronizes UTC with
`time.google.com` using SNTP, and requests `https://httpbin.org/get` on a dedicated
thread. The default start-to-start interval is 60 seconds. There is no need to
enter `net iface up 1` after boot.

Each cycle uses a new TCP/TLS connection, requires certificate-chain, date and
hostname verification, streams the response through a 1024-byte buffer, and
closes the socket. Success requires HTTP 200 and the HTTP parser's complete-message
marker. Response bodies are counted, not printed. TLS session caching is disabled
by the socket default; session tickets are disabled in the build.

The existing modem UART pins, DTR/RI, 115200 baud, CMUX MTU 127, and five-second
UART idle/wake configuration are unchanged. This does not request cellular PSM,
System OFF, or change the nRF9151 firmware. The unused zperf utility is disabled
to fit TLS in RAM. RTT shell and network inspection commands remain available.

## Build and interval

From this sample directory:

```sh
NCS_TOOLCHAIN=/home/uc/ncs/toolchains/43683a87ea \
  ~/bin/ncs-west v3.2.1 build --sysbuild \
  -b raytac_an54l15q_db/nrf54l15/cpuapp -d build .
```

The flash image is `build/merged.hex`. Change
`CONFIG_HTTPS_TEST_INTERVAL_SECONDS=60` in `prj.conf`, then rebuild with:

```sh
NCS_TOOLCHAIN=/home/uc/ncs/toolchains/43683a87ea \
  ~/bin/ncs-west v3.2.1 build -d build
```

## Host-controlled cellular power profiles

The nRF54 can now change the requested profile at runtime while the nRF9151
continues to use one unchanged Serial Modem image. Apply the compatibility
patch to the pinned host SDK, then build without a profile overlay:

```sh
cd /home/uc/ncs/v3.2.1
patch --batch -p1 < /home/uc/Documents/appblocks/ncs-serial-modem/samples/sm_ppp_shell/patches/ncs-v3.2.1-nrf91-slm-uart-handoff.patch

cd /home/uc/Documents/appblocks/ncs-serial-modem/samples/sm_ppp_shell
NCS_TOOLCHAIN=/home/uc/ncs/toolchains/43683a87ea \
  ~/bin/ncs-west v3.2.1 build --sysbuild -p always \
  -b raytac_an54l15q_db/nrf54l15/cpuapp \
  -d build-runtime-handoff . -- \
  -DZEPHYR_EXTRA_MODULES=/home/uc/Documents/appblocks/ncs-serial-modem
```

Use the RTT shell without reflashing or resetting either processor:

```text
power_profile status
power_profile psm
power_profile edrx
power_profile psm_edrx
power_profile registered
```

`psm` requests the configured TAU and Active-Time and disables eDRX; `edrx`
requests eDRX and disables PSM; `psm_edrx` requests both; and `registered`
disables both. Each change stops PPP, waits until the
legacy driver is suspended, temporarily gives its closed raw UART to
`sm_at_client`, sends `AT+CFUN=4` and the profile commands, restores the
driver's DTR/RI configuration and UART callback, and restarts PPP. The normal
legacy dial sequence then sends `AT+CFUN=1`, starting a new registration with
the requested settings.

The command reports that the modem accepted and read back the request. Network
grant and actual PSM entry still require network diagnostics or measurement.
The HTTPS worker serializes network operations against profile switching.

The profile overlays below remain useful for selecting a repeatable initial
profile, but runtime switching is included even in the default
`HOST_POWER_PROFILE_NONE` build.

Build profile 1, registered with PSM and eDRX disabled:

```sh
NCS_TOOLCHAIN=/home/uc/ncs/toolchains/43683a87ea \
  ~/bin/ncs-west v3.2.1 build --sysbuild -p always \
  -b raytac_an54l15q_db/nrf54l15/cpuapp \
  -d build-host-registered . -- \
  -DZEPHYR_EXTRA_MODULES=/home/uc/Documents/appblocks/ncs-serial-modem \
  -DEXTRA_CONF_FILE=overlay-power-registered.conf
```

Build profile 2, PSM requested with eDRX disabled:

```sh
NCS_TOOLCHAIN=/home/uc/ncs/toolchains/43683a87ea \
  ~/bin/ncs-west v3.2.1 build --sysbuild -p always \
  -b raytac_an54l15q_db/nrf54l15/cpuapp \
  -d build-host-psm . -- \
  -DZEPHYR_EXTRA_MODULES=/home/uc/Documents/appblocks/ncs-serial-modem \
  -DEXTRA_CONF_FILE=overlay-power-psm.conf
```

Build the host-only XSLEEP experiment after applying its small NCS v3.2.1
legacy-driver patch:

```sh
cd /home/uc/ncs/v3.2.1
patch -p1 < /home/uc/Documents/appblocks/ncs-serial-modem/samples/sm_ppp_shell/patches/ncs-v3.2.1-nrf91-slm-xsleep.patch

cd /home/uc/Documents/appblocks/ncs-serial-modem/samples/sm_ppp_shell
NCS_TOOLCHAIN=/home/uc/ncs/toolchains/43683a87ea \
  ~/bin/ncs-west v3.2.1 build --sysbuild -p always \
  -b raytac_an54l15q_db/nrf54l15/cpuapp \
  -d build-host-xsleep-hostonly . -- \
  -DZEPHYR_EXTRA_MODULES=/home/uc/Documents/appblocks/ncs-serial-modem \
  -DEXTRA_CONF_FILE=overlay-power-xsleep.conf
```

The patch adds a shutdown script only when
`CONFIG_HOST_POWER_PROFILE_XSLEEP=y`. When the host calls `net_if_down()`,
the driver stops PPP and sends `AT+CFUN=4` followed by `AT#XSLEEP=1` while
the AT DLCI is still open. After the command succeeds, the unmodified nRF9151
Serial Modem enters System OFF. At the next request deadline, `net_if_up()`
opens the UART, asserts DTR, and causes the nRF9151 to wake through reset before
the driver rebuilds CMUX and PPP.

This profile requires the existing DTR and RI wiring. It does not require P0.22
or an nRESET wire. `AT+CFUN=4` is sent before XSLEEP to avoid the `CFUN=0`
nonvolatile-memory write described for an active modem. XSLEEP remains an
experimental power-measurement feature and is not a production power-control
interface.

Flash only the corresponding nRF54 `merged.hex`. Continue using the unchanged
nRF9151 Serial Modem image:

- `build-host-registered/merged.hex`
- `build-host-psm/merged.hex`
- `build-host-xsleep-hostonly/merged.hex`

The XSLEEP build should log:

```text
Host power profile: host-issued XSLEEP between cycles; PSM=off; eDRX=off; verified
PPP suspended; host shutdown script requested
Sleep deadline reached; resuming PPP and waking Serial Modem with DTR
```

For a measurement run, do not attach the nRF9151 debugger after it enters
System OFF because starting a debug session wakes the device. Revert the local SDK patch when this experiment is no longer needed:

```sh
cd /home/uc/ncs/v3.2.1
patch -R -p1 < /home/uc/Documents/appblocks/ncs-serial-modem/samples/sm_ppp_shell/patches/ncs-v3.2.1-nrf91-slm-xsleep.patch
```

The registered build must log `Runtime power profile applied: registered`.
The PSM build must log `Runtime power profile applied: psm`. A verification
failure stops startup or makes the shell command fail, so a power measurement
cannot silently run with the wrong profile. `AT+CPSMS?` verifies
the request stored by the modem; the network-granted timers still depend on the
serving network. Confirm actual PSM entry electrically or through a separate
diagnostic run.

Requests begin after PPP has a preferred IPv4 address and SNTP succeeds. Failed
SNTP attempts retry after 10 seconds; HTTPS is not attempted with an unset clock.
The host waits two seconds before its first PPP start so the Serial Modem can
finish booting. If PPP still has no IPv4 address after 90 seconds, the host cycles
the interface automatically, waits two seconds, and tries again. This reproduces
the manual `net iface down 1` / `net iface up 1` recovery without rebooting either
board.
DNS has a 5-second socket resolver timeout; TCP connect and TLS handshake each
use the SDK's 15-second connect timeout. Socket send/receive and HTTP processing
have 15-second timeouts. These are stage limits, not a single total-cycle limit.
A cycle can exceed its configured interval. Missed deadlines are counted and skipped, never
queued; a single worker prevents overlap. Network loss pauses requests until
PPP connectivity returns and then begins a new schedule. Neither board is
rebooted automatically. SNTP runs once per application boot, not per request.

Remove the runtime-handoff SDK patch with:

```sh
cd /home/uc/ncs/v3.2.1
patch --batch -R -p1 < /home/uc/Documents/appblocks/ncs-serial-modem/samples/sm_ppp_shell/patches/ncs-v3.2.1-nrf91-slm-uart-handoff.patch
```

## Hardware validation

Flash only the nRF54 image after deliberately selecting its debug target. Start
with a full two-board power cycle so an old CMUX session is not left behind.
Wait for registration, IP assignment and the time synchronization message.
Expected result format (values below are illustrative):

```text
https_test: cycle=1 start
https_test: cycle=1 SUCCESS status=200 bytes=253 complete=1 duration_ms=1200 error=0
```

- Observe at least ten successive successful cycles; confirm the RTT shell still
  responds to `net iface` and `net dns httpbin.org`.
- Confirm existing CMUX logs show idle entry between requests and wake on the
  next request. If activity lasts almost the whole interval, increase the interval
  before expecting a five-second UART idle period.
- Enter `net iface down 1`: requests must pause after any in-flight operation
  finishes or times out. Enter `net iface up 1`: after reconnection requests must
  resume. This explicit test does not prove recovery from every network fault.
- With connectivity unavailable during boot, verify it waits without attempting
  HTTPS. If SNTP cannot reach its server, verify retries and no HTTPS requests.
- During a request, interrupt the network path; verify a failure is reported,
  resources are released, and the shell remains responsive. Restore connectivity
  and verify subsequent cycles succeed.
- Set the interval to one second temporarily to exercise missed-deadline logging
  with slower requests; there must be no concurrent/catch-up requests. Restore 60.
- For certificate rejection tests, temporarily use a wrong CA, wrong hostname,
  or an out-of-validity clock in a separate test build; none may report SUCCESS.
  Do not disable verification to make a connection work.

Build and desktop endpoint checks have passed; these board tests are pending.
Network pools now reserve 16 buffers of 256 bytes per direction and eight packet
descriptors per direction. The TLS heap is 40 KiB and the general heap is 4 KiB.
The 16 KiB TLS receive record and 8 KiB HTTPS thread stack are retained; reducing
them needs runtime evidence. Sustained hardware operation must verify heap and
stack sufficiency with these smaller allocations.
Verbose modem/CMUX logging and an attached debugger affect power measurements.
Use identical logging settings across future profiles and distinguish functional
runs from final measurements. No PSM or System OFF savings are claimed here.

## Endpoint and certificate maintenance

The endpoint is a public test service, documented at https://httpbin.org/legacy.
Availability and response size are outside our control. For controlled long-term
measurements, move to a managed endpoint with a fixed payload.

`src/httpbin_ca.h` embeds Amazon Root CA 1 in DER format, sourced from the local
system CA store. Its SHA-256 fingerprint is:

```text
8E:CD:E6:88:4F:3D:87:B1:12:5B:A3:1A:C3:FC:B1:3D:70:16:DE:7F:57:CC:90:4F:E1:CB:97:C6:AE:98:19:6E
```

Official roots: https://www.amazontrust.com/repository/
The observed httpbin.org leaf was issued by Amazon RSA 2048 M01. Recheck the
server chain when updating the endpoint or when verification begins failing;
rotate the trusted root deliberately rather than embedding a short-lived leaf.

`src/tls_time.h` explicitly enables mbed TLS time/date checks because NCS's
`nrf-config.h` does not map Zephyr's date-validation option. It is applied to the
TLS library through `MBEDTLS_USER_CONFIG_FILE`. SNTP supplies time but is itself
unauthenticated; this is a laboratory baseline, not a complete trusted-time design.
NCS v3.2.1's RSA TLS configuration requires the legacy crypto option and emits a
deprecation warning. Hardware-backed PSA/CRACEN randomness remains enabled; the
sample's insecure test random generator is disabled.
