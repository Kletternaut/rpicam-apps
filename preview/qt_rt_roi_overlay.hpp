/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "core/rt_roi_overlay.hpp"

#include <QByteArray>
#include <QPointF>
#include <QSize>
#include <QSvgRenderer>

class QPainter;

// This object is used exclusively by the Qt GUI thread. The callbacks bridge
// to the application's synchronized runtime-control state.
class QtRtRoiOverlay
{
public:
	static constexpr int NoHit = -1, ToggleHit = -2, ResetHit = -3;
	void Set(std::shared_ptr<const rt_roi::Config> config, std::function<rt_roi::State()> read,
			 std::function<void(const std::string &, double)> write);
	rt_roi::State Snapshot() const;
	bool Configured() const { return bool(config_); }
	int Hit(const QPointF &position, const QSize &size, const rt_roi::State &state) const;
	void Paint(QPainter &painter, const QSize &size, const rt_roi::State &state);
	void Press(int index, const QPointF &position, const QSize &size, const rt_roi::State &state);
	void Release(const QPointF &position, const QSize &size, const rt_roi::State &state);
	void Drag(const QPointF &position, const QSize &size, const rt_roi::State &state);
	void Wheel(int index, int delta, const rt_roi::State &state);
	void CancelDrag() { dragging_ = -1; toggle_pressed_ = false; }

private:
	struct CachedSvg
	{
		QByteArray source;
		std::unique_ptr<QSvgRenderer> renderer;
	};
	bool Visible(const rt_roi::State &state) const;
	std::pair<double, double> Range(const rt_roi::Control &control, const rt_roi::State &state) const;
	void SetValue(int index, double value, const rt_roi::State &state);
	void SetSlider(int index, double fraction, const rt_roi::State &state);
	void ResetAll();
	std::shared_ptr<const rt_roi::Config> config_;
	std::function<rt_roi::State()> read_;
	std::function<void(const std::string &, double)> write_;
	std::vector<CachedSvg> cache_;
	CachedSvg toggle_cache_;
	CachedSvg reset_cache_;
	int dragging_ = -1;
	bool controls_visible_ = true;
	bool toggle_pressed_ = false;
};
