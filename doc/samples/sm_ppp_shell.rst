.. _sm_ppp_shell_sample:

PPP shell sample
################

.. contents::
   :local:
   :depth: 2

The |SM| PPP shell sample demonstrates how to use the nRF91 Series SiP as a Zephyr-compatible cellular modem through a Point-to-Point Protocol (PPP) interface.
This sample enables an external MCU or Linux host to use Zephyr's native networking stack and shell commands to control the cellular modem, test network connectivity, and measure network performance.

Requirements
************

The |SM| application must be built with the following overlays:

* :file:`app/cmux.conf` - To enable CMUX support
* :file:`app/ppp.conf` - To enable PPP support

If using an external MCU as the controlling device, also include:

* :file:`app/external-mcu.overlay` - To configure UART pins, DTR, and RI signals

The sample supports the following development kits:

.. list-table::
   :widths: auto
   :header-rows: 1

   * - Hardware platforms
     - PCA
     - Board name
     - Board target
   * - `nRF54L15 DK <nRF54L15 DK_>`_
     - PCA10156
     - `nrf54l15dk`_
     - ``nrf54l15dk/nrf54l15/cpuapp``
   * - Native simulator
     - N/A
     - ``native_sim``
     - ``native_sim``

The nRF54L15 DK modem UART uses the same UART22 pinout as the OSS_ZEPHYR host
application. It and the |SM| ``external-mcu.overlay`` use 1,000,000 baud with
hardware flow control. The shell remains on the separate VCOM1 UART.

Hardware setup for nRF54L15 DK
==============================

When using the nRF54L15 DK as the controlling device, connect the DK pins to the corresponding pins on the nRF91 Series DK as follows:

.. list-table::
   :header-rows: 1

   * - nRF54L15 DK
     - nRF91 Series DK
   * - UART TX P1.12
     - UART RX P0.03
   * - UART RX P1.13
     - UART TX P0.02
   * - UART CTS P1.14
     - UART RTS P0.06
   * - UART RTS P1.11
     - UART CTS P0.07
   * - DTR OUT P0.04
     - DTR IN P0.31
   * - RI IN P0.03
     - RI OUT P0.30
   * - GPIO GND
     - GPIO GND

.. note::
   Buttons 0 and 3 and LED 3 are unavailable while this modem pinout is used,
   because their GPIOs are reassigned to the modem interface.

.. note::
   The GPIO output levels on the nRF91 Series device and nRF54L15 DK must be the same.

   * Set the VDD voltages for both devices using the `Board Configurator app`_.

Native simulator setup
======================

When running on the native simulator, the sample expects the |SM| to be available at ``/dev/ttyACM0`` on the Linux host system.
Connect the nRF91 Series DK running |SM| to your Linux host through USB.

Overview
********

This sample showcases the integration between Zephyr's cellular modem driver and the |SM| application running on an nRF91 Series SiP.
Instead of using AT commands directly, the controlling device can use Zephyr's standard networking APIs and shell commands to interact with the modem.

The sample provides:

* Full Zephyr networking stack (IPv4 and IPv6 support).
* Network shell commands for interface management.
* A one-byte HTTP response every 60 seconds for power-mode validation.
* DNS resolution support.
* Automatic modem connection management.
* CMUX (GSM 07.10) multiplexing support with user pipes for AT command access.
* Power management with runtime power saving.
* Runtime PSM and eDRX configuration over a Serial Modem v2 user pipe.

For more information about using the nRF91 Series SiP as a Zephyr-compatible modem, including how to use user pipes for AT commands alongside PPP, see :ref:`sm_cellular_modem`.

Configuration
*************

The sample is configured with the following key features:

* PPP Layer 2 networking
* IPv4 and IPv6 support
* TCP and UDP protocols
* DNS resolver with automatic DNS server configuration from PPP
* Network connection manager for automatic reconnection
* Zephyr cellular modem driver with CMUX support
* Runtime power management with automatic power saving
* Network shell commands
* A deterministic one-byte transfer derived from the OSS_ZEPHYR host test

The CMUX configuration enables:

* Power save mode that closes CMUX pipes when idle
* 5 second idle timeout before entering power save
* MTU size of 127 bytes for optimal performance
* User pipes on DLC channels 3 and 4 for AT command access (available when the network interface is up)

Periodic HTTP and power profiles
================================

The sample automatically brings up PPP and sends
``GET http://httpbin.org/bytes/1?seed=54`` once every 60 seconds. Each cycle
creates and closes a TCP connection and succeeds only after receiving an HTTP
200 response with exactly one body byte. This is the minimum-sized ``/bytes``
response and is derived from the proven OSS_ZEPHYR host test, keeping TLS
outside the cellular power-mode validation. Change
``CONFIG_HTTP_TEST_INTERVAL_SECONDS`` to select another start-to-start interval.

The default build leaves the existing PSM and eDRX requests unchanged. Use one
of these configuration fragments to apply and verify a profile after the first
PPP connection:

* ``overlay-power-registered.conf`` - disable PSM and eDRX.
* ``overlay-power-psm.conf`` - request PSM and disable eDRX.
* ``overlay-power-edrx.conf`` - disable PSM and request eDRX.
* ``overlay-power-psm-edrx.conf`` - request both PSM and eDRX.

The profile implementation uses a Serial Modem v2 CMUX user pipe. It sends
``AT+CFUN=4``, configures and reads back ``AT+CPSMS`` and ``AT+CEDRXS``, then
sends ``AT+CFUN=1`` and waits for the existing PPP interface to reconnect. It
does not stop the network interface or take ownership of the raw UART while
changing a profile.

Profiles can also be selected while running:

.. code-block:: console

   uart:~$ power_profile registered
   uart:~$ power_profile psm
   uart:~$ power_profile edrx
   uart:~$ power_profile psm_edrx
   uart:~$ power_profile status
   uart:~$ power_profile network

The ``network`` command logs ``AT%XMONITOR`` and ``AT+CEDRXRDP`` responses so
the network-provided PSM and eDRX values can be compared with the requested
settings. UART runtime power saving is independent of cellular PSM and eDRX;
use current measurement to confirm actual sleep entry.

Building and running
********************

This sample can be found under :file:`samples/sm_ppp_shell`.

nRF54L15 DK
===========

To build the sample for the nRF54L15 DK:

.. code-block:: bash

   west build -b nrf54l15dk/nrf54l15/cpuapp

Flash the sample to your board and open a serial terminal to view the shell interface.

For nRF54L15 DK, the shell is available on UART1 (VCOM1).

Native simulator
================

To build the sample for the native simulator:

.. code-block:: bash

   west build -b native_sim

For native simulator, run the executable with the ``-attach_uart`` option:

.. code-block:: bash

   build/zephyr/zephyr.exe -attach_uart

Testing
*******

After the sample starts, you will see the Zephyr shell prompt. The periodic
HTTP worker starts the PPP interface automatically.

Bringing up the network interface
=================================

To start the cellular modem manually if automatic startup failed:

.. code-block:: console

   uart:~$ net iface up 1

The modem begins the network registration process.
You can monitor the connection status with:

.. code-block:: console

   uart:~$ net iface

The network management event monitor (enabled by default) will also display connection events automatically.

Once connected, the interface will show as "UP" and you will see an assigned IP address.

Testing network connectivity
============================

After the network interface is up, you can test basic connectivity.

To ping a (DNS) server:

.. code-block:: console

   uart:~$ net ping 8.8.8.8

To perform DNS resolution:

.. code-block:: console

   uart:~$ net dns google.com

Performance testing with zperf
==============================

The sample includes the zperf tool for network performance measurements:

To run a UDP upload test:

.. code-block:: console

   uart:~$ zperf udp upload <server-ip> <port> <duration> <packet-size> <rate>

See the `zperf: Network Traffic Generator`_ for more details on using zperf.

Send AT commands through user pipes
===================================

When the network interface is up, you can access the modem's AT command interface through the user pipes on CMUX DLC channels 3 and 4.
By default, the shell sample is configured to use the user pipe on DLC channel 3.

To send an AT command:

.. code-block:: console

   uart:~$ modem at AT+CFUN=4
   EVENT: L3 [1] IPv4 address del 100.92.136.41
   EVENT: L4 [1] disconnected
   EVENT: L4 [1] IPv4 connectivity lost
   EVENT: L2 [1] interface down
   OK
   [00:14:52.706,554] <dbg> modem_cellular: modem_cellular_log_state_changed: switch from registered to await PPP dead
   [00:14:52.706,738] <dbg> modem_cellular: modem_cellular_log_event: event ppp dead
   [00:14:53.095,303] <dbg> modem_cellular: modem_cellular_log_event: event deregistered
   [00:14:54.706,661] <dbg> modem_cellular: modem_cellular_log_event: event timeout
   [00:14:54.706,720] <dbg> modem_cellular: modem_cellular_log_state_changed: switch from await PPP dead to await registered
   [00:14:56.706,792] <dbg> modem_cellular: modem_cellular_log_event: event timeout
   uart:~$ modem at AT+CFUN?
   +CFUN: 4
   OK
   uart:~$ modem at AT+CFUN=1
   OK
   [00:15:16.308,209] <dbg> modem_cellular: modem_cellular_log_event: event deregistered
   EVENT: L2 [1] interface up
   [00:15:18.072,762] <dbg> modem_cellular: modem_cellular_log_event: event registered
   [00:15:18.072,803] <dbg> modem_cellular: modem_cellular_log_state_changed: switch from await registered to run dial script
   [00:15:18.072,861] <dbg> modem_cellular: modem_cellular_log_event: event timeout
   [00:15:18.100,659] <dbg> modem_cellular: modem_cellular_log_event: event script success
   [00:15:18.100,740] <dbg> modem_cellular: modem_cellular_log_state_changed: switch from run dial script to registered
   EVENT: L3 [1] IPv6 address add fe80::3330:3539:3831:3936
   EVENT: L3 [1] IPv6 neighbor add fe80::5eff:fe00:533a
   EVENT: L3 [1] IPv4 address add 100.92.136.41
   EVENT: L4 [1] connected
   EVENT: L4 [1] IPv4 connectivity available
   uart:~$

Shutting down the network interface
===================================

To shut down the cellular connection and power off the modem:

.. code-block:: console

   uart:~$ net iface down 1

Dependencies
************

The sample enables the following key features:

* Networking stack (``CONFIG_NETWORKING``)
* PPP L2 layer (``CONFIG_NET_L2_PPP``)
* Cellular modem driver (``CONFIG_MODEM_CELLULAR``)
* Network shell (``CONFIG_NET_SHELL``)
* Zperf (``CONFIG_NET_ZPERF``)
* Power management (``CONFIG_PM_DEVICE``)
* AT command shell (``CONFIG_MODEM_AT_SHELL``)

References
**********

* :ref:`sm_cellular_modem`
* :ref:`serial_modem_app`
* `Zephyr Networking`_
* `Zephyr Network APIs`_
* `Zephyr Modem modules`_
