/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * watermark_stage.cpp - runtime-aware watermark stage skeleton.
 */

#include <libcamera/formats.h>

#include "core/rpicam_app.hpp"
#include "core/runtime_data_store.hpp"
#include "post_processing_stages/post_processing_stage.hpp"

class WatermarkStage : public PostProcessingStage
{
public:
	WatermarkStage(RPiCamApp *app) : PostProcessingStage(app)
	{
	}

	char const *Name() const override;

	void Read(boost::property_tree::ptree const &params) override;

	void Configure() override;

	bool Process(CompletedRequestPtr &completed_request) override;

private:
	using Stream = libcamera::Stream;

	Stream *stream_ = nullptr;
	StreamInfo info_;
	RuntimeDataStore *runtime_data_store_ = nullptr;
	bool enabled_ = true;
};

#define NAME "watermark"

char const *WatermarkStage::Name() const
{
	return NAME;
}

void WatermarkStage::Read(boost::property_tree::ptree const &params)
{
	enabled_ = params.get<bool>("enabled", true);
}

void WatermarkStage::Configure()
{
	stream_ = app_->GetMainStream();
	if (!stream_)
		throw std::runtime_error("WatermarkStage: main stream is not available");

	if (stream_->configuration().pixelFormat != libcamera::formats::YUV420)
		throw std::runtime_error("WatermarkStage: only YUV420 main stream is supported");

	info_ = app_->GetStreamInfo(stream_);
	runtime_data_store_ = &app_->GetRuntimeDataStore();

	LOG(1, "WatermarkStage configured for " << info_.width << "x" << info_.height
																				<< " YUV420");
}

bool WatermarkStage::Process(CompletedRequestPtr &completed_request)
{
	(void)completed_request;
	if (!enabled_ || !runtime_data_store_)
		return false;

	// Phase 3 only wires the stage to the shared runtime store. Rendering and
	// generation-aware overlay caching are implemented in later phases.
	return false;
}

static PostProcessingStage *Create(RPiCamApp *app)
{
	return new WatermarkStage(app);
}

static RegisterStage reg(NAME, &Create);
