# PSM and eDRX control with the NCS 3.2.1 legacy Serial Modem driver

## Scope

This note records the power-profile investigation for an nRF54 host running the
`sm_ppp_shell` sample and an nRF9151 running Serial Modem. The current product
constraint is that the host remains on nRF Connect SDK (NCS) v3.2.1 and uses
Zephyr's legacy `nordic,nrf91-slm` cellular modem driver.

The retained test profiles are:

1. Registered, PSM disabled, eDRX disabled.
2. PSM requested, eDRX disabled.

The current host-side implementation is suitable for controlled power
measurements. It is not the intended production architecture.

## The two retained power mechanisms

These mechanisms should not be treated as interchangeable:

- **UART/CMUX runtime power saving** turns off the UART while CMUX and PPP are
  idle. DTR wakes the UART and RI tells the host that the modem has data. This
  saves interface power but does not configure cellular PSM or eDRX.
- **Cellular PSM and eDRX** are 3GPP network features configured with
  `AT+CPSMS` and `AT+CEDRXS`. The network decides the timers it grants. PSM can
  put the cellular modem into deep sleep while the device remains registered.

UART idling may occur in both the registered and PSM profiles. Seeing a CMUX
power-save message proves only that the UART link became idle; it does not prove
that the cellular modem entered PSM.

## Root problem in NCS v3.2.1

The legacy host driver is:

```text
/home/uc/ncs/v3.2.1/zephyr/drivers/modem/modem_cellular.c
```

It is a generic, roughly 3,000-line cellular driver containing common state
management plus support for several modem vendors. The nRF91 Serial Modem
command sequences look like declarative configuration, but they are C macro
tables compiled directly into the driver implementation:

```c
MODEM_CHAT_SCRIPT_CMDS_DEFINE(nordic_nrf91_slm_init_chat_script_cmds,
	MODEM_CHAT_SCRIPT_CMD_RESP_MULT("AT", allow_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("AT+CFUN=4", ok_match),
	/* identification and registration commands */
	MODEM_CHAT_SCRIPT_CMD_RESP("AT#XCMUX=1", ok_match));

MODEM_CHAT_SCRIPT_CMDS_DEFINE(nordic_nrf91_slm_dial_chat_script_cmds,
	MODEM_CHAT_SCRIPT_CMD_RESP("AT#XPPP=1", ok_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("AT+CFUN=1", ok_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("AT#XCMUX=2", ok_match));
```

In the installed v3.2.1 source these tables are around lines 2816-2842. The
`MODEM_CELLULAR_DEVICE_NORDIC_NRF91_SLM` macro selects the tables around lines
3187-3203. The device is initialized at `POST_KERNEL` priority 99.

Devicetree can select `compatible = "nordic,nrf91-slm"` and configure UART,
GPIO and CMUX power-management properties. It cannot insert commands into the
init or dial table. Kconfig also provides no PSM/eDRX insertion hook.

Consequently, changing two AT commands through a clean application-level
configuration is not possible. Replacing the driver as a module means carrying
the entire implementation or maintaining a patch against SDK source, even
though the desired policy is only a short command sequence.

After CMUX starts, the driver owns its AT channel. The legacy nRF91 definition
exposes DLCI 3 as `gnss_pipe`; it does not expose a supported general-purpose AT
command API to the host application. A second raw-UART client cannot safely run
at the same time.

## Meaning of the PSM and eDRX commands

### Registered profile

```text
AT+CFUN=4
AT+CPSMS=0
AT+CEDRXS=0
```

- `AT+CFUN=4` puts the modem in flight mode while its requested settings are
  changed.
- `AT+CPSMS=0` disables the UE's PSM request.
- `AT+CEDRXS=0` removes the UE's eDRX requests.

The profile reads `AT+CPSMS?` and `AT+CEDRXS?` back to confirm that the modem
accepted the requested state.

### PSM profile

```text
AT+CFUN=4
AT+CPSMS=1,,,"00100001","00000000"
AT+CEDRXS=0
```

- `00100001` requests a one-hour periodic TAU.
- `00000000` requests zero seconds of Active-Time.
- eDRX is explicitly disabled so the measurement changes one cellular power
  mechanism at a time.

The legacy driver subsequently sends `AT+CFUN=1`, which begins cellular
registration with those requested settings.

`AT+CPSMS?` verifies the UE request, not the network grant. The network may
change or reject the requested timers. A complete validation needs the granted
timers from registration information and evidence of actual sleep entry. Power
measurement is the most direct confirmation for these experiments.

## Workaround 1: temporary host AT client before driver initialization

This is the implementation on branch `v321_host_power_profiles`.

Relevant files:

```text
samples/sm_ppp_shell/src/host_power_profile.c
samples/sm_ppp_shell/src/host_power_profile.h
samples/sm_ppp_shell/overlay-power-registered.conf
samples/sm_ppp_shell/overlay-power-psm.conf
samples/sm_ppp_shell/Kconfig
samples/sm_ppp_shell/src/main.c
```

The profile code registers:

```c
SYS_INIT(host_power_profile_init, POST_KERNEL, 98);
```

This is an initialization callback, not a dedicated thread. Priority 98 runs
immediately before the cellular driver's priority 99 initialization. It:

1. Waits two seconds for Serial Modem to boot.
2. Initializes `sm_at_client` on the raw modem UART.
3. Sends `AT`, `AT+CFUN=4`, and the selected PSM/eDRX commands.
4. Queries the requested state and records success or failure.
5. Uninitializes `sm_at_client` and releases the UART.
6. Waits 100 ms before the legacy driver takes ownership.

The HTTPS worker later calls `host_power_profile_apply()` immediately before
starting PPP. Despite its current name, that function does not apply the
profile again. It returns the result recorded by the early callback so PPP does
not start after failed or unverified configuration.

Advantages:

- Leaves the nRF9151 Serial Modem firmware unchanged.
- Leaves the installed NCS driver unchanged.
- Keeps the experimental policy in the host sample.
- Produces separate, reproducible test builds.

Disadvantages:

- Relies on NCS v3.2.1 initialization priorities.
- Blocks system initialization during the two-second startup wait.
- Temporarily transfers ownership of one UART between two independent clients.
- Works only before CMUX/PPP starts; it is not a general runtime AT interface.
- Assumes the nRF9151 is in raw AT mode. A host-only reset while the nRF9151
  remains in CMUX mode can fail.
- Is too timing-dependent for the intended production architecture.

Conclusion: acceptable for collecting comparable power data on the pinned test
setup; not recommended as the product solution.

## Workaround 2: configure the profile in the nRF9151 application

The earlier `v321_power_tests` branch implemented the commands in Serial Modem:

```text
app/src/sm_power_profile_registered.c
app/src/sm_power_profile_psm.c
```

The nRF9151 has direct access to `nrf_modem_lib`, so it can send `AT+CPSMS` and
`AT+CEDRXS` without competing for the external UART. It can also receive modem
registration and sleep events directly, which made it possible to log the
network-granted PSM timers and actual PSM entry/exit.

Advantages:

- No UART ownership race.
- Settings can be applied at a well-defined modem-library lifecycle point.
- Direct access to network and modem-sleep diagnostics.
- More robust than the current host startup workaround.

Disadvantages:

- Requires custom nRF9151 firmware rather than an unchanged Serial Modem build.
- Couples product power policy to the modem-side application.
- Separate profiles must be configured through builds or a new command/API.
- The customization must be carried when updating the Serial Modem repository.

This is a reasonable NCS v3.2.1 product direction if the team accepts owning a
small nRF9151 customization. It is also the cleanest way to collect detailed
PSM diagnostics with the legacy host driver.

`NRF_MODEM_LIB_ON_INIT` and `NRF_MODEM_LIB_ON_CFUN` are relevant only to this
nRF9151-side approach. The nRF54 host does not run `nrf_modem_lib`; it reaches
the nRF9151 through Serial Modem over UART.

## Workaround 3: patch or fork the legacy host driver

The PSM/eDRX commands can be added to the nRF91 init command table in
`modem_cellular.c`, possibly selected by new Kconfig options.

Advantages:

- Commands run naturally inside the driver's serialized initialization flow.
- No temporary transfer of UART ownership.
- Can remain entirely host-controlled.

Disadvantages:

- Editing the installed SDK affects other NCS v3.2.1 applications using that
  SDK checkout.
- Replacing the driver as an external module requires carrying its common state
  machine and vendor support, not merely a command-list data file.
- A downstream patch must be reapplied and tested against SDK changes.
- It still does not automatically provide a public runtime AT API.

If chosen for a product pinned permanently to NCS v3.2.1, maintain a small,
reviewable patch rather than copying an untracked modified SDK tree. This is
cleaner at runtime than the priority-98 workaround but creates an explicit SDK
maintenance obligation.

## Workaround 4: provision settings before normal operation

A manufacturing or service procedure could place the nRF9151 in an AT-client
mode, set PSM/eDRX, verify the settings, and then flash or start the normal
Serial Modem application.

This is useful only if the required settings persist across all relevant reset,
firmware-update and modem-state transitions. Persistence must be verified on
the exact modem firmware. It also does not satisfy a requirement to change
profiles dynamically in the deployed product.

## Workaround 5: extend Serial Modem with a product-specific command

Serial Modem could expose a small custom command such as a product power-profile
selector. The nRF54 would send one command and the nRF9151 would apply and
verify the standard `AT+CPSMS` and `AT+CEDRXS` commands internally.

This has the same ownership tradeoff as Workaround 2, but it permits runtime
selection without exposing raw modem control to the host. It is appropriate if
the product already expects to maintain custom Serial Modem firmware.

## Future Serial Modem v2 route

The separate `ncs-serial-modem` v2 previews contain the host-side
`nordic,nrf91-sm-v2` driver and Serial Modem AT Client/user-pipe support. That
architecture lets a Zephyr host send application AT commands on a CMUX pipe
without taking the driver's private UART or AT channel.

The new driver does not automatically choose PSM/eDRX policy. The host would
still send and verify `AT+CPSMS` and `AT+CEDRXS`, but it would do so through a
supported AT pipe rather than the current initialization-order workaround.

At the time of this investigation, Serial Modem v2.0.0 is not a final release.
The available v2.0.0 versions are explicitly preview releases, and adopting the
new driver requires moving both the nRF9151 application and nRF54 host
integration to the matching add-on generation. The product is also not ready
to move its shared host platform away from NCS v3.2.1.

Therefore, updating only the nRF9151 to a v2 preview would not solve the host
control problem. The legacy host driver would continue using `AT#XCMUX` and
`AT#XPPP` and would still lack the supported application AT interface.

## Current decision

For the present power-data collection:

- Keep both sides on the tested NCS v3.2.1-compatible baseline.
- Keep the nRF9151 Serial Modem application unchanged.
- Use the temporary priority-98 host AT client to produce the registered and
  PSM test images.
- Treat the workaround as test instrumentation.
- Verify requests through AT responses and verify actual behavior through
  network timer information where available and current measurements.

Before production, choose between:

1. A small, maintained nRF9151-side power-profile implementation.
2. A maintained patch to the pinned legacy host driver.
3. Migration of both sides to the finalized Serial Modem v2 host interface when
   its release and the shared host SDK schedule permit it.
