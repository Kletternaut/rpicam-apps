/* SPDX-License-Identifier: BSD-2-Clause */
// Standalone regression tests: no camera, display server or Qt dependency.
#include "core/rt_roi_overlay.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <set>

namespace
{
int failures = 0;

void check(bool condition, const char *description)
{
	if (!condition)
	{
		std::cerr << "FAIL: " << description << '\n';
		++failures;
	}
}

bool close(double a, double b)
{
	return std::abs(a - b) < 1e-9;
}

void parameter_contracts()
{
	std::set<std::string> names;
	for (const auto &control : rt_roi::Parameters())
	{
		check(names.insert(control.parameter).second, "parameter names must be unique");
		check(control.min < control.max && control.step > 0, "parameter range permits snapping");
		check(std::isfinite(control.min) && std::isfinite(control.max) && std::isfinite(control.step),
			  "parameter ranges must be finite");
		check(&rt_roi::Parameter(control.parameter) == &control, "parameter lookup returns registry entry");
		if (!control.choices.empty())
		{
			check(control.min == 0 && control.max == control.choices.size() - 1 && control.step == 1,
				  "enum index range must cover every mode exactly once");
			check(std::set<std::string>(control.choices.begin(), control.choices.end()).size() == control.choices.size(),
				  "enum mode names must be unique");
		}
	}
	bool rejected = false;
	try { rt_roi::Parameter("not-a-control"); }
	catch (const std::runtime_error &) { rejected = true; }
	check(rejected, "unknown parameters must be rejected");
}

void numeric_input()
{
	const auto &brightness = rt_roi::Parameter("brightness");
	check(close(rt_roi::SnapValue(brightness, 0.026), 0.05), "slider rounds to configured step");
	check(close(rt_roi::SnapValue(brightness, -0.026), -0.05), "negative slider values snap correctly");
	check(rt_roi::SnapValue(brightness, 100) == 1, "drag beyond right endpoint is clamped");
	check(rt_roi::SnapValue(brightness, -100) == -1, "drag beyond left endpoint is clamped");
	check(rt_roi::SnapValue(brightness, std::numeric_limits<double>::quiet_NaN()) == -1,
		  "NaN input cannot enter the control state");
	check(rt_roi::SnapValue(brightness, std::numeric_limits<double>::infinity()) == -1,
		  "infinite input cannot enter the control state");
	const auto &sharpness = rt_roi::Parameter("sharpness");
	check(close(rt_roi::SnapValue(sharpness, sharpness.max), sharpness.max),
		  "non-step-aligned upper endpoint remains reachable");
	auto custom = brightness;
	custom.min = 0;
	custom.max = 0.94;
	custom.step = 0.1;
	check(close(rt_roi::SnapValue(custom, custom.max), custom.max),
		  "custom upper endpoint remains reachable when rounding would lower it");
	const auto &awb = rt_roi::Parameter("awb");
	check(rt_roi::SnapValue(awb, 7.6) == 8, "manual AWB mode remains reachable");
	check(rt_roi::SnapValue(awb, 9) == 8, "mode selection cannot exceed choices");
}

void exclusion_regions()
{
	rt_roi::Config config;
	auto row = rt_roi::Parameter("zoom");
	row.x = 0.1;
	row.y = 0.2;
	row.width = 0.3;
	row.height = 0.1;
	check(rt_roi::Contains(config, row, 0.1, 0.2), "top/left edges belong to the control");
	check(rt_roi::Contains(config, row, 0.2, 0.25), "control interior excludes ROI");
	check(!rt_roi::Contains(config, row, row.x + row.width, 0.25), "right boundary is exclusive");
	check(!rt_roi::Contains(config, row, 0.2, row.y + row.height), "bottom boundary is exclusive");
	check(!rt_roi::Contains(config, row, 0.09, 0.25), "outside preview region remains available to ROI");
	config.background_opacity = 0;
	check(rt_roi::Contains(config, row, 0.2, 0.25), "transparent row background still reserves input");
	config.scale = 2;
	check(rt_roi::Contains(config, row, 0.65, 0.35), "scaled rendering area has a matching hit region");
	check(!rt_roi::Contains(config, row, 0.71, 0.35), "scaled region does not extend past rendered bounds");
	config.opacity = 0;
	check(!rt_roi::Contains(config, row, 0.2, 0.25), "fully hidden overlay cannot block ROI selection");
	config.opacity = 0.01;
	check(rt_roi::Contains(config, row, 0.2, 0.25), "partially transparent overlay still reserves input");
	check(!rt_roi::Contains(config, row, std::numeric_limits<double>::quiet_NaN(), 0.25),
		  "invalid pointer coordinate cannot hit a control");
}

void persistent_toggle()
{
	rt_roi::Config config;
	config.toggle.x = 0.8;
	config.toggle.y = 0.1;
	config.toggle.width = 0.1;
	config.toggle.height = 0.1;
	check(rt_roi::ContainsToggle(config, 0.8, 0.1), "toggle top/left edges reserve input");
	check(rt_roi::ContainsToggle(config, 0.85, 0.15), "toggle interior reserves input");
	check(!rt_roi::ContainsToggle(config, config.toggle.x + config.toggle.width, 0.15),
		  "toggle right boundary is exclusive");
	check(!rt_roi::ContainsToggle(config, 0.85, config.toggle.y + config.toggle.height),
		  "toggle bottom boundary is exclusive");
	config.initially_visible = false;
	config.opacity = 0;
	check(rt_roi::ContainsToggle(config, 0.85, 0.15), "hidden panel retains an independently visible toggle");
	config.scale = 2;
	check(!rt_roi::ContainsToggle(config, 0.95, 0.15), "panel scale does not resize the independent toggle");
}
} // namespace

int main()
{
	parameter_contracts();
	numeric_input();
	exclusion_regions();
	persistent_toggle();
	if (failures)
		return EXIT_FAILURE;
	std::cout << "RT ROI overlay model tests passed\n";
	return EXIT_SUCCESS;
}
