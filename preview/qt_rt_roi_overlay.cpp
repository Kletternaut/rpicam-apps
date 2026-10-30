/* SPDX-License-Identifier: BSD-2-Clause */
#include "qt_rt_roi_overlay.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

#include <QColor>
#include <QFontMetrics>
#include <QPainter>
#include <QRectF>

namespace
{
constexpr double canvas_height = 42;
constexpr double track_left = 50, track_right_margin = 50;

QRectF bounds(const rt_roi::Config &config, const rt_roi::Control &control, const QSize &size)
{
	return { control.x * size.width(), control.y * size.height(),
			 control.width * config.scale * size.width(), control.height * config.scale * size.height() };
}

// The canvas is a fixed-height virtual surface whose width follows the actual
// on-screen rectangle. Both scale factors then stay equal, so circles, buttons
// and text keep their shape at any window size; only the track stretches.
double canvasWidth(const rt_roi::Config &config, const rt_roi::Control &control, const QSize &size)
{
	const QRectF rectangle = bounds(config, control, size);
	if (rectangle.width() <= 0 || rectangle.height() <= 0)
		return 280;
	return canvas_height * rectangle.width() / rectangle.height();
}

double trackRight(double width)
{
	return width - track_right_margin;
}

QRectF toggleBounds(const rt_roi::Config &config, const QSize &size)
{
	return { config.toggle.x * size.width(), config.toggle.y * size.height(),
			 config.toggle.width * size.width(), config.toggle.height * size.height() };
}

QRectF resetBounds(const rt_roi::Config &config, const QSize &size)
{
	return { config.reset.x * size.width(), config.reset.y * size.height(),
			 config.reset.width * size.width(), config.reset.height * size.height() };
}

QPointF logicalPosition(const rt_roi::Config &config, const rt_roi::Control &control,
						const QPointF &position, const QSize &size)
{
	const QRectF rectangle = bounds(config, control, size);
	if (rectangle.width() <= 0 || rectangle.height() <= 0)
		return { 0, 0 };
	const double width = canvasWidth(config, control, size);
	return { (position.x() - rectangle.x()) * width / rectangle.width(),
			 (position.y() - rectangle.y()) * canvas_height / rectangle.height() };
}

bool modeInRange(int mode, const rt_roi::Control &control, double minimum, double maximum)
{
	return mode >= 0 && mode < static_cast<int>(control.choices.size()) && mode >= minimum && mode <= maximum;
}

bool hasSupportedMode(const rt_roi::Control &control, const rt_roi::State &state, double minimum, double maximum)
{
	const auto modes = state.supported_modes.find(control.parameter);
	if (control.choices.empty() || modes == state.supported_modes.end())
		return true;
	return std::any_of(modes->second.begin(), modes->second.end(),
		[&](int mode) { return modeInRange(mode, control, minimum, maximum); });
}
} // namespace

void QtRtRoiOverlay::Set(std::shared_ptr<const rt_roi::Config> config, std::function<rt_roi::State()> read,
						 std::function<void(const std::string &, double)> write)
{
	config_ = std::move(config);
	read_ = std::move(read);
	write_ = std::move(write);
	cache_.clear();
	toggle_cache_ = {};
	if (config_)
	{
		cache_.resize(config_->controls.size());
		controls_visible_ = config_->initially_visible;
	}
	CancelDrag();
}

rt_roi::State QtRtRoiOverlay::Snapshot() const
{
	return config_ && read_ ? read_() : rt_roi::State {};
}

bool QtRtRoiOverlay::Visible(const rt_roi::State &state) const
{
	return config_ && controls_visible_ && state.enabled && config_->opacity > 0;
}

int QtRtRoiOverlay::Hit(const QPointF &position, const QSize &size, const rt_roi::State &state) const
{
	if (!config_ || size.width() <= 0 || size.height() <= 0)
		return NoHit;
	// The toggle remains reachable even with fully transparent or disabled rows.
	if (rt_roi::ContainsToggle(*config_, position.x() / size.width(), position.y() / size.height()))
		return ToggleHit;
	if (rt_roi::ContainsReset(*config_, position.x() / size.width(), position.y() / size.height()))
		return ResetHit;
	if (!Visible(state))
		return NoHit;
	// Reverse paint order also makes overlapping custom layouts deterministic.
	for (int index = static_cast<int>(config_->controls.size()) - 1; index >= 0; --index)
		if (rt_roi::Contains(*config_, config_->controls[index], position.x() / size.width(),
							 position.y() / size.height()))
			return index;
	return NoHit;
}

std::pair<double, double> QtRtRoiOverlay::Range(const rt_roi::Control &control, const rt_roi::State &state) const
{
	double minimum = control.min, maximum = control.max;
	auto range = state.ranges.find(control.parameter);
	if (range != state.ranges.end())
	{
		minimum = std::max(minimum, range->second.first);
		maximum = std::min(maximum, range->second.second);
	}
	return { minimum, maximum };
}

void QtRtRoiOverlay::Paint(QPainter &painter, const QSize &size, const rt_roi::State &state)
{
	if (!config_)
		return;
	const QString foreground = QColor(QString::fromStdString(config_->foreground)).name();
	const QString accent = QColor(QString::fromStdString(config_->accent)).name();
	const QString background = QColor(QString::fromStdString(config_->background)).name();
	for (size_t index = 0; Visible(state) && index < config_->controls.size(); ++index)
	{
		const auto &control = config_->controls[index];
		const auto value = state.values.find(control.parameter);
		const auto [minimum, maximum] = Range(control, state);
		const bool available = value != state.values.end() && maximum >= minimum &&
			hasSupportedMode(control, state, minimum, maximum);
		const double fraction = available && maximum > minimum ?
			std::clamp((value->second - minimum) / (maximum - minimum), 0.0, 1.0) : 0;
		const double width = canvasWidth(*config_, control, size);
		const double right_track = trackRight(width);
		const QString thumb = QString::number(track_left + (right_track - track_left) * fraction, 'f', 2);
		const QString active = available ? accent : foreground;
		const QByteArray svg = (QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 ") +
			QString::number(width, 'f', 2) + QStringLiteral(" 42\">") +
			"<rect x=\"0\" y=\"0\" width=\"" + QString::number(width, 'f', 2) + "\" height=\"42\" rx=\"6\" fill=\"" + background +
			"\" fill-opacity=\"" + QString::number(config_->background_opacity) + "\"/>" +
			"<g fill=\"none\" stroke=\"" + foreground + "\" stroke-width=\"2\">" +
			"<rect x=\"8\" y=\"17\" width=\"26\" height=\"23\" rx=\"5\"/>" +
			"<rect x=\"" + QString::number(width - 34.0, 'f', 2) + "\" y=\"17\" width=\"26\" height=\"23\" rx=\"5\"/>" +
			"<path d=\"M14 28h14 M" + QString::number(width - 28.0, 'f', 2) + " 28h14 M" + QString::number(width - 21.0, 'f', 2) + " 21v14\"/>" +
			"<path d=\"M50 28H" + QString::number(right_track, 'f', 2) + "\" stroke-opacity=\"0.45\" stroke-width=\"3\" stroke-linecap=\"round\"/>" +
			"</g><path d=\"M50 28H" + thumb + "\" stroke=\"" + active +
			"\" stroke-width=\"3\" stroke-linecap=\"round\"/><circle cx=\"" + thumb +
			"\" cy=\"28\" r=\"5\" fill=\"" + active + "\"/></svg>").toUtf8();
		auto &cached = cache_[index];
		if (!cached.renderer || cached.source != svg)
		{
			cached.source = svg;
			cached.renderer = std::make_unique<QSvgRenderer>(svg);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
			cached.renderer->setAspectRatioMode(Qt::IgnoreAspectRatio);
#endif
		}
		const QRectF rectangle = bounds(*config_, control, size);
		painter.save();
		painter.setOpacity(config_->opacity * (available ? 1.0 : 0.4));
		cached.renderer->render(&painter, rectangle);
		// Text uses Qt's native font handling; only the controls themselves are SVG.
		painter.translate(rectangle.topLeft());
		painter.scale(rectangle.width() / width, rectangle.height() / canvas_height);
		painter.setPen(QColor(foreground));
		QFont font = painter.font();
		font.setPixelSize(12);
		painter.setFont(font);
		QString label = QString::fromStdString(control.label);
		QString displayed = QStringLiteral("unavailable");
		if (available)
		{
			const auto choice = static_cast<int>(std::llround(value->second));
			displayed = !control.choices.empty() && choice >= 0 && choice < static_cast<int>(control.choices.size()) ?
				QString::fromStdString(control.choices[choice]) : QString::number(value->second, 'g', 5);
		}
		label += QStringLiteral(": ") + displayed;
		painter.drawText(QRectF(40, 1, width - 80, 15), Qt::AlignLeft | Qt::AlignVCenter,
						 QFontMetrics(font).elidedText(label, Qt::ElideRight, std::max(1, int(width - 80))));
		painter.restore();
	}
	// Paint last so the toggle wins both visually and in hit testing.
	const QRectF toggle = toggleBounds(*config_, size);
	painter.save();
	painter.setOpacity(config_->toggle.opacity);
	const double toggle_width = toggle.height() > 0 ? std::max(60.0, 40.0 * toggle.width() / toggle.height()) : 100.0;
	const QByteArray toggle_svg = (QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 ") +
		QString::number(toggle_width, 'f', 2) + QStringLiteral(" 40\">") +
		"<rect x=\"1\" y=\"1\" width=\"" + QString::number(toggle_width - 2.0, 'f', 2) + "\" height=\"38\" rx=\"7\" fill=\"" + background +
		"\" fill-opacity=\"0.8\" stroke=\"" + accent + "\" stroke-width=\"2\"/></svg>").toUtf8();
	if (!toggle_cache_.renderer || toggle_cache_.source != toggle_svg)
	{
		toggle_cache_.source = toggle_svg;
		toggle_cache_.renderer = std::make_unique<QSvgRenderer>(toggle_svg);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
		toggle_cache_.renderer->setAspectRatioMode(Qt::IgnoreAspectRatio);
#endif
	}
	toggle_cache_.renderer->render(&painter, toggle);
	painter.translate(toggle.topLeft());
	painter.scale(toggle.width() / toggle_width, toggle.height() / 40);
	painter.setPen(QColor(foreground));
	QFont toggle_font = painter.font();
	toggle_font.setPixelSize(17);
	painter.setFont(toggle_font);
	painter.drawText(QRectF(4, 2, toggle_width - 8, 36), Qt::AlignCenter,
					 controls_visible_ ? QStringLiteral("Hide") : QStringLiteral("Controls"));
	painter.restore();

	// Reset button returns every control to its hardware default.
	const QRectF reset = resetBounds(*config_, size);
	painter.save();
	painter.setOpacity(config_->reset.opacity);
	const double reset_width = reset.height() > 0 ? std::max(60.0, 40.0 * reset.width() / reset.height()) : 100.0;
	const QByteArray reset_svg = (QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 ") +
		QString::number(reset_width, 'f', 2) + QStringLiteral(" 40\">") +
		"<rect x=\"1\" y=\"1\" width=\"" + QString::number(reset_width - 2.0, 'f', 2) + "\" height=\"38\" rx=\"7\" fill=\"" + background +
		"\" fill-opacity=\"0.8\" stroke=\"" + accent + "\" stroke-width=\"2\"/></svg>").toUtf8();
	if (!reset_cache_.renderer || reset_cache_.source != reset_svg)
	{
		reset_cache_.source = reset_svg;
		reset_cache_.renderer = std::make_unique<QSvgRenderer>(reset_svg);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
		reset_cache_.renderer->setAspectRatioMode(Qt::IgnoreAspectRatio);
#endif
	}
	reset_cache_.renderer->render(&painter, reset);
	painter.translate(reset.topLeft());
	painter.scale(reset.width() / reset_width, reset.height() / 40);
	painter.setPen(QColor(foreground));
	QFont reset_font = painter.font();
	reset_font.setPixelSize(17);
	painter.setFont(reset_font);
	painter.drawText(QRectF(4, 2, reset_width - 8, 36), Qt::AlignCenter, QStringLiteral("Reset"));
	painter.restore();
}

void QtRtRoiOverlay::SetValue(int index, double value, const rt_roi::State &state)
{
	if (!Visible(state) || !write_ || index < 0 || index >= static_cast<int>(config_->controls.size()))
		return;
	const auto &control = config_->controls[index];
	if (!state.values.count(control.parameter))
		return;
	const auto [minimum, maximum] = Range(control, state);
	if (maximum < minimum)
		return;
	value = std::clamp(rt_roi::SnapValue(control, value), minimum, maximum);
	if (const auto modes = state.supported_modes.find(control.parameter);
		!control.choices.empty() && modes != state.supported_modes.end())
	{
		std::vector<int> allowed;
		for (int mode : modes->second)
			if (modeInRange(mode, control, minimum, maximum))
				allowed.push_back(mode);
		if (allowed.empty())
			return;
		std::sort(allowed.begin(), allowed.end());
		const double current = state.values.at(control.parameter);
		if (dragging_ == index || value == current)
		{
			// A slider chooses the nearest supported value, with a stable lower
			// tie-break. Repeated events in the same gap must not oscillate.
			value = *std::min_element(allowed.begin(), allowed.end(), [value](int a, int b) {
				return std::abs(a - value) < std::abs(b - value);
			});
		}
		else if (value > current)
		{
			// +/- and the wheel skip holes without stepping outside custom limits.
			auto next = std::lower_bound(allowed.begin(), allowed.end(), value);
			if (next == allowed.end())
				return;
			value = *next;
		}
		else
		{
			auto next = std::upper_bound(allowed.begin(), allowed.end(), value);
			if (next == allowed.begin())
				return;
			value = *std::prev(next);
		}
	}
	write_(control.parameter, value);
}

void QtRtRoiOverlay::SetSlider(int index, double fraction, const rt_roi::State &state)
{
	const auto [minimum, maximum] = Range(config_->controls[index], state);
	if (maximum > minimum)
		SetValue(index, minimum + std::clamp(fraction, 0.0, 1.0) * (maximum - minimum), state);
}

void QtRtRoiOverlay::Press(int index, const QPointF &position, const QSize &size, const rt_roi::State &state)
{
	CancelDrag();
	if (config_ && index == ToggleHit)
	{
		toggle_pressed_ = true;
		return;
	}
	if (config_ && index == ResetHit)
	{
		ResetAll();
		return;
	}
	if (!Visible(state) || index < 0 || index >= static_cast<int>(config_->controls.size()))
		return;
	const auto &control = config_->controls[index];
	const auto value = state.values.find(control.parameter);
	if (value == state.values.end())
		return;
	const QPointF point = logicalPosition(*config_, control, position, size);
	const double width = canvasWidth(*config_, control, size);
	const double right_track = trackRight(width);
	if (QRectF(8, 17, 26, 23).contains(point))
		SetValue(index, value->second - control.step, state);
	else if (QRectF(width - 34.0, 17, 26, 23).contains(point))
		SetValue(index, value->second + control.step, state);
	else if (QRectF(track_left - 8, 17, right_track - track_left + 16, 24).contains(point))
	{
		dragging_ = index;
		SetSlider(index, (point.x() - track_left) / (right_track - track_left), state);
	}
}

void QtRtRoiOverlay::Release(const QPointF &position, const QSize &size, const rt_roi::State &state)
{
	if (toggle_pressed_ && Hit(position, size, state) == ToggleHit)
		controls_visible_ = !controls_visible_;
	CancelDrag();
}

void QtRtRoiOverlay::Drag(const QPointF &position, const QSize &size, const rt_roi::State &state)
{
	if (!Visible(state) || dragging_ < 0 || size.width() <= 0 || size.height() <= 0)
		return;
	const QPointF point = logicalPosition(*config_, config_->controls[dragging_], position, size);
	const double right_track = trackRight(canvasWidth(*config_, config_->controls[dragging_], size));
	SetSlider(dragging_, (point.x() - track_left) / (right_track - track_left), state);
}

void QtRtRoiOverlay::Wheel(int index, int delta, const rt_roi::State &state)
{
	if (!Visible(state) || index < 0 || delta == 0)
		return;
	const auto &control = config_->controls[index];
	const auto value = state.values.find(control.parameter);
	if (value != state.values.end())
		SetValue(index, value->second + (delta > 0 ? control.step : -control.step), state);
}

void QtRtRoiOverlay::ResetAll()
{
	if (!read_)
		return;
	auto state = read_();
	auto reset_one = [&](const rt_roi::Control &control) {
		auto found = state.defaults.find(control.parameter);
		if (found != state.defaults.end() && write_)
			write_(control.parameter, found->second);
	};
	// Value sliders first: their implicit side-effects (focus → manual AF,
	// manual gains → manual AWB, etc.) must not override the mode defaults.
	for (const auto &control : config_->controls)
		if (control.choices.empty())
			reset_one(control);
	// Mode selectors (af, awb, exposure, …) last, so their regular defaults win.
	for (const auto &control : config_->controls)
		if (!control.choices.empty())
			reset_one(control);
}
