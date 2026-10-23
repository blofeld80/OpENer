# OpENer on Zephyr

OpENer as a Zephyr module (`zephyr/module.yml`). The port uses Zephyr's native network API only
(`zsock_*` with `zsock_poll()`, `net_if`); no POSIX layer, `CONFIG_POSIX_API` stays off.

| File | Content |
|---|---|
| `networkhandler.c` | OpENer's network handler on `zsock_poll()`: listeners, sessions, I/O sockets |
| `networkconfig.c` | `IfaceGetMacAddress`, `IfaceGetConfiguration`, `IfaceWaitForIp`, `GetHostName` on `net_if` |
| `platform_network_includes.h` | BSD socket names mapped onto Zephyr's `net_*` / `zsock_*` |
| `opener_user_conf.h` | OpENer's configuration, generated from Kconfig (`CONFIG_OPENER_*`) |
| `opener_zephyr.h` | `opener_zephyr_stats_get()`: connection and session counters |
| `compat/` | the `<netinet/in.h>` and `<sys/socket.h>` that `endianconv.c` includes |

## Use

West manifest:

```yaml
- name: opener
  remote: <the fork>
  repo-path: OpENer
  revision: <commit>
  path: external/modules/lib/opener
```

`prj.conf`:

```
CONFIG_NETWORKING=y
CONFIG_NET_IPV4=y
CONFIG_NET_UDP=y
CONFIG_NET_TCP=y
CONFIG_NET_SOCKETS=y
CONFIG_NET_CONTEXT_REUSEADDR=y
CONFIG_NET_CONTEXT_DSCP_ECN=y
CONFIG_NET_CONTEXT_SNDTIMEO=y
CONFIG_NET_HOSTNAME_ENABLE=y
CONFIG_OPENER=y
```

The stack's options (device identity for `devicedata.h`, connection counts, trace level, ...) are
`CONFIG_OPENER_*`, see `zephyr/Kconfig`. Do not provide an `opener_user_conf.h` or a
`devicedata.h` of your own: the module generates both from Kconfig, and the stack and the
application must be compiled with the same configuration.

## Quick start without application code

`CONFIG_OPENER_AUTOSTART=y` runs the start-up sequence below in a thread of its own, and
`CONFIG_OPENER_SAMPLE_APPLICATION=y` provides the hooks of OpENer's sample application (assemblies
100, 150, 151; output mirrored to input). With a static address from `NET_CONFIG_SETTINGS` or
DHCPv4 that is a running EtherNet/IP adapter. A device turns the sample off and implements the
hooks itself.

## What the application provides

The CIP hooks of `opener_api.h`: `ApplicationInitialization()`, `HandleApplication()`,
`CheckIoConnectionEvent()`, `AfterAssemblyDataReceived()`, `BeforeAssemblyDataSend()`,
`RunIdleChanged()`, `ResetDevice()`,
`ResetDeviceToInitialConfiguration()`, `CipCalloc()` and `CipFree()`.

The start-up sequence, after the interface is up and has an IPv4 address:

1. `DoublyLinkedListInitialize(&connection_list, CipConnectionObjectListArrayAllocator,
   CipConnectionObjectListArrayFree)` and `SetXorShiftSeed()` (OpENer seeds with `time(NULL)`).
2. `CipStackInit()` (calls `ApplicationInitialization()`), then `CipEthernetLinkSetMac()` and the
   TCP/IP object: `g_tcpip.config_control`, `GetHostName(&g_tcpip.hostname)`,
   `IfaceGetConfiguration()`.
3. `NetworkHandlerInitialize()`.
4. A thread that calls `NetworkHandlerProcessCyclic()` in a loop.

Bringing the interface up (static IPv4, DHCPv4) is also the application's job.

## Notes

- Zephyr's `NET_AF_INET` is 1, the EtherNet/IP wire value of `sin_family` is 2: the core keeps 2
  (`AF_INET` in `platform_network_includes.h`), the port converts where an address reaches a socket.
- Traces (`CONFIG_OPENER_TRACE_LEVEL` > 0) go to `stderr`, that is the console.
