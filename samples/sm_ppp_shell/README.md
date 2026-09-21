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
