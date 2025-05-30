/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2021, Raspberry Pi (Trading) Limited
 * Copyright (C) 2025, github.com/Kletternaut
 *
 * template_match_stage.cpp
 * Version 0.4
 *
 * TemplateMatchStage:
 * This post-processing stage enables template matching in the camera stream using OpenCV.
 * An external template image (e.g. from an RTSP stream) is searched within the main image.
 * The result is visualized as a rectangle overlay. Configuration is done via JSON.
 * Supports multiple matching methods and debug output.
 */

#include <libcamera/stream.h>
#include "core/rpicam_app.hpp"
#include "post_processing_stages/post_processing_stage.hpp"
#include "opencv2/core.hpp"
#include "opencv2/imgproc.hpp"
#include "opencv2/imgcodecs.hpp"
#include "opencv2/videoio.hpp"

using namespace cv;
using Stream = libcamera::Stream;

// PostProcessingStage for template matching with OpenCV
class TemplateMatchStage : public PostProcessingStage
{
public:
    // Constructor: Initializes GStreamer VideoCapture for external stream
    TemplateMatchStage(RPiCamApp *app)
        : PostProcessingStage(app), active_methods_{true, true, true}
    {
        // Do NOT open cap_ here!
    }

    // Name of the stage (for registration)
    char const *Name() const override { return "template_match"; }

    // Reads configuration parameters from JSON (via boost::property_tree)
    void Read(boost::property_tree::ptree const &params) override {
        std::cout << "Read() called!" << std::endl;
        if (params.get_optional<bool>("debug"))
            debug_ = params.get<bool>("debug");
        std::cout << "debug_ after reading: " << debug_ << std::endl;

        // Debug options
        if (params.get_optional<bool>("debug_center"))
            debug_center_ = params.get<bool>("debug_center");

        // Enabled matching methods
        if (params.get_child_optional("active_methods")) {
            size_t i = 0;
            for (auto &v : params.get_child("active_methods")) {
                if (i < active_methods_.size())
                    active_methods_[i++] = v.second.get_value<bool>();
            }
        }

        // Matching FPS
        if (params.get_optional<int>("match_fps"))
            match_fps_ = params.get<int>("match_fps");
        min_match_interval_ms_ = 1000 / std::max(1, match_fps_);

        // Optional fixed template size
        if (params.get_optional<int>("template_width"))
            template_width_ = params.get<int>("template_width");
        if (params.get_optional<int>("template_height"))
            template_height_ = params.get<int>("template_height");

        // Scaling parameters
        if (params.get_optional<double>("scale_min"))
            scale_min_ = params.get<double>("scale_min");
        if (params.get_optional<double>("scale_max"))
            scale_max_ = params.get<double>("scale_max");
        if (params.get_optional<double>("scale_step"))
            scale_step_ = params.get<double>("scale_step");

        // Thresholds
        if (params.get_optional<double>("maxval_threshold"))
            maxval_threshold_ = params.get<double>("maxval_threshold");
        if (params.get_optional<double>("rescale_threshold"))
            rescale_threshold_ = params.get<double>("rescale_threshold");
        if (params.get_optional<double>("match_threshold"))
            match_threshold_ = params.get<double>("match_threshold");

        // Fixed scaling factor
        if (params.get_optional<double>("fixed_scale"))
            fixed_scale_ = params.get<double>("fixed_scale");

        // RTSP port
        if (params.get_optional<int>("rtsp_port"))
            rtsp_port_ = params.get<int>("rtsp_port");

        if (params.get_optional<int>("debug_level"))
            debug_level_ = params.get<int>("debug_level");
        // Optional: Kompatibilität zu debug (true/false)
        if (params.get_optional<bool>("debug"))
            debug_level_ = params.get<bool>("debug") ? 1 : 0;

        if (params.get_optional<bool>("auto_scale"))
            auto_scale_ = params.get<bool>("auto_scale");
    }

    // Configures the streams (e.g. "video" and "lores")
    void Configure() override
    {
        std::cout << "Available streams:" << std::endl;
        for (const auto &kv : app_->GetStreams())
            std::cout << "  " << kv.first << std::endl;

        stream0_ = app_->GetStream("video");
        stream1_ = app_->GetStream("lores");
        if (!stream0_ || !stream1_)
            throw std::runtime_error("TemplateMatchStage: Streams not found!");

        std::ostringstream gst_str;
        gst_str << "udpsrc port=" << rtsp_port_
                << " caps=\"application/x-rtp, media=(string)video, clock-rate=(int)90000, encoding-name=(string)H264, payload=(int)96\" "
                << "! rtph264depay ! h264parse ! avdec_h264 ! videoconvert ! appsink";
        cap_.open(gst_str.str(), cv::CAP_GSTREAMER);
    }

    // Main processing: template matching and visualization
    bool Process(CompletedRequestPtr &completed_request) override
    {
        // Debug: Zeige an, in welchem Modus wir sind
        if (debug_level_ >= 2)
            std::cout << "[DEBUG] auto_scale_: " << auto_scale_ << " | autoscale_locked_: " << autoscale_locked_ << std::endl;

        // Framerate control: only perform matching every X ms
        auto now = std::chrono::steady_clock::now();
        int elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_match_time_).count();
        bool do_match = elapsed >= min_match_interval_ms_;
        if (do_match)
            last_match_time_ = now;

        // Get main image (Y-plane, grayscale)
        StreamInfo info0 = app_->GetStreamInfo(stream0_);
        BufferWriteSync w0(app_, completed_request->buffers[stream0_]);
        libcamera::Span<uint8_t> buffer0 = w0.Get()[0];
        Mat img0(info0.height, info0.width, CV_8U, (void*)buffer0.data(), info0.stride);

        // Get template image from external stream (e.g. RTSP/H264)
        cv::Mat cam1_frame, img1;
        if (!cap_.isOpened() || !cap_.read(cam1_frame) || cam1_frame.empty()) {
            std::cerr << "RTSP stream not available or no frame received!" << std::endl;
            return false;
        }
        // Convert to grayscale if necessary
        if (cam1_frame.channels() == 3)
            cvtColor(cam1_frame, img1, cv::COLOR_BGR2GRAY);
        else
            img1 = cam1_frame;

        // Resize template to fixed size if specified
        if (template_width_ > 0 && template_height_ > 0) {
            cv::resize(img1, img1, cv::Size(template_width_, template_height_), 0, 0, cv::INTER_AREA);
        }

        // Only perform matching if interval reached
        if (!do_match) {
            // Optionally: only show overlay, no matching
            return false;
        }

        // Template must be smaller than search image
        if (img1.empty() || img1.cols > img0.cols || img1.rows > img0.rows)
            return false;

        // Declare matching method arrays at the beginning of the function
        const int num_methods = 3;
        int methods[num_methods] = { cv::TM_CCOEFF_NORMED, cv::TM_SQDIFF_NORMED, cv::TM_CCORR_NORMED };
        cv::Scalar colors[num_methods] = { cv::Scalar(255), cv::Scalar(127), cv::Scalar(0) }; // White, Gray, Black
        int thickness[num_methods] = { 4, 2, 1 };

        if (auto_scale_) {
            if (!autoscale_locked_) {
                if (debug_level_ >= 2)
                    std::cout << "[DEBUG] === AUTOSCALING SUCHMODUS ===" << std::endl;
                double best_score = -1e9;
                double best_scale = fixed_scale_;
                int best_method = 0;
                Point best_center, best_p1, best_p2;

                for (double scale = scale_min_; scale <= scale_max_; scale += scale_step_) {
                    cv::Mat scaled_img1;
                    cv::resize(img1, scaled_img1, cv::Size(), scale, scale, cv::INTER_AREA);

                    if (debug_level_ >= 3)
                        std::cout << "[Auto-Scale][DEBUG] Teste scale: " << scale
                                  << " | Templategröße: " << scaled_img1.cols << "x" << scaled_img1.rows << std::endl;

                    if (scaled_img1.cols > img0.cols || scaled_img1.rows > img0.rows)
                        continue;

                    for (int i = 0; i < num_methods; ++i) {
                        if (!active_methods_[i]) continue;

                        cv::Mat result;
                        matchTemplate(img0, scaled_img1, result, methods[i]);
                        double minVal, maxVal;
                        Point minLoc, maxLoc;
                        minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc);

                        double score = (methods[i] == cv::TM_SQDIFF || methods[i] == cv::TM_SQDIFF_NORMED) ? -minVal : maxVal;
                        Point matchLoc = (methods[i] == cv::TM_SQDIFF || methods[i] == cv::TM_SQDIFF_NORMED) ? minLoc : maxLoc;
                        Point p1 = matchLoc;
                        Point p2 = Point(matchLoc.x + scaled_img1.cols, matchLoc.y + scaled_img1.rows);
                        Point center((p1.x + p2.x) / 2, (p1.y + p2.y) / 2);

                        // Grün für höchste Matching-Quote
                        if (debug_level_ >= 3) {
                            if (score > best_score) {
                                std::cout << "\033[32m"; // Grün
                            }
                            std::cout << "[Auto-Scale][SEARCH] scale: " << scale
                                      << " | method: " << i
                                      << " | score: " << score
                                      << " | center: (" << center.x << "," << center.y << ")"
                                      << " | minVal: " << minVal << " | maxVal: " << maxVal
                                      << "\033[0m" << std::endl; // Reset Farbe
                        }

                        if (score > best_score) {
                            best_score = score;
                            best_scale = scale;
                            best_method = i;
                            best_center = center;
                            best_p1 = p1;
                            best_p2 = p2;
                        }
                    }
                }

                rectangle(img0, best_p1, best_p2, colors[best_method], thickness[best_method]);
                if (debug_level_ >= 1) {
                    std::cout << "[Auto-Scale][SEARCH] Best scale: " << best_scale
                              << " | Method: " << best_method
                              << " | Score: " << best_score
                              << " | Center: (" << best_center.x << "," << best_center.y << ")"
                              << std::endl;
                }
                if (debug_level_ >= 1) {
                    cv::imwrite("debug_match_frame.png", img0);
                }

                if (best_score > match_threshold_) {
                    autoscale_success_counter_++;
                    if (autoscale_success_counter_ >= autoscale_success_needed_) {
                        autoscale_locked_ = true;
                        locked_scale_ = best_scale;
                        if (debug_level_ >= 1)
                            std::cout << "[Auto-Scale][LOCKED] Locked scale: " << locked_scale_ << std::endl;
                    }
                } else {
                    autoscale_success_counter_ = 0;
                }
            } else {
                if (debug_level_ >= 2)
                    std::cout << "[DEBUG] === AUTOSCALING LOCKED === (locked_scale_=" << locked_scale_ << ")" << std::endl;
                cv::Mat scaled_img1;
                cv::resize(img1, scaled_img1, cv::Size(), locked_scale_, locked_scale_, cv::INTER_AREA);

                if (scaled_img1.cols > img0.cols || scaled_img1.rows > img0.rows)
                    return false;

                double best_score_locked = -1e9;
                int best_method_locked = 0;
                Point best_center_locked, best_p1_locked, best_p2_locked;

                for (int i = 0; i < num_methods; ++i) {
                    if (!active_methods_[i]) continue;

                    cv::Mat result;
                    matchTemplate(img0, scaled_img1, result, methods[i]);
                    double minVal, maxVal;
                    Point minLoc, maxLoc;
                    minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc);

                    double score = (methods[i] == cv::TM_SQDIFF || methods[i] == cv::TM_SQDIFF_NORMED) ? -minVal : maxVal;
                    Point matchLoc = (methods[i] == cv::TM_SQDIFF || methods[i] == cv::TM_SQDIFF_NORMED) ? minLoc : maxLoc;
                    Point p1 = matchLoc;
                    Point p2 = Point(matchLoc.x + scaled_img1.cols, matchLoc.y + scaled_img1.rows);
                    Point center((p1.x + p2.x) / 2, (p1.y + p2.y) / 2);

                    if (score > best_score_locked) {
                        best_score_locked = score;
                        best_method_locked = i;
                        best_center_locked = center;
                        best_p1_locked = p1;
                        best_p2_locked = p2;
                    }
                }

                rectangle(img0, best_p1_locked, best_p2_locked, colors[best_method_locked], thickness[best_method_locked]);
                if (debug_level_ >= 1) {
                    std::cout << "[Auto-Scale][LOCKED] LOCKED scale: " << locked_scale_
                              << " | Method: " << best_method_locked
                              << " | Score: " << best_score_locked
                              << " | Center: (" << best_center_locked.x << "," << best_center_locked.y << ")"
                              << std::endl;
                }
                if (debug_level_ >= 1) {
                    cv::imwrite("debug_match_frame.png", img0);
                }

                // Unlock autoscaling if no match
                if (best_score_locked < match_threshold_) {
                    autoscale_locked_ = false;
                    autoscale_success_counter_ = 0;
                    if (debug_level_ >= 1)
                        std::cout << "[Auto-Scale][UNLOCK] Unlocking, resume searching..." << std::endl;
                }
            }
        } else {
            if (debug_level_ >= 2)
                std::cout << "[DEBUG] === FIXED SCALE MODUS === (fixed_scale_=" << fixed_scale_ << ")" << std::endl;
            cv::Mat scaled_img1;
            cv::resize(img1, scaled_img1, cv::Size(), fixed_scale_, fixed_scale_, cv::INTER_AREA);

            if (scaled_img1.cols > img0.cols || scaled_img1.rows > img0.rows)
                return false;

            double best_score_fixed = -1e9;
            int best_method_fixed = 0;
            Point best_center_fixed, best_p1_fixed, best_p2_fixed;

            for (int i = 0; i < num_methods; ++i) {
                if (!active_methods_[i]) continue;

                cv::Mat result;
                matchTemplate(img0, scaled_img1, result, methods[i]);
                double minVal, maxVal;
                Point minLoc, maxLoc;
                minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc);

                double score = (methods[i] == cv::TM_SQDIFF || methods[i] == cv::TM_SQDIFF_NORMED) ? -minVal : maxVal;
                Point matchLoc = (methods[i] == cv::TM_SQDIFF || methods[i] == cv::TM_SQDIFF_NORMED) ? minLoc : maxLoc;
                Point p1 = matchLoc;
                Point p2 = Point(matchLoc.x + scaled_img1.cols, matchLoc.y + scaled_img1.rows);
                Point center((p1.x + p2.x) / 2, (p1.y + p2.y) / 2);

                if (score > best_score_fixed) {
                    best_score_fixed = score;
                    best_method_fixed = i;
                    best_center_fixed = center;
                    best_p1_fixed = p1;
                    best_p2_fixed = p2;
                }

                if (debug_level_ >= 2) {
                    std::cout << "[Fixed-Scale][SEARCH] Method: " << i
                              << " | Scale: " << fixed_scale_
                              << " | Score: " << score
                              << " | Center: (" << center.x << "," << center.y << ")"
                              << " | Size: (" << scaled_img1.cols << "x" << scaled_img1.rows << ")"
                              << " | minVal: " << minVal << " | maxVal: " << maxVal
                              << std::endl;
                }
            }

            rectangle(img0, best_p1_fixed, best_p2_fixed, colors[best_method_fixed], thickness[best_method_fixed]);
            if (debug_level_ >= 1) {
                std::cout << "[Fixed-Scale][SEARCH] Best method: " << best_method_fixed
                          << " | Scale: " << fixed_scale_
                          << " | Score: " << best_score_fixed
                          << " | Center: (" << best_center_fixed.x << "," << best_center_fixed.y << ")"
                          << " | Size: (" << scaled_img1.cols << "x" << scaled_img1.rows << ")"
                          << std::endl;
            }
            if (debug_level_ >= 1) {
                cv::imwrite("debug_match_frame.png", img0);
            }
        }

        return false;
    }

private:
    // Streams for main image and template
    Stream *stream0_;
    Stream *stream1_;
    std::vector<bool> active_methods_;
    cv::VideoCapture cap_;

    // Configurable parameters
    int match_fps_ = 5;
    int min_match_interval_ms_ = 200;
    double scale_min_ = 0.03;
    double scale_max_ = 0.15;
    double scale_step_ = 0.01;
    double last_scale_ = 0.1;
    double last_maxval_ = 0.0;
    Point last_loc_;
    cv::Mat last_img1_;
    double maxval_threshold_ = 0.8;
    double rescale_threshold_ = 0.7;
    double match_threshold_ = 0.8;
    int rescale_counter_ = 0;
    const int rescale_interval_ = 30;
    double fixed_scale_ = 0.1;
    bool debug_ = false;
    bool debug_center_ = false;
    int debug_level_ = 0;
    bool auto_scale_ = false;
    bool autoscale_locked_ = false;

    // --- Additional members ---
    int template_width_ = 0;
    int template_height_ = 0;
    int rtsp_port_ = 8554; // Default port for RTSP/UDP stream
    double locked_scale_ = 0.1;
    int autoscale_success_counter_ = 0;
    const int autoscale_success_needed_ = 3; // Number of consecutive matches to lock autoscale
    std::chrono::steady_clock::time_point last_match_time_{};
};

// Registration of the stage in the framework
static PostProcessingStage *Create(RPiCamApp *app)
{
    return new TemplateMatchStage(app);
}
static RegisterStage reg("template_match", &Create);
