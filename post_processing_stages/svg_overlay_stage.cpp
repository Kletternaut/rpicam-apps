/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * svg_overlay_stage.cpp - Configurable SVG image overlay for YUV420 frames.
 *
 * The SVG is rasterised once during Configure(). Each frame only blends the
 * cached RGBA artwork into the main stream. If a URL is configured, Qt preview
 * builds also receive a matching click target; encoded video frames themselves
 * are, of course, not clickable.
 */

#include <libcamera/stream.h>

#include "core/buffer_sync.hpp"
#include "core/logging.hpp"
#include "core/rpicam_app.hpp"
#include "post_processing_stages/gamepad_overlay_resample.hpp"
#include "post_processing_stages/post_processing_stage.hpp"

#include <cairo/cairo.h>
#include <librsvg/rsvg.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using Stream = libcamera::Stream;

namespace
{
struct OverlayPixel
{
	uint8_t y = 0;
	uint8_t u = 128;
	uint8_t v = 128;
	uint8_t a = 0;
};

static bool isHttpUrl(const std::string &url)
{
	return (url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0) &&
		url.find_first_of(" \t\r\n") == std::string::npos;
}

static std::vector<uint8_t> rasterizeSvg(const std::string &path, int width, int height)
{
	GError *error = nullptr;
	RsvgHandle *handle = rsvg_handle_new_from_file(path.c_str(), &error);
	if (!handle)
	{
		std::string message = error ? error->message : "unknown SVG loading error";
		if (error)
			g_error_free(error);
		throw std::runtime_error("SvgOverlayStage: cannot load '" + path + "': " + message);
	}

	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
	{
		cairo_surface_destroy(surface);
		g_object_unref(handle);
		throw std::runtime_error("SvgOverlayStage: unable to allocate SVG raster surface");
	}
	cairo_t *cr = cairo_create(surface);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	RsvgRectangle viewport{ 0.0, 0.0, double(width), double(height) };
	GError *render_error = nullptr;
	gboolean rendered = rsvg_handle_render_document(handle, cr, &viewport, &render_error);
	cairo_destroy(cr);
	if (!rendered || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
	{
		std::string message = render_error ? render_error->message : "SVG rendering failed";
		if (render_error)
			g_error_free(render_error);
		cairo_surface_destroy(surface);
		g_object_unref(handle);
		throw std::runtime_error("SvgOverlayStage: " + message);
	}
	if (render_error)
		g_error_free(render_error);
	cairo_surface_flush(surface);

	const unsigned char *data = cairo_image_surface_get_data(surface);
	const int stride = cairo_image_surface_get_stride(surface);
	std::vector<uint8_t> rgba(size_t(width) * height * 4);
	for (int y = 0; y < height; ++y)
	{
		for (int x = 0; x < width; ++x)
		{
			const unsigned char *p = data + size_t(y) * stride + size_t(x) * 4;
			const unsigned int a = p[3];
			unsigned int r = p[2], g = p[1], b = p[0];
			// Cairo's ARGB32 uses premultiplied alpha; keep straight RGBA here.
			if (a > 0 && a < 255)
			{
				r = std::min(255U, (r * 255U + a / 2U) / a);
				g = std::min(255U, (g * 255U + a / 2U) / a);
				b = std::min(255U, (b * 255U + a / 2U) / a);
			}
			const size_t index = (size_t(y) * width + x) * 4;
			rgba[index] = uint8_t(r);
			rgba[index + 1] = uint8_t(g);
			rgba[index + 2] = uint8_t(b);
			rgba[index + 3] = uint8_t(a);
		}
	}
	cairo_surface_destroy(surface);
	g_object_unref(handle);
	return rgba;
}

// Aspect ratio (width/height) of the SVG artwork. Falls back to a square when
// the file carries no intrinsic dimensions.
static double svgAspectRatio(const std::string &path)
{
	GError *error = nullptr;
	RsvgHandle *handle = rsvg_handle_new_from_file(path.c_str(), &error);
	if (!handle)
	{
		if (error)
			g_error_free(error);
		return 1.0;
	}
	double aspect = 1.0;
	gboolean has_width = FALSE, has_height = FALSE;
	RsvgLength width = {}, height = {};
	rsvg_handle_get_intrinsic_dimensions(handle, &has_width, &width, &has_height, &height, nullptr, nullptr);
	if (has_width && has_height && width.length > 0 && height.length > 0)
		aspect = width.length / height.length;
	g_object_unref(handle);
	return std::max(aspect, 0.01);
}

class SvgOverlayStage : public PostProcessingStage
{
public:
	explicit SvgOverlayStage(RPiCamApp *app) : PostProcessingStage(app) {}

	char const *Name() const override { return "svg_overlay"; }

	void Read(const boost::property_tree::ptree &params) override
	{
		path_ = params.get<std::string>("svg_path", "");
		if (path_.empty())
			throw std::runtime_error("SvgOverlayStage: 'svg_path' is required");
		x_ = params.get<double>("x", 0.43);
		y_ = params.get<double>("y", 0.02);
		width_ = params.get<double>("width", 0.14);
		opacity_ = params.get<double>("opacity", 1.0);
		url_ = params.get<std::string>("url", "");
		if (params.find("height") != params.not_found())
			LOG(1, "SvgOverlayStage: 'height' is ignored; the logo height follows from the SVG aspect ratio and 'width'");
		auto check = [](double value, double lo, double hi, const char *name) {
			if (!std::isfinite(value) || value < lo || value > hi)
				throw std::runtime_error(std::string("SvgOverlayStage: invalid ") + name);
		};
		check(x_, 0.0, 1.0, "x");
		check(y_, 0.0, 1.0, "y");
		check(width_, 0.01, 1.0, "width");
		check(opacity_, 0.0, 1.0, "opacity");
		if (!url_.empty() && !isHttpUrl(url_))
			throw std::runtime_error("SvgOverlayStage: 'url' must start with http:// or https:// and contain no whitespace");
	}

	void Configure() override
	{
		stream_ = app_->GetMainStream();
		if (!stream_)
			throw std::runtime_error("SvgOverlayStage: no main stream available");
		if (stream_->configuration().pixelFormat != libcamera::formats::YUV420)
			throw std::runtime_error("SvgOverlayStage: only YUV420 main streams are supported");
		info_ = app_->GetStreamInfo(stream_);
		if (!info_.width || !info_.height || !info_.stride)
			throw std::runtime_error("SvgOverlayStage: invalid main stream dimensions");

		// 'width' is the actual on-screen logo width. The height follows from
		// the SVG's own aspect ratio, so the artwork is never letterboxed.
		const double aspect = svgAspectRatio(path_);
		overlay_width_ = std::max(1, int(std::lround(width_ * info_.width)));
		overlay_height_ = std::max(1, int(std::lround(overlay_width_ / aspect)));
		if (overlay_height_ > int(info_.height))
		{
			overlay_height_ = int(info_.height);
			overlay_width_ = std::max(1, int(std::lround(overlay_height_ * aspect)));
		}
		// x/y anchor the logo on the usable range: 0 keeps it flush at the
		// left/top frame edge, 1 flush at the right/bottom edge. Values in
		// between scale linearly, so no combination can leave the frame.
		const int max_left = int(info_.width) - overlay_width_;
		const int max_top = int(info_.height) - overlay_height_;
		left_ = std::clamp(int(std::lround(x_ * max_left)), 0, max_left);
		top_ = std::clamp(int(std::lround(y_ * max_top)), 0, max_top);

		// Rasterise at a reference resolution and reduce with area averaging so
		// thin strokes survive the downscale (same approach as the gamepad stage).
		const int ref_width = std::min(std::max(overlay_width_ * 2, 512), 2048);
		const int ref_height = std::max(1, int(std::lround(ref_width / aspect)));
		const std::vector<uint8_t> ref_rgba = rasterizeSvg(path_, ref_width, ref_height);
		GamepadOverlayResampler resampler;
		const std::vector<uint8_t> &rgba = resampler.resize(ref_rgba.data(), ref_width, ref_height,
																 overlay_width_, overlay_height_, 0, 0,
																 overlay_width_, overlay_height_);
		pixels_.resize(size_t(overlay_width_) * overlay_height_);
		using ColorSpace = libcamera::ColorSpace;
		const ColorSpace cs = info_.colour_space.value_or(ColorSpace::Sycc);
		double kr = 0.299, kb = 0.114;
		if (cs.ycbcrEncoding == ColorSpace::YcbcrEncoding::Rec709)
		{
			kr = 0.2126;
			kb = 0.0722;
		}
		else if (cs.ycbcrEncoding == ColorSpace::YcbcrEncoding::Rec2020)
		{
			kr = 0.2627;
			kb = 0.0593;
		}
		const double kg = 1.0 - kr - kb;
		const bool limited = cs.range == ColorSpace::Range::Limited;
		const double y_scale = limited ? 219.0 / 255.0 : 1.0;
		const double uv_scale = limited ? 224.0 / 255.0 : 1.0;
		const int y_offset = limited ? 16 : 0;
		for (size_t i = 0; i < pixels_.size(); ++i)
		{
			const double r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2];
			const double luma = kr * r + kg * g + kb * b;
			OverlayPixel &pixel = pixels_[i];
			pixel.y = uint8_t(std::clamp(int(std::lround(y_offset + y_scale * luma)), 0, 255));
			pixel.u = uint8_t(std::clamp(int(std::lround(128.0 + uv_scale * (b - luma) / (2.0 * (1.0 - kb)))), 0, 255));
			pixel.v = uint8_t(std::clamp(int(std::lround(128.0 + uv_scale * (r - luma) / (2.0 * (1.0 - kr)))), 0, 255));
			pixel.a = uint8_t(std::lround(rgba[i * 4 + 3] * opacity_));
		}

		if (!app_->SetPreviewClickTarget(double(left_) / info_.width, double(top_) / info_.height,
										 double(overlay_width_) / info_.width, double(overlay_height_) / info_.height,
										 url_) &&
			!url_.empty())
			LOG(1, "SvgOverlayStage: URL click target available only in interactive preview backends");
	}

	void Teardown() override
	{
		if (app_)
			app_->SetPreviewClickTarget(0, 0, 0, 0, "");
	}

	bool Process(CompletedRequestPtr &completed_request) override
	{
		if (!stream_ || pixels_.empty())
			return false;
		auto buffer = completed_request->buffers.find(stream_);
		if (buffer == completed_request->buffers.end())
			return false;
		BufferWriteSync sync(app_, buffer->second);
		const auto &planes = sync.Get();
		if (planes.empty())
			return false;
		// PostProcessor runs one detached thread per frame; serialise the blend
		// so the scratch buffers are never used by two frames at once.
		std::lock_guard<std::mutex> lock(processMutex_);
		composite(planes[0]);
		return false;
	}

private:
	const OverlayPixel &sample(int x, int y) const
	{
		static const OverlayPixel transparent{};
		if (x < left_ || y < top_ || x >= left_ + overlay_width_ || y >= top_ + overlay_height_)
			return transparent;
		return pixels_[size_t(y - top_) * overlay_width_ + (x - left_)];
	}

	void composite(std::span<uint8_t> frame)
	{
		const int fw = int(info_.width), fh = int(info_.height), stride = int(info_.stride);
		const size_t y_size = size_t(fh) * stride;
		const size_t uv_size = size_t(fh / 2) * (stride / 2);
		if (frame.size() < y_size + 2 * uv_size)
			return;
		uint8_t *yPlane = frame.data();
		uint8_t *uPlane = yPlane + y_size;
		uint8_t *vPlane = uPlane + uv_size;
		const int right = std::min(fw, left_ + overlay_width_);
		const int bottom = std::min(fh, top_ + overlay_height_);
		if (right <= left_ || bottom <= top_)
			return;

		// Blend a cached copy of the region and write it back with bulk
		// transfers. Per-pixel read-modify-write on uncached DMA memory is
		// latency-bound and dominates the frame cost; sequential memcpy of
		// whole rows pipelines far better.
		const size_t yw = size_t(right - left_), yh = size_t(bottom - top_);
		regionY_.resize(yw * yh);
		for (int y = 0; y < int(yh); ++y)
			memcpy(regionY_.data() + size_t(y) * yw, yPlane + (size_t(top_) + y) * stride + left_, yw);
		for (int y = 0; y < int(yh); ++y)
		{
			uint8_t *row = regionY_.data() + size_t(y) * yw;
			for (int x = 0; x < int(yw); ++x)
			{
				const OverlayPixel &p = sample(left_ + x, top_ + y);
				if (!p.a)
					continue;
				const unsigned int a = p.a;
				row[x] = uint8_t((static_cast<unsigned int>(row[x]) * (255 - a) + static_cast<unsigned int>(p.y) * a + 127) / 255);
			}
		}
		for (int y = 0; y < int(yh); ++y)
			memcpy(yPlane + (size_t(top_) + y) * stride + left_, regionY_.data() + size_t(y) * yw, yw);

		// Chroma: same cached-region approach for the shared 2x2 samples.
		const int cLeft = (left_ & ~1) / 2, cTop = (top_ & ~1) / 2;
		const int cRight = (right + 1) / 2, cBottom = (bottom + 1) / 2;
		const size_t cw = size_t(cRight - cLeft), ch = size_t(cBottom - cTop);
		regionUV_.resize(cw * ch * 2);
		uint8_t *uDst = regionUV_.data();
		uint8_t *vDst = regionUV_.data() + cw * ch;
		for (int y = 0; y < int(ch); ++y)
		{
			memcpy(uDst + size_t(y) * cw, uPlane + (size_t(cTop) + y) * (stride / 2) + cLeft, cw);
			memcpy(vDst + size_t(y) * cw, vPlane + (size_t(cTop) + y) * (stride / 2) + cLeft, cw);
		}
		for (int y = 0; y < int(ch); ++y)
			for (int x = 0; x < int(cw); ++x)
			{
				unsigned int alphaSum = 0, uAcc = 0, vAcc = 0;
				for (int dy = 0; dy < 2; ++dy)
					for (int dx = 0; dx < 2; ++dx)
					{
						const OverlayPixel &p = sample(2 * (cLeft + x) + dx, 2 * (cTop + y) + dy);
						alphaSum += p.a;
						uAcc += p.a * p.u;
						vAcc += p.a * p.v;
					}
				if (!alphaSum)
					continue;
				const size_t ci = size_t(y) * cw + x;
				uDst[ci] = uint8_t((static_cast<unsigned int>(uDst[ci]) * (1020 - alphaSum) + uAcc + 510) / 1020);
				vDst[ci] = uint8_t((static_cast<unsigned int>(vDst[ci]) * (1020 - alphaSum) + vAcc + 510) / 1020);
			}
		for (int y = 0; y < int(ch); ++y)
		{
			memcpy(uPlane + (size_t(cTop) + y) * (stride / 2) + cLeft, uDst + size_t(y) * cw, cw);
			memcpy(vPlane + (size_t(cTop) + y) * (stride / 2) + cLeft, vDst + size_t(y) * cw, cw);
		}
	}

	std::string path_, url_;
	double x_ = 0.43, y_ = 0.02, width_ = 0.14, opacity_ = 1.0;
	Stream *stream_ = nullptr;
	StreamInfo info_;
	int left_ = 0, top_ = 0, overlay_width_ = 0, overlay_height_ = 0;
	std::vector<OverlayPixel> pixels_;
	std::mutex processMutex_;
	std::vector<uint8_t> regionY_, regionUV_;
};

static PostProcessingStage *Create(RPiCamApp *app)
{
	return new SvgOverlayStage(app);
}

static RegisterStage reg("svg_overlay", &Create);
} // namespace
