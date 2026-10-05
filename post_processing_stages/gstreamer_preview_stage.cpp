/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2026, rpiStudio
 *
 * gstreamer_preview_stage.cpp - post-processing stage that feeds camera
 * frames into a GStreamer H.264/RTP UDP pipeline, completely independent
 * of the main encode path of rpicam-vid.
 *
 * Lifecycle (verified against core/rpicam_app.cpp and core/post_processor.cpp):
 *   Read()         - PostProcessor::Read(), before camera configuration.
 *   AdjustConfig() - not used; the stage never modifies the main stream.
 *   Configure()    - after camera_->configure() and after the streams_ map is
 *                    populated; pipeline is built here.
 *   Start()        - after camera_->start(), before the first request is
 *                    queued; pipeline goes to PLAYING here.
 *   Process()      - once per completed request (on a detached thread),
 *                    before the main encoder sees the frame. Copies the
 *                    YUV420 buffer into a GstBuffer and offers it to the
 *                    preview thread without ever blocking the capture path.
 *   Stop()         - after camera_->stop().
 *   Teardown()     - frees the remaining GStreamer resources.
 */

#include "core/buffer_sync.hpp"
#include "core/logging.hpp"
#include "core/rpicam_app.hpp"

#include "post_processing_stages/post_processing_stage.hpp"

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video-info.h>

#include <libcamera/formats.h>

#include <boost/property_tree/json_parser.hpp>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

class GstreamerPreviewStage : public PostProcessingStage
{
public:
	GstreamerPreviewStage(RPiCamApp *app) : PostProcessingStage(app), pipeline_(nullptr), appsrc_(nullptr) {}

	~GstreamerPreviewStage() override;

	char const *Name() const override;
	void Read(boost::property_tree::ptree const &params) override;
	void Configure() override;
	void Start() override;
	bool Process(CompletedRequestPtr &completed_request) override;
	void Stop() override;
	void Teardown() override;

private:
	std::string makePipelineString() const;
	void previewThread();
	void controlThread();
	void handleControlConnection(int cfd);
	std::string processControlRequest(std::string const &req);
	void applyParam(std::string const &key, std::string const &value,
					std::vector<std::string> &applied, std::map<std::string, std::string> &rejected);
	friend GstBusSyncReply busSyncHandler(GstBus *bus, GstMessage *msg, gpointer data);

	// Stream we read frames from: the lores stream if configured, otherwise
	// the main (video) stream. Lores is preferred so that the preview encode
	// does not compete with the main encode for bandwidth.
	libcamera::Stream *stream_;
	StreamInfo info_;

	// Configuration, all set from the JSON file.
	std::string host_ = "127.0.0.1";
	uint16_t port_ = 5600;
	unsigned int fps_ = 30;
	unsigned int bitrate_kbps_ = 1500;
	unsigned int gop_ = 15;
	std::string encoder_ = "x264enc";
	std::string preset_ = "ultrafast";
	unsigned int queue_depth_ = 2;
	unsigned int mtu_ = 1400;

	GstElement *pipeline_;
	GstElement *appsrc_;
	GAsyncQueue *queue_ = nullptr;
	std::thread thread_;
	std::atomic<bool> running_ { false };
	std::atomic<bool> enabled_ { false };
	std::atomic<bool> pipeline_error_ { false };
	std::atomic<unsigned int> queued_ { 0 };
	uint64_t frames_pushed_ = 0;
	uint64_t frames_dropped_ = 0;

	GstElement *enc_ = nullptr;
	GstElement *sink_ = nullptr;

	// Protects the runtime-changeable parameters below (host_, port_,
	// bitrate_kbps_, gop_, queue_depth_). The frame path reads queue_depth_
	// under the lock; the control thread writes.
	std::mutex params_mutex_;

	// Runtime control socket (rpiStudio client -> this stage).
	std::atomic<bool> control_enabled_ { false };
	std::thread control_thread_;
	int control_fd_ = -1;
	// Directory the control socket lives in (from the "control_socket" JSON
	// key). The final path is always "<dir>/rpicam-gst-preview-<pid>.sock" so
	// that parallel instances stay unique even when the directory is set.
	std::string control_socket_dir_;
	std::string control_socket_path_;

	// Set in Stop()/Teardown(); makes the preview thread terminate without a
	// NULL sentinel (g_async_queue_push() rejects NULL, so a sentinel would
	// never reach the queue and Stop() would hang in join()).
	std::atomic<bool> stop_ { false };
};

#define NAME "gstreamer_preview"

char const *GstreamerPreviewStage::Name() const
{
	return NAME;
}

void GstreamerPreviewStage::Read(boost::property_tree::ptree const &params)
{
	host_ = params.get<std::string>("host", "127.0.0.1");
	port_ = params.get<uint16_t>("port", 5600);
	fps_ = params.get<unsigned int>("fps", 30);
	bitrate_kbps_ = params.get<unsigned int>("bitrate_kbps", 1500);
	gop_ = params.get<unsigned int>("gop", 15);
	encoder_ = params.get<std::string>("encoder", "x264enc");
	preset_ = params.get<std::string>("preset", "ultrafast");
	queue_depth_ = params.get<unsigned int>("queue_depth", 2);
	mtu_ = params.get<unsigned int>("mtu", 1400);
	control_socket_dir_ = params.get<std::string>("control_socket", "");
	if (queue_depth_ == 0)
		queue_depth_ = 1;

	if (encoder_ != "x264enc" && encoder_ != "v4l2h264enc" && encoder_ != "avenc_h264")
	{
		LOG_ERROR("gstreamer_preview: unknown encoder \"" << encoder_ << "\", falling back to x264enc");
		encoder_ = "x264enc";
	}
}

GstBusSyncReply busSyncHandler(GstBus *bus, GstMessage *msg, gpointer data)
{
	(void)bus;
	if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
	{
		GError *err = nullptr;
		gchar *dbg = nullptr;
		gst_message_parse_error(msg, &err, &dbg);
		LOG_ERROR("gstreamer_preview: pipeline error: " << (err ? err->message : "unknown"));
		if (dbg)
			LOG(2, "gstreamer_preview: " << dbg);
		g_error_free(err);
		g_free(dbg);
		static_cast<GstreamerPreviewStage *>(data)->pipeline_error_ = true;
	}
	return GST_BUS_PASS;
}

std::string GstreamerPreviewStage::makePipelineString() const
{
	// The encoders use different units for the bitrate: x264enc takes
	// kbit/s, avenc_h264 and the v4l2 video_bitrate control take bit/s.
	std::string encoder_section;
	if (encoder_ == "v4l2h264enc")
	{
		// Hardware H.264 encoder (Pi 4 and earlier only - bcm2835-codec).
		// v4l2convert performs the dma-buf import into the V4L2 device.
		encoder_section = "v4l2convert ! v4l2h264enc name=enc extra-controls=s,controls,video_bitrate=" +
						  std::to_string(bitrate_kbps_ * 1000) + ",video_gop_size=" + std::to_string(gop_);
	}
	else if (encoder_ == "avenc_h264")
	{
		encoder_section = "avenc_h264 name=enc bitrate=" + std::to_string(bitrate_kbps_ * 1000) +
						  " gop-size=" + std::to_string(gop_) + " max-bframes=0";
	}
	else
	{
		encoder_section = "x264enc name=enc tune=zerolatency speed-preset=" + preset_ + " key-int-max=" + std::to_string(gop_) +
						  " bitrate=" + std::to_string(bitrate_kbps_);
	}

	// Cap the appsrc internal queue to two frames so that a stalled network
	// only ever blocks the dedicated preview thread, never the capture path.
	unsigned long long max_bytes = (unsigned long long)info_.stride * info_.height * 3 / 2 * 2;

	std::string framerate = fps_ ? "framerate=" + std::to_string(fps_) + "/1," : "";

	return "appsrc name=src is-live=true do-timestamp=true format=time block=true max-bytes=" +
		   std::to_string(max_bytes) + " ! video/x-raw,format=I420,width=" + std::to_string(info_.width) +
		   ",height=" + std::to_string(info_.height) + "," + framerate + " ! " + encoder_section +
		   " ! h264parse config-interval=1 ! rtph264pay mtu=" + std::to_string(mtu_) +
		   " config-interval=1 ! udpsink name=sink host=" + host_ + " port=" + std::to_string(port_) +
		   " sync=false async=false";
}

void GstreamerPreviewStage::Configure()
{
	// The lores stream only exists if --lores-width/--lores-height or the
	// "rpicam-apps": { "lores": ... } section of the JSON file requested it.
	// Fall back to the main stream, otherwise the stage would silently do
	// nothing.
	stream_ = app_->LoresStream(&info_);
	if (!stream_)
	{
		LOG(1, "gstreamer_preview: no lores stream configured, using main stream");
		stream_ = app_->GetMainStream();
		if (stream_)
			info_ = app_->GetStreamInfo(stream_);
	}

	if (!stream_ || info_.pixel_format != libcamera::formats::YUV420)
	{
		LOG_ERROR("gstreamer_preview: no YUV420 stream available, stage disabled");
		return;
	}

	if (!gst_init_check(nullptr, nullptr, nullptr))
	{
		LOG_ERROR("gstreamer_preview: failed to initialize GStreamer, stage disabled");
		return;
	}

	GError *error = nullptr;
	pipeline_ = gst_parse_launch(makePipelineString().c_str(), &error);
	if (!pipeline_)
	{
		LOG_ERROR("gstreamer_preview: failed to create pipeline: " << (error ? error->message : "unknown error"));
		g_error_free(error);
		return;
	}

	appsrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "src");
	enc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "enc");
	sink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
	if (!appsrc_)
	{
		LOG_ERROR("gstreamer_preview: pipeline has no appsrc element");
		gst_object_unref(pipeline_);
		pipeline_ = nullptr;
		return;
	}

	gst_bus_set_sync_handler(gst_pipeline_get_bus(GST_PIPELINE(pipeline_)), busSyncHandler, this, nullptr);

	queue_ = g_async_queue_new();
	enabled_ = true;
	LOG(1, "gstreamer_preview: sending " << info_.width << "x" << info_.height << " preview to " << host_ << ":"
										 << port_ << " (" << encoder_ << ")");

	// Set up the runtime control socket (rpiStudio client -> this stage).
	// "control_socket" names a directory; the socket file always carries the
	// PID so parallel instances stay unique. Defaults to /run (root-only).
	std::string dir = control_socket_dir_.empty() ? "/run" : control_socket_dir_;
	control_socket_path_ = dir + "/rpicam-gst-preview-" + std::to_string(getpid()) + ".sock";

	control_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
	if (control_fd_ < 0)
	{
		LOG_ERROR("gstreamer_preview: cannot create control socket: " << strerror(errno));
		return;
	}

	struct sockaddr_un addr {};
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, control_socket_path_.c_str(), sizeof(addr.sun_path) - 1);

	unlink(control_socket_path_.c_str());
	if (bind(control_fd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0)
	{
		LOG_ERROR("gstreamer_preview: cannot bind control socket " << control_socket_path_ << ": " << strerror(errno));
		close(control_fd_);
		control_fd_ = -1;
		return;
	}
	if (listen(control_fd_, 4) < 0)
	{
		LOG_ERROR("gstreamer_preview: cannot listen on control socket: " << strerror(errno));
		close(control_fd_);
		control_fd_ = -1;
		return;
	}
	control_enabled_ = true;
	LOG(1, "gstreamer_preview: control socket listening on " << control_socket_path_);
}

void GstreamerPreviewStage::Start()
{
	if (!enabled_)
		return;

	running_ = true;
	thread_ = std::thread(&GstreamerPreviewStage::previewThread, this);
	if (control_enabled_)
		control_thread_ = std::thread(&GstreamerPreviewStage::controlThread, this);

	if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
	{
		// Keep the stage "enabled" so that Stop()/Teardown() still clean up
		// the thread and the pipeline; the error flag makes the preview
		// thread drop every frame.
		LOG_ERROR("gstreamer_preview: failed to start pipeline");
		pipeline_error_ = true;
	}
}

bool GstreamerPreviewStage::Process(CompletedRequestPtr &completed_request)
{
	if (!enabled_ || !running_ || pipeline_error_)
		return false;

	auto it = completed_request->buffers.find(stream_);
	if (it == completed_request->buffers.end())
		return false;

	// DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ has already been performed when
	// the request completed; BufferReadSync just hands out the mapped planes.
	BufferReadSync sync(app_, it->second);
	std::vector<libcamera::Span<uint8_t>> const &planes = sync.Get();

	// The camera buffers are allocated as a SINGLE DMA-heap plane (see
	// rpicam_app.cpp: one plane covering config.frameSize, one mmap). The
	// whole frame lives in one contiguous span in I420 PLANAR layout:
	// Y at full res, then U at half-w/half-h, then V at half-w/half-h
	// (verified against object_blur_stage.cpp, which reads the same buffer
	// as I420). NOT NV12 - the chroma is planar, not interleaved.
	size_t y_size = (size_t)info_.stride * info_.height;
	size_t chroma_h = info_.height / 2;
	size_t chroma_stride = info_.stride / 2;
	size_t uv_size = chroma_stride * chroma_h;
	size_t total = y_size + 2 * uv_size;
	if (planes.size() < 1 || planes[0].size() < total)
		return false;

	GstBuffer *buffer = gst_buffer_new_allocate(nullptr, total, nullptr);
	GstMapInfo map;
	if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE))
	{
		gst_buffer_unref(buffer);
		return false;
	}
	memcpy(map.data, planes[0].data(), total);
	gst_buffer_unmap(buffer, &map);

	// The width is usually smaller than the stride, so tell downstream about
	// the real geometry via GstVideoMeta (3 planes: Y, U, V).
	gsize offsets[3] = { 0, (gsize)y_size, (gsize)(y_size + uv_size) };
	gint strides[3] = { (gint)info_.stride, (gint)chroma_stride, (gint)chroma_stride };
	gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_I420, info_.width,
								   info_.height, 3, offsets, strides);

	// The copy is complete, so the camera buffer may be recycled as soon as
	// this function returns. Hand the frame to the preview thread without
	// blocking: if the queue is full the oldest frame wins and this one is
	// dropped - the capture path must never stall behind the network.
	unsigned int qdepth;
	{
		std::lock_guard<std::mutex> lock(params_mutex_);
		qdepth = queue_depth_;
	}
	if (queued_.fetch_add(1) < qdepth)
	{
		g_async_queue_push(queue_, buffer);
		frames_pushed_++;
	}
	else
	{
		queued_.fetch_sub(1);
		gst_buffer_unref(buffer);
		frames_dropped_++;
		if (frames_dropped_ % 300 == 1)
			LOG(1, "gstreamer_preview: " << frames_dropped_ << " frames dropped (queue full)");
	}

	return false; // never drop the main encode
}

void GstreamerPreviewStage::previewThread()
{
	while (!stop_)
	{
		// Timeout pop (no NULL sentinel): g_async_queue_push() rejects NULL,
		// so a sentinel would never reach us and Stop() would hang in join().
		// The 100 ms timeout makes the loop terminate promptly on stop_.
		gpointer item = g_async_queue_timeout_pop(queue_, 100000);
		if (!item)
			continue;

		queued_.fetch_sub(1);
		GstBuffer *buffer = GST_BUFFER_CAST(item);

		if (pipeline_error_)
		{
			gst_buffer_unref(buffer);
			frames_dropped_++;
			continue;
		}

		GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buffer);
		if (ret != GST_FLOW_OK)
		{
			// With appsrc, the buffer ownership stays with us on failure.
			gst_buffer_unref(buffer);
			frames_dropped_++;
			if (ret == GST_FLOW_FLUSHING || ret == GST_FLOW_EOS)
				break; // pipeline is being shut down
		}
	}
}

void GstreamerPreviewStage::Stop()
{
	if (!enabled_)
		return;

	running_ = false;
	stop_ = true;

	// Close the control socket first: shutdown() unblocks a pending accept()
	// and the 100 ms poll timeout in handleControlConnection() lets the
	// control thread notice control_enabled_ == false.
	control_enabled_ = false;
	if (control_fd_ >= 0)
	{
		shutdown(control_fd_, SHUT_RDWR);
		close(control_fd_);
		control_fd_ = -1;
	}
	if (control_thread_.joinable())
		control_thread_.join();

	// Unblocks a push that is waiting on the appsrc queue being full. The
	// preview thread then observes stop_ (or the FLUSHING return) and exits.
	if (pipeline_)
		gst_element_set_state(pipeline_, GST_STATE_NULL);

	if (thread_.joinable())
		thread_.join();

	LOG(1, "gstreamer_preview: stopped (" << frames_pushed_ << " frames sent, " << frames_dropped_ << " dropped)");
}

void GstreamerPreviewStage::Teardown()
{
	// Normally the app calls Stop() before Teardown(), but make sure a thread
	// started in Start() can never outlive the queue it blocks on.
	running_ = false;
	stop_ = true;

	control_enabled_ = false;
	if (control_fd_ >= 0)
	{
		shutdown(control_fd_, SHUT_RDWR);
		close(control_fd_);
		control_fd_ = -1;
	}
	if (control_thread_.joinable())
		control_thread_.join();

	if (pipeline_)
		gst_element_set_state(pipeline_, GST_STATE_NULL);
	if (thread_.joinable())
		thread_.join();

	if (queue_)
	{
		while (gpointer item = g_async_queue_try_pop(queue_))
			gst_buffer_unref(GST_BUFFER_CAST(item));
		g_async_queue_unref(queue_);
		queue_ = nullptr;
	}

	if (sink_)
	{
		gst_object_unref(sink_);
		sink_ = nullptr;
	}

	if (enc_)
	{
		gst_object_unref(enc_);
		enc_ = nullptr;
	}

	if (appsrc_)
	{
		gst_object_unref(appsrc_);
		appsrc_ = nullptr;
	}

	if (pipeline_)
	{
		gst_object_unref(pipeline_);
		pipeline_ = nullptr;
	}

	if (!control_socket_path_.empty())
	{
		unlink(control_socket_path_.c_str());
		control_socket_path_.clear();
	}
}

GstreamerPreviewStage::~GstreamerPreviewStage()
{
	Teardown();
}

void GstreamerPreviewStage::controlThread()
{
	while (control_enabled_ && control_fd_ >= 0)
	{
		int cfd = accept(control_fd_, nullptr, nullptr);
		if (cfd < 0)
		{
			// Interrupted by shutdown() in Stop()/Teardown().
			if (!control_enabled_)
				break;
			continue;
		}

		handleControlConnection(cfd);
		close(cfd);
	}
}

void GstreamerPreviewStage::handleControlConnection(int cfd)
{
	std::string line;
	char buf[4096];
	while (control_enabled_)
	{
		struct pollfd pfd { cfd, POLLIN, 0 };
		int r = poll(&pfd, 1, 100);
		if (r < 0)
			return;
		if (r == 0)
			continue;

		ssize_t n = read(cfd, buf, sizeof(buf) - 1);
		if (n <= 0)
			break;
		buf[n] = '\0';
		line.append(buf, n);
		if (line.find('\n') != std::string::npos)
			break;
	}

	if (line.empty())
		return;

	// Strip a trailing newline (and any trailing carriage return).
	while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
		line.pop_back();

	std::string resp = processControlRequest(line);
	resp += "\n";
	size_t off = 0;
	while (off < resp.size())
	{
		ssize_t w = write(cfd, resp.data() + off, resp.size() - off);
		if (w <= 0)
			break;
		off += static_cast<size_t>(w);
	}
}

std::string GstreamerPreviewStage::processControlRequest(std::string const &req)
{
	std::vector<std::string> applied;
	std::map<std::string, std::string> rejected;

	try
	{
		std::istringstream ss(req);
		boost::property_tree::ptree pt;
		boost::property_tree::json_parser::read_json(ss, pt);

		auto set_child = pt.get_child_optional("set");
		if (!set_child)
			return std::string("{\"ok\":false,\"error\":\"missing 'set' object\"}");

		for (auto const &kv : *set_child)
			applyParam(kv.first, kv.second.get_value<std::string>(), applied, rejected);
	}
	catch (std::exception const &e)
	{
		return std::string("{\"ok\":false,\"error\":\"") + e.what() + "\"}";
	}

	std::string resp = "{\"ok\":true,\"applied\":[";
	for (size_t i = 0; i < applied.size(); i++)
	{
		if (i)
			resp += ",";
		resp += "\"" + applied[i] + "\"";
	}
	resp += "],\"rejected\":{";
	bool first = true;
	for (auto const &rj : rejected)
	{
		if (!first)
			resp += ",";
		first = false;
		resp += "\"" + rj.first + "\":\"" + rj.second + "\"";
	}
	resp += "}}";
	return resp;
}

void GstreamerPreviewStage::applyParam(std::string const &key, std::string const &value,
									   std::vector<std::string> &applied, std::map<std::string, std::string> &rejected)
{
	try
	{
		if (key == "host")
		{
			{
				std::lock_guard<std::mutex> lock(params_mutex_);
				host_ = value;
			}
			if (sink_)
				g_object_set(sink_, "host", value.c_str(), nullptr);
			applied.push_back(key);
		}
		else if (key == "port")
		{
			unsigned int v = static_cast<unsigned int>(std::stoul(value));
			{
				std::lock_guard<std::mutex> lock(params_mutex_);
				port_ = static_cast<uint16_t>(v);
			}
			if (sink_)
				g_object_set(sink_, "port", static_cast<gint>(v), nullptr);
			applied.push_back(key);
		}
		else if (key == "bitrate_kbps")
		{
			if (encoder_ != "x264enc")
			{
				rejected[key] = "bitrate only settable for x264enc";
				return;
			}
			unsigned int v = static_cast<unsigned int>(std::stoul(value));
			{
				std::lock_guard<std::mutex> lock(params_mutex_);
				bitrate_kbps_ = v;
			}
			if (enc_)
				g_object_set(enc_, "bitrate", static_cast<gint>(v), nullptr);
			applied.push_back(key);
		}
		else if (key == "gop")
		{
			if (encoder_ != "x264enc")
			{
				rejected[key] = "gop only settable for x264enc";
				return;
			}
			unsigned int v = static_cast<unsigned int>(std::stoul(value));
			{
				std::lock_guard<std::mutex> lock(params_mutex_);
				gop_ = v;
			}
			if (enc_)
				g_object_set(enc_, "key-int-max", static_cast<gint>(v), nullptr);
			applied.push_back(key);
		}
		else if (key == "queue_depth")
		{
			unsigned int v = static_cast<unsigned int>(std::stoul(value));
			if (v == 0)
				v = 1;
			{
				std::lock_guard<std::mutex> lock(params_mutex_);
				queue_depth_ = v;
			}
			applied.push_back(key);
		}
		else if (key == "preset" || key == "fps" || key == "mtu" || key == "encoder" ||
				 key == "lores_width" || key == "lores_height")
		{
			rejected[key] = "requires camera restart";
		}
		else
		{
			rejected[key] = "unknown parameter";
		}
	}
	catch (std::exception const &e)
	{
		rejected[key] = std::string("invalid value: ") + e.what();
	}
}

static PostProcessingStage *Create(RPiCamApp *app)
{
	return new GstreamerPreviewStage(app);
}

static RegisterStage reg(NAME, &Create);
