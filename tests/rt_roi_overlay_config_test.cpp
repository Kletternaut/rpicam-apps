/* SPDX-License-Identifier: BSD-2-Clause */
#include "core/rt_roi_overlay_config.hpp"

#define BOOST_BIND_GLOBAL_PLACEHOLDERS
#include <boost/property_tree/json_parser.hpp>
#include <cstdlib>
#include <iostream>
#include <sstream>

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

rt_roi::Config parse(const char *json)
{
	boost::property_tree::ptree input;
	std::istringstream stream(json);
	boost::property_tree::read_json(stream, input);
	return rt_roi::ReadConfig(input);
}

void rejects(const char *json, const char *description)
{
	try
	{
		parse(json);
		check(false, description);
	}
	catch (const std::exception &) {}
}

void defaults_and_customisation()
{
	auto defaults = parse("{}");
	check(defaults.controls.size() == 5, "default panel contains five selected parameters");
	check(defaults.controls.front().parameter == "brightness" && defaults.controls.back().parameter == "zoom",
		  "default control order is stable");
	check(defaults.controls[1].y > defaults.controls[0].y, "default controls stack vertically");
	auto custom = parse(R"({"scale":0.5,"opacity":0.6,"background_opacity":0,
		"foreground":"#AAbbCC","accent":"#123456","background":"#000000",
		"controls":[{"parameter":"zoom","x":0.7,"y":0.8,"width":0.4,"height":0.2,
		"min":2,"max":5,"step":0.5,"label":"Digital zoom"}]})");
	const auto &zoom = custom.controls.front();
	check(custom.controls.size() == 1 && zoom.parameter == "zoom", "only configured controls appear");
	check(zoom.label == "Digital zoom" && zoom.min == 2 && zoom.max == 5 && zoom.step == 0.5,
		  "labels and reduced control ranges are configurable");
	check(custom.scale == 0.5 && custom.opacity == 0.6 && custom.background_opacity == 0,
		  "size and independent alpha values survive parsing");
	check(rt_roi::Contains(custom, zoom, 0.8, 0.85), "custom geometry reserves the rendered control");
	check(!rt_roi::Contains(custom, zoom, 0.95, 0.85), "custom scale shrinks the hit region");
	check(parse(R"({"controls":[]})").controls.empty(), "explicit empty control list hides the panel");
	check(parse(R"({"controls":[{"parameter":"zoom","visible":false}]})").controls.empty(),
		  "invisible controls reserve no region");
	check(parse(R"({"opacity":0})").opacity == 0, "fully transparent panel can be configured");
	check(parse(R"({"controls":[{"parameter":"awb","min":1,"max":5}]})").controls[0].max == 5,
		  "enum controls support a reduced integer range");
	auto hidden = parse(R"({"initially_visible":false,"opacity":0,
		"toggle":{"x":0.1,"y":0.2,"width":0.15,"height":0.12,"opacity":0.4}})");
	check(!hidden.initially_visible && hidden.opacity == 0 && hidden.toggle.opacity == 0.4,
		  "toggle opacity is independent of panel visibility and opacity");
	check(hidden.toggle.x == 0.1 && hidden.toggle.y == 0.2 && hidden.toggle.width == 0.15 && hidden.toggle.height == 0.12,
		  "toggle position and size are configurable");
}

void invalid_configuration()
{
	rejects(R"("bad")", "scalar stage configuration rejected");
	rejects(R"({"toggle":"bad"})", "scalar toggle configuration rejected");
	rejects(R"({"controls":"zoom"})", "scalar control list rejected");
	rejects(R"({"controls":{"zoom":{"parameter":"zoom"}}})", "named control object rejected");
	rejects(R"({"controls":["zoom"]})", "non-object control entry rejected");
	rejects(R"({"controls":[{}]})", "missing parameter rejected");
	rejects(R"({"controls":[{"parameter":"unknown"}]})", "unknown parameter rejected");
	rejects(R"({"controls":[{"parameter":"zoom"},{"parameter":"zoom"}]})", "duplicate control rejected");
	rejects(R"({"controls":[{"parameter":"zoom","min":5,"max":2}]})", "reversed range rejected");
	rejects(R"({"controls":[{"parameter":"zoom","min":2,"max":2}]})", "zero-width range rejected");
	rejects(R"({"controls":[{"parameter":"zoom","min":0}]})", "range below parameter bounds rejected");
	rejects(R"({"controls":[{"parameter":"zoom","max":99}]})", "range beyond parameter bounds rejected");
	rejects(R"({"controls":[{"parameter":"zoom","step":0}]})", "zero step rejected");
	rejects(R"({"controls":[{"parameter":"zoom","step":-0.1}]})", "negative step rejected");
	rejects(R"({"controls":[{"parameter":"zoom","step":20}]})", "step wider than range rejected");
	rejects(R"({"controls":[{"parameter":"awb","step":0.5}]})", "fractional enum step rejected");
	rejects(R"({"controls":[{"parameter":"awb","min":0.5}]})", "fractional enum boundary rejected");
	rejects(R"({"scale":0})", "zero scale rejected");
	rejects(R"({"scale":3})", "default panel cannot extend outside preview");
	rejects(R"({"opacity":1.1})", "opacity above one rejected");
	rejects(R"({"opacity":-0.1})", "negative opacity rejected");
	rejects(R"({"background_opacity":2})", "background opacity above one rejected");
	rejects(R"({"scale":"nan"})", "NaN scale rejected");
	rejects(R"({"opacity":"nonsense"})", "malformed number rejected instead of replaced by default");
	rejects(R"({"controls":[{"parameter":"zoom","width":"bad"}]})", "malformed control number rejected");
	rejects(R"({"initially_visible":"bad"})", "malformed visibility rejected");
	rejects(R"({"toggle":{"x":-0.1}})", "negative toggle position rejected");
	rejects(R"({"toggle":{"x":0.99,"width":0.1}})", "offscreen toggle rejected");
	rejects(R"({"toggle":{"height":0}})", "zero-sized toggle rejected");
	rejects(R"({"toggle":{"opacity":0}})", "invisible toggle rejected");
	rejects(R"({"toggle":{"opacity":1.1}})", "toggle alpha above one rejected");
	rejects(R"({"controls":[{"parameter":"zoom","x":-0.01}]})", "negative position rejected");
	rejects(R"({"controls":[{"parameter":"zoom","width":0}]})", "zero-sized region rejected");
	rejects(R"({"controls":[{"parameter":"zoom","x":0.9}]})", "offscreen control rejected");
	rejects(R"({"foreground":"red"})", "named color rejected");
	rejects(R"({"accent":"#12345g"})", "invalid hex color rejected");
	rejects(R"({"background":"#12345600"})", "embedded color alpha rejected");
	rejects(R"({"foreground":"\"/><script/>"})", "color cannot inject SVG markup");
}
} // namespace

int main()
{
	defaults_and_customisation();
	invalid_configuration();
	if (failures)
		return EXIT_FAILURE;
	std::cout << "RT ROI overlay configuration tests passed\n";
	return EXIT_SUCCESS;
}
