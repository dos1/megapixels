#pragma once

#include "camera_config.h"

struct mp_process_pipeline_state {
	const struct mp_camera_config *camera;
	MPCameraMode mode;
	bool is_present;

	int burst_length;

	int preview_width;
	int preview_height;

	bool gain_is_manual;
	int gain;
	int gain_max;
	int gain_min;

	bool exposure_is_manual;
	int exposure;
	int exposure_max;
	int exposure_min;

	bool has_auto_focus_continuous;
	bool has_auto_focus_start;
	int focus;

	int wb;
};

void mp_process_pipeline_start();
void mp_process_pipeline_stop();

void mp_process_pipeline_process_image(MPImage image);
void mp_process_pipeline_capture();
void mp_process_pipeline_update_state(const struct mp_process_pipeline_state *state);
