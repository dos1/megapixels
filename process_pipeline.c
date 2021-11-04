#include "process_pipeline.h"

#include "pipeline.h"
#include "zbar_pipeline.h"
#include "main.h"
#include "config.h"
#include "matrix.h"
#include "quickpreview.h"
#include "wb.h"
#include <tiffio.h>
#include <assert.h>
#include <math.h>
#include <wordexp.h>
#include <gtk/gtk.h>

#define SCRIPT_FORMAT "%s/millipixels/%s"

#define TIFFTAG_FORWARDMATRIX1 50964

static const float colormatrix_srgb[] = { 3.2409, -1.5373, -0.4986, -0.9692, 1.8759,
					  0.0415, 0.0556,  -0.2039, 1.0569 };


/// Having one global instead of many makes it clear that
/// each source file is meant to bahave like an object would.
/// Object-oriented conventions are easier applied from here.
struct module {
	MPPipeline *pipeline;

	char burst_dir[23];

	// FIXME: volatile is dangerous, use atomics instead.
	volatile bool is_capturing;
	volatile int frames_processed;
	volatile int frames_received;

	const struct mp_camera_config *camera;

	int burst_length;
	int captures_remaining;

	int preview_width;
	int preview_height;

	// bool gain_is_manual;
	int gain;
	int gain_max;
	int gain_min;

	bool exposure_is_manual;
	int exposure;
	int exposure_max;
	int exposure_min;

	int wb;
	int focus;
};

static struct module module = {
	.is_capturing = false,
	.frames_processed = 0,
	.frames_received = 0,
	.captures_remaining = 0,
};

static const char *pixel_format_names[MP_PIXEL_FMT_MAX] = {
    "\002\001\001\000", // fallback
    "\002\001\001\000",//    "BGGR8",
    "\001\002\000\001",//    "GBRG8",
    "\001\000\002\001",//    "GRBG8",
    "\000\001\001\002",//    "RGGB8",
    "\002\001\001\000",//    "BGGR10P",
    "\001\002\000\001",//    "GBRG10P",
    "\001\000\002\001",//    "GRBG10P",
    "\000\001\001\002",//    "RGGB10P",
    "\002\001\001\000",//    "BGGR10",
    "\001\002\000\001",//    "GBRG10",
    "\001\000\002\001",//    "GRBG10",
    "\000\001\001\002",//    "RGGB10",
    "UYVY",//    does not apply
    "YUYV",//    does not apply
};

static const char *
mp_pixel_format_to_cfa_pattern(uint32_t pixel_format)
{
	g_return_val_if_fail(pixel_format < MP_PIXEL_FMT_MAX, "\002\001\001\000");
	return pixel_format_names[pixel_format];
}

static void
register_custom_tiff_tags(TIFF *tif)
{
	static const TIFFFieldInfo custom_fields[] = {
		{ TIFFTAG_FORWARDMATRIX1, -1, -1, TIFF_SRATIONAL, FIELD_CUSTOM, 1, 1,
		  "ForwardMatrix1" },
	};

	// Add missing dng fields
	TIFFMergeFieldInfo(tif, custom_fields,
			   sizeof(custom_fields) / sizeof(custom_fields[0]));
}

static bool
find_processor(char *script)
{
	char *xdg_config_home;
	char filename[] = "postprocess.sh";
	wordexp_t exp_result;

	// Resolve XDG stuff
	if ((xdg_config_home = getenv("XDG_CONFIG_HOME")) == NULL) {
		xdg_config_home = "~/.config";
	}
	wordexp(xdg_config_home, &exp_result, 0);
	xdg_config_home = strdup(exp_result.we_wordv[0]);
	wordfree(&exp_result);

	// Check postprocess.h in the current working directory
	sprintf(script, "%s", filename);
	if (access(script, F_OK) != -1) {
		sprintf(script, "./%s", filename);
		printf("Found postprocessor script at %s\n", script);
		return true;
	}

	// Check for a script in XDG_CONFIG_HOME
	sprintf(script, SCRIPT_FORMAT, xdg_config_home, filename);
	if (access(script, F_OK) != -1) {
		printf("Found postprocessor script at %s\n", script);
		return true;
	}

	// Check user overridden /etc/megapixels/postprocessor.sh
	sprintf(script, SCRIPT_FORMAT, SYSCONFDIR, filename);
	if (access(script, F_OK) != -1) {
		printf("Found postprocessor script at %s\n", script);
		return true;
	}

	// Check packaged /usr/share/megapixels/postprocessor.sh
	sprintf(script, SCRIPT_FORMAT, DATADIR, filename);
	if (access(script, F_OK) != -1) {
		printf("Found postprocessor script at %s\n", script);
		return true;
	}

	return false;
}

static void
setup(MPPipeline *pipeline, const void *data)
{
	TIFFSetTagExtender(register_custom_tiff_tags);

	if (!find_processor(pipeline->processing_script)) {
		g_printerr("Could not find any post-process script\n");
		exit(1);
	}
}

void
mp_process_pipeline_start()
{
	module.pipeline = mp_pipeline_new();

	mp_pipeline_invoke(module.pipeline, setup, NULL, 0);


	mp_zbar_pipeline_start();
}

void
mp_process_pipeline_stop()
{
	mp_pipeline_free(module.pipeline);

	mp_zbar_pipeline_stop();
}

static cairo_surface_t *
process_image_for_preview(const MPImage *image)
{
	uint32_t surface_width, surface_height, skip;
	quick_preview_size(&surface_width, &surface_height, &skip, module.preview_width,
			   module.preview_height, image->width, image->height,
			   image->pixel_format, module.camera->rotate);

	cairo_surface_t *surface = cairo_image_surface_create(
		CAIRO_FORMAT_RGB24, surface_width, surface_height);

	uint8_t *pixels = cairo_image_surface_get_data(surface);

	quick_preview((uint32_t *)pixels, surface_width, surface_height, image->data,
		      image->width, image->height, image->pixel_format,
			  module.camera->rotate, module.camera->mirrored,
			  module.camera->previewmatrix[0] == 0 ? NULL : module.camera->previewmatrix,
			  module.camera->blacklevel, skip);

	// Create a thumbnail from the preview for the last capture
	cairo_surface_t *thumb = NULL;
	if (module.captures_remaining == 1) {
		printf("Making thumbnail\n");
		thumb = cairo_image_surface_create(
			CAIRO_FORMAT_ARGB32, MP_MAIN_THUMB_SIZE, MP_MAIN_THUMB_SIZE);

		cairo_t *cr = cairo_create(thumb);
		draw_surface_scaled_centered(
			cr, MP_MAIN_THUMB_SIZE, MP_MAIN_THUMB_SIZE, surface);
		cairo_destroy(cr);
	}

	// Pass processed preview to main and zbar
	mp_zbar_pipeline_process_image(cairo_surface_reference(surface));
	mp_main_set_preview(surface);

	return thumb;
}

static void
process_image_for_capture(const MPImage *image, int count)
{
	time_t rawtime;
	time(&rawtime);
	struct tm tim = *(localtime(&rawtime));

	char datetime[20] = { 0 };
	strftime(datetime, 20, "%Y:%m:%d %H:%M:%S", &tim);

	char fname[255];
	sprintf(fname, "%s/%d.dng", module.burst_dir, count);

	TIFF *tif = TIFFOpen(fname, "w");
	if (!tif) {
		printf("Could not open tiff\n");
	}

	// Define TIFF thumbnail
	int thumbnail_width = image->width >> 4;
	int thumbnail_height = image->height >> 4;
	TIFFSetField(tif, TIFFTAG_SUBFILETYPE, 1);
	TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
	TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
	TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
	TIFFSetField(tif, TIFFTAG_MAKE, mp_get_device_make());
	TIFFSetField(tif, TIFFTAG_MODEL, mp_get_device_model());
	uint16_t orientation;
	if (module.camera->rotate == 0) {
		orientation = module.camera->mirrored ? ORIENTATION_TOPRIGHT :
						 ORIENTATION_TOPLEFT;
	} else if (module.camera->rotate == 90) {
		orientation = module.camera->mirrored ? ORIENTATION_RIGHTBOT :
						 ORIENTATION_LEFTBOT;
	} else if (module.camera->rotate == 180) {
		orientation = module.camera->mirrored ? ORIENTATION_BOTLEFT :
						 ORIENTATION_BOTRIGHT;
	} else {
		orientation = module.camera->mirrored ? ORIENTATION_LEFTTOP :
						 ORIENTATION_RIGHTTOP;
	}
	TIFFSetField(tif, TIFFTAG_ORIENTATION, orientation);
	TIFFSetField(tif, TIFFTAG_DATETIME, datetime);
	TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 4);
	int extrasamples = EXTRASAMPLE_UNSPECIFIED;
	TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, &extrasamples);
	TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
	TIFFSetField(tif, TIFFTAG_SOFTWARE, "Millipixels");
	long sub_offset = 0;
	TIFFSetField(tif, TIFFTAG_SUBIFD, 1, &sub_offset);
	TIFFSetField(tif, TIFFTAG_DNGVERSION, "\001\001\0\0");
	TIFFSetField(tif, TIFFTAG_DNGBACKWARDVERSION, "\001\0\0\0");
	char uniquecameramodel[255];
	sprintf(uniquecameramodel, "%s %s", mp_get_device_make(),
		mp_get_device_model());
	TIFFSetField(tif, TIFFTAG_UNIQUECAMERAMODEL, uniquecameramodel);
	if (module.camera->colormatrix[0]) {
		TIFFSetField(tif, TIFFTAG_COLORMATRIX1, 9, module.camera->colormatrix);
	} else {
		TIFFSetField(tif, TIFFTAG_COLORMATRIX1, 9, colormatrix_srgb);
	}
	if (module.camera->forwardmatrix[0]) {
		TIFFSetField(tif, TIFFTAG_FORWARDMATRIX1, 9, module.camera->forwardmatrix);
	}
	if (module.camera->colormatrix[0] && module.camera->forwardmatrix[0]) {
		float neutral[3];
		matrix_white_point(module.camera->previewmatrix, neutral);
		TIFFSetField(tif, TIFFTAG_ASSHOTNEUTRAL, 3, neutral);
	} else {
		TIFFSetField(tif, TIFFTAG_ASSHOTNEUTRAL, 3, WB_WHITEPOINTS[module.wb]);
	}
	TIFFSetField(tif, TIFFTAG_CALIBRATIONILLUMINANT1, 21); // D65
	{
		uint32_t surface_width, surface_height, skip;
		quick_preview_size(&surface_width, &surface_height, &skip, thumbnail_width,
				thumbnail_height, image->width, image->height,
				image->pixel_format, 0);

		TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, surface_width);
		TIFFSetField(tif, TIFFTAG_IMAGELENGTH, surface_height);

		uint8_t *pixels = malloc(surface_width * 4 * surface_height);
		quick_preview((uint32_t *)pixels, surface_width, surface_height, image->data,
			image->width, image->height, image->pixel_format,
			0, false,
			module.camera->previewmatrix[0] == 0 ? NULL : module.camera->previewmatrix,
			module.camera->blacklevel, skip);

		for (int row = 0; row < thumbnail_height; row++) {
			TIFFWriteScanline(tif, pixels + surface_width * 4 * row, row, 0);
		}
		free(pixels);
	}
	TIFFWriteDirectory(tif);

	// Define main photo
	TIFFSetField(tif, TIFFTAG_SUBFILETYPE, 0);
	TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, image->width);
	TIFFSetField(tif, TIFFTAG_IMAGELENGTH, image->height);
	TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, mp_pixel_format_bits_per_pixel(image->pixel_format));
	TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_CFA);
	TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
	TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
	static const short cfapatterndim[] = { 2, 2 };
	TIFFSetField(tif, TIFFTAG_CFAREPEATPATTERNDIM, cfapatterndim);

	const char *cfa_pattern = mp_pixel_format_to_cfa_pattern(image->pixel_format);
#if (TIFFLIB_VERSION < 20201219) && !LIBTIFF_CFA_PATTERN
	TIFFSetField(tif, TIFFTAG_CFAPATTERN, cfa_pattern);
#else
	TIFFSetField(tif, TIFFTAG_CFAPATTERN, 4, cfa_pattern);
#endif
	printf("TIFF version %d\n", TIFFLIB_VERSION);
	int whitelevel = module.camera->whitelevel;
	if (!whitelevel) {
		whitelevel = (1 << mp_pixel_format_pixel_depth(image->pixel_format)) - 1;
	}
	TIFFSetField(tif, TIFFTAG_WHITELEVEL, 1, &whitelevel);
	if (module.camera->blacklevel) {
		const float blacklevel = module.camera->blacklevel;
		TIFFSetField(tif, TIFFTAG_BLACKLEVEL, 1, &blacklevel);
	}
	TIFFCheckpointDirectory(tif);
	printf("Writing frame to %s\n", fname);

	for (int row = 0; row < image->height; row++) {
		TIFFWriteScanline(tif, image->data + (row * mp_pixel_format_width_to_bytes(image->pixel_format, image->width)), row, 0);
	}
	TIFFWriteDirectory(tif);

	// Add an EXIF block to the tiff
	TIFFCreateEXIFDirectory(tif);
	// 1 = manual, 2 = full auto, 3 = aperture priority, 4 = shutter priority
	if (!module.exposure_is_manual) {
		TIFFSetField(tif, EXIFTAG_EXPOSUREPROGRAM, 2);
	} else {
		TIFFSetField(tif, EXIFTAG_EXPOSUREPROGRAM, 1);
	}

	//TIFFSetField(tif, EXIFTAG_EXPOSURETIME,
	//	     (mode.frame_interval.numerator /
	//	      (float)mode.frame_interval.denominator) /
	//		     ((float)image->height / (float)exposure));
	if (module.camera->iso_min && module.camera->iso_max) {
		uint16_t isospeed = remap(module.gain - 1, module.gain_min, module.gain_max, module.camera->iso_min,
					  module.camera->iso_max);
		TIFFSetField(tif, EXIFTAG_ISOSPEEDRATINGS, 1, &isospeed);
	}
	TIFFSetField(tif, EXIFTAG_FLASH, 0);
	TIFFSetField(tif, EXIFTAG_WHITEBALANCE, 1);

	TIFFSetField(tif, EXIFTAG_DATETIMEORIGINAL, datetime);
	TIFFSetField(tif, EXIFTAG_DATETIMEDIGITIZED, datetime);
	if (module.camera->fnumber) {
		TIFFSetField(tif, EXIFTAG_FNUMBER, module.camera->fnumber);
	}
	if (module.camera->focallength) {
		TIFFSetField(tif, EXIFTAG_FOCALLENGTH, module.camera->focallength);
	}
	if (module.camera->focallength && module.camera->cropfactor) {
		TIFFSetField(tif, EXIFTAG_FOCALLENGTHIN35MMFILM,
				 (short)(module.camera->focallength * module.camera->cropfactor));
	}
	char makernote[255] = {};
	snprintf(makernote, 255, "L5MP Gain: %d; Exposure: %d; Focus: %d; Balance: %s", module.gain, module.exposure, module.focus, WB_ILLUMINANTS[module.wb]);
	TIFFSetField(tif, EXIFTAG_MAKERNOTE, strlen(makernote) + 1, makernote);
	uint64_t exif_offset = 0;
	TIFFWriteCustomDirectory(tif, &exif_offset);
	TIFFFreeDirectory(tif);

	// Update exif pointer
	TIFFSetDirectory(tif, 0);
	TIFFSetField(tif, TIFFTAG_EXIFIFD, exif_offset);
	TIFFRewriteDirectory(tif);

	TIFFClose(tif);
}

static void
post_process_finished(GSubprocess *proc, GAsyncResult *res, cairo_surface_t *thumb)
{
	char *stdout;
	g_subprocess_communicate_utf8_finish(proc, res, &stdout, NULL, NULL);

	// The last line contains the file name
	int end = strlen(stdout);
	// Skip the newline at the end
	stdout[--end] = '\0';

	char *path = path = stdout + end - 1;
	do {
		if (*path == '\n') {
			path++;
			break;
		}
		--path;
	} while (path > stdout);

	mp_main_capture_completed(thumb, path);
}

static void
process_capture_burst(cairo_surface_t *thumb)
{
	static char capture_fname[255];
	time_t rawtime;
	time(&rawtime);
	struct tm tim = *(localtime(&rawtime));

	char timestamp[30];
	strftime(timestamp, 30, "%Y%m%d%H%M%S", &tim);

	if (g_get_user_special_dir(G_USER_DIRECTORY_PICTURES) != NULL) {
		sprintf(capture_fname,
			"%s/IMG%s",
			g_get_user_special_dir(G_USER_DIRECTORY_PICTURES),
			timestamp);
	} else if (getenv("XDG_PICTURES_DIR") != NULL) {
		sprintf(capture_fname,
			"%s/IMG%s",
			getenv("XDG_PICTURES_DIR"),
			timestamp);
	} else {
		sprintf(capture_fname,
			"%s/Pictures/IMG%s",
			getenv("HOME"),
			timestamp);
	}


	// Start post-processing the captured burst
	g_print("Post process %s to %s.ext\n", module.burst_dir, capture_fname);
	GError *error = NULL;
	GSubprocess *proc = g_subprocess_new(
		G_SUBPROCESS_FLAGS_STDOUT_PIPE,
		&error,
		module.pipeline->processing_script,
		module.burst_dir,
		capture_fname,
		NULL);

	if (!proc) {
		g_printerr("Failed to spawn postprocess process: %s\n",
			   error->message);
		return;
	}

	g_subprocess_communicate_utf8_async(
		proc,
		NULL,
		NULL,
		(GAsyncReadyCallback)post_process_finished,
		thumb);
}

static void
process_image(MPPipeline *pipeline, const MPImage *image)
{
	cairo_surface_t *thumb = process_image_for_preview(image);

	if (module.captures_remaining > 0) {
		int count = module.burst_length - module.captures_remaining;
		--module.captures_remaining;

		process_image_for_capture(image, count);

		if (module.captures_remaining == 0) {
			assert(thumb);
			process_capture_burst(thumb);
		} else {
			assert(!thumb);
		}
	} else {
		assert(!thumb);
	}

	free(image->data);

	++module.frames_processed;
	if (module.captures_remaining == 0) {
		module.is_capturing = false;
	}
}

void
mp_process_pipeline_process_image(MPImage image)
{
	// If we haven't processed the previous frame yet, drop this one
	if (module.frames_received != module.frames_processed && !module.is_capturing) {
		//printf("Dropped frame at capture\n");
		free(image.data);
		return;
	}

	++module.frames_received;

	mp_pipeline_invoke(module.pipeline, (MPPipelineCallback)process_image, &image,
			   sizeof(MPImage));
}

static void
capture()
{
	char template[] = "/tmp/megapixels.XXXXXX";
	char *tempdir;
	tempdir = mkdtemp(template);

	if (tempdir == NULL) {
		g_printerr("Could not make capture directory %s\n", template);
		exit(EXIT_FAILURE);
	}

	strcpy(module.burst_dir, tempdir);

	module.captures_remaining = module.burst_length;
}

void
mp_process_pipeline_capture()
{
	module.is_capturing = true;

	mp_pipeline_invoke(module.pipeline, capture, NULL, 0);
}

static void
update_state(MPPipeline *pipeline, const struct mp_process_pipeline_state *state)
{
	module.camera = state->camera;

	module.burst_length = state->burst_length;

	module.preview_width = state->preview_width;
	module.preview_height = state->preview_height;

	// gain_is_manual = state->gain_is_manual;
	module.gain = state->gain;
	module.gain_max = state->gain_max;
	module.gain_min = state->gain_min;

	module.exposure_is_manual = state->exposure_is_manual;
	module.exposure = state->exposure;
	module.exposure_max = state->exposure_max;
	module.exposure_min = state->exposure_min;

	module.wb = state->wb;
	module.focus = state->focus;

	struct mp_main_state main_state = {
		.camera = module.camera,
		.mode = state->mode,
		.is_present = state->is_present,
		.gain_is_manual = state->gain_is_manual,
		.gain = module.gain,
		.gain_max = module.gain_max,
		.gain_min = module.gain_min,
		.exposure_is_manual = module.exposure_is_manual,
		.exposure = module.exposure,
		.exposure_max = module.exposure_max,
		.exposure_min = module.exposure_min,
		.has_auto_focus_continuous = state->has_auto_focus_continuous,
		.has_auto_focus_start = state->has_auto_focus_start,
		.focus = module.focus,
	};
	mp_main_update_state(&main_state);
}

void
mp_process_pipeline_update_state(const struct mp_process_pipeline_state *new_state)
{
	mp_pipeline_invoke(module.pipeline, (MPPipelineCallback)update_state, new_state,
			   sizeof(struct mp_process_pipeline_state));
}
