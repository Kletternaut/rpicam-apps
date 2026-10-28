/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2026, Kletternaut
 *
 * gamepad_overlay_stage.cpp - Read-only Xbox 360 joydev monitor that renders a
 * live controller overlay on top of the camera frame.
 *
 * This stage is a passive observer of /dev/input/jsX. It never grabs the
 * device, never issues any writing joystick ioctl and never forwards input.
 * It can therefore run side-by-side with other software (e.g. PTSApp+) that
 * uses the same joystick.
 */

#include <libcamera/stream.h>

#include "core/rpicam_app.hpp"
#include "core/logging.hpp"

#include "post_processing_stages/post_processing_stage.hpp"
#include "post_processing_stages/gamepad_overlay_resample.hpp"

#include <linux/joystick.h>

#include <librsvg/rsvg.h>
#include <cairo/cairo.h>

#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

using Stream = libcamera::Stream;

namespace
{
// Artwork keeps its original logical coordinates, but the unused top is
// cropped from the raster and SVG viewBox so the overlay is genuinely shorter.
constexpr int kCanvasW = 2000;
constexpr int kCanvasTop = 144;
constexpr int kCanvasH = 925 - kCanvasTop;

// Defaults, overridable through the stage JSON.
constexpr double kDeadzone = 0.25;
constexpr int kRingLifetimeMs = 820;
constexpr double kOverlayScale = 0.72; // overlay width as a fraction of frame width
constexpr int kReconnectDelayMs = 1500;

constexpr double clamp01(double v)
{
	return std::max(0.0, std::min(1.0, v));
}

// ---------------------------------------------------------------------------
// Minimal RGBA software canvas. No Qt, no OpenGL: everything is rasterised
// directly so the stage stays lightweight and dependency-free.
// ---------------------------------------------------------------------------
struct RGBA
{
	uint8_t r, g, b, a;
};

class Canvas
{
public:
	Canvas(int w, int h, int originY = 0) : w_(w), h_(h), originY_(originY), buf_(size_t(w) * h * 4) {}

	int width() const { return w_; }
	int height() const { return h_; }

	uint8_t *pixels() { return buf_.data(); }
	const uint8_t *pixels() const { return buf_.data(); }

	void clear()
	{
		std::fill(buf_.begin(), buf_.end(), 0);
	}

	// Source-over blend, keeping RGB unassociated (straight alpha), just like
	// loadSvgRgba() and the input expected by the resampler.
	void blend(int x, int y, RGBA c)
	{
		y -= originY_;
		if (x < 0 || y < 0 || x >= w_ || y >= h_)
			return;
		size_t i = (size_t(y) * w_ + x) * 4;
		uint32_t a = c.a;
		if (a == 0)
			return;
		uint32_t ia = 255 - a;
		uint32_t dstWeight = buf_[i + 3] * ia;
		uint32_t outAlpha = a * 255 + dstWeight;
		buf_[i + 0] = uint8_t((c.r * a * 255 + buf_[i + 0] * dstWeight + outAlpha / 2) / outAlpha);
		buf_[i + 1] = uint8_t((c.g * a * 255 + buf_[i + 1] * dstWeight + outAlpha / 2) / outAlpha);
		buf_[i + 2] = uint8_t((c.b * a * 255 + buf_[i + 2] * dstWeight + outAlpha / 2) / outAlpha);
		buf_[i + 3] = uint8_t((outAlpha + 127) / 255);
	}

	void fillCircle(int cx, int cy, int radius, RGBA c)
	{
		// 1px soft edge to keep the circles clean.
		for (int y = cy - radius - 1; y <= cy + radius + 1; ++y)
		{
			for (int x = cx - radius - 1; x <= cx + radius + 1; ++x)
			{
				double dx = x - cx, dy = y - cy;
				double d = dx * dx + dy * dy;
				double coverage = clamp01(radius - std::sqrt(d) + 0.5);
				if (coverage <= 0.0)
					continue;
				RGBA cc = c;
				cc.a = uint8_t(c.a * coverage);
				blend(x, y, cc);
			}
		}
	}

	void fillEllipse(int cx, int cy, int rx, int ry, RGBA c)
	{
		if (rx <= 0 || ry <= 0)
			return;
		for (int y = cy - ry - 1; y <= cy + ry + 1; ++y)
		{
			for (int x = cx - rx - 1; x <= cx + rx + 1; ++x)
			{
				double nx = double(x - cx) / rx, ny = double(y - cy) / ry;
				double coverage = clamp01((1.0 - std::sqrt(nx * nx + ny * ny)) * std::min(rx, ry) + 0.5);
				if (coverage <= 0.0)
					continue;
				RGBA cc = c;
				cc.a = uint8_t(c.a * coverage);
				blend(x, y, cc);
			}
		}
	}

	void strokeEllipse(int cx, int cy, int rx, int ry, int thickness, RGBA c)
	{
		if (rx <= 0 || ry <= 0 || thickness <= 0)
			return;
		int border = (thickness + 1) / 2 + 1;
		for (int y = cy - ry - border; y <= cy + ry + border; ++y)
			for (int x = cx - rx - border; x <= cx + rx + border; ++x)
			{
				double nx = double(x - cx) / rx, ny = double(y - cy) / ry;
				double distance = std::abs(std::sqrt(nx * nx + ny * ny) - 1.0) * std::min(rx, ry);
				double coverage = clamp01(thickness * 0.5 - distance + 0.5);
				if (coverage <= 0.0)
					continue;
				RGBA cc = c;
				cc.a = uint8_t(c.a * coverage);
				blend(x, y, cc);
			}
	}

	void strokeCircle(int cx, int cy, int radius, int thickness, RGBA c)
	{
		double outer = radius + thickness * 0.5;
		double inner = radius - thickness * 0.5;
		for (int y = cy - int(outer) - 1; y <= cy + int(outer) + 1; ++y)
		{
			for (int x = cx - int(outer) - 1; x <= cx + int(outer) + 1; ++x)
			{
				double d = std::sqrt(double(x - cx) * (x - cx) + double(y - cy) * (y - cy));
				double coverage = clamp01(std::min(d - inner, outer - d) + 0.5);
				if (coverage <= 0.0)
					continue;
				RGBA cc = c;
				cc.a = uint8_t(c.a * coverage);
				blend(x, y, cc);
			}
		}
	}

	void fillRect(int x, int y, int w, int h, RGBA c)
	{
		for (int yy = y; yy < y + h; ++yy)
			for (int xx = x; xx < x + w; ++xx)
				blend(xx, yy, c);
	}

	void fillRoundedRect(int x, int y, int w, int h, int radius, RGBA c)
	{
		int r = std::min(radius, std::min(w, h) / 2);
		fillRect(x + r, y, w - 2 * r, h, c);
		fillRect(x, y + r, w, h - 2 * r, c);
		fillCircle(x + r, y + r, r, c);
		fillCircle(x + w - r - 1, y + r, r, c);
		fillCircle(x + r, y + h - r - 1, r, c);
		fillCircle(x + w - r - 1, y + h - r - 1, r, c);
	}

	void strokeRoundedRect(int x, int y, int w, int h, int radius, int thickness, RGBA c)
	{
		fillRoundedRect(x, y, w, h, radius, c);
		// Punch out the interior by re-filling with a transparent colour is not
		// possible without a source-over; approximate with four edge segments.
		(void)thickness;
		RGBA hole{0, 0, 0, 0};
		int t = std::max(1, thickness);
		fillRoundedRect(x + t, y + t, w - 2 * t, h - 2 * t, std::max(1, radius - t), hole);
	}

	void fillTriangle(int x1, int y1, int x2, int y2, int x3, int y3, RGBA c)
	{
		int minx = std::max(0, std::min({x1, x2, x3}));
		int maxx = std::min(w_ - 1, std::max({x1, x2, x3}));
		int miny = std::max(originY_, std::min({y1, y2, y3}));
		int maxy = std::min(originY_ + h_ - 1, std::max({y1, y2, y3}));
		for (int y = miny; y <= maxy; ++y)
		{
			for (int x = minx; x <= maxx; ++x)
			{
				if (insideTriangle(x, y, x1, y1, x2, y2, x3, y3))
					blend(x, y, c);
			}
		}
	}

private:
	static bool insideTriangle(int px, int py, int ax, int ay, int bx, int by, int cx, int cy)
	{
		auto sign = [](int px, int py, int ax, int ay, int bx, int by) {
			return (px - bx) * (ay - by) - (ax - bx) * (py - by);
		};
		int d1 = sign(px, py, ax, ay, bx, by);
		int d2 = sign(px, py, bx, by, cx, cy);
		int d3 = sign(px, py, cx, cy, ax, ay);
		bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
		bool pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
		return !(neg && pos);
	}

	int w_, h_, originY_;
	std::vector<uint8_t> buf_;
};

// Store only a layer's alpha bounding box, in the same logical coordinates
// used by the live drawing. Shoulder masks are a few hundred pixels wide.
struct OverlayMask
{
	int x = 0, y = 0, width = 0, height = 0;
	std::vector<uint8_t> alpha;

	static OverlayMask fromRgba(const std::vector<uint8_t> &rgba, int w, int h, int originY)
	{
		OverlayMask mask;
		if (rgba.size() != size_t(w) * h * 4)
			return mask;
		int left = w, top = h, right = 0, bottom = 0;
		for (int yy = 0; yy < h; ++yy)
			for (int xx = 0; xx < w; ++xx)
				if (rgba[(size_t(yy) * w + xx) * 4 + 3])
				{
					left = std::min(left, xx);
					top = std::min(top, yy);
					right = std::max(right, xx + 1);
					bottom = std::max(bottom, yy + 1);
				}
		if (right <= left || bottom <= top)
			return mask;
		mask.x = left;
		mask.y = top + originY;
		mask.width = right - left;
		mask.height = bottom - top;
		mask.alpha.resize(size_t(mask.width) * mask.height);
		for (int yy = 0; yy < mask.height; ++yy)
			for (int xx = 0; xx < mask.width; ++xx)
				mask.alpha[size_t(yy) * mask.width + xx] = rgba[(size_t(top + yy) * w + left + xx) * 4 + 3];
		return mask;
	}

	void draw(Canvas &cv, RGBA colour, double amount = 1.0) const
	{
		const double filledWidth = width * clamp01(amount);
		for (int yy = 0; yy < height; ++yy)
			for (int xx = 0; xx < width && xx < filledWidth; ++xx)
			{
				RGBA c = colour;
				c.a = uint8_t(std::lround(colour.a * (alpha[size_t(yy) * width + xx] / 255.0) *
										clamp01(filledWidth - xx)));
				cv.blend(x + xx, y + yy, c);
			}
	}
};

// ---------------------------------------------------------------------------
// Tiny 5x7 bitmap font for the handful of glyphs the overlay needs.
// ---------------------------------------------------------------------------
static const uint8_t *glyph5x7(char c, uint8_t out[7])
{
	static const uint8_t glyphs[][7] = {
		// A B C D E F G H I J K L M N O P Q R S T U V W X Y Z
		{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, // A
		{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, // B
		{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, // C
		{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, // D
		{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, // E
		{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, // F
		{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, // G
		{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, // H
		{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, // I
		{0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, // J
		{0x11,0x12,0x14,0x18,0x14,0x12,0x11}, // K
		{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, // L
		{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, // M
		{0x11,0x11,0x19,0x15,0x13,0x11,0x11}, // N
		{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, // O
		{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, // P
		{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, // Q
		{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, // R
		{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, // S
		{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, // T
		{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, // U
		{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, // V
		{0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, // W
		{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, // X
		{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, // Y
		{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, // Z
		{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
		{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
		{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, // 2
		{0x0E,0x11,0x01,0x06,0x01,0x11,0x0E}, // 3
		{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
		{0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}, // 5
		{0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E}, // 6
		{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 7
		{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
		{0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}, // 9
	};
	static const uint8_t dash[7] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00};
	static const uint8_t dot[7]  = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C};
	static const uint8_t colon[7]= {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00};
	static const uint8_t slash[7]= {0x01,0x02,0x04,0x08,0x10,0x00,0x00};
	static const uint8_t pct[7]  = {0x19,0x1A,0x02,0x04,0x08,0x0B,0x13};
	static const uint8_t mid[7]  = {0x00,0x00,0x00,0x00,0x00,0x06,0x06};

	memset(out, 0, 7);
	if (c >= 'A' && c <= 'Z')
		memcpy(out, glyphs[c - 'A'], 7);
	else if (c >= '0' && c <= '9')
		memcpy(out, glyphs[26 + (c - '0')], 7);
	else if (c == '-')
		memcpy(out, dash, 7);
	else if (c == '.')
		memcpy(out, dot, 7);
	else if (c == ':')
		memcpy(out, colon, 7);
	else if (c == '/')
		memcpy(out, slash, 7);
	else if (c == '%')
		memcpy(out, pct, 7);
	else if (c == 0xb7) // middle dot
		memcpy(out, mid, 7);
	return out;
}

static void drawText(Canvas &cv, int x, int y, double scale, RGBA c, const std::string &text)
{
	if (!std::isfinite(scale) || scale <= 0.0)
		return;
	double px = x;
	for (char ch : text)
	{
		if (ch == ' ')
		{
			px += 3 * scale;
			continue;
		}
		uint8_t g[7];
		glyph5x7(ch, g);
		// Sum the covered glyph-cell areas before blending each destination
		// pixel. This supports fractional sizes without seams between cells.
		for (int yy = y; yy < int(std::ceil(y + 7 * scale)); ++yy)
			for (int xx = int(std::floor(px)); xx < int(std::ceil(px + 5 * scale)); ++xx)
			{
				const int firstRow = std::max(0, int(std::floor((yy - y) / scale)));
				const int lastRow = std::min(6, int(std::floor((yy + 1 - y) / scale)));
				const int firstCol = std::max(0, int(std::floor((xx - px) / scale)));
				const int lastCol = std::min(4, int(std::floor((xx + 1 - px) / scale)));
				double coverage = 0.0;
				for (int row = firstRow; row <= lastRow; ++row)
					for (int col = firstCol; col <= lastCol; ++col)
						if (g[row] & (1 << (4 - col)))
						{
							const double dx = std::max(0.0, std::min(double(xx + 1), px + (col + 1) * scale) -
													 std::max(double(xx), px + col * scale));
							const double dy = std::max(0.0, std::min(double(yy + 1), y + (row + 1) * scale) -
													 std::max(double(yy), y + row * scale));
							coverage += dx * dy;
						}
				if (coverage > 0.0)
				{
					RGBA pixel = c;
					pixel.a = uint8_t(std::lround(c.a * clamp01(coverage)));
					cv.blend(xx, yy, pixel);
				}
			}
		px += 6 * scale;
	}
}

static int textWidth(double scale, const std::string &text)
{
	int w = 0;
	for (char ch : text)
		w += (ch == ' ') ? 3 : 6;
	return int(std::ceil(w * scale));
}

// Button hot-spots on the logical canvas (xpad driver joydev indices).
// Coordinates are in the 2000x925 SVG viewBox space.
static void buttonPosition(int number, int &x, int &y)
{
	struct P { int n, x, y; };
	static const P positions[] = {
		{0, 1351, 541},  // A
		{1, 1448, 461},  // B
		{2, 1258, 471},  // X
		{3, 1358, 393},  // Y
		{4, 656, 264},   // LB
		{5, 1344, 264},  // RB, mirrored about the shell centre x=1000
		{6, 887, 477},   // Back
		{7, 1143, 477},  // Start
		{8, 1003, 475},  // Guide
		{9, 648, 486},   // Left stick click
		{10, 1163, 633}, // Right stick click
		{11, 825, 582},  // D-pad up
		{12, 825, 690},  // D-pad down
		{13, 773, 636},  // D-pad left
		{14, 883, 636},  // D-pad right
	};
	for (const P &p : positions)
	{
		if (p.n == number)
		{
			x = p.x;
			y = p.y;
			return;
		}
	}
	x = 1000;
	y = 460;
}

struct RGB
{
	uint8_t r, g, b;
};

static RGB buttonColour(int number)
{
	switch (number)
	{
	case 0: return {65, 220, 130};
	case 1: return {255, 92, 104};
	case 2: return {77, 164, 255};
	case 3: return {255, 205, 75};
	default: return {73, 226, 220};
	}
}

struct RingPulse
{
	int centerX, centerY;
	std::string label;
	RGB colour;
	int64_t startedMs;
	bool stickInput = false;
};

// Rasterise an SVG file into a straight-alpha RGBA buffer of the given size
// using librsvg + cairo. Returns an empty vector on failure.
static std::vector<uint8_t> loadSvgRgba(const std::string &path, int w, int h, const char *layer = nullptr)
{
	GError *err = nullptr;
	RsvgHandle *handle = rsvg_handle_new_from_file(path.c_str(), &err);
	if (!handle)
	{
		if (err)
		{
			LOG(1, "GamepadOverlayStage: cannot load SVG '" << path << "': " << err->message);
			g_error_free(err);
		}
		return {};
	}

	// A cropped viewBox and the original 2000x925 artwork must map the same
	// logical control coordinates to the same pixels, without letterboxing.
	RsvgRectangle viewport = {0.0, -double(kCanvasTop), double(w), double(h + kCanvasTop)};
	gboolean hasViewBox = false;
	RsvgRectangle viewBox{};
	rsvg_handle_get_intrinsic_dimensions(handle, nullptr, nullptr, nullptr, nullptr, &hasViewBox, &viewBox);
	if (hasViewBox && std::abs(viewBox.width - kCanvasW) < 0.01 && viewBox.height > 0)
		viewport = {viewBox.x, viewBox.y - kCanvasTop, viewBox.width, viewBox.height};
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_t *cr = cairo_create(surface);

	GError *renderErr = nullptr;
	bool ok = layer ? rsvg_handle_render_layer(handle, cr, layer, &viewport, &renderErr)
					: rsvg_handle_render_document(handle, cr, &viewport, &renderErr);
	if (renderErr)
	{
		LOG(1, "GamepadOverlayStage: SVG render failed: " << renderErr->message);
		g_error_free(renderErr);
	}
	if (!ok)
	{
		cairo_destroy(cr);
		cairo_surface_destroy(surface);
		g_object_unref(handle);
		return {};
	}

	cairo_surface_flush(surface);
	const unsigned char *data = cairo_image_surface_get_data(surface);
	const int stride = cairo_image_surface_get_stride(surface);

	// ARGB32 is premultiplied with BGRA byte order on little-endian.
	std::vector<uint8_t> rgba(size_t(w) * h * 4);
	for (int y = 0; y < h; ++y)
	{
		for (int x = 0; x < w; ++x)
		{
			const unsigned char *p = data + size_t(y) * stride + size_t(x) * 4;
			unsigned char b = p[0], g = p[1], r = p[2], a = p[3];
			if (a > 0 && a < 255)
			{
				r = uint8_t((int(r) * 255) / a);
				g = uint8_t((int(g) * 255) / a);
				b = uint8_t((int(b) * 255) / a);
			}
			uint8_t *out = &rgba[(size_t(y) * w + x) * 4];
			out[0] = r;
			out[1] = g;
			out[2] = b;
			out[3] = a;
		}
	}

	cairo_destroy(cr);
	cairo_surface_destroy(surface);
	g_object_unref(handle);
	return rgba;
}

} // namespace

class GamepadOverlayStage : public PostProcessingStage
{
public:
	GamepadOverlayStage(RPiCamApp *app) : PostProcessingStage(app) {}

	~GamepadOverlayStage() override { Stop(); }

	char const *Name() const override { return "gamepad_overlay"; }

	void Read(boost::property_tree::ptree const &params) override
	{
		devicePath_ = params.get<std::string>("device", "/dev/input/js0");
		deadzone_ = params.get<double>("deadzone", kDeadzone);
		ringLifetimeMs_ = params.get<int>("ring_lifetime_ms", kRingLifetimeMs);
		overlayScale_ = params.get<double>("scale", kOverlayScale);
		reconnectDelayMs_ = params.get<int>("reconnect_delay_ms", kReconnectDelayMs);
		position_ = std::clamp(params.get<int>("position", 8), 1, 9);
		showStatus_ = params.get<bool>("show_status", true);
		holdPulseMs_ = std::max(50, params.get<int>("hold_pulse_ms", 400));
		offsetX_ = params.get<int>("offset_x", 0);
		offsetY_ = params.get<int>("offset_y", 0);
		svgPath_ = params.get<std::string>("svg_path", "/usr/local/share/rpi-camera-assets/open_gamepad.svg");
	}

	void Configure() override
	{
		stream_ = app_->GetMainStream();
		if (!stream_ || stream_->configuration().pixelFormat != libcamera::formats::YUV420)
			throw std::runtime_error("GamepadOverlayStage: only YUV420 format supported");
		info_ = app_->GetStreamInfo(stream_);
		canvas_ = std::make_unique<Canvas>(kCanvasW, kCanvasH, kCanvasTop);

		svgPixels_ = loadSvgRgba(svgPath_, kCanvasW, kCanvasH);
		if (svgPixels_.empty())
			LOG(1, "GamepadOverlayStage: SVG '" << svgPath_ << "' not loaded; falling back to vector drawing");
		const char *shoulders[] = {"#button-lb", "#button-rb"};
		const char *triggers[] = {"#trigger-left-meter", "#trigger-right-meter"};
		for (int i = 0; i < 2; ++i)
		{
			shoulderMasks_[i] = OverlayMask::fromRgba(loadSvgRgba(svgPath_, kCanvasW, kCanvasH, shoulders[i]),
													 kCanvasW, kCanvasH, kCanvasTop);
			triggerMasks_[i] = OverlayMask::fromRgba(loadSvgRgba(svgPath_, kCanvasW, kCanvasH, triggers[i]),
													kCanvasW, kCanvasH, kCanvasTop);
		}
	}

	void Start() override
	{
		running_ = true;
		openDevice();
		readerThread_ = std::thread(&GamepadOverlayStage::readerLoop, this);
	}

	void Stop() override
	{
		running_ = false;
		if (readerThread_.joinable())
			readerThread_.join();
		closeDevice("Stopped");
	}

	void Teardown() override
	{
		Stop();
	}

	bool Process(CompletedRequestPtr &completed_request) override
	{
		if (!stream_ || !canvas_)
			return false;

		// PostProcessor::Process() runs stage->Process() in one detached
		// thread per completed request, so multiple frames can be processed
		// concurrently. Serialise access to the render canvas and the snapshot
		// state to avoid data races / use-after-free.
		std::lock_guard<std::mutex> processLock(processMutex_);

		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			axisValues_ = axisValuesShared_;
			buttonValues_ = buttonValuesShared_;
			rings_ = ringsShared_;
			deviceName_ = deviceNameShared_;
			errorText_ = errorTextShared_;
			connected_ = fd_ >= 0;
		}

		BufferWriteSync w(app_, completed_request->buffers[stream_]);
		std::span<uint8_t> buffer = w.Get()[0];

		renderOverlay();
		composite(buffer.data());

		return false;
	}

private:
	// -- Joystick handling -------------------------------------------------

	void openDevice()
	{
		int fd = ::open(devicePath_.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
		{
			setError("Cannot open " + devicePath_ + ": " + std::strerror(errno));
			return;
		}

		unsigned char axes = 0, buttons = 0;
		if (::ioctl(fd, JSIOCGAXES, &axes) < 0 || ::ioctl(fd, JSIOCGBUTTONS, &buttons) < 0)
		{
			setError("Cannot query joystick: " + std::string(std::strerror(errno)));
			::close(fd);
			return;
		}

		char name[256] = {};
		std::string devName = "Xbox 360 Wireless Receiver";
		if (::ioctl(fd, JSIOCGNAME(sizeof(name)), name) >= 0 && name[0] != '\0')
			devName = name;

		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			fd_ = fd;
			axisValuesShared_.assign(axes, 0);
			buttonValuesShared_.assign(buttons, false);
			deviceNameShared_ = devName;
			errorTextShared_.clear();
		}
	}

	void closeDevice(const std::string &reason)
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		if (fd_ >= 0)
		{
			::close(fd_);
			fd_ = -1;
		}
		axisValuesShared_.clear();
		buttonValuesShared_.clear();
		errorTextShared_ = reason;
	}

	void setError(const std::string &text)
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		errorTextShared_ = text;
	}

	void readerLoop()
	{
		while (running_)
		{
			int fd = currentFd();
			if (fd < 0)
			{
				// Wait then try to reconnect.
				poll(nullptr, 0, reconnectDelayMs_);
				openDevice();
				continue;
			}

			struct pollfd pfd = {fd, POLLIN, 0};
			int rc = ::poll(&pfd, 1, 200);
			if (rc < 0)
			{
				if (errno == EINTR)
					continue;
				closeDevice("Poll failed: " + std::string(std::strerror(errno)));
				continue;
			}
			if (rc == 0)
			{
				emitHoldPulses();
				continue;
			}
			if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
			{
				closeDevice("Joystick disconnected");
				continue;
			}
			if (pfd.revents & POLLIN)
				readEvents(fd);
		}
	}

	int currentFd()
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		return fd_;
	}

	void readEvents(int fd)
	{
		for (;;)
		{
			js_event event{};
			ssize_t n = ::read(fd, &event, sizeof(event));
			if (n == ssize_t(sizeof(event)))
			{
				const bool initial = (event.type & JS_EVENT_INIT) != 0;
				const unsigned char type = event.type & ~JS_EVENT_INIT;

				std::lock_guard<std::mutex> lock(stateMutex_);
				if (type == JS_EVENT_BUTTON)
				{
					if (event.number < buttonValuesShared_.size())
					{
						bool pressed = event.value != 0;
						if (!initial && pressed && !buttonValuesShared_[event.number])
							addRingLocked(event.number, -1);
						buttonValuesShared_[event.number] = pressed;
					}
				}
				else if (type == JS_EVENT_AXIS)
				{
					if (event.number < axisValuesShared_.size())
					{
						double before = normalise(axisValuesShared_[event.number]);
						axisValuesShared_[event.number] = event.value;
						double after = normalise(axisValuesShared_[event.number]);
						if (!initial)
							handleAxisTransitionLocked(event.number, before, after);
					}
				}
				continue;
			}
			if (n < 0 && errno == EINTR)
				continue;
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
				return;
			if (n == 0)
			{
				closeDevice("Joystick disconnected");
				return;
			}
			closeDevice("Incomplete joystick event");
			return;
		}
	}

	static double normalise(int value)
	{
		return std::max(-1.0, std::min(1.0, value / 32767.0));
	}

	// Emit a periodic ring pulse for every button and axis that is still being
	// held, so the animation keeps running while the input remains engaged.
	// joydev only reports *changes*, so a button or stick held at a constant
	// value produces no further events. Called from the reader thread's poll
	// timeout branch (holds stateMutex_).
	void emitHoldPulses()
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		int64_t now = nowMs();

		// Buttons.
		if (lastHoldPulseMs_.size() != buttonValuesShared_.size())
			lastHoldPulseMs_.assign(buttonValuesShared_.size(), 0);
		for (size_t i = 0; i < buttonValuesShared_.size(); ++i)
		{
			if (buttonValuesShared_[i])
			{
				if (lastHoldPulseMs_[i] == 0)
				{
					// First observation of the held state: the press transition
					// already emitted a pulse, so only arm the timer here.
					lastHoldPulseMs_[i] = now;
				}
				else if (now - lastHoldPulseMs_[i] >= holdPulseMs_)
				{
					lastHoldPulseMs_[i] = now;
					addRingLocked(int(i), -1);
				}
			}
			else
			{
				lastHoldPulseMs_[i] = 0;
			}
		}

		// Axes (sticks, triggers, D-pad hat axes).
		if (lastHoldPulseAxisMs_.size() != axisValuesShared_.size())
			lastHoldPulseAxisMs_.assign(axisValuesShared_.size(), 0);
		for (size_t i = 0; i < axisValuesShared_.size(); ++i)
		{
			if (axisActiveShared(int(i)))
			{
				if (lastHoldPulseAxisMs_[i] == 0)
				{
					// First observation of the active state: the activation
					// transition already emitted a pulse, so arm the timer.
					lastHoldPulseAxisMs_[i] = now;
				}
				else if (now - lastHoldPulseAxisMs_[i] >= holdPulseMs_)
				{
					lastHoldPulseAxisMs_[i] = now;
					addRingLocked(-1, int(i));
				}
			}
			else
			{
				lastHoldPulseAxisMs_[i] = 0;
			}
		}
	}

	void addRingLocked(int buttonId, int axisId)
	{
		int x, y;
		if (axisId >= 0)
			axisHotspot(axisId, x, y);
		else
			buttonPosition(buttonId, x, y);

		std::string label = axisId >= 0 ? "AX" + std::to_string(axisId) : "B" + std::to_string(buttonId);
		RGB colour = axisId >= 0 ? RGB{83, 221, 228} : buttonColour(buttonId);
		bool stickInput = axisId == 0 || axisId == 1 || axisId == 3 || axisId == 4 ||
						  (axisId < 0 && (buttonId == 9 || buttonId == 10));
		ringsShared_.push_back(RingPulse{x, y, label, colour, nowMs(), stickInput});
	}

	void handleAxisTransitionLocked(int id, double before, double after)
	{
		if (id == 2 || id == 5) // triggers
		{
			double oldP = (before + 1.0) * 0.5;
			double newP = (after + 1.0) * 0.5;
			if (oldP < deadzone_ && newP >= deadzone_)
				addRingLocked(-1, id);
			return;
		}

		if (id == 0 || id == 1 || id == 3 || id == 4 || id == 6 || id == 7)
		{
			bool nowActive = std::abs(after) >= deadzone_;
			bool prevActive = std::abs(before) >= deadzone_;
			bool changedSide = (before < -deadzone_ && after > deadzone_) ||
							   (before > deadzone_ && after < -deadzone_);
			if (nowActive && (!prevActive || changedSide))
				addRingLocked(-1, id);
			return;
		}

		bool nowActive = std::abs(after) >= deadzone_;
		bool prevActive = std::abs(before) >= deadzone_;
		if (nowActive && !prevActive)
			addRingLocked(-1, id);
	}

	void axisHotspot(int id, int &x, int &y)
	{
		switch (id)
		{
		case 0:
		case 1: x = 648; y = 486; break;  // left stick
		case 3:
		case 4: x = 1163; y = 633; break; // right stick
		case 2: x = 675; y = 200; break;  // shortened left trigger
		case 5: x = 1325; y = 200; break; // mirrored shortened right trigger
		case 6: x = axisValueShared(id) < 0 ? 773 : 883; y = 636; break; // D-pad L/R
		case 7: x = 825; y = axisValueShared(id) < 0 ? 582 : 690; break;  // D-pad U/D
		default: x = 1000; y = 460; break;
		}
	}

	int axisValueShared(int id) const
	{
		return (id >= 0 && id < int(axisValuesShared_.size())) ? axisValuesShared_[id] : 0;
	}

	// Whether an axis is outside its deadzone (i.e. actively engaged).
	bool axisActiveShared(int id) const
	{
		if (id < 0 || id >= int(axisValuesShared_.size()))
			return false;
		double v = normalise(axisValuesShared_[id]);
		if (id == 2 || id == 5) // triggers
			return ((v + 1.0) * 0.5) >= deadzone_;
		return std::abs(v) >= deadzone_;
	}

	int axisValue(int id) const
	{
		return (id >= 0 && id < int(axisValues_.size())) ? axisValues_[id] : 0;
	}

	bool buttonPressed(int id) const
	{
		return id >= 0 && id < int(buttonValues_.size()) && buttonValues_[id];
	}

	double triggerPressure(int axisId) const
	{
		if (axisId < 0 || axisId >= int(axisValues_.size()))
			return 0.0;
		return clamp01((normalise(axisValues_[axisId]) + 1.0) * 0.5);
	}

	void stickPosition(int xAxis, int yAxis, int &x, int &y) const
	{
		double nx = normalise(axisValue(xAxis));
		double ny = normalise(axisValue(yAxis));
		// Joydev axes can reach both extrema simultaneously. Limit the visual
		// travel radially so the projected cap stays inside its elliptical well.
		double length = std::hypot(nx, ny);
		if (length > 1.0)
		{
			nx /= length;
			ny /= length;
		}
		int bx = (xAxis == 0) ? 648 : 1163;
		int by = (xAxis == 0) ? 486 : 633;
		x = int(bx + nx * 20.0);
		y = int(by + ny * 15.0); // Foreshortened travel in the oblique artwork.
	}

	bool dpadAxisActive(int buttonId) const
	{
		double x = normalise(axisValue(6));
		double y = normalise(axisValue(7));
		switch (buttonId)
		{
		case 11: return y < -deadzone_;
		case 12: return y > deadzone_;
		case 13: return x < -deadzone_;
		case 14: return x > deadzone_;
		default: return false;
		}
	}

	static int64_t nowMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
				   std::chrono::steady_clock::now().time_since_epoch())
			.count();
	}

	// -- Rendering ---------------------------------------------------------

	void renderOverlay()
	{
		Canvas &cv = *canvas_;
		cv.clear();

		// Base artwork: rasterised SVG, or a minimal fallback silhouette.
		if (!svgPixels_.empty())
			memcpy(cv.pixels(), svgPixels_.data(), svgPixels_.size());
		else
			drawControllerFallback(cv);

		int64_t now = nowMs();
		rings_.erase(std::remove_if(rings_.begin(), rings_.end(),
									[&](const RingPulse &r) { return now - r.startedMs > ringLifetimeMs_ + 360; }),
					 rings_.end());

		drawHighlights(cv);
		drawRings(cv, now);
		drawStickDirection(cv, 0, 1);
		drawStickDirection(cv, 3, 4);
		if (showStatus_)
			drawStatus(cv);
	}

	void drawControllerFallback(Canvas &cv)
	{
		// Minimal silhouette used only when the SVG asset is missing.
		cv.fillRoundedRect(400, 120, 1200, 560, 120, RGBA{37, 49, 65, 244});
	}

	void drawHighlights(Canvas &cv)
	{
		for (int id = 0; id <= 3; ++id)
			drawFaceHighlight(cv, id);
		drawShoulderHighlight(cv, 4);
		drawShoulderHighlight(cv, 5);
		drawTriggerFill(cv, 2, 675, 200);
		drawTriggerFill(cv, 5, 1325, 200);
		drawDpadHighlight(cv);

		int sx, sy;
		stickPosition(0, 1, sx, sy);
		drawStickCap(cv, sx, sy, buttonPressed(9));
		stickPosition(3, 4, sx, sy);
		drawStickCap(cv, sx, sy, buttonPressed(10));

		drawSystemHighlight(cv, 6);
		drawSystemHighlight(cv, 7);
		drawSystemHighlight(cv, 8);
	}

	void drawFaceHighlight(Canvas &cv, int id)
	{
		if (!buttonPressed(id))
			return;
		int x, y;
		buttonPosition(id, x, y);
		RGB col = buttonColour(id);
		cv.fillEllipse(x, y, 44, 34, RGBA{col.r, col.g, col.b, 170});
		cv.strokeEllipse(x, y, 44, 34, 4, RGBA{col.r, col.g, col.b, 255});
	}

	void drawShoulderHighlight(Canvas &cv, int id)
	{
		if (!buttonPressed(id))
			return;
		const OverlayMask &mask = shoulderMasks_[id == 4 ? 0 : 1];
		if (!mask.alpha.empty())
		{
			mask.draw(cv, RGBA{66, 205, 190, 200});
			return;
		}
		int x, y;
		buttonPosition(id, x, y);
		cv.fillEllipse(x, y, 85, 22, RGBA{66, 205, 190, 200});
	}

	void drawTriggerFill(Canvas &cv, int axisId, int x, int y)
	{
		double amount = triggerPressure(axisId);
		if (amount <= 0.0)
			return;
		const OverlayMask &mask = triggerMasks_[axisId == 2 ? 0 : 1];
		if (!mask.alpha.empty())
			mask.draw(cv, RGBA{244, 160, 60, 220}, amount);
		else
			cv.fillRect(x - 24, y - 6, int(48 * amount), 12, RGBA{244, 160, 60, 220});
	}

	void drawDpadHighlight(Canvas &cv)
	{
		const int ids[] = {11, 12, 13, 14};
		for (int id : ids)
		{
			if (!(buttonPressed(id) || dpadAxisActive(id)))
				continue;
			int x, y;
			buttonPosition(id, x, y);
			cv.fillEllipse(x, y, 20, 15, RGBA{40, 186, 171, 200});
		}
	}

	void drawStickCap(Canvas &cv, int mx, int my, bool clicked)
	{
		// Projected rubber cap: visible lower edge and an oval top surface.
		cv.fillEllipse(mx, my + 6, 63, 48, RGBA{12, 15, 18, 255});
		cv.fillEllipse(mx, my, 62, 47, clicked ? RGBA{81, 225, 201, 255} : RGBA{65, 70, 76, 255});
		cv.strokeEllipse(mx, my, 56, 41, 3, clicked ? RGBA{163, 255, 221, 230} : RGBA{114, 122, 129, 220});
		cv.fillEllipse(mx, my, 46, 33, RGBA{29, 33, 38, 255});
	}

	void drawSystemHighlight(Canvas &cv, int id)
	{
		if (!buttonPressed(id))
			return;
		int x, y;
		buttonPosition(id, x, y);
		if (id == 8)
			cv.fillEllipse(x, y, 41, 35, RGBA{68, 219, 204, 200});
		else
			cv.fillEllipse(x, y, 30, 20, RGBA{42, 184, 166, 200});
	}

	void drawStickDirection(Canvas &cv, int xAxis, int yAxis) const
	{
		double nx = normalise(axisValue(xAxis));
		double ny = normalise(axisValue(yAxis));
		if (std::max(std::abs(nx), std::abs(ny)) <= deadzone_)
			return;
		const double length = std::hypot(nx, ny);
		if (length == 0.0)
			return;
		nx /= length;
		ny /= length;
		int mx, my;
		stickPosition(xAxis, yAxis, mx, my);
		// Rotate in the stick's plane, then project vertically like its cap.
		// The arrow remains inside the dark top face at every direction.
		auto project = [&](double along, double across, int &x, int &y) {
			x = int(std::lround(mx + along * nx - across * ny));
			y = int(std::lround(my + 0.75 * (along * ny + across * nx)));
		};
		int tipX, tipY, leftX, leftY, rightX, rightY;
		project(34, 0, tipX, tipY);
		project(8, 16, leftX, leftY);
		project(8, -16, rightX, rightY);
		const RGBA colour{141, 255, 237, 255};
		cv.fillTriangle(tipX, tipY, leftX, leftY, rightX, rightY, colour);
		int ax, ay, bx, by, cx, cy, dx, dy;
		project(-25, -5, ax, ay);
		project(8, -5, bx, by);
		project(8, 5, cx, cy);
		project(-25, 5, dx, dy);
		cv.fillTriangle(ax, ay, bx, by, cx, cy, colour);
		cv.fillTriangle(ax, ay, cx, cy, dx, dy, colour);
	}

	void drawRings(Canvas &cv, int64_t now)
	{
		for (const RingPulse &ring : rings_)
		{
			int64_t age = now - ring.startedMs;
			for (int wave = 0; wave < 3; ++wave)
			{
				double progress = (age - wave * 135.0) / double(ringLifetimeMs_);
				if (progress < 0.0 || progress > 1.0)
					continue;
				double radius = 32.0 + progress * 184.0;
				int alpha = int((1.0 - progress) * 210.0);
				int thickness = std::max(2, int(6.0 - progress * 4.0));
				cv.strokeCircle(ring.centerX, ring.centerY, int(radius), thickness,
								RGBA{ring.colour.r, ring.colour.g, ring.colour.b, uint8_t(alpha)});
			}
			// Keep the waves, but let the moving cap and direction arrow remain
			// visible instead of covering sticks with an AX/B label disc.
			if (ring.stickInput)
				continue;
			double progress = clamp01(age / double(ringLifetimeMs_));
			cv.fillCircle(ring.centerX, ring.centerY, 32, RGBA{7, 17, 26, 230});
			RGBA labelCol{245, 253, 255, uint8_t(255 * (1.0 - progress * 0.35))};
			int tw = textWidth(1, ring.label);
			drawText(cv, ring.centerX - tw / 2, ring.centerY - 10, 1, labelCol, ring.label);
		}
	}

	void drawStatus(Canvas &cv)
	{
		// 30% narrower, two bar heights higher; centre icon and larger text
		// together for both the live and disconnected messages.
		constexpr int barX = 790, barY = 755, barWidth = 420, barHeight = 50;
		constexpr double textScale = 2.6; // 30% larger than the former scale 2.
		// Blend the translucent pill once per pixel: overlapping primitive
		// rectangles/circles would leave darker patches at the rounded ends.
		constexpr double radius = 22.0;
		for (int y = barY; y < barY + barHeight; ++y)
			for (int x = barX; x < barX + barWidth; ++x)
			{
				const double dx = std::abs(x + 0.5 - (barX + barWidth * 0.5)) - (barWidth * 0.5 - radius);
				const double dy = std::abs(y + 0.5 - (barY + barHeight * 0.5)) - (barHeight * 0.5 - radius);
				const double distance = std::hypot(std::max(dx, 0.0), std::max(dy, 0.0)) +
										std::min(std::max(dx, dy), 0.0) - radius;
				const uint8_t alpha = uint8_t(std::lround(195 * clamp01(0.5 - distance)));
				cv.blend(x, y, RGBA{8, 16, 26, alpha});
			}
		std::string status = connected_ ? "XBOX 360  LIVE INPUT" : "XBOX 360  DISCONNECTED";
		const int contentX = barX + (barWidth - textWidth(textScale, status) - 32) / 2;
		cv.fillCircle(contentX + 8, barY + barHeight / 2, 8,
					  connected_ ? RGBA{62, 225, 168, 255} : RGBA{255, 105, 105, 255});
		const int textY = barY + int(std::floor((barHeight - 7 * textScale) / 2));
		drawText(cv, contentX + 32, textY, textScale, RGBA{224, 239, 246, 255}, status);

		if (!errorText_.empty())
		{
			cv.fillRoundedRect(500, 895, 1000, 28, 10, RGBA{35, 8, 15, 225});
			int tw = textWidth(1, errorText_);
			drawText(cv, 1000 - tw / 2, 903, 1, RGBA{255, 207, 207, 255}, errorText_);
		}
	}

	// -- YUV420 compositing ------------------------------------------------

	void composite(uint8_t *frame)
	{
		const int fw = info_.width;
		const int fh = info_.height;
		const int stride = info_.stride;

		// Overlay size: width = scale * frame width.
		double scaledWidth = fw * overlayScale_;
		if (!std::isfinite(scaledWidth) || scaledWidth < 1.0 ||
			scaledWidth > std::numeric_limits<int>::max())
			return;
		int ow = int(scaledWidth);
		int oh = int(ow * double(kCanvasH) / kCanvasW);

		// 3x3 position grid (keypad layout):
		//   1 2 3   top-left, top-centre, top-right
		//   4 5 6   mid-left, mid-centre, mid-right
		//   7 8 9   bottom-left, bottom-centre, bottom-right
		const int col = (position_ - 1) % 3;
		const int row = (position_ - 1) / 3;
		const int margin = int(std::min(fw, fh) * 0.03);
		int64_t availW = int64_t(fw) - ow - 2 * margin;
		int64_t availH = int64_t(fh) - oh - 2 * margin;
		int64_t ox = margin + col * availW / 2 + offsetX_;
		int64_t oy = margin + row * availH / 2 + offsetY_;

		if (ow <= 0 || oh <= 0)
			return;

		// Clip before rounding down to the shared 2x2 chroma grid. Negative
		// offsets must never become negative indices into a frame plane.
		int left = int(std::clamp<int64_t>(ox, 0, fw));
		int top = int(std::clamp<int64_t>(oy, 0, fh));
		int right = int(std::clamp<int64_t>(ox + ow, 0, fw));
		int bottom = int(std::clamp<int64_t>(oy + oh, 0, fh));
		if (left >= right || top >= bottom)
			return;

		uint8_t *yPlane = frame;
		uint8_t *uPlane = frame + size_t(fh) * stride;
		uint8_t *vPlane = uPlane + size_t(fh / 2) * (stride / 2);

		// Filter the combined SVG and live graphics over each output pixel's
		// complete source footprint. Axis weights and scratch buffers are reused
		// across frames; clipped overlays only allocate their visible rectangle.
		const int visibleWidth = right - left;
		const std::vector<uint8_t> &scaled = resampler_.resize(
			canvas_->pixels(), kCanvasW, kCanvasH, ow, oh,
			int(left - ox), int(top - oy), visibleWidth, bottom - top);

		// Convert only the overlay to the stream's Y'CbCr encoding and range.
		// The camera pixels stay in YUV, avoiding a lossy RGB round trip even
		// for transparent pixels. sYCC is the Raspberry Pi pipeline default
		// when no colour space is specified.
		using ColorSpace = libcamera::ColorSpace;
		const ColorSpace colourSpace = info_.colour_space.value_or(ColorSpace::Sycc);
		double kr = 0.299, kb = 0.114;
		if (colourSpace.ycbcrEncoding == ColorSpace::YcbcrEncoding::Rec709)
		{
			kr = 0.2126;
			kb = 0.0722;
		}
		else if (colourSpace.ycbcrEncoding == ColorSpace::YcbcrEncoding::Rec2020)
		{
			kr = 0.2627;
			kb = 0.0593;
		}
		const double kg = 1.0 - kr - kb;
		const bool limited = colourSpace.range == ColorSpace::Range::Limited;
		const double yScale = limited ? 219.0 / 255.0 : 1.0;
		const double uvScale = limited ? 224.0 / 255.0 : 1.0;
		const int yOffset = limited ? 16 : 0;
		auto toYuv = [&](const uint8_t *px, int &y, int &u, int &v) {
			double luma = kr * px[0] + kg * px[1] + kb * px[2];
			y = std::clamp(int(std::lround(yOffset + yScale * luma)), 0, 255);
			u = std::clamp(int(std::lround(128.0 + uvScale * (px[2] - luma) / (2.0 * (1.0 - kb)))), 0, 255);
			v = std::clamp(int(std::lround(128.0 + uvScale * (px[0] - luma) / (2.0 * (1.0 - kr)))), 0, 255);
		};

		auto sample = [&](int fx, int fy, uint8_t out[4]) {
			// Chroma blocks may straddle the overlay's visible rectangle.
			if (fx < left || fy < top || fx >= right || fy >= bottom)
			{
				out[0] = out[1] = out[2] = out[3] = 0;
				return;
			}
			const uint8_t *p = scaled.data() + (size_t(fy - top) * visibleWidth + fx - left) * 4;
			memcpy(out, p, 4);
		};

		// Process 2x2 blocks so the shared U/V chroma stays correct.
		for (int y = top & ~1; y < bottom && y < fh - 1; y += 2)
		{
			for (int x = left & ~1; x < right && x < fw - 1; x += 2)
			{
				uint8_t pixels[4][4];
				sample(x, y, pixels[0]);
				sample(x + 1, y, pixels[1]);
				sample(x, y + 1, pixels[2]);
				sample(x + 1, y + 1, pixels[3]);
				int alphaSum = pixels[0][3] + pixels[1][3] + pixels[2][3] + pixels[3][3];
				if (alphaSum == 0)
					continue; // Preserve every byte of a fully transparent block.

				int uAcc = 0, vAcc = 0;
				for (int i = 0; i < 4; ++i)
				{
					int a = pixels[i][3];
					if (a == 0)
						continue;
					int overlayY, overlayU, overlayV;
					toYuv(pixels[i], overlayY, overlayU, overlayV);
					size_t yi = size_t(y + i / 2) * stride + x + i % 2;
					yPlane[yi] = uint8_t((overlayY * a + yPlane[yi] * (255 - a) + 127) / 255);
					uAcc += overlayU * a;
					vAcc += overlayV * a;
				}

				// Each of the four pixels contributes its own opacity to the
				// shared chroma sample. Uncovered pixels retain camera chroma.
				size_t ui = size_t(y / 2) * (stride / 2) + x / 2;
				uPlane[ui] = uint8_t((uAcc + uPlane[ui] * (4 * 255 - alphaSum) + 510) / (4 * 255));
				vPlane[ui] = uint8_t((vAcc + vPlane[ui] * (4 * 255 - alphaSum) + 510) / (4 * 255));
			}
		}
	}

	// -- Members -----------------------------------------------------------

	Stream *stream_ = nullptr;
	StreamInfo info_;

	std::string devicePath_ = "/dev/input/js0";
	double deadzone_ = kDeadzone;
	int ringLifetimeMs_ = kRingLifetimeMs;
	double overlayScale_ = kOverlayScale;
	int reconnectDelayMs_ = kReconnectDelayMs;
	int position_ = 8; // 1..9 keypad grid
	int holdPulseMs_ = 400; // period for repeat pulses while a button is held
	int offsetX_ = 0; // pixel offset applied after grid placement
	int offsetY_ = 0;
	std::string svgPath_ = "/usr/local/share/rpi-camera-assets/open_gamepad.svg";
	bool showStatus_ = true;
	std::vector<uint8_t> svgPixels_; // RGBA, kCanvasW*kCanvasH, empty if not loaded
	OverlayMask shoulderMasks_[2], triggerMasks_[2];

	std::unique_ptr<Canvas> canvas_;
	GamepadOverlayResampler resampler_;

	// Joystick state (guarded by stateMutex_).
	int fd_ = -1;
	std::atomic<bool> running_{false};
	std::thread readerThread_;
	std::mutex stateMutex_;
	std::mutex processMutex_;
	std::vector<int> axisValuesShared_;
	std::vector<bool> buttonValuesShared_;
	std::vector<RingPulse> ringsShared_;
	std::string deviceNameShared_;
	std::string errorTextShared_;
	std::vector<int64_t> lastHoldPulseMs_;
	std::vector<int64_t> lastHoldPulseAxisMs_;

	// Snapshot taken for the current frame (per-Process copy).
	std::vector<int> axisValues_;
	std::vector<bool> buttonValues_;
	std::vector<RingPulse> rings_;
	std::string deviceName_;
	std::string errorText_;
	bool connected_ = false;
};

static PostProcessingStage *Create(RPiCamApp *app)
{
	return new GamepadOverlayStage(app);
}

static RegisterStage reg("gamepad_overlay", &Create);
