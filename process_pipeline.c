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

static MPPipeline *pipeline;

static char burst_dir[23];
static char processing_script[512];

static volatile bool is_capturing = false;
static volatile int frames_processed = 0;
static volatile int frames_received = 0;

static const struct mp_camera_config *camera;

static int burst_length;
static int captures_remaining = 0;

static int preview_width;
static int preview_height;

// static bool gain_is_manual;
static int gain;
static int gain_max;
static int gain_min;

static bool exposure_is_manual;
static int exposure;
static int exposure_max;
static int exposure_min;

static int wb;
static int focus;

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

	if (!find_processor(processing_script)) {
		g_printerr("Could not find any post-process script\n");
		exit(1);
	}
}

void
mp_process_pipeline_start()
{
	pipeline = mp_pipeline_new();

	mp_pipeline_invoke(pipeline, setup, NULL, 0);


	mp_zbar_pipeline_start();
}

void
mp_process_pipeline_stop()
{
	mp_pipeline_free(pipeline);

	mp_zbar_pipeline_stop();
}

static cairo_surface_t *
process_image_for_preview(const MPImage *image)
{
	uint32_t surface_width, surface_height, skip;
	quick_preview_size(&surface_width, &surface_height, &skip, preview_width,
			   preview_height, image->width, image->height,
			   image->pixel_format, camera->rotate);

	cairo_surface_t *surface = cairo_image_surface_create(
		CAIRO_FORMAT_RGB24, surface_width, surface_height);

	uint8_t *pixels = cairo_image_surface_get_data(surface);

	quick_preview((uint32_t *)pixels, surface_width, surface_height, image->data,
		      image->width, image->height, image->pixel_format,
		      camera->rotate, camera->mirrored,
		      camera->previewmatrix[0] == 0 ? NULL : camera->previewmatrix,
		      camera->blacklevel, skip);

	// Create a thumbnail from the preview for the last capture
	cairo_surface_t *thumb = NULL;
	if (captures_remaining == 1) {
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
	sprintf(fname, "%s/%d.dng", burst_dir, count);

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
	if (camera->rotate == 0) {
		orientation = camera->mirrored ? ORIENTATION_TOPRIGHT :
						 ORIENTATION_TOPLEFT;
	} else if (camera->rotate == 90) {
		orientation = camera->mirrored ? ORIENTATION_RIGHTBOT :
						 ORIENTATION_LEFTBOT;
	} else if (camera->rotate == 180) {
		orientation = camera->mirrored ? ORIENTATION_BOTLEFT :
						 ORIENTATION_BOTRIGHT;
	} else {
		orientation = camera->mirrored ? ORIENTATION_LEFTTOP :
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
	if (camera->colormatrix[0]) {
		TIFFSetField(tif, TIFFTAG_COLORMATRIX1, 9, camera->colormatrix);
	} else {
		TIFFSetField(tif, TIFFTAG_COLORMATRIX1, 9, colormatrix_srgb);
	}
	if (camera->forwardmatrix[0]) {
		TIFFSetField(tif, TIFFTAG_FORWARDMATRIX1, 9, camera->forwardmatrix);
	}
	if (camera->colormatrix[0] && camera->forwardmatrix[0]) {
		float neutral[3];
		matrix_white_point(camera->previewmatrix, neutral);
		TIFFSetField(tif, TIFFTAG_ASSHOTNEUTRAL, 3, neutral);
	} else {
		TIFFSetField(tif, TIFFTAG_ASSHOTNEUTRAL, 3, WB_WHITEPOINTS[wb]);
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
			camera->previewmatrix[0] == 0 ? NULL : camera->previewmatrix,
			camera->blacklevel, skip);

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
	int whitelevel = camera->whitelevel;
	if (!whitelevel) {
		whitelevel = (1 << mp_pixel_format_pixel_depth(image->pixel_format)) - 1;
	}
	TIFFSetField(tif, TIFFTAG_WHITELEVEL, 1, &whitelevel);
	if (camera->blacklevel) {
		const float blacklevel = camera->blacklevel;
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
	if (!exposure_is_manual) {
		TIFFSetField(tif, EXIFTAG_EXPOSUREPROGRAM, 2);
	} else {
		TIFFSetField(tif, EXIFTAG_EXPOSUREPROGRAM, 1);
	}

	//TIFFSetField(tif, EXIFTAG_EXPOSURETIME,
	//	     (mode.frame_interval.numerator /
	//	      (float)mode.frame_interval.denominator) /
	//		     ((float)image->height / (float)exposure));
	if (camera->iso_min && camera->iso_max) {
		uint16_t isospeed = remap(gain - 1, gain_min, gain_max, camera->iso_min,
					  camera->iso_max);
		TIFFSetField(tif, EXIFTAG_ISOSPEEDRATINGS, 1, &isospeed);
	}
	TIFFSetField(tif, EXIFTAG_FLASH, 0);
	TIFFSetField(tif, EXIFTAG_WHITEBALANCE, 1);

	TIFFSetField(tif, EXIFTAG_DATETIMEORIGINAL, datetime);
	TIFFSetField(tif, EXIFTAG_DATETIMEDIGITIZED, datetime);
	if (camera->fnumber) {
		TIFFSetField(tif, EXIFTAG_FNUMBER, camera->fnumber);
	}
	if (camera->focallength) {
		TIFFSetField(tif, EXIFTAG_FOCALLENGTH, camera->focallength);
	}
	if (camera->focallength && camera->cropfactor) {
		TIFFSetField(tif, EXIFTAG_FOCALLENGTHIN35MMFILM,
			     (short)(camera->focallength * camera->cropfactor));
	}
	char makernote[255] = {};
	snprintf(makernote, 255, "L5MP Gain: %d; Exposure: %d; Focus: %d; Balance: %s", gain, exposure, focus, WB_ILLUMINANTS[wb]);
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
	g_print("Post process %s to %s.ext\n", burst_dir, capture_fname);
	GError *error = NULL;
	GSubprocess *proc = g_subprocess_new(
		G_SUBPROCESS_FLAGS_STDOUT_PIPE,
		&error,
		processing_script,
		burst_dir,
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

	if (captures_remaining > 0) {
		int count = burst_length - captures_remaining;
		--captures_remaining;

		process_image_for_capture(image, count);

		if (captures_remaining == 0) {
			assert(thumb);
			process_capture_burst(thumb);
		} else {
			assert(!thumb);
		}
	} else {
		assert(!thumb);
	}

	free(image->data);

	++frames_processed;
	if (captures_remaining == 0) {
		is_capturing = false;
	}
}

void
mp_process_pipeline_process_image(MPImage image)
{
	// If we haven't processed the previous frame yet, drop this one
	if (frames_received != frames_processed && !is_capturing) {
		//printf("Dropped frame at capture\n");
		free(image.data);
		return;
	}

	++frames_received;

	mp_pipeline_invoke(pipeline, (MPPipelineCallback)process_image, &image,
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

	strcpy(burst_dir, tempdir);

	captures_remaining = burst_length;
}

void
mp_process_pipeline_capture()
{
	is_capturing = true;

	mp_pipeline_invoke(pipeline, capture, NULL, 0);
}

static void
update_state(MPPipeline *pipeline, const struct mp_process_pipeline_state *state)
{
	camera = state->camera;

	burst_length = state->burst_length;

	preview_width = state->preview_width;
	preview_height = state->preview_height;

	// gain_is_manual = state->gain_is_manual;
	gain = state->gain;
	gain_max = state->gain_max;
	gain_min = state->gain_min;

	exposure_is_manual = state->exposure_is_manual;
	exposure = state->exposure;
	exposure_max = state->exposure_max;
	exposure_min = state->exposure_min;

	wb = state->wb;
	focus = state->focus;

	struct mp_main_state main_state = {
		.camera = camera,
		.mode = state->mode,
		.is_present = state->is_present,
		.gain_is_manual = state->gain_is_manual,
		.gain = gain,
		.gain_max = gain_max,
		.gain_min = gain_min,
		.exposure_is_manual = exposure_is_manual,
		.exposure = exposure,
		.exposure_max = exposure_max,
		.exposure_min = exposure_min,
		.has_auto_focus_continuous = state->has_auto_focus_continuous,
		.has_auto_focus_start = state->has_auto_focus_start,
		.focus = focus,
	};
	mp_main_update_state(&main_state);
}

void
mp_process_pipeline_update_state(const struct mp_process_pipeline_state *new_state)
{
	mp_pipeline_invoke(pipeline, (MPPipelineCallback)update_state, new_state,
			   sizeof(struct mp_process_pipeline_state));
}
