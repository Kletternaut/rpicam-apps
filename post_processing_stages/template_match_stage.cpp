/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2021, Raspberry Pi (Trading) Limited
 *
 * template_match.cpp - Template matching implementation, using OpenCV
 * Version 0.3a
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

// PostProcessingStage für Template Matching mit OpenCV
class TemplateMatchStage : public PostProcessingStage
{
public:
    // Konstruktor: Initialisiert GStreamer-VideoCapture für externen Stream
    TemplateMatchStage(RPiCamApp *app)
        : PostProcessingStage(app), active_methods_{true, true, true},
          cap_("udpsrc port=8554 caps=\"application/x-rtp, media=(string)video, clock-rate=(int)90000, encoding-name=(string)H264, payload=(int)96\" "
               "! rtph264depay ! h264parse ! avdec_h264 ! videoconvert ! appsink",
               cv::CAP_GSTREAMER)
    {}

    // Name der Stage (für Registrierung)
    char const *Name() const override { return "template_match"; }

    // Liest Konfigurationsparameter aus JSON (über boost::property_tree)
    void Read(boost::property_tree::ptree const &params) override {
        // Aktivierte Matching-Methoden (nur relevant bei mehreren Methoden)
        if (params.get_child_optional("active_methods")) {
            size_t i = 0;
            for (auto &v : params.get_child("active_methods")) {
                if (i < active_methods_.size())
                    active_methods_[i++] = v.second.get_value<bool>();
            }
        }
        // Wie oft pro Sekunde Matching durchgeführt wird
        if (params.get_optional<int>("match_fps"))
            match_fps_ = params.get<int>("match_fps");
        min_match_interval_ms_ = 1000 / std::max(1, match_fps_);

        // Optionale feste Template-Größe
        if (params.get_optional<int>("template_width"))
            template_width_ = params.get<int>("template_width");
        if (params.get_optional<int>("template_height"))
            template_height_ = params.get<int>("template_height");

        // (Nicht genutzt bei festem Scaling, aber für spätere Erweiterung)
        if (params.get_optional<double>("scale_min"))
            scale_min_ = params.get<double>("scale_min");
        if (params.get_optional<double>("scale_max"))
            scale_max_ = params.get<double>("scale_max");
        if (params.get_optional<double>("scale_step"))
            scale_step_ = params.get<double>("scale_step");
        if (params.get_optional<double>("maxval_threshold"))
            maxval_threshold_ = params.get<double>("maxval_threshold");

        // Schwellwerte für Matching und Rescaling
        if (params.get_optional<double>("rescale_threshold"))
            rescale_threshold_ = params.get<double>("rescale_threshold");
        if (params.get_optional<double>("match_threshold"))
            match_threshold_ = params.get<double>("match_threshold");
        // Fester Skalierungsfaktor für das Template
        if (params.get_optional<double>("fixed_scale"))
            fixed_scale_ = params.get<double>("fixed_scale");
        // Debug-Ausgaben aktivieren
        if (params.get_optional<bool>("debug"))
            debug_ = params.get<bool>("debug");
        if (params.get_optional<bool>("debug_center"))
            debug_center_ = params.get<bool>("debug_center");
    }

    // Konfiguriert die Streams (z.B. "video" und "lores")
    void Configure() override
    {
        std::cout << "Verfügbare Streams:" << std::endl;
        for (const auto &kv : app_->GetStreams())
            std::cout << "  " << kv.first << std::endl;

        // Passe die Namen an die Ausgabe an!
        stream0_ = app_->GetStream("video");
        stream1_ = app_->GetStream("lores");
        if (!stream0_ || !stream1_)
            throw std::runtime_error("TemplateMatchStage: Streams nicht gefunden!");
    }

    // Hauptverarbeitung: Template Matching und Visualisierung
    bool Process(CompletedRequestPtr &completed_request) override
    {
        // Framerate-Steuerung: Matching nur alle X ms
        auto now = std::chrono::steady_clock::now();
        int elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_match_time_).count();
        bool do_match = elapsed >= min_match_interval_ms_;
        if (do_match)
            last_match_time_ = now;

        // Hole Hauptbild (Y-Plane, Graustufen)
        StreamInfo info0 = app_->GetStreamInfo(stream0_);
        BufferWriteSync w0(app_, completed_request->buffers[stream0_]);
        libcamera::Span<uint8_t> buffer0 = w0.Get()[0];
        Mat img0(info0.height, info0.width, CV_8U, (void*)buffer0.data(), info0.stride);

        // Hole Template-Bild aus externem Stream (z.B. RTSP/H264)
        cv::Mat cam1_frame, img1;
        if (!cap_.isOpened() || !cap_.read(cam1_frame) || cam1_frame.empty()) {
            std::cerr << "RTSP-Stream nicht verfügbar oder kein Frame empfangen!" << std::endl;
            return false;
        }
        // In Graustufen konvertieren, falls nötig
        if (cam1_frame.channels() == 3)
            cvtColor(cam1_frame, img1, cv::COLOR_BGR2GRAY);
        else
            img1 = cam1_frame;

        // Template ggf. auf feste Größe skalieren
        if (template_width_ > 0 && template_height_ > 0) {
            cv::resize(img1, img1, cv::Size(template_width_, template_height_), 0, 0, cv::INTER_AREA);
        }

        // Matching nur, wenn Intervall erreicht
        if (!do_match) {
            // Optional: Nur Overlay anzeigen, kein Matching
            return false;
        }

        // Template muss kleiner als Suchbild sein
        if (img1.empty() || img1.cols > img0.cols || img1.rows > img0.rows)
            return false;

        // Template auf festen Skalierungsfaktor bringen
        cv::Mat scaled_img1;
        cv::resize(img1, scaled_img1, cv::Size(), fixed_scale_, fixed_scale_, cv::INTER_AREA);

        if (scaled_img1.cols > img0.cols || scaled_img1.rows > img0.rows)
            return false;

        // Array für Methoden und zugehörige Graustufenfarben
        const int num_methods = 3;
        int methods[num_methods] = { cv::TM_CCOEFF_NORMED, cv::TM_SQDIFF_NORMED, cv::TM_CCORR_NORMED };
        cv::Scalar colors[num_methods] = { cv::Scalar(255), cv::Scalar(127), cv::Scalar(0) }; // Weiß, Grau, Schwarz
        int thickness[num_methods] = { 4, 2, 1 };

        // Für jede aktivierte Methode Matching und Rahmen zeichnen
        for (int i = 0; i < num_methods; ++i) {
            if (!active_methods_[i]) continue;

            cv::Mat result;
            matchTemplate(img0, scaled_img1, result, methods[i]);
            double minVal, maxVal;
            Point minLoc, maxLoc;
            minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc);

            Point matchLoc = (methods[i] == cv::TM_SQDIFF || methods[i] == cv::TM_SQDIFF_NORMED) ? minLoc : maxLoc;
            Point p1 = matchLoc;
            Point p2 = Point(matchLoc.x + scaled_img1.cols, matchLoc.y + scaled_img1.rows);

            // Mittelpunkt berechnen
            Point center((p1.x + p2.x) / 2, (p1.y + p2.y) / 2);

            rectangle(img0, p1, p2, colors[i], thickness[i]);

            // Debug-Ausgabe für Center-Point NUR wenn Wahrscheinlichkeit hoch ist
            bool is_high_prob = false;
            if (methods[i] == cv::TM_SQDIFF || methods[i] == cv::TM_SQDIFF_NORMED)
                is_high_prob = (minVal <= match_threshold_);
            else
                is_high_prob = (maxVal >= match_threshold_);

            if (debug_ && debug_center_ && is_high_prob) {
                std::cout << "Methode " << i << " | maxVal: " << maxVal
                          << " | pos: (" << matchLoc.x << "," << matchLoc.y << ")"
                          << " | center: (" << center.x << "," << center.y << ")" << std::endl;
            }
        }

        // Debug-Bild speichern (außerhalb der Schleife)
        if (debug_) {
            cv::imwrite("debug_match_frame.png", img0);
        }

        return false;
    }

private:
    // Streams für Hauptbild und Template
    Stream *stream0_;
    Stream *stream1_;
    std::vector<bool> active_methods_;
    cv::VideoCapture cap_;

    // Konfigurierbare Parameter
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

    // --- Fehlende Member ergänzen ---
    int template_width_ = 0;
    int template_height_ = 0;
    std::chrono::steady_clock::time_point last_match_time_{};
};

// Registrierung der Stage im Framework
static PostProcessingStage *Create(RPiCamApp *app)
{
    return new TemplateMatchStage(app);
}
static RegisterStage reg("template_match", &Create);
