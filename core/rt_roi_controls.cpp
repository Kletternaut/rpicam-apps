/* SPDX-License-Identifier: BSD-2-Clause */
// Camera-side runtime control bridge for the preview overlay.
#include "core/rpicam_app.hpp"
#include "preview/preview.hpp"

#include <cmath>
#include <optional>
#include <span>

namespace
{
using FloatControl = libcamera::Control<float>;
const std::map<std::string, const FloatControl *> float_controls = {
	{ "brightness", &controls::Brightness }, { "contrast", &controls::Contrast },
	{ "saturation", &controls::Saturation }, { "ev", &controls::ExposureValue },
	{ "sharpness", &controls::Sharpness }, { "gain", &controls::AnalogueGain },
	{ "focus", &controls::LensPosition }
};

struct EnumControl
{
	const libcamera::Control<int32_t> *control;
	std::map<std::string, int32_t> modes;
};

const std::map<std::string, EnumControl> enum_controls = {
	{ "awb", { &controls::AwbMode,
		{ { "auto", controls::AwbAuto }, { "incandescent", controls::AwbIncandescent },
		  { "tungsten", controls::AwbTungsten }, { "fluorescent", controls::AwbFluorescent },
		  { "indoor", controls::AwbIndoor }, { "daylight", controls::AwbDaylight },
		  { "cloudy", controls::AwbCloudy }, { "custom", controls::AwbCustom } } } },
	{ "metering", { &controls::AeMeteringMode,
		{ { "centre", controls::MeteringCentreWeighted }, { "spot", controls::MeteringSpot },
		  { "average", controls::MeteringMatrix }, { "custom", controls::MeteringCustom } } } },
	{ "exposure", { &controls::AeExposureMode,
		{ { "normal", controls::ExposureNormal }, { "sport", controls::ExposureShort },
		  { "long", controls::ExposureLong }, { "custom", controls::ExposureCustom } } } },
	{ "denoise", { &controls::draft::NoiseReductionMode,
		{ { "auto", controls::draft::NoiseReductionModeMinimal },
		  { "off", controls::draft::NoiseReductionModeOff },
		  { "cdn_off", controls::draft::NoiseReductionModeMinimal },
		  { "cdn_fast", controls::draft::NoiseReductionModeFast },
		  { "cdn_hq", controls::draft::NoiseReductionModeHighQuality } } } },
	{ "hdr", { &controls::HdrMode,
		{ { "off", controls::HdrModeOff }, { "single-exp", controls::HdrModeSingleExposure } } } },
	{ "af", { &controls::AfMode,
		{ { "manual", controls::AfModeManual }, { "auto", controls::AfModeAuto },
		  { "continuous", controls::AfModeContinuous } } } }
};

template <typename T> double number(const libcamera::ControlValue &value, double fallback, size_t index = 0)
{
	if (value.isNone())
		return fallback;
	if (value.isArray())
	{
		auto values = value.get<std::span<const T>>();
		return values.empty() ? fallback : static_cast<double>(values[std::min(index, values.size() - 1)]);
	}
	return static_cast<double>(value.get<T>());
}

bool supportsMode(const libcamera::ControlInfo &info, int32_t value)
{
	if (!info.values().empty())
		return std::any_of(info.values().begin(), info.values().end(),
						   [value](const auto &v) { return v.template get<int32_t>() == value; });
	return value >= number<int32_t>(info.min(), value) && value <= number<int32_t>(info.max(), value);
}

std::optional<double> modeIndex(const std::string &parameter, int32_t mode)
{
	const auto &choices = rt_roi::Parameter(parameter).choices;
	const auto &modes = enum_controls.at(parameter).modes;
	for (size_t i = 0; i < choices.size(); ++i)
	{
		auto found = modes.find(choices[i]);
		if (found != modes.end() && found->second == mode)
			return static_cast<double>(i);
	}
	return {};
}

double namedIndex(const std::string &parameter, const std::string &name)
{
	const auto &choices = rt_roi::Parameter(parameter).choices;
	auto found = std::find(choices.begin(), choices.end(), name);
	return found == choices.end() ? 0 : std::distance(choices.begin(), found);
}
} // namespace

bool RPiCamApp::SetPreviewRtRoiOverlay(std::shared_ptr<const rt_roi::Config> config)
{
	if (!preview_)
		return false;
	if (!config)
		return preview_->SetRtRoiOverlay({}, {}, {});
	// The callbacks refer only to the app, whose preview is destroyed before the app.
	return preview_->SetRtRoiOverlay(std::move(config), [this] { return GetRtRoiState(); },
		[this](const std::string &parameter, double value) { SetRtRoiValue(parameter, value); });
}

bool RPiCamApp::SetPreviewClickTarget(double x, double y, double width, double height, const std::string &url)
{
	if (!preview_)
		return false;
	return preview_->SetClickTarget(x, y, width, height, url);
}

rt_roi::State RPiCamApp::GetRtRoiState()
{
	std::lock_guard<std::mutex> lock(control_mutex_);
	auto state = rt_roi_state_;
	if (state.ranges.count("shutter") && state.values.count("framerate") && state.values.at("framerate") > 0)
		state.ranges.at("shutter").second =
			std::min(state.ranges.at("shutter").second, 1000000.0 / state.values.at("framerate"));
	return state;
}

void RPiCamApp::initialiseRtRoiState()
{
	rt_roi_state_ = {};
	rt_roi_sensor_ = GetSensorArea();
	rt_roi_default_crop_ = rt_roi_sensor_;
	const auto &info = camera_->controls();
	auto add = [&](const std::string &name, double value, double lo, double hi) {
		if (!std::isfinite(lo) || !std::isfinite(hi) || hi < lo)
			return;
		rt_roi_state_.values[name] = std::clamp(value, lo, hi);
		rt_roi_state_.defaults[name] = std::clamp(value, lo, hi);
		rt_roi_state_.ranges[name] = { lo, hi };
	};
	for (const auto &[name, control] : float_controls)
	{
		auto found = info.find(control);
		if (found == info.end())
			continue;
		const auto &parameter = rt_roi::Parameter(name);
		double lo = number<float>(found->second.min(), parameter.min);
		double hi = number<float>(found->second.max(), parameter.max);
		double value = number<float>(found->second.def(), name == "brightness" || name == "ev" ? 0 : 1);
		if (name == "gain")
		{
			if (!info.count(&controls::AnalogueGainMode))
				continue;
			lo = value = 0; // zero is the requested automatic mode, not measured gain
		}
		add(name, value, lo, hi);
	}
	if (auto found = info.find(&controls::ExposureTime); found != info.end() &&
		info.count(&controls::ExposureTimeMode))
		add("shutter", 0, 0, number<int32_t>(found->second.max(), 100000));
	if (auto found = info.find(&controls::FrameDurationLimits); found != info.end())
	{
		double shortest = number<int64_t>(found->second.min(), 1);
		if (shortest > 0)
			add("framerate", 0, 0, 1000000.0 / shortest);
	}
	if (auto found = info.find(&controls::ColourGains); found != info.end() && info.count(&controls::AwbEnable))
	{
		double lo = std::max(0.001, number<float>(found->second.min(), 0.1));
		double hi = number<float>(found->second.max(), 8);
		double value = number<float>(found->second.def(), 1);
		add("awb_red", value, lo, hi);
		add("awb_blue", number<float>(found->second.def(), value, 1), lo, hi);
	}
	for (const auto &[name, binding] : enum_controls)
	{
		auto found = info.find(binding.control);
		if (found == info.end() || (name == "awb" && !info.count(&controls::AwbEnable)))
			continue;
		const auto &choices = rt_roi::Parameter(name).choices;
		std::vector<int> supported;
		for (size_t index = 0; index < choices.size(); ++index)
		{
			auto mode = binding.modes.find(choices[index]);
			bool available = mode != binding.modes.end() && supportsMode(found->second, mode->second);
			if (name == "awb" && choices[index] == "manual")
				available = rt_roi_state_.values.count("awb_red") && rt_roi_state_.values.count("awb_blue");
			if (available)
				supported.push_back(static_cast<int>(index));
		}
		if (supported.empty())
			continue;
		// The AF mode has a runtime default of auto. The camera's ControlInfo
		// default() may report manual (0), so always use auto for af. Other
		// enum controls fall back to their first mode (0).
		double value;
		if (name == "af")
			value = modeIndex(name, controls::AfModeAuto).value_or(0);
		else
			value = modeIndex(name, static_cast<int32_t>(number<int32_t>(found->second.def(), 0))).value_or(0);
		if (std::find(supported.begin(), supported.end(), static_cast<int>(value)) == supported.end())
			value = supported.front();
		add(name, value, 0, choices.size() - 1);
		rt_roi_state_.supported_modes[name] = std::move(supported);
	}
	if (rt_roi_sensor_.width && rt_roi_sensor_.height && info.count(&controls::ScalerCrop))
	{
		const auto &default_crop = info.at(&controls::ScalerCrop).def();
		if (!default_crop.isNone())
			rt_roi_default_crop_ = default_crop.get<Rectangle>();
		double min_width = 1.0 / rt_roi_sensor_.width;
		double min_height = 1.0 / rt_roi_sensor_.height;
		const auto &minimum = info.at(&controls::ScalerCrop).min();
		if (!minimum.isNone())
		{
			auto crop = minimum.get<Rectangle>();
			min_width = std::max(min_width, static_cast<double>(crop.width) / rt_roi_sensor_.width);
			min_height = std::max(min_height, static_cast<double>(crop.height) / rt_roi_sensor_.height);
		}
		min_width = std::min(min_width, 1.0);
		min_height = std::min(min_height, 1.0);
		add("roi_x", 0, 0, 1 - min_width);
		add("roi_y", 0, 0, 1 - min_height);
		add("roi_width", 1, min_width, 1);
		add("roi_height", 1, min_height, 1);
		add("zoom", 1, 1, std::min(1 / min_width, 1 / min_height));
	}
}

void RPiCamApp::updateRtRoiState(const ControlList &changes)
{
	auto &values = rt_roi_state_.values;
	auto set = [&](const std::string &name, double value) {
		if (values.count(name) && std::isfinite(value))
			values[name] = value;
	};
	for (const auto &[name, control] : float_controls)
		if (auto value = changes.get(*control))
			set(name, *value);
	for (const auto &[name, binding] : enum_controls)
		if (auto value = changes.get(*binding.control))
			if (auto index = modeIndex(name, *value))
				set(name, *index);
	if (auto value = changes.get(controls::ExposureTime))
		set("shutter", *value);
	if (auto value = changes.get(controls::ExposureTimeMode); value && *value == controls::ExposureTimeModeAuto)
		set("shutter", 0);
	if (auto value = changes.get(controls::AeEnable); value && *value &&
		changes.get(controls::ExposureTimeMode).value_or(controls::ExposureTimeModeAuto) != controls::ExposureTimeModeManual)
		set("shutter", 0);
	if (auto value = changes.get(controls::AnalogueGainMode); value && *value == controls::AnalogueGainModeAuto)
		set("gain", 0);
	if (auto value = changes.get(controls::AeEnable); value && *value &&
		changes.get(controls::AnalogueGainMode).value_or(controls::AnalogueGainModeAuto) != controls::AnalogueGainModeManual)
		set("gain", 0);
	if (auto value = changes.get(controls::FrameDurationLimits); value && value->size() == 2)
		set("framerate", (*value)[0] > 0 && (*value)[0] == (*value)[1] ? 1000000.0 / (*value)[0] : 0);
	if (auto value = changes.get(controls::ColourGains); value && value->size() == 2)
	{
		set("awb_red", (*value)[0]);
		set("awb_blue", (*value)[1]);
		set("awb", namedIndex("awb", "manual"));
	}
	if (auto value = changes.get(controls::AwbEnable))
	{
		if (!*value)
			set("awb", namedIndex("awb", "manual"));
		else if (!changes.get(controls::AwbMode) && values.count("awb") &&
				 values.at("awb") == namedIndex("awb", "manual"))
			set("awb", namedIndex("awb", "auto"));
		else if (auto mode = changes.get(controls::AwbMode))
			set("awb", modeIndex("awb", *mode).value_or(0));
	}
	std::optional<Rectangle> crop = changes.get(controls::ScalerCrop);
	if (auto crops = changes.get(controls::rpi::ScalerCrops); crops && !crops->empty())
		crop = (*crops)[0];
	if (crop && values.count("zoom") && rt_roi_sensor_.width && rt_roi_sensor_.height)
	{
		double x = static_cast<double>(crop->x - rt_roi_sensor_.x) / rt_roi_sensor_.width;
		double y = static_cast<double>(crop->y - rt_roi_sensor_.y) / rt_roi_sensor_.height;
		double w = static_cast<double>(crop->width) / rt_roi_sensor_.width;
		double h = static_cast<double>(crop->height) / rt_roi_sensor_.height;
		set("roi_x", x);
		set("roi_y", y);
		set("roi_width", w);
		set("roi_height", h);
		if (w > 0 && h > 0)
			set("zoom", 1 / std::max(w, h));
		// The configured default can be an aspect-ratio crop. It must not prevent
		// the first interactive selection on an otherwise unzoomed preview.
		rt_roi_state_.roi_active = *crop != rt_roi_sensor_ && *crop != rt_roi_default_crop_;
	}
}

void RPiCamApp::SetRtRoiValue(const std::string &parameter, double value)
{
	std::lock_guard<std::mutex> lock(control_mutex_);
	auto &state = rt_roi_state_;
	if (!state.enabled || !std::isfinite(value) || !state.values.count(parameter))
		return;
	const auto &info = camera_->controls();
	const auto &range = state.ranges.at(parameter);
	value = std::clamp(value, range.first, range.second);
	std::optional<int> requested_mode;
	if (auto supported = state.supported_modes.find(parameter); supported != state.supported_modes.end())
	{
		int index = static_cast<int>(std::round(value));
		if (std::find(supported->second.begin(), supported->second.end(), index) == supported->second.end())
			return;
		requested_mode = index;
	}
	ControlList changes(controls::controls);
	if (auto found = float_controls.find(parameter); found != float_controls.end())
	{
		if (parameter == "gain")
		{
			double minimum = number<float>(info.at(found->second).min(), 1);
			// Allow a minus click to cross the gap between the smallest manual
			// gain and the auto sentinel. A plus click from auto enters manual.
			if (value > 0 && value < minimum)
				value = state.values.at(parameter) > value ? 0 : minimum;
			changes.set(controls::AnalogueGainMode,
				value == 0 ? controls::AnalogueGainModeAuto : controls::AnalogueGainModeManual);
			if (value == 0)
			{
				setControlsLocked(changes);
				return;
			}
		}
		if (parameter == "ev" && info.count(&controls::AeEnable))
		{
			changes.set(controls::AeEnable, true);
			if (info.count(&controls::ExposureTimeMode))
				changes.set(controls::ExposureTimeMode, controls::ExposureTimeModeAuto);
		}
		if (parameter == "focus" && info.count(&controls::AfMode))
			changes.set(controls::AfMode, controls::AfModeManual);
		changes.set(*found->second, static_cast<float>(value));
	}
	else if (parameter == "shutter")
	{
		double minimum = number<int32_t>(info.at(&controls::ExposureTime).min(), 1);
		if (value > 0 && value < minimum)
			value = state.values.at(parameter) > value ? 0 : minimum;
		changes.set(controls::ExposureTimeMode,
			value == 0 ? controls::ExposureTimeModeAuto : controls::ExposureTimeModeManual);
		if (value > 0)
		{
			if (state.values.count("framerate") && state.values.at("framerate") > 0)
				value = std::min(value, 1000000.0 / state.values.at("framerate"));
			changes.set(controls::ExposureTime, static_cast<int32_t>(std::round(std::max(value, minimum))));
		}
	}
	else if (parameter == "framerate")
	{
		const auto &limits = info.at(&controls::FrameDurationLimits);
		int64_t shortest = static_cast<int64_t>(number<int64_t>(limits.min(), 100));
		int64_t longest = static_cast<int64_t>(number<int64_t>(limits.max(), 1000000));
		if (value > 0)
		{
			int64_t duration = std::clamp(static_cast<int64_t>(std::llround(1000000.0 / value)), shortest, longest);
			changes.set(controls::FrameDurationLimits, std::span<const int64_t, 2>({ duration, duration }));
			if (state.values.count("shutter") && state.values.at("shutter") > duration)
				changes.set(controls::ExposureTime, static_cast<int32_t>(std::min<int64_t>(duration, INT32_MAX)));
		}
		else
			changes.set(controls::FrameDurationLimits, std::span<const int64_t, 2>({ shortest, longest }));
	}
	else if (parameter == "awb_red" || parameter == "awb_blue" ||
		(parameter == "awb" && std::round(value) == namedIndex("awb", "manual")))
	{
		if (!state.values.count("awb_red") || !state.values.count("awb_blue"))
			return;
		const auto &red_range = state.ranges.at("awb_red");
		const auto &blue_range = state.ranges.at("awb_blue");
		float red = std::clamp(parameter == "awb_red" ? value : state.values.at("awb_red"),
							   red_range.first, red_range.second);
		float blue = std::clamp(parameter == "awb_blue" ? value : state.values.at("awb_blue"),
								blue_range.first, blue_range.second);
		changes.set(controls::AwbEnable, false);
		changes.set(controls::ColourGains, std::span<const float, 2>({ red, blue }));
	}
	else if (auto found = enum_controls.find(parameter); found != enum_controls.end())
	{
		const auto &choices = rt_roi::Parameter(parameter).choices;
		const auto &choice = choices.at(static_cast<size_t>(std::round(value)));
		auto mode = found->second.modes.find(choice);
		if (mode == found->second.modes.end() || !supportsMode(info.at(found->second.control), mode->second))
			return;
		if (parameter == "awb")
			changes.set(controls::AwbEnable, true);
		changes.set(*found->second.control, mode->second);
	}
	else if (parameter == "zoom" || parameter.compare(0, 4, "roi_") == 0)
	{
		double x = state.values.at("roi_x"), y = state.values.at("roi_y");
		double width = state.values.at("roi_width"), height = state.values.at("roi_height");
		if (parameter == "zoom")
		{
			double centre_x = x + width / 2, centre_y = y + height / 2;
			width = height = 1 / value;
			x = centre_x - width / 2;
			y = centre_y - height / 2;
		}
		else if (parameter == "roi_x") x = value;
		else if (parameter == "roi_y") y = value;
		else if (parameter == "roi_width") width = value;
		else if (parameter == "roi_height") height = value;
		width = std::clamp(width, state.ranges.at("roi_width").first, 1.0);
		height = std::clamp(height, state.ranges.at("roi_height").first, 1.0);
		x = std::clamp(x, 0.0, 1 - width);
		y = std::clamp(y, 0.0, 1 - height);
		Rectangle crop(rt_roi_sensor_.x + static_cast<int>(std::round(x * rt_roi_sensor_.width)),
					   rt_roi_sensor_.y + static_cast<int>(std::round(y * rt_roi_sensor_.height)),
					   static_cast<unsigned int>(std::round(width * rt_roi_sensor_.width)),
					   static_cast<unsigned int>(std::round(height * rt_roi_sensor_.height)));
		crop.x = std::min(crop.x, rt_roi_sensor_.x + static_cast<int>(rt_roi_sensor_.width - crop.width));
		crop.y = std::min(crop.y, rt_roi_sensor_.y + static_cast<int>(rt_roi_sensor_.height - crop.height));
		if (SupportsScalerCrops())
		{
			std::vector<Rectangle> crops(1 + (LoresStream() ? 1 : 0), crop);
			changes.set(controls::rpi::ScalerCrops, std::span<const Rectangle>(crops.data(), crops.size()));
		}
		else
			changes.set(controls::ScalerCrop, crop);
	}
	if (!changes.empty())
	{
		setControlsLocked(changes);
		// Different UI choices may map to the same camera enum (auto/cdn_off).
		// Keep an overlay-origin choice so +/- can advance past such aliases.
		if (requested_mode)
			state.values[parameter] = *requested_mode;
	}
}
