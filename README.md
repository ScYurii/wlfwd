# wlfwd

A small open replacement for Qualcomm's `cnss-daemon`: the userspace half of
WLAN firmware bring-up on downstream Android kernels with the `icnss` (v1)
driver, for running a plain Linux distribution (such as postmarketOS) without
Android.

On these devices the WLAN firmware runs inside the modem as a protection
domain (`wlan_pd`) and exposes the WLFW service (QMI service `0x45`) over QRTR.
The kernel driver waits for `FW_READY`, but the firmware only gets there after
the host has walked it through a sequence that `icnss` v1 leaves to userspace.
Without it, `echo ON > /dev/wlan` fails with `EINVAL`.

`wlfwd` does that sequence:

1. `IND_REGISTER` for the firmware indications
2. `CAP`, to read the `board_id`
3. wait for `FW_MEM_READY` (up to 10 s)
4. `BDF_DOWNLOAD` of `regdb.bin` (BDF type 4)
5. `BDF_DOWNLOAD` of the board data file (BDF type 0)
6. `CAL_REPORT` with the calibration files saved earlier
7. `MAC_ADDR`

It then keeps serving calibration indications: when the firmware offers
calibration data it is pulled with `CAL_UPDATE` and saved, and on the next
start it is reported and sent back with `CAL_DOWNLOAD`.

`wlfwd` follows the WLFW service through the QRTR name service (`NEW_LOOKUP`).
When the WLAN PD restarts, the service comes back on a new port and the whole
sequence is replayed, so Wi-Fi recovers without a reboot. The service tends to
appear before the firmware is ready for `CAP` (QMI error 90); the bring-up is
retried every 2 s for as long as the same server is alive.

## Usage

```
wlfwd <firmware dir> <MAC> [state dir]
```

- `<firmware dir>` must contain `regdb.bin` and the board data file. The BDF
  is looked up as `bdwlan.bXX` / `bdwlan.XXX` for the `board_id` reported by
  the firmware, falling back to `bdwlan.bin`.
- `<MAC>` is the address to give the interface, `aa:bb:cc:dd:ee:ff`.
- `[state dir]` keeps the calibration files `wlfw_cal_NN.bin`; default
  `/var/lib/wlfwd`.

Logs go to stdout. `wlfwd` runs until it is killed.

**Use the board files of the unit itself.** On the OnePlus Nord the BDF and
regdb that work are the per-unit calibrated copies on the `persist` partition;
the generic copies in the modem partition make the firmware crash right after
`CAL_REPORT`.

`wlfwd` only talks to the firmware. Bringing up the rest is up to the caller:
a QRTR name service (`qrtr-ns`), `rmtfs`, `tqftpserv` and `pd-mapper` for the
modem, holding the modem open (`/dev/subsys_modem`), and `echo ON > /dev/wlan`
once `FW_READY` is reported.

### pdr-restart

```
pdr-restart <node> <port> <pd name>
```

Asks the service registry notifier (QMI service `0x42`) to restart a protection
domain, the same request the kernel's `icnss_trigger_recovery()` sends. It is a
test tool: use it to check that `wlfwd` brings Wi-Fi back after a WLAN PD
restart, e.g. `pdr-restart 0 2 msm/modem/wlan_pd` (node and port of the
servreg service vary by device).

## Building

Only libc and the kernel UAPI headers (`linux/qrtr.h`) are needed:

```
make
make install PREFIX=/usr DESTDIR=...
```

## Tested on

- OnePlus Nord (AC2003, `avicii`, SM7250), LineageOS 4.19 kernel,
  postmarketOS: 2.4 and 5 GHz, PD restart recovery.

Other devices with downstream `icnss` v1 and a modem-hosted WLAN PD may work;
reports are welcome.

Packaged for postmarketOS in
[ScYurii/pmaports](https://github.com/ScYurii/pmaports/tree/avicii-upstream/device/downstream/avicii-wlan)
(`avicii-wlan`). Documentation of the port:
[ScYurii/oneplus-nord-postmarketos](https://github.com/ScYurii/oneplus-nord-postmarketos).

## License

GPL-2.0-only, see [LICENSE](LICENSE).
