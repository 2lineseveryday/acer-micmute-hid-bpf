# Acer 노트북 마이크 음소거 키 고치기 (Linux)

일부 Acer 노트북의 마이크 음소거 핫키는 자신의 리포트 서술자 선언 범위를
벗어난 HID 값을 보내서, 커널이 조용히 버림: 모든 `/dev/input/event*` 노드
무응답, hidraw에만 원시 리포트 보임. 이 저장소는 1바이트 HID-BPF
서술자 패치 + hwdb 키 리매핑으로 고침.

검증 환경: CachyOS, kernel 7.2.9, systemd/udev 262, Hyprland.

## 해당 하드웨어

- HID 장치 `0018:048D:83DC.0001` (ITE), modalias
  `hid:b0018g0001v0000048Dp000083DC`
- 확인:

```bash
udevadm info /sys/bus/hid/devices/0018:048D:83DC.0001 | grep -E "MODALIAS|HID_NAME"
sudo od -An -tx1 /dev/hidraw1   # 32로 시작하는 9바이트 리포트 반복 확인
```

증상: 후보 event 노드 전부(`--grab` 포함) 무응답, hidraw에는
`32 00 00 6a ...` (눌림) / `32 00 00 00 ...` (뗌) 반복.

## 원인

리포트 ID `0x32` (키보드) 키 배열 선언:

```
95 06, 75 08, 15 00, 25 65, 05 07, 19 00, 29 e7, 81 00
```

Logical Maximum **101 (`0x65`)**인데 실제 전송값은 `0x6a`(106 = HID **F15**).
범위 밖이라 hid-input이 버리고, `hwdb` 단계 이전이라 hwdb만으로 해결 불가.

## 고치기 원리

1. **HID-BPF** (`0010-ITE__83DC-MicMute.bpf.c`)가 probe 시 서술자 offset
   `0x37`을 `0x65` → `0xe7`로. 이후 커널이 Keyboard 노드(`event7`)에 **F15** 방출.
2. **hwdb**가 F15 (`MSC_SCAN 7006a`)를 **F20**으로 리매핑. `micmute`로 하지
   않는 이유: `KEY_MICMUTE`(248)는 XKB 키코드 상한(255) 초과(evdev+8=256)라
   Wayland/X11 합성기까지 도달 못 함. `KEY_F20`(190→198)은 정상 통과.
3. 합성기에서 F20 바인딩 (아래 Hyprland 예시).

## 전제 조건

- `udev-hid-bpf`, `clang`, `bpftool`, `libbpf` 설치
- `CONFIG_HID_BPF=y` + BTF (`/sys/kernel/btf/vmlinux` 존재)

```bash
zgrep CONFIG_HID_BPF /proc/config.gz
ls /sys/kernel/btf/vmlinux
```

## 설치

### 1. BPF 프로그램 빌드

```bash
# udev-hid-bpf 프로젝트에서 헤더 확보
git clone --depth 1 https://gitlab.freedesktop.org/libevdev/udev-hid-bpf.git
bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h

clang -target bpf -O2 -g -Wall -c 0010-ITE__83DC-MicMute.bpf.c \
  -o 0010-ITE__83DC-MicMute.bpf.o \
  -I./udev-hid-bpf/src/bpf -I.
llvm-strip -g 0010-ITE__83DC-MicMute.bpf.o
udev-hid-bpf list-bpf-programs --bpfdir .   # 목록에 뜨면 정상
```

### 2. 수동 테스트

```bash
sudo udev-hid-bpf add /sys/bus/hid/devices/0018:048D:83DC.0001 \
  ./0010-ITE__83DC-MicMute.bpf.o
sudo evtest /dev/input/event7   # 키 누르면 F15 나와야 성공
sudo udev-hid-bpf remove /sys/bus/hid/devices/0018:048D:83DC.0001  # 원복
```

장착·제거 때마다 장치가 disconnect→reprobe 되어 키보드 입력이 1~2초 끊김 (정상).

### 3. 정식 설치 (udev 자동 장착)

```bash
sudo cp 0010-ITE__83DC-MicMute.bpf.o /usr/lib/firmware/hid/bpf/
```

`/etc/udev/hwdb.d/82-acer-micmute.hwdb` (modalias가 다르면 ID 교체):

```
hid-bpf:hid:b0018g0001v0000048Dp000083DC
 HID_BPF_S_100=0010-ITE__83DC-MicMute.bpf.o
 .HID_BPF=1
```

```bash
sudo systemd-hwdb update
# 주의: 그냥 `udevadm trigger`는 action "change"라 hid-bpf 룰을 안 탐. 반드시 add:
sudo udevadm trigger --action=add /sys/bus/hid/devices/0018:048D:83DC.0001
```

확인:

```bash
udevadm info /sys/bus/hid/devices/0018:048D:83DC.0001 | grep HID_BPF
ls /sys/fs/bpf/hid/   # 장치 ID가 뜨면 장착됨
```

### 4. 키 리매핑

evtest에서 스캔코드 확인 (`type 4 (EV_MSC) ... value 7006a`).

`/etc/udev/hwdb.d/83-acer-micmute.hwdb`:

```
evdev:input:b0018v048Dp83DC*
 KEYBOARD_KEY_7006a=f20
```

```bash
sudo systemd-hwdb update
sudo udevadm trigger --action=change /dev/input/event7
sudo evtest /dev/input/event7   # KEY_F20(190)이면 성공
```

### 5. Hyprland 바인딩

```conf
bind = , F20, exec, wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle
```

`wev`로 이벤트 도달 확인 가능. F20이 이미 쓰이면 F21~F24 중 빈 것을
hwdb와 바인딩 양쪽에.

## 다른 펌웨어에 적용

`.bpf.c`의 offset (`RDESC_LENGTH 193`, 패치 위치 `0x37`)은 이 기기
펌웨어 전용. 다르면:

```bash
od -An -tx1 /sys/bus/hid/devices/<장치ID>/report_descriptor
```

`85 32` (리포트 ID 0x32) 뒤 키 배열의 `25 XX`를 찾아 그 값 바이트가
패치 위치, 전체 바이트 수가 `RDESC_LENGTH`. `#define`과 가드를 고치면 됨.
가드 불일치 시 패치 없이 통과라 잘못된 offset은 안전하게 무효화됨.

## 문제 해결

| 증상 | 원인 / 조치 |
|---|---|
| 수동은 되고 정식 설치 후 무응답 | `trigger`에 `--action=add` 필요. `udevadm info`에서 `HID_BPF_S_100` 확인 |
| `hwdb update` 후에도 `HID_BPF_*` 없음 | hwdb 문법: 매치줄 무들여쓰기, 속성줄 한 칸 들여쓰기,末尾 빈 줄 |
| evtest엔 나오고 Hyprland 무응답 | XKB 키코드 초과 — `micmute` 말고 F20으로 |
| 펌웨어 업데이트 후 사망 | 서술자 변경으로 가드가 패치 비활성화 — offset 재측정 (위 참조) |

## 제거

```bash
sudo rm /usr/lib/firmware/hid/bpf/0010-ITE__83DC-MicMute.bpf.o \
  /etc/udev/hwdb.d/82-acer-micmute.hwdb /etc/udev/hwdb.d/83-acer-micmute.hwdb
sudo systemd-hwdb update
sudo udevadm trigger --action=add /sys/bus/hid/devices/0018:048D:83DC.0001
```

## 참고

- [HID-BPF 커널 문서](https://docs.kernel.org/hid/hid-bpf.html)
- [udev-hid-bpf](https://gitlab.freedesktop.org/libevdev/udev-hid-bpf/)

## AI assistance

이 수정(진단, BPF 프로그램, 문서) 일부는 AI assistance로 작성되어 실기
(`evtest`, hwdb, Hyprland 종단)로 검증됨.

## License

MIT — `LICENSE` 참조. 예외: BPF 프로그램
(`0010-ITE__83DC-MicMute.bpf.c`)은 커널 요구(HID-BPF)상 GPL-2.0-only
(SPDX 헤더 참조).
