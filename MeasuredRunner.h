#pragma once

#include "CCL_RLE.h"

namespace bench
{
	struct RoiTiming
	{
		int nLane = 0;
		CRect rtInspect{};
		int nBlobCount = 0;
		size_t nReturnedCount = 0;
		double dBufferMs = 0.0;
		double dColumnCorrectionMs = 0.0;
		double dThresholdRunsMs = 0.0;
		double dLabelMergeMs = 0.0;
		double dStatisticsMs = 0.0;
		double dFeatureMs = 0.0;
		double dOutputMergeMs = 0.0;
		double dTimeMs = 0.0;
	};
}

void RunMeasuredBatch(CCL_RLE& detector, const uint8_t* whiteImage, const uint8_t* darkImage,
	int width, int height, const std::vector<rle::AVX_DATA>& jobs,
	std::vector<rle::RleBlobStats>& output, bool use8Connectivity,
	std::vector<bench::RoiTiming>& roiTiming);
