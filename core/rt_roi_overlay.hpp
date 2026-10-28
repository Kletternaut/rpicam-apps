/* SPDX-License-Identifier: BSD-2-Clause */
// Preview-only runtime controls. This model has no Qt or camera dependencies.
#pragma once

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace rt_roi
{
struct Control
{
	std::string parameter;
	std::string label;
	double x = 0.02, y = 0.03, width = 0.36, height = 0.128;
	double min = 0, max = 1, step = 0.01;
	std::vector<std::string> choices;
};

struct Toggle
{
	double x = 0.88, y = 0.9, width = 0.1, height = 0.07;
	double opacity = 0.9;
};

// Global grid layout for all enabled controls. x/y/width/height are fractions of
// the preview size; step_x/step_y are the distances between column/row starts.
struct Layout
{
	int columns = 2;
	double x = 0.02, y = 0.03;
	double width = 0.36, height = 0.128;
	double step_x = 0.60, step_y = 0.145;
};

struct Config
{
	bool initially_visible = true;
	Layout layout;
	Toggle toggle;
	Toggle reset;
	double scale = 1.0;
	double opacity = 0.9;
	double background_opacity = 0.3;
	std::string foreground = "#ffffff";
	std::string accent = "#00b4ff";
	std::string background = "#101820";
	std::vector<Control> controls;
};

struct State
{
	// Requested settings, not changing exposure measurements in auto mode.
	// Missing values identify controls unsupported by the current camera.
	std::map<std::string, double> values;
	std::map<std::string, std::pair<double, double>> ranges;
	std::map<std::string, std::vector<int>> supported_modes;
	// Hardware defaults captured at startup, used by the reset button.
	std::map<std::string, double> defaults;
	bool roi_active = false;
	bool enabled = false;
};

inline const std::vector<Control> &Parameters()
{
	static const std::vector<Control> parameters = [] {
		std::vector<Control> result;
		auto add = [&](const char *name, const char *label, double min, double max, double step,
					   std::vector<std::string> choices = {}) {
			Control control;
			control.parameter = name;
			control.label = label;
			control.min = min;
			control.max = max;
			control.step = step;
			control.choices = std::move(choices);
			result.push_back(std::move(control));
		};
		add("brightness", "Brightness", -1, 1, 0.05);
		add("contrast", "Contrast", 0, 3, 0.05);
		add("saturation", "Saturation", 0, 3, 0.05);
		add("ev", "EV", -3, 3, 0.1);
		add("sharpness", "Sharpness", 0, 15.99, 0.1);
		add("gain", "Gain (0 = auto)", 0, 16, 0.1);
		add("shutter", "Shutter us (0 = auto)", 0, 100000, 100);
		add("framerate", "FPS (0 = auto)", 0, 120, 1);
		add("focus", "Focus (dpt)", -32, 32, 0.1);
		add("awb_red", "AWB red", 0.1, 8, 0.05);
		add("awb_blue", "AWB blue", 0.1, 8, 0.05);
		add("zoom", "Zoom", 1, 10, 0.1);
		add("roi_x", "ROI X", 0, 0.99, 0.01);
		add("roi_y", "ROI Y", 0, 0.99, 0.01);
		add("roi_width", "ROI width", 0.01, 1, 0.01);
		add("roi_height", "ROI height", 0.01, 1, 0.01);
		add("awb", "AWB", 0, 8, 1,
			{ "auto", "incandescent", "tungsten", "fluorescent", "indoor", "daylight", "cloudy", "custom", "manual" });
		add("af", "AF", 0, 2, 1, { "manual", "auto", "continuous" });
		add("metering", "Metering", 0, 3, 1, { "centre", "spot", "average", "custom" });
		add("exposure", "Exposure", 0, 3, 1, { "normal", "sport", "long", "custom" });
		add("denoise", "Denoise", 0, 4, 1, { "auto", "off", "cdn_off", "cdn_fast", "cdn_hq" });
		add("hdr", "HDR", 0, 1, 1, { "off", "single-exp" });
		return result;
	}();
	return parameters;
}

inline const Control &Parameter(const std::string &name)
{
	for (const auto &parameter : Parameters())
		if (parameter.parameter == name)
			return parameter;
	throw std::runtime_error("rt_roi_overlay: unknown parameter '" + name + "'");
}

inline double SnapValue(const Control &control, double value)
{
	if (!std::isfinite(value))
		return control.min;
	if (value <= control.min)
		return control.min;
	if (value >= control.max)
		return control.max;
	return std::clamp(control.min + std::round((value - control.min) / control.step) * control.step,
					  control.min, control.max);
}

// Use precisely the same bounds for drawing and input, including transparent pixels.
inline bool Contains(const Config &config, const Control &control, double x, double y)
{
	return config.opacity > 0 && x >= control.x && y >= control.y &&
		   x < control.x + control.width * config.scale && y < control.y + control.height * config.scale;
}

inline bool ContainsToggle(const Config &config, double x, double y)
{
	const auto &toggle = config.toggle;
	return x >= toggle.x && y >= toggle.y && x < toggle.x + toggle.width && y < toggle.y + toggle.height;
}

inline bool ContainsReset(const Config &config, double x, double y)
{
	const auto &reset = config.reset;
	return x >= reset.x && y >= reset.y && x < reset.x + reset.width && y < reset.y + reset.height;
}
} // namespace rt_roi
