/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Alpha-aware resampling for the gamepad overlay's straight-RGBA canvas.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

class GamepadOverlayResampler
{
public:
	// Return the requested rectangle of the resized image, tightly packed RGBA.
	// firstX/firstY are coordinates in the full destination image. Caching only
	// this visible rectangle also bounds allocations for a large, clipped overlay.
	const std::vector<uint8_t> &resize(const uint8_t *source, int sourceWidth, int sourceHeight,
									 int destinationWidth, int destinationHeight,
									 int firstX, int firstY, int width, int height)
	{
		if (!source || sourceWidth <= 0 || sourceHeight <= 0 || destinationWidth <= 0 ||
			destinationHeight <= 0 || firstX < 0 || firstY < 0 || width <= 0 || height <= 0 ||
			int64_t(firstX) + width > destinationWidth || int64_t(firstY) + height > destinationHeight)
		{
			pixels_.clear();
			return pixels_;
		}

		pixels_.resize(size_t(width) * height * 4);
		if (sourceWidth == destinationWidth && sourceHeight == destinationHeight)
		{
			for (int y = 0; y < height; ++y)
				std::copy_n(source + (size_t(firstY + y) * sourceWidth + firstX) * 4,
							size_t(width) * 4, pixels_.data() + size_t(y) * width * 4);
			return pixels_;
		}
		xAxis_.configure(sourceWidth, destinationWidth, firstX, width);
		yAxis_.configure(sourceHeight, destinationHeight, firstY, height);
		// Each source row is horizontally filtered at most once per frame. Only
		// rows touched by one vertical footprint need to exist simultaneously.
		const int rowCount = yAxis_.maxTaps;
		rows_.resize(size_t(rowCount) * width);
		rowIndices_.assign(rowCount, -1);

		for (int y = 0; y < height; ++y)
		{
			const Span &ys = yAxis_.spans[y];
			for (int t = 0; t < ys.count; ++t)
			{
				const Tap &yt = yAxis_.taps[ys.offset + t];
				const int slot = yt.index % rowCount;
				if (rowIndices_[slot] == yt.index)
					continue;
				Premultiplied *row = rows_.data() + size_t(slot) * width;
				const uint8_t *input = source + size_t(yt.index) * sourceWidth * 4;
				for (int x = 0; x < width; ++x)
				{
					Premultiplied sum{};
					const Span &xs = xAxis_.spans[x];
					for (int j = 0; j < xs.count; ++j)
					{
						const Tap &xt = xAxis_.taps[xs.offset + j];
						const uint8_t *p = input + size_t(xt.index) * 4;
						if (p[3] == 0)
							continue;
						const float a = p[3] * xt.weight;
						sum.r += p[0] * a;
						sum.g += p[1] * a;
						sum.b += p[2] * a;
						sum.a += a;
					}
					row[x] = sum;
				}
				rowIndices_[slot] = yt.index;
			}

			for (int x = 0; x < width; ++x)
			{
				Premultiplied sum{};
				for (int t = 0; t < ys.count; ++t)
				{
					const Tap &yt = yAxis_.taps[ys.offset + t];
					const Premultiplied &p = rows_[size_t(yt.index % rowCount) * width + x];
					sum.r += p.r * yt.weight;
					sum.g += p.g * yt.weight;
					sum.b += p.b * yt.weight;
					sum.a += p.a * yt.weight;
				}
				uint8_t *out = pixels_.data() + (size_t(y) * width + x) * 4;
				out[3] = toByte(sum.a);
				if (out[3] == 0)
				{
					out[0] = out[1] = out[2] = 0;
					continue;
				}
				// Unassociate only after filtering both axes. Hidden RGB cannot
				// bleed into edges and a translucent line keeps its true colour.
				out[0] = toByte(sum.r / sum.a);
				out[1] = toByte(sum.g / sum.a);
				out[2] = toByte(sum.b / sum.a);
			}
		}
		return pixels_;
	}

private:
	struct Tap
	{
		int index;
		float weight;
	};
	struct Span
	{
		size_t offset;
		int count;
	};
	struct Premultiplied
	{
		float r = 0, g = 0, b = 0, a = 0;
	};
	struct Axis
	{
		int sourceSize = 0, destinationSize = 0, firstPixel = 0, pixelCount = 0;
		int maxTaps = 0;
		std::vector<Tap> taps;
		std::vector<Span> spans;

		void configure(int source, int destination, int first, int count)
		{
			if (sourceSize == source && destinationSize == destination && firstPixel == first && pixelCount == count)
				return;
			sourceSize = source;
			destinationSize = destination;
			firstPixel = first;
			pixelCount = count;
			maxTaps = 0;
			taps.clear();
			spans.clear();
			spans.reserve(count);
			const double scale = double(source) / destination;
			for (int i = 0; i < count; ++i)
			{
				const size_t offset = taps.size();
				if (source > destination)
				{
					// Integrate the complete source footprint of this output
					// pixel. Four-neighbour bilinear sampling misses thin detail
					// between sample locations when shrinking an already-raster SVG.
					const double begin = double(first + i) * scale;
					const double end = std::min(double(source), double(first + i + 1) * scale);
					const int start = std::max(0, int(std::floor(begin)));
					const int stop = std::min(source, int(std::ceil(end)));
					for (int s = start; s < stop; ++s)
					{
						const double coverage = std::min(end, double(s + 1)) - std::max(begin, double(s));
						if (coverage > 0.0)
							taps.push_back({s, float(coverage / (end - begin))});
					}
				}
				else
				{
					// Pixel-centre alignment makes 1:1 an identity and enlargement
					// symmetric. Clamp edge samples rather than fading the border.
					const double position = std::clamp((first + i + 0.5) * scale - 0.5, 0.0, double(source - 1));
					const int s = int(position);
					const double fraction = position - s;
					taps.push_back({s, float(1.0 - fraction)});
					if (fraction > 0.0 && s + 1 < source)
						taps.push_back({s + 1, float(fraction)});
				}
				const int tapCount = int(taps.size() - offset);
				spans.push_back({offset, tapCount});
				maxTaps = std::max(maxTaps, tapCount);
			}
		}
	};

	static uint8_t toByte(float value)
	{
		return uint8_t(std::clamp(int(std::lround(value)), 0, 255));
	}

	Axis xAxis_, yAxis_;
	std::vector<Premultiplied> rows_;
	std::vector<int> rowIndices_;
	std::vector<uint8_t> pixels_;
};
