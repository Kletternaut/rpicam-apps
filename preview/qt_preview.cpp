/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2021, Raspberry Pi (Trading) Ltd.
 *
 * qt_preview.cpp - Qt preview window
 */

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string.h>
#include <thread>

// This header must be before the QT headers, as the latter #defines slot and emit!
#include "core/options.hpp"

#include <QApplication>
#include <QDesktopServices>
#include <QImage>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QThread>
#include <QUrl>
#include <QWheelEvent>
#include <QWidget>

#include "preview.hpp"
#include "preview_plane_resample.hpp"
#ifdef QT_RT_ROI_OVERLAY
#include "qt_rt_roi_overlay.hpp"
#endif

class QtPreview; // forward declaration for MyWidget

class MyMainWindow : public QMainWindow
{
public:
	MyMainWindow() : QMainWindow()
	{
	}
	bool quit = false;

protected:
	void closeEvent(QCloseEvent *event) override
	{
		event->ignore();
		quit = true;
	}
};

class MyWidget : public QWidget
{
public:
	MyWidget(QWidget *parent, int w, int h, QtPreview *owner = nullptr) : QWidget(parent), size(w, h), owner_(owner)
	{
		image = QImage(size, QImage::Format_RGB888);
		image.fill(0);
		setMouseTracking(true);
	}
	QSize size;
	QImage image;
	QRectF link_rect_;
	QString link_url_;

	QRectF selection_ {};
	bool selecting_ = false;
	bool roi_active_ = false; // true while a sub-frame ROI is applied
	QPointF start_pos_;
	QtPreview *owner_ = nullptr;

	enum class Gesture { None, Overlay, Roi, Reset, Link, Ignore };
	Gesture gesture_ = Gesture::None;
	Qt::MouseButton gesture_button_ = Qt::NoButton;
	bool gesture_multiple_buttons_ = false;
#ifdef QT_RT_ROI_OVERLAY
	QtRtRoiOverlay overlay_;
#endif
	void CancelGesture()
	{
		gesture_ = Gesture::None;
		gesture_button_ = Qt::NoButton;
		gesture_multiple_buttons_ = false;
		selecting_ = false;
		selection_ = QRectF();
#ifdef QT_RT_ROI_OVERLAY
		overlay_.CancelDrag();
#endif
		update();
	}
	void SetClickTarget(double x, double y, double width, double height, const std::string &url)
	{
		link_rect_ = url.empty() ? QRectF() : QRectF(x, y, width, height);
		link_url_ = QString::fromStdString(url);
		update();
	}
	bool LinkContains(const QPointF &position) const
	{
		return !link_url_.isEmpty() && width() > 0 && height() > 0 &&
			link_rect_.contains(QPointF(position.x() / width(), position.y() / height()));
	}

protected:
	bool event(QEvent *event) override
	{
		if (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide ||
			event->type() == QEvent::UngrabMouse || event->type() == QEvent::Resize)
			CancelGesture();
		return QWidget::event(event);
	}
	rt_roi::State OverlayState()
	{
#ifdef QT_RT_ROI_OVERLAY
		auto state = overlay_.Snapshot();
		if (overlay_.Configured())
			roi_active_ = state.roi_active;
		return state;
#else
		return {};
#endif
	}
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
		painter.drawImage(rect(), image, image.rect());
		if (!selection_.isNull())
		{
			QPen pen(QColor(0, 180, 255, 200));
			pen.setWidth(1);
			painter.setPen(pen);
			painter.setBrush(Qt::NoBrush);
			painter.drawRect(selection_);
		}
#ifdef QT_RT_ROI_OVERLAY
		overlay_.Paint(painter, QSize(width(), height()), OverlayState());
#endif
	}
	void mousePressEvent(QMouseEvent *event) override
	{
		// The first pressed button owns the complete gesture. Additional buttons
		// cannot transfer an overlay drag to ROI selection (or reset it).
		event->accept();
		if (gesture_ != Gesture::None)
		{
			gesture_multiple_buttons_ = true;
			return;
		}
		[[maybe_unused]] const auto state = OverlayState();
		gesture_button_ = event->button();
		gesture_multiple_buttons_ = false;
#ifdef QT_RT_ROI_OVERLAY
		const int hit = overlay_.Hit(event->pos(), QSize(width(), height()), state);
		if (hit != QtRtRoiOverlay::NoHit)
		{
			gesture_ = Gesture::Overlay;
			if (event->button() == Qt::LeftButton)
				overlay_.Press(hit, event->pos(), QSize(width(), height()), state);
			update();
			return;
		}
#endif
		gesture_ = Gesture::Ignore;
		if (event->button() == Qt::RightButton)
			gesture_ = Gesture::Reset;
		else if (event->button() == Qt::LeftButton && LinkContains(event->pos()))
			gesture_ = Gesture::Link;
		else if (event->button() == Qt::LeftButton && !roi_active_ && width() > 0 && height() > 0)
		{
			gesture_ = Gesture::Roi;
			selecting_ = true;
			start_pos_ = event->pos();
			selection_ = QRectF(start_pos_, start_pos_);
			update();
		}
	}
	void mouseMoveEvent(QMouseEvent *event) override
	{
		event->accept();
		if (gesture_ == Gesture::None)
			setCursor(LinkContains(event->pos()) ? Qt::PointingHandCursor : Qt::ArrowCursor);
		[[maybe_unused]] const auto state = OverlayState();
#ifdef QT_RT_ROI_OVERLAY
		if (gesture_ == Gesture::Overlay && gesture_button_ == Qt::LeftButton &&
			(event->buttons() & Qt::LeftButton))
		{
			overlay_.Drag(event->pos(), QSize(width(), height()), state);
			update();
			return;
		}
#endif
		if (selecting_)
		{
			if (roi_active_)
			{
				// A socket or other control changed the crop during selection.
				selecting_ = false;
				selection_ = QRectF();
				gesture_ = Gesture::Ignore;
			}
			else
				UpdateSelection(event->pos());
			update();
		}
	}
	void UpdateSelection(const QPointF &position)
	{
		if (width() <= 0 || height() <= 0)
			return;
		const double dx = position.x() - start_pos_.x(), dy = position.y() - start_pos_.y();
		const double sign_x = dx < 0 ? -1 : 1, sign_y = dy < 0 ? -1 : 1;
		// Equal normalized width/height preserves the preview aspect ratio.
		// Limit both axes together so every drag direction stays inside the image.
		double fraction = std::max(std::abs(dx) / width(), std::abs(dy) / height());
		fraction = std::min(fraction, (sign_x > 0 ? width() - start_pos_.x() : start_pos_.x()) / width());
		fraction = std::min(fraction, (sign_y > 0 ? height() - start_pos_.y() : start_pos_.y()) / height());
		selection_ = QRectF(start_pos_, start_pos_ + QPointF(sign_x * fraction * width(),
															 sign_y * fraction * height())).normalized();
	}
	void wheelEvent(QWheelEvent *event) override
	{
		[[maybe_unused]] const auto state = OverlayState();
#ifdef QT_RT_ROI_OVERLAY
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
		const QPointF position = event->position();
#else
		const QPointF position = event->posF();
#endif
		const int hit = overlay_.Hit(position, QSize(width(), height()), state);
		if (gesture_ != Gesture::None || hit != QtRtRoiOverlay::NoHit)
		{
			// Scrolling during a drag must not act on a second control.
			if (gesture_ == Gesture::None)
				overlay_.Wheel(hit, event->angleDelta().y(), state);
			event->accept();
			update();
			return;
		}
#endif
		event->ignore();
	}
	void mouseReleaseEvent(QMouseEvent *event) override;
	QSize sizeHint() const override
	{
		return size;
	}
};

class QtPreview : public Preview
{
public:
	QtPreview(Options const *options) : Preview(options)
	{
		window_width_ = options->Get().preview_width;
		window_height_ = options->Get().preview_height;
		if (window_width_ % 2 || window_height_ % 2)
			throw std::runtime_error("QtPreview: expect even dimensions");
		// This preview window is expensive, so make it small by default.
		if (window_width_ == 0 || window_height_ == 0)
			window_width_ = 512, window_height_ = 384;
		// As a hint, reserve twice the binned width for our widest current camera (V3)
		tmp_stripe_.reserve(4608);
		thread_ = std::thread(&QtPreview::threadFunc, this, options);
		std::unique_lock lock(mutex_);
		while (!pane_)
			cond_var_.wait(lock);
		LOG(2, "Made Qt preview");
	}
	~QtPreview()
	{
		application_->exit();
		thread_.join();
	}
	void SetInfoText(const std::string &text) override
	{
		main_window_->setWindowTitle(QString::fromStdString(text));
	}
	void SetRoiCallback(RoiCallback cb) override
	{
		auto set = [this, cb = std::move(cb)]() mutable { roi_callback_ = std::move(cb); };
		if (QThread::currentThread() == pane_->thread())
			set();
		else
			QMetaObject::invokeMethod(pane_, std::move(set), Qt::BlockingQueuedConnection);
	}
	bool SetRtRoiOverlay(std::shared_ptr<const rt_roi::Config> config, std::function<rt_roi::State()> read,
						 std::function<void(const std::string &, double)> write) override
	{
#ifdef QT_RT_ROI_OVERLAY
		auto set = [this, config = std::move(config), read = std::move(read), write = std::move(write)]() mutable {
			pane_->CancelGesture();
			pane_->overlay_.Set(std::move(config), std::move(read), std::move(write));
			pane_->update();
		};
		// Synchronous removal also waits for a callback already running on Qt.
		if (QThread::currentThread() == pane_->thread())
			set();
		else
			QMetaObject::invokeMethod(pane_, std::move(set), Qt::BlockingQueuedConnection);
		return true;
#else
		return false;
#endif
	}
	bool SetClickTarget(double x, double y, double width, double height, const std::string &url) override
	{
		// Snapshot for Show(): the overlay region gets a filtered preview
		// re-render while the rest of the frame stays cheap nearest-neighbour.
		overlay_x_ = x;
		overlay_y_ = y;
		overlay_w_ = width;
		overlay_h_ = height;
		overlay_region_active_.store(width > 0 && height > 0, std::memory_order_release);
		auto set = [this, x, y, width, height, url]() {
			pane_->SetClickTarget(x, y, width, height, url);
		};
		if (QThread::currentThread() == pane_->thread())
			set();
		else
			QMetaObject::invokeMethod(pane_, std::move(set), Qt::BlockingQueuedConnection);
		return true;
	}
	virtual void Show(int fd, std::span<uint8_t> span, StreamInfo const &info) override
	{
		// Quick and simple nearest-neighbour-ish resampling is used here.
		// We further share U,V samples between adjacent output pixel pairs
		// (even when downscaling) to speed up the conversion.
		unsigned x_step = (info.width << 16) / window_width_;
		unsigned y_step = (info.height << 16) / window_height_;

		// Choose the right matrix to convert YUV back to RGB.
		static const float YUV2RGB[3][9] = {
			{ 1.0, 0.0, 1.402, 1.0, -0.344, -0.714, 1.0, 1.772, 0.0 }, // JPEG
			{ 1.164, 0.0, 1.596, 1.164, -0.392, -0.813, 1.164, 2.017, 0.0 }, // SMPTE170M
			{ 1.164, 0.0, 1.793, 1.164, -0.213, -0.533, 1.164, 2.112, 0.0 }, // Rec709
		};
		int offsetY;
		float coeffY, coeffVR, coeffUG, coeffVG, coeffUB;
		if (info.colour_space == libcamera::ColorSpace::Smpte170m)
		{
			offsetY = 16;
			coeffY = YUV2RGB[1][0];
			coeffVR = YUV2RGB[1][2];
			coeffUG = YUV2RGB[1][4];
			coeffVG = YUV2RGB[1][5];
			coeffUB = YUV2RGB[1][7];
		}
		else if (info.colour_space == libcamera::ColorSpace::Rec709)
		{
			offsetY = 16;
			coeffY = YUV2RGB[2][0];
			coeffVR = YUV2RGB[2][2];
			coeffUG = YUV2RGB[2][4];
			coeffVG = YUV2RGB[2][5];
			coeffUB = YUV2RGB[2][7];
		}
		else
		{
			offsetY = 0;
			coeffY = YUV2RGB[0][0];
			coeffVR = YUV2RGB[0][2];
			coeffUG = YUV2RGB[0][4];
			coeffVG = YUV2RGB[0][5];
			coeffUB = YUV2RGB[0][7];
			if (info.colour_space != libcamera::ColorSpace::Sycc)
				LOG(1, "QtPreview: unexpected colour space " << libcamera::ColorSpace::toString(info.colour_space));
		}

		// Because the source buffer is uncached, and we want to read it a byte at a time,
		// take a copy of each row used. This is a speedup provided memcpy() is vectorized.
		tmp_stripe_.resize(2 * info.stride);
		uint8_t const *Y_start = span.data();
		uint8_t *Y_row = &tmp_stripe_[0];
		uint8_t *U_row = Y_row + info.stride;
		uint8_t *V_row = U_row + (info.stride >> 1);

		// Possibly this should be locked in case a repaint is happening? In practice the risk
		// is only that there might be some tearing, so I don't think we worry. We could speed
		// it up by getting the ISP to supply RGB, but I'm not sure I want to handle that extra
		// possibility in our main application code, so we'll put up with the slow conversion.
		for (unsigned int y = 0; y < window_height_; y++)
		{
			unsigned row = (y * y_step) >> 16;
			uint8_t *dest = pane_->image.scanLine(y);
			unsigned x_pos = x_step >> 1;

			memcpy(Y_row, Y_start + row * info.stride, info.stride);
			memcpy(U_row, Y_start + ((4 * info.height + row) >> 1) * (info.stride >> 1), info.stride >> 1);
			memcpy(V_row, Y_start + ((5 * info.height + row) >> 1) * (info.stride >> 1), info.stride >> 1);

			for (unsigned int x = 0; x < window_width_; x += 2)
			{
				int Y0 = Y_row[x_pos >> 16];
				x_pos += x_step;
				int Y1 = Y_row[x_pos >> 16];
				int U = U_row[x_pos >> 17];
				int V = V_row[x_pos >> 17];
				x_pos += x_step;
				Y0 -= offsetY;
				Y1 -= offsetY;
				U -= 128;
				V -= 128;
				int R0 = coeffY * Y0 + coeffVR * V;
				int G0 = coeffY * Y0 + coeffUG * U + coeffVG * V;
				int B0 = coeffY * Y0 + coeffUB * U;
				int R1 = coeffY * Y1 + coeffVR * V;
				int G1 = coeffY * Y1 + coeffUG * U + coeffVG * V;
				int B1 = coeffY * Y1 + coeffUB * U;
				*(dest++) = std::clamp(R0, 0, 255);
				*(dest++) = std::clamp(G0, 0, 255);
				*(dest++) = std::clamp(B0, 0, 255);
				*(dest++) = std::clamp(R1, 0, 255);
				*(dest++) = std::clamp(G1, 0, 255);
				*(dest++) = std::clamp(B1, 0, 255);
			}
		}

		// The overlay region gets a properly filtered re-render so its thin
		// strokes survive the downscale; the rest of the frame keeps the cheap
		// nearest-neighbour path above. Filtering just this rectangle costs a
		// fraction of a full-frame filter.
		if (overlay_region_active_.load(std::memory_order_acquire))
		{
			const double ox = std::clamp(overlay_x_, 0.0, 1.0);
			const double oy = std::clamp(overlay_y_, 0.0, 1.0);
			const double ow = std::clamp(overlay_w_, 0.0, 1.0 - ox);
			const double oh = std::clamp(overlay_h_, 0.0, 1.0 - oy);
			const int dx0 = std::max(0, std::min(int(window_width_) - 1, int(std::lround(ox * window_width_))));
			const int dy0 = std::max(0, std::min(int(window_height_) - 1, int(std::lround(oy * window_height_))));
			const int dw = std::max(1, std::min(int(window_width_) - dx0, int(std::lround(ow * window_width_))));
			const int dh = std::max(1, std::min(int(window_height_) - dy0, int(std::lround(oh * window_height_))));
			const int sx0 = std::max(0, std::min(int(info.width) - 1, int(std::lround(ox * info.width))));
			const int sy0 = std::max(0, std::min(int(info.height) - 1, int(std::lround(oy * info.height))));
			const int sw = std::max(1, std::min(int(info.width) - sx0, int(std::lround(ow * info.width))));
			const int sh = std::max(1, std::min(int(info.height) - sy0, int(std::lround(oh * info.height))));
			const size_t yBytes = size_t(info.stride) * info.height;
			const size_t uvBytes = size_t(info.stride / 2) * (info.height / 2);
			const uint8_t *yBase = span.data() + size_t(sy0) * info.stride + sx0;
			const uint8_t *uBase = span.data() + yBytes + size_t(sy0 / 2) * (info.stride / 2) + sx0 / 2;
			const uint8_t *vBase = span.data() + yBytes + uvBytes + size_t(sy0 / 2) * (info.stride / 2) + sx0 / 2;
			const auto &luma = yScaler_.resize(yBase, sw, sh, info.stride, dw, dh);
			const auto &chromaU = uScaler_.resize(uBase, sw / 2, sh / 2, info.stride / 2, dw, dh);
			const auto &chromaV = vScaler_.resize(vBase, sw / 2, sh / 2, info.stride / 2, dw, dh);
			for (int y = 0; y < dh; ++y)
			{
				uint8_t *dest = pane_->image.scanLine(dy0 + y) + size_t(dx0) * 3;
				for (int x = 0; x < dw; ++x)
				{
					const size_t i = size_t(y) * dw + x;
					int Y = int(luma[i]) - offsetY;
					int U = int(chromaU[i]) - 128;
					int V = int(chromaV[i]) - 128;
					int R = coeffY * Y + coeffVR * V;
					int G = coeffY * Y + coeffUG * U + coeffVG * V;
					int B = coeffY * Y + coeffUB * U;
					*(dest++) = std::clamp(R, 0, 255);
					*(dest++) = std::clamp(G, 0, 255);
					*(dest++) = std::clamp(B, 0, 255);
				}
			}
		}

		pane_->update();

		// Return the buffer to the camera system.
		done_callback_(fd);
	}
	// Reset the preview window, clearing the current buffers and being ready to
	// show new ones.
	void Reset() override
	{
	}
	// Check if preview window has been shut down.
	bool Quit() override
	{
		return main_window_->quit;
	}
	// There is no particular limit to image sizes, though large images will be very slow.
	virtual void MaxImageSize(unsigned int &w, unsigned int &h) const override
	{
		w = h = 0;
	}

private:
	friend class MyWidget;

	void SelectionFinished(float x, float y, float w, float h)
	{
		if (roi_callback_)
			roi_callback_(x, y, w, h);
	}

	void threadFunc(Options const *options)
	{
		// This acts as Qt's event loop. Really Qt prefers to own the application's event loop
		// but we've supplied our own and only want Qt for rendering. This works, but I
		// wouldn't write a proper Qt application like this.
		int argc = 0;
		char **argv = NULL;
		QApplication application(argc, argv);
		application_ = &application;
		MyMainWindow main_window;
		main_window_ = &main_window;
		MyWidget pane(&main_window, window_width_, window_height_, this);
		main_window.setCentralWidget(&pane);
		// Need to get the window border sizes (it seems to be unreasonably difficult...)
		main_window.move(options->Get().preview_x + 2, options->Get().preview_y + 28);
		main_window.show();
		{
			std::lock_guard lock(mutex_);
			pane_ = &pane;
		}
		cond_var_.notify_one();
		application.exec();
	}
	QApplication *application_ = nullptr;
	MyMainWindow *main_window_ = nullptr;
	MyWidget *pane_ = nullptr;
	std::thread thread_;
	unsigned int window_width_, window_height_;
	std::mutex mutex_;
	std::condition_variable cond_var_;
	std::vector<uint8_t> tmp_stripe_;
	preview_resample::PlaneScaler yScaler_, uScaler_, vScaler_;
	double overlay_x_ = 0.0, overlay_y_ = 0.0, overlay_w_ = 0.0, overlay_h_ = 0.0;
	std::atomic<bool> overlay_region_active_{false};
	RoiCallback roi_callback_;
};

// Out-of-line definition: QtPreview must be complete before MyWidget::mouseReleaseEvent
// can call owner_->SelectionFinished().
void MyWidget::mouseReleaseEvent(QMouseEvent *event)
{
	event->accept();
	OverlayState();
#ifdef QT_RT_ROI_OVERLAY
	if (event->button() == gesture_button_ && gesture_button_ == Qt::LeftButton &&
		gesture_ == Gesture::Overlay && !gesture_multiple_buttons_)
		overlay_.Release(event->pos(), QSize(width(), height()), overlay_.Snapshot());
#endif
	if (event->button() == gesture_button_ && gesture_button_ == Qt::LeftButton && gesture_ == Gesture::Link &&
		LinkContains(event->pos()))
		QDesktopServices::openUrl(QUrl(link_url_));
	if (event->button() == gesture_button_ && gesture_ == Gesture::Reset)
	{
		// Only a right-button gesture started outside the controls can reset ROI.
		selecting_ = false;
		roi_active_ = false;
		selection_ = QRectF();
		if (owner_)
			owner_->SelectionFinished(0.0f, 0.0f, 1.0f, 1.0f);
	}
	else if (event->button() == gesture_button_ && gesture_ == Gesture::Roi && selecting_ && !roi_active_)
	{
		selecting_ = false;
		// Apply the same aspect lock and bounds used while displaying the drag.
		UpdateSelection(event->pos());

		if (owner_ && selection_.width() >= 3 && selection_.height() >= 3 && width() > 0 && height() > 0)
		{
			float roi_x = float(selection_.left()) / float(width());
			float roi_y = float(selection_.top()) / float(height());
			float roi_w = float(selection_.width()) / float(width());
			float roi_h = float(selection_.height()) / float(height());
			roi_active_ = true;
			owner_->SelectionFinished(roi_x, roi_y, roi_w, roi_h);
		}
	}
	if (event->button() == gesture_button_)
	{
		// Keep consuming any additional buttons until the whole gesture ends.
		gesture_ = Gesture::Ignore;
		selecting_ = false;
		selection_ = QRectF();
#ifdef QT_RT_ROI_OVERLAY
		overlay_.CancelDrag();
#endif
	}
	if (event->buttons() == Qt::NoButton)
		CancelGesture();
	update();
}

static Preview *Create(Options const *options)
{
	return new QtPreview(options);
}

static RegisterPreview reg("qt", &Create);
