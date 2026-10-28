/* SPDX-License-Identifier: BSD-2-Clause */
// Exercises the actual Qt overlay controller independently of libcamera.
#include "preview/qt_rt_roi_overlay.hpp"

#include <QApplication>
#include <QFontDatabase>
#include <QImage>
#include <QPainter>
#include <cstdlib>
#include <iostream>

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

void interactions()
{
	const QSize size(1000, 500);
	auto config = std::make_shared<rt_roi::Config>();
	auto control = rt_roi::Parameter("brightness");
	control.x = 0.1;
	control.y = 0.1;
	control.width = 0.6;
	control.height = 0.2;
	config->controls = { control };
	config->background_opacity = 0;
	config->toggle.x = 0.85;
	config->toggle.y = 0.02;
	config->toggle.width = 0.1;
	config->toggle.height = 0.1;
	const QPointF toggle(900, 30);
	auto point = [](double x, double y) { return QPointF(100 + x * 1.5, 50 + y * 1.25); };
	rt_roi::State state;
	state.enabled = true;
	state.values["brightness"] = 0;
	state.ranges["brightness"] = { -1, 1 };
	int writes = 0;
	QtRtRoiOverlay overlay;
	auto read = [&] { return state; };
	auto write = [&](const std::string &parameter, double value) {
		++writes;
		state.values[parameter] = value;
	};
	overlay.Set(config, read, write);
	check(overlay.Hit(point(5, 5), size, state) == 0, "blank transparent row pixels reserve input");
	check(overlay.Hit(QPointF(701, 100), size, state) == QtRtRoiOverlay::NoHit,
		  "outside row allows ROI gesture");
	check(overlay.Hit(toggle, size, state) == QtRtRoiOverlay::ToggleHit, "toggle has its own hit target");
	overlay.Press(0, point(370, 53), size, state);
	check(writes == 1 && close(state.values["brightness"], 0.05), "plus increments configured step");
	overlay.Press(0, point(30, 53), size, state);
	check(writes == 2 && close(state.values["brightness"], 0), "minus decrements configured step");
	overlay.Press(0, point(200, 15), size, state);
	check(writes == 2, "row label consumes input without changing a value");
	overlay.Press(0, point(200, 53), size, state);
	overlay.Drag(QPointF(2000, 2000), size, state);
	check(close(state.values["brightness"], 1), "captured slider drag outside preview clamps to maximum");
	overlay.Drag(QPointF(-100, -100), size, state);
	check(close(state.values["brightness"], -1), "captured slider drag outside preview clamps to minimum");
	overlay.CancelDrag();
	int before = writes;
	overlay.Drag(point(200, 53), size, state);
	check(writes == before, "cancelled drag cannot keep changing controls");
	overlay.Wheel(0, 120, state);
	check(close(state.values["brightness"], -0.95), "wheel increments a control");
	overlay.Wheel(QtRtRoiOverlay::ToggleHit, 120, state);
	check(writes == before + 1, "scrolling over toggle changes no camera value");

	// A release outside the toggle must not hide the panel.
	overlay.Press(QtRtRoiOverlay::ToggleHit, toggle, size, state);
	overlay.Release(point(200, 53), size, state);
	check(overlay.Hit(point(5, 5), size, state) == 0, "toggle release outside cancels activation");
	overlay.Press(QtRtRoiOverlay::ToggleHit, toggle, size, state);
	overlay.Release(toggle, size, state);
	check(overlay.Hit(point(5, 5), size, state) == QtRtRoiOverlay::NoHit, "hidden rows release ROI area");
	check(overlay.Hit(toggle, size, state) == QtRtRoiOverlay::ToggleHit, "toggle remains reachable after hiding");
	overlay.Press(QtRtRoiOverlay::ToggleHit, toggle, size, state);
	overlay.Release(toggle, size, state);
	check(overlay.Hit(point(5, 5), size, state) == 0, "toggle restores rows");

	state.values.clear();
	before = writes;
	overlay.Press(0, point(370, 53), size, state);
	check(writes == before, "unsupported control never writes");
	check(overlay.Hit(point(5, 5), size, state) == 0, "unsupported visible row still excludes ROI");
	state.enabled = false;
	check(overlay.Hit(point(5, 5), size, state) == QtRtRoiOverlay::NoHit, "stopped camera hides inactive rows");
	check(overlay.Hit(toggle, size, state) == QtRtRoiOverlay::ToggleHit, "toggle remains available while stopped");
	state.enabled = true;
	state.values["brightness"] = 0;
	config->opacity = 0;
	overlay.Set(config, read, write);
	check(overlay.Hit(point(5, 5), size, state) == QtRtRoiOverlay::NoHit, "transparent rows have no hit targets");
	check(overlay.Hit(toggle, size, state) == QtRtRoiOverlay::ToggleHit, "toggle ignores panel opacity");

	// Painting exercises the real generated SVG and Qt font/rendering path.
	config->opacity = 0.8;
	config->background_opacity = 0.5;
	overlay.Set(config, read, write);
	QImage image(size, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	{
		QPainter painter(&image);
		overlay.Paint(painter, size, state);
	}
	check(qAlpha(image.pixel(400, 135)) > 0, "SVG control background is rendered");
	check(qAlpha(image.pixel(900, 30)) > 0, "persistent SVG toggle is rendered");
	check(qAlpha(image.pixel(800, 400)) == 0, "painting leaves preview outside overlay transparent");
	auto has_text = [&](int left, int top, int right, int bottom) {
		for (int y = top; y < bottom; ++y)
			for (int x = left; x < right; ++x)
			{
				const auto pixel = image.pixel(x, y);
				if (qRed(pixel) > 100 && qGreen(pixel) > 100 && qBlue(pixel) > 100)
					return true;
			}
		return false;
	};
	check(has_text(118, 56, 682, 89), "control label and requested value are rendered");
	check(has_text(860, 20, 940, 50), "toggle text is rendered");
	const QString image_path = qEnvironmentVariable("RT_ROI_TEST_IMAGE");
	if (!image_path.isEmpty())
		check(image.save(image_path), "optional SVG rendering sample is saved");
	overlay.Set({}, {}, {});
	check(!overlay.Configured() && !overlay.Snapshot().enabled, "detach clears configuration and callbacks");
	check(overlay.Hit(toggle, size, state) == QtRtRoiOverlay::NoHit, "detached overlay has no hit targets");
}

void sparse_modes()
{
	const QSize size(1000, 500);
	auto config = std::make_shared<rt_roi::Config>();
	auto control = rt_roi::Parameter("awb");
	control.x = 0.1;
	control.y = 0.1;
	control.width = 0.6;
	control.height = 0.2;
	config->controls = { control };
	auto point = [](double x, double y) { return QPointF(100 + x * 1.5, 50 + y * 1.25); };
	rt_roi::State state;
	state.enabled = true;
	state.values["awb"] = 1;
	state.ranges["awb"] = { 0, 8 };
	state.supported_modes["awb"] = { 0, 1, 8 };
	int writes = 0;
	QtRtRoiOverlay overlay;
	auto read = [&] { return state; };
	auto write = [&](const std::string &parameter, double value) {
		++writes;
		state.values[parameter] = value;
	};
	overlay.Set(config, read, write);
	overlay.Press(0, point(370, 53), size, state);
	check(writes == 1 && state.values["awb"] == 8, "plus skips unsupported mode indices");
	overlay.Press(0, point(30, 53), size, state);
	check(writes == 2 && state.values["awb"] == 1, "minus skips unsupported mode indices");
	overlay.Wheel(0, 120, state);
	check(state.values["awb"] == 8, "wheel skips unsupported mode indices");

	config->controls[0].max = 7;
	state.values["awb"] = 1;
	overlay.Set(config, read, write);
	overlay.Press(0, point(370, 53), size, state);
	check(state.values["awb"] == 1, "plus never selects a mode outside configured limits");
	overlay.Press(0, point(330, 53), size, state);
	check(state.values["awb"] == 1, "slider endpoint respects intersection of supported modes and limits");
	state.supported_modes["awb"].clear();
	const int before = writes;
	overlay.Press(0, point(370, 53), size, state);
	overlay.Wheel(0, -120, state);
	overlay.Press(0, point(200, 53), size, state);
	overlay.Drag(point(330, 53), size, state);
	check(writes == before, "empty supported mode list disables all writes despite stale requested value");
}
} // namespace

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
#ifdef Q_OS_WIN
	// Qt's offscreen Windows platform does not enumerate the native font database.
	// Register a system font explicitly so the headless test can verify labels.
	const int font_id = QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR") + "/Fonts/arial.ttf");
	if (font_id >= 0)
		app.setFont(QFont(QFontDatabase::applicationFontFamilies(font_id).front()));
#endif
	interactions();
	sparse_modes();
	if (failures)
		return EXIT_FAILURE;
	std::cout << "Qt RT ROI overlay tests passed\n";
	return EXIT_SUCCESS;
}
