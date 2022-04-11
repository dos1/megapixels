#include <inttypes.h>
#include <libcamera/libcamera.h>
#include <memory>
#include <string>
#include <variant>

#include "camera.h"

using namespace libcamera;


// See https://arne-mertz.de/2018/05/modern-c-features-stdvariant-and-stdvisit/
// Variant used here as Result<Object, Error>.
static std::variant<std::tuple<Size, PixelFormat>, int> setMode(std::string desired, uint32_t width, uint32_t height) {
	std::unique_ptr<CameraManager> cm = std::make_unique<CameraManager>();
	cm->start();
	auto camera = cm->get(desired);
	if (!camera) {
		printf("No such camera: %s", desired.c_str());
		return -1;
	}
	camera->acquire();
	std::unique_ptr<CameraConfiguration> config =
		camera->generateConfiguration( { StreamRole::Viewfinder } );
	StreamConfiguration &streamConfig = config->at(0);
	streamConfig.size.width = width;
	streamConfig.size.height = height;
	// FIXME: if validate() modified the config, return the modified one
	config->validate();
	auto ret = camera->configure(config.get());
	camera->release();
	camera.reset();
	cm->stop();
	if (ret) {
		return ret;
	}
	return std::tuple(streamConfig.size, streamConfig.pixelFormat);
}

MPPixelFormat pf_from_format(PixelFormat format) {
	switch (format) {
	case formats::SRGGB8:
		return MP_PIXEL_FMT_RGGB8;
	case formats::SGRBG8:
		return MP_PIXEL_FMT_GRBG8;
	case formats::SGBRG8:
		return MP_PIXEL_FMT_GBRG8;
	case formats::SBGGR8:
		return MP_PIXEL_FMT_BGGR8;
	case formats::SRGGB10:
		return MP_PIXEL_FMT_RGGB10;
	case formats::SGRBG10:
		return MP_PIXEL_FMT_GRBG10;
	case formats::SGBRG10:
		return MP_PIXEL_FMT_GBRG10;
	case formats::SBGGR10:
		return MP_PIXEL_FMT_BGGR10;
	case formats::SRGGB16:
		return MP_PIXEL_FMT_RGGB16;
	case formats::SGRBG16:
		return MP_PIXEL_FMT_GRBG16;
	case formats::SGBRG16:
		return MP_PIXEL_FMT_GBRG16;
	case formats::SBGGR16:
		return MP_PIXEL_FMT_BGGR16;
	default:
		return MP_PIXEL_FMT_UNSUPPORTED;
	}
}

extern "C" {
	/// Attempts to set the mode.
	/// May end up applying a different one (FIXME).
	///
	/// Note that width/height are sent explicitly,
	/// but there's no information conveyed about which pipeline to use:
	/// viewfinder or photo.
	/// It's not a problem yet because Librem 5 pipelines don't make the distinction,
	/// but it will have to get split into two modes,
	/// and possibly 3 if live video is included.
	///
	/// Resolution is how Megapixels communicates its intention,
	/// so that will have to get reworked in the future.
	MPCameraMode set_mode(char id[260], uint32_t width, uint32_t height) {
		printf("Setting mode %d %d\n", width, height);
		auto ret = setMode(id, width, height);
		if (ret.index() == 1) {
			printf("invalid\n");
			return mp_camera_mode_new_invalid();
		} else {
			printf("ok\n");
			auto mode = std::get<0>(ret);
			// This translation is ugly, but C doesn't have sum types.
			// I figured it's easier to use sum types whenever possible,
			// and translate where necessary.
			MPCameraMode selected = {
				.pixel_format = pf_from_format(std::get<1>(mode)),
				.frame_interval = {0, 0}, //unused
				.width = std::get<0>(mode).width,
				.height = std::get<0>(mode).height,
			};
			return selected;
		}
	}
}
