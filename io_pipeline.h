#pragma once

#include "camera_config.h"

struct mp_io_pipeline_state {
	const struct mp_camera_config *camera;

	int burst_length;

	int preview_width;
	int preview_height;

	bool gain_is_manual;
	int gain;

	bool exposure_is_manual;
	int exposure;

	int focus;

	int wb;
};

void mp_io_pipeline_start();
void mp_io_pipeline_stop();

void mp_io_pipeline_focus();
void mp_io_pipeline_capture();
void mp_io_pipeline_release(void);

void mp_io_pipeline_update_state(const struct mp_io_pipeline_state *state);


/*
 * TODO remove all DW9714 device-specific code below once (absolute) focus
 * is available as a control.
 */
#define DW9714_FOCUS_VAL(data, s) ((data) << 4 | (s))
#define DW9714_FOCUS_VAL_MAX 1023
/*
 * S[3:2] = 0x00, codes per step for "Linear Slope Control"
 * S[1:0] = 0x00, step period
 */
#define DW9714_DEFAULT_S 0x0

static void mp_write_dw9714_focus(int val)
{
	uint16_t focus_reg = 0;
	char buf[42] = {};

	focus_reg = DW9714_FOCUS_VAL(val, DW9714_DEFAULT_S);
	snprintf(buf, 42, "sudo i2ctransfer -f -y 3 w2@0xc 0x%02x 0x%02x",
		 (uint8_t)(focus_reg >> 8), (uint8_t)(focus_reg & 0xff));
	g_spawn_command_line_async(buf, NULL);
}
