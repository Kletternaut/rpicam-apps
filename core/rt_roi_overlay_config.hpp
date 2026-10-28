/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include <set>
#include <boost/property_tree/ptree.hpp>

#include "core/rt_roi_overlay.hpp"

namespace rt_roi
{
template <typename T>
T ReadOption(const boost::property_tree::ptree &params, const std::string &key, const T &fallback)
{
	// get(key, fallback) also hides conversion errors. Only absent keys default.
	return params.get_child_optional(key) ? params.get<T>(key) : fallback;
}

inline void CheckNumber(double value, double min, double max, const std::string &name)
{
	if (!std::isfinite(value) || value < min || value > max)
		throw std::runtime_error("rt_roi_overlay: invalid " + name);
}

inline void CheckColour(const std::string &colour)
{
	if (colour.size() != 7 || colour[0] != '#' ||
		colour.find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos)
		throw std::runtime_error("rt_roi_overlay: colours must use #rrggbb");
}

inline Config ReadConfig(const boost::property_tree::ptree &params)
{
	if (!params.data().empty())
		throw std::runtime_error("rt_roi_overlay: configuration must be an object");
	if (auto toggle = params.get_child_optional("toggle"); toggle && !toggle->data().empty())
		throw std::runtime_error("rt_roi_overlay: toggle must be an object");
	Config config;
	config.initially_visible = ReadOption<bool>(params, "initially_visible", config.initially_visible);
	config.toggle.x = ReadOption<double>(params, "toggle.x", config.toggle.x);
	config.toggle.y = ReadOption<double>(params, "toggle.y", config.toggle.y);
	config.toggle.width = ReadOption<double>(params, "toggle.width", config.toggle.width);
	config.toggle.height = ReadOption<double>(params, "toggle.height", config.toggle.height);
	config.toggle.opacity = ReadOption<double>(params, "toggle.opacity", config.toggle.opacity);
	CheckNumber(config.toggle.x, 0, 1, "toggle.x");
	CheckNumber(config.toggle.y, 0, 1, "toggle.y");
	CheckNumber(config.toggle.width, 0.02, 1, "toggle.width");
	CheckNumber(config.toggle.height, 0.02, 1, "toggle.height");
	CheckNumber(config.toggle.opacity, 0.1, 1, "toggle.opacity");
	if (config.toggle.x + config.toggle.width > 1.0000001 ||
		config.toggle.y + config.toggle.height > 1.0000001)
		throw std::runtime_error("rt_roi_overlay: toggle button outside preview");
	config.layout.columns = ReadOption<int>(params, "layout.columns", config.layout.columns);
	config.layout.x = ReadOption<double>(params, "layout.x", config.layout.x);
	config.layout.y = ReadOption<double>(params, "layout.y", config.layout.y);
	config.layout.width = ReadOption<double>(params, "layout.width", config.layout.width);
	config.layout.height = ReadOption<double>(params, "layout.height", config.layout.height);
	config.layout.step_x = ReadOption<double>(params, "layout.step_x", config.layout.step_x);
	config.layout.step_y = ReadOption<double>(params, "layout.step_y", config.layout.step_y);
	CheckNumber(config.layout.columns, 1, 16, "layout.columns");
	CheckNumber(config.layout.x, 0, 1, "layout.x");
	CheckNumber(config.layout.y, 0, 1, "layout.y");
	CheckNumber(config.layout.width, 0.01, 1, "layout.width");
	CheckNumber(config.layout.height, 0.01, 1, "layout.height");
	CheckNumber(config.layout.step_x, 0.01, 1, "layout.step_x");
	CheckNumber(config.layout.step_y, 0.01, 1, "layout.step_y");
	config.reset.x = ReadOption<double>(params, "reset.x", 0.74);
	config.reset.y = ReadOption<double>(params, "reset.y", 0.90);
	config.reset.width = ReadOption<double>(params, "reset.width", 0.1);
	config.reset.height = ReadOption<double>(params, "reset.height", 0.07);
	config.reset.opacity = ReadOption<double>(params, "reset.opacity", 0.9);
	CheckNumber(config.reset.x, 0, 1, "reset.x");
	CheckNumber(config.reset.y, 0, 1, "reset.y");
	CheckNumber(config.reset.width, 0.02, 1, "reset.width");
	CheckNumber(config.reset.height, 0.02, 1, "reset.height");
	CheckNumber(config.reset.opacity, 0.1, 1, "reset.opacity");
	if (config.reset.x + config.reset.width > 1.0000001 ||
		config.reset.y + config.reset.height > 1.0000001)
		throw std::runtime_error("rt_roi_overlay: reset button outside preview");
	config.scale = ReadOption<double>(params, "scale", config.scale);
	config.opacity = ReadOption<double>(params, "opacity", config.opacity);
	config.background_opacity = ReadOption<double>(params, "background_opacity", config.background_opacity);
	config.foreground = ReadOption<std::string>(params, "foreground", config.foreground);
	config.accent = ReadOption<std::string>(params, "accent", config.accent);
	config.background = ReadOption<std::string>(params, "background", config.background);
	CheckNumber(config.scale, 0.05, 10, "scale");
	CheckNumber(config.opacity, 0, 1, "opacity");
	CheckNumber(config.background_opacity, 0, 1, "background_opacity");
	CheckColour(config.foreground);
	CheckColour(config.accent);
	CheckColour(config.background);

	std::vector<Control> enabled_controls;

	auto controls = params.get_child_optional("controls");
	if (!controls)
	{
		// No controls list: enable every parameter.
		for (const auto &parameter : Parameters())
			enabled_controls.push_back(parameter);
	}
	else
	{
		if (!controls->data().empty())
			throw std::runtime_error("rt_roi_overlay: controls must be an array");
		std::set<std::string> names;
		for (const auto &entry : *controls)
		{
			if (!entry.first.empty() || !entry.second.data().empty())
				throw std::runtime_error("rt_roi_overlay: controls must contain objects");
			const auto &node = entry.second;
			const auto name = node.get<std::string>("parameter");
			Control control = Parameter(name);
			if (!names.insert(name).second)
				throw std::runtime_error("rt_roi_overlay: duplicate parameter '" + name + "'");
			if (!ReadOption<bool>(node, "enable", true))
				continue;
			control.label = ReadOption<std::string>(node, "label", control.label);
			control.min = ReadOption<double>(node, "min", control.min);
			control.max = ReadOption<double>(node, "max", control.max);
			control.step = ReadOption<double>(node, "step", control.step);
			CheckNumber(control.min, Parameter(name).min, Parameter(name).max, name + ".min");
			CheckNumber(control.max, control.min, Parameter(name).max, name + ".max");
			if (control.min == control.max || !std::isfinite(control.step) || control.step <= 0 ||
				control.step > control.max - control.min)
				throw std::runtime_error("rt_roi_overlay: invalid range or step for '" + name + "'");
			if (!control.choices.empty() &&
				(control.step != 1 || std::floor(control.min) != control.min || std::floor(control.max) != control.max))
				throw std::runtime_error("rt_roi_overlay: mode controls need integer limits and step 1");
			enabled_controls.push_back(std::move(control));
		}
	}

	// Assign grid positions from the global layout.
	for (size_t i = 0; i < enabled_controls.size(); ++i)
	{
		int col = static_cast<int>(i) % config.layout.columns;
		int row = static_cast<int>(i) / config.layout.columns;
		enabled_controls[i].x = config.layout.x + col * config.layout.step_x;
		enabled_controls[i].y = config.layout.y + row * config.layout.step_y;
		enabled_controls[i].width = config.layout.width;
		enabled_controls[i].height = config.layout.height;
	}
	config.controls = std::move(enabled_controls);

	for (const auto &control : config.controls)
	{
		CheckNumber(control.x, 0, 1, control.parameter + ".x");
		CheckNumber(control.y, 0, 1, control.parameter + ".y");
		CheckNumber(control.width, 0.01, 1, control.parameter + ".width");
		CheckNumber(control.height, 0.01, 1, control.parameter + ".height");
		if (control.x + control.width * config.scale > 1.0000001 ||
			control.y + control.height * config.scale > 1.0000001)
			throw std::runtime_error("rt_roi_overlay: scaled control outside preview: '" + control.parameter + "'");
	}
	return config;
}
} // namespace rt_roi
