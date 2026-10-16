# Acer Laptop Mic-Mute Key Fix (Linux)

[한국어](README.ko.md)

The microphone-mute hotkey on some Acer laptops sends a HID usage its own
report descriptor declares out of range, so the kernel silently drops it:
no input event on any `/dev/input/event*` node, while the raw report is
still visible on hidraw. This repo fixes it with a one-byte HID-BPF
report-descriptor patch plus a hwdb key remap.

Tested on: CachyOS, kernel 7.2.9, systemd/udev 262, Hyprland.

## Affected hardware

- HID device `0018:048D:83DC.0001` (ITE), modalias
  `hid:b0018g0001v0000048Dp000083DC`
- Check yours:

```bash
udevadm info /sys/bus/hid/devices/0018:048D:83DC.0001 | grep -E "MODALIAS|HID_NAME"
sudo od -An -tx1 /dev/hidraw1   # look for 9-byte reports starting with 32
```

Symptoms: `evtest` on every candidate event node (including `--grab`)
shows nothing when the key is pressed, but hidraw shows repeating
`32 00 00 6a ...` (press) / `32 00 00 00 ...` (release).

## Root cause

Report ID `0x32` (keyboard) declares its key array as:

```
95 06, 75 08, 15 00, 25 65, 05 07, 19 00, 29 e7, 81 00
```

i.e. Logical Maximum **101 (`0x65`)**, but the mic-mute key sends usage
`0x6a` (106 = HID **F15**). The out-of-range value is dropped by hid-input
before any `hwdb`/keymap layer ever sees it — which is why a hwdb entry
alone cannot fix this device.

## How the fix works

1. **HID-BPF** (`0010-ITE__83DC-MicMute.bpf.c`) widens Logical Maximum
   `0x65` → `0xe7` at descriptor offset `0x37` when the device is probed.
   The kernel then emits **F15** on the Keyboard node (`event7`).
2. **hwdb** remaps F15 (`MSC_SCAN 7006a`) to **F20** — not `micmute`:
   `KEY_MICMUTE` (248) becomes XKB keycode 256 (evdev+8), exceeding the
   255 limit, so Wayland/X11 compositors never receive it. `KEY_F20`
   (190 → 198) passes through fine.
3. Your compositor binds F20 (Hyprland example below).

## Prerequisites

- `udev-hid-bpf`, `clang`, `bpftool`, `libbpf` installed
- Kernel with `CONFIG_HID_BPF=y` and BTF (`/sys/kernel/btf/vmlinux` exists)

```bash
zgrep CONFIG_HID_BPF /proc/config.gz
ls /sys/kernel/btf/vmlinux
```

## Install

### 1. Build the BPF program

```bash
# headers from the udev-hid-bpf project
git clone --depth 1 https://gitlab.freedesktop.org/libevdev/udev-hid-bpf.git
bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h

clang -target bpf -O2 -g -Wall -c 0010-ITE__83DC-MicMute.bpf.c \
  -o 0010-ITE__83DC-MicMute.bpf.o \
  -I./udev-hid-bpf/src/bpf -I.
llvm-strip -g 0010-ITE__83DC-MicMute.bpf.o
udev-hid-bpf list-bpf-programs --bpfdir .   # listed = OK
```

### 2. Test manually

```bash
sudo udev-hid-bpf add /sys/bus/hid/devices/0018:048D:83DC.0001 \
  ./0010-ITE__83DC-MicMute.bpf.o
sudo evtest /dev/input/event7   # F15 must appear on key press
sudo udev-hid-bpf remove /sys/bus/hid/devices/0018:048D:83DC.0001  # revert
```

Attaching/detaching disconnects and reprobes the device; keyboard input
pauses for 1–2 seconds. That is normal.

### 3. Install permanently (auto-attach via udev)

```bash
sudo cp 0010-ITE__83DC-MicMute.bpf.o /usr/lib/firmware/hid/bpf/
```

`/etc/udev/hwdb.d/82-acer-micmute.hwdb` (replace IDs if your modalias differs):

```
hid-bpf:hid:b0018g0001v0000048Dp000083DC
 HID_BPF_S_100=0010-ITE__83DC-MicMute.bpf.o
 .HID_BPF=1
```

```bash
sudo systemd-hwdb update
# NOTE: plain `udevadm trigger` sends action "change", which the hid-bpf
# udev rule ignores. Use "add":
sudo udevadm trigger --action=add /sys/bus/hid/devices/0018:048D:83DC.0001
```

Verify:

```bash
udevadm info /sys/bus/hid/devices/0018:048D:83DC.0001 | grep HID_BPF
ls /sys/fs/bpf/hid/   # device ID listed = attached
```

### 4. Remap the key

Get the scancode from evtest (`type 4 (EV_MSC) ... value 7006a`).

`/etc/udev/hwdb.d/83-acer-micmute.hwdb`:

```
evdev:input:b0018v048Dp83DC*
 KEYBOARD_KEY_7006a=f20
```

```bash
sudo systemd-hwdb update
sudo udevadm trigger --action=change /dev/input/event7
sudo evtest /dev/input/event7   # KEY_F20 (190) = success
```

### 5. Hyprland binding

```conf
bind = , F20, exec, wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle
```

Check delivery with `wev`. If F20 is already taken, use any free F21–F24
in both the hwdb line and the bind.

## Adapting to other firmware

The offsets in the `.bpf.c` (`RDESC_LENGTH 193`, patch offset `0x37`)
match this device's firmware. If yours differs:

```bash
od -An -tx1 /sys/bus/hid/devices/<your-id>/report_descriptor
```

Find `85 32` (Report ID 0x32), then the key-array item `25 XX` after it —
that value byte is your patch offset, and the total byte count is your
`RDESC_LENGTH`. Update the `#define`s and the guard accordingly; the
program refuses to patch if the guards don't match, so a wrong offset
fails safe (no patch) instead of corrupting the descriptor.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Works manually, not after install | `trigger` needs `--action=add`; check `HID_BPF_S_100` in `udevadm info` |
| `HID_BPF_*` missing after `hwdb update` | hwdb syntax: match line unindented, property lines one space, trailing blank line |
| Key works in `evtest`, not in Hyprland | XKB keycode overflow — map to F20, not `micmute` |
| Dead after firmware update | descriptor changed, guard disabled the patch — re-measure offsets (above) |

## Uninstall

```bash
sudo rm /usr/lib/firmware/hid/bpf/0010-ITE__83DC-MicMute.bpf.o \
  /etc/udev/hwdb.d/82-acer-micmute.hwdb /etc/udev/hwdb.d/83-acer-micmute.hwdb
sudo systemd-hwdb update
sudo udevadm trigger --action=add /sys/bus/hid/devices/0018:048D:83DC.0001
```

## References

- [HID-BPF kernel docs](https://docs.kernel.org/hid/hid-bpf.html)
- [udev-hid-bpf](https://gitlab.freedesktop.org/libevdev/udev-hid-bpf/)

## AI assistance

Parts of this fix (diagnosis, BPF program, docs) were developed with AI assistance
and verified on real hardware (`evtest`, hwdb, Hyprland end-to-end).

## License

MIT — see `LICENSE`.
