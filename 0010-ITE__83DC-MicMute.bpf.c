// SPDX-License-Identifier: GPL-2.0-only
/* ITE 048D:83DC (Acer laptop hotkeys) mic-mute fix.
 *
 * Report ID 0x32 (keyboard) declares its key array as:
 *   95 06, 75 08, 15 00, 25 65, 05 07, 19 00, 29 e7, 81 00
 * i.e. Logical Maximum 101 (0x65), but the mic-mute key sends usage
 * 0x6a (106, KEY_F15). The out-of-range value is dropped by hid-input,
 * so no input event is ever emitted (hidraw still shows the raw report).
 * Widen Logical Maximum to 0xe7 (== Usage Maximum) so F15 passes through.
 */

#include "vmlinux.h"
#include "hid_bpf.h"
#include "hid_bpf_helpers.h"
#include <bpf/bpf_tracing.h>

#define VID_ITE	0x048D
#define PID_83DC	0x83DC

HID_BPF_CONFIG(
	HID_DEVICE(BUS_I2C, HID_GROUP_GENERIC, VID_ITE, PID_83DC),
);

/* Full report descriptor length of 0018:048D:83DC.0001 */
#define RDESC_LENGTH 193
/* Offset of the Logical Maximum value byte of the report-0x32 key array */
#define LOGICAL_MAX_OFFSET 0x37

SEC(HID_BPF_RDESC_FIXUP)
int BPF_PROG(acer_micmute_fix_rdesc, struct hid_bpf_ctx *hctx)
{
	__u8 *data = hid_bpf_get_data(hctx, 0 /* offset */, HID_MAX_DESCRIPTOR_SIZE /* size */);

	if (!data)
		return 0; /* EPERM check */

	if (hctx->size != RDESC_LENGTH)
		return 0;

	/* Guard: must be the report-ID-0x32 keyboard collection ... */
	if (data[6] != 0x85 || data[7] != 0x32)
		return 0;
	/* ... with the known-buggy Logical Maximum 101 */
	if (data[LOGICAL_MAX_OFFSET] != 0x65)
		return 0;

	data[LOGICAL_MAX_OFFSET] = 0xe7;

	return 0;
}

HID_BPF_OPS(acer_micmute) = {
	.hid_rdesc_fixup = (void *)acer_micmute_fix_rdesc,
};

char _license[] SEC("license") = "GPL";
