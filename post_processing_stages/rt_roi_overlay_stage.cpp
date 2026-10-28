/* SPDX-License-Identifier: BSD-2-Clause */
/* rt_roi_overlay_stage.cpp - Configurable SVG runtime controls in Qt preview. */

#include "core/rpicam_app.hpp"
#include "core/rt_roi_overlay_config.hpp"
#include "post_processing_stages/post_processing_stage.hpp"

class RtRoiOverlayStage : public PostProcessingStage
{
public:
	explicit RtRoiOverlayStage(RPiCamApp *app) : PostProcessingStage(app) {}

	char const *Name() const override { return "rt_roi_overlay"; }

	void Read(const boost::property_tree::ptree &params) override
	{
		config_ = std::make_shared<const rt_roi::Config>(rt_roi::ReadConfig(params));
	}

	void Configure() override
	{
		if (!config_)
			config_ = std::make_shared<const rt_roi::Config>(rt_roi::ReadConfig({}));
		if (!app_->SetPreviewRtRoiOverlay(config_))
			throw std::runtime_error("rt_roi_overlay requires --qt-preview and a Qt preview built with Qt Svg");
	}

	// The controls are composed by the preview after the image. Camera/encoder
	// buffers are untouched; callback lifetime belongs to the app, not this stage.
	bool Process(CompletedRequestPtr &) override { return false; }

private:
	std::shared_ptr<const rt_roi::Config> config_;
};

static PostProcessingStage *Create(RPiCamApp *app)
{
	return new RtRoiOverlayStage(app);
}

static RegisterStage reg("rt_roi_overlay", &Create);
