#include "stdafx.h"

#undef OutputDebugString
#define CString CStringA
#define OutputDebugString OutputDebugStringA
#define private public
#include "CCL_RLE.cpp"
#undef private
#undef OutputDebugString
#undef CString

#include "MeasuredRunner.h"

namespace
{
	using MeasureClock = std::chrono::high_resolution_clock;

	double ElapsedMs(const MeasureClock::time_point& started)
	{
		return std::chrono::duration<double, std::milli>(MeasureClock::now() - started).count();
	}

	int RunMeasuredOneRoi(CCL_RLE& detector, const uint8_t* source, int width, int height,
		const CRect& inputRoi, const rle::AVX_DATA& data, bool use8Connectivity,
		std::vector<rle::RleBlobStats>& defects, bench::RoiTiming& timing)
	{
		auto phaseStarted = MeasureClock::now();
		rle::ColCorrPrecomp correction;
		if (!detector.BuildColCorrPrecomp(source, width, height, data.nTHB, data.nTHW, inputRoi, correction))
		{
			timing.dColumnCorrectionMs = ElapsedMs(phaseStarted);
			return 0;
		}
		timing.dColumnCorrectionMs = ElapsedMs(phaseStarted);

		CRect roi = inputRoi;
		roi.left = std::max<long>(0, roi.left);
		roi.top = std::max<long>(0, roi.top);
		roi.right = std::min<long>(width, roi.right);
		roi.bottom = std::min<long>(height, roi.bottom);
		const int roiWidth = roi.Width();
		const int roiHeight = roi.Height();
		if (!source || roiWidth <= 0 || roiHeight <= 0 ||
			static_cast<int>(correction.m_vecOffset.size()) < roiWidth)
			return 0;

		phaseStarted = MeasureClock::now();
		std::vector<rle::Run> runs;
		runs.reserve(static_cast<size_t>(roiHeight) * 64);
		std::vector<int> rowStart(static_cast<size_t>(roiHeight) + 1, 0);
		for (int rowIndex = 0; rowIndex < roiHeight; ++rowIndex)
		{
			rowStart[static_cast<size_t>(rowIndex)] = static_cast<int>(runs.size());
			const int absoluteY = static_cast<int>(roi.top) + rowIndex;
			const uint8_t* row = source + static_cast<size_t>(absoluteY) * width;
			detector.AppendRunsRow_GrayCorrThresh_AVX2<false>(
				row, static_cast<int>(roi.left), static_cast<int>(roi.right), rowIndex,
				static_cast<int>(roi.left), correction.m_vecOffset.data(),
				data.nTHB, data.nTHW, data.mode, runs, nullptr);
		}
		rowStart[static_cast<size_t>(roiHeight)] = static_cast<int>(runs.size());
		timing.dThresholdRunsMs = ElapsedMs(phaseStarted);

		const int runCount = static_cast<int>(runs.size());
		if (runCount == 0)
			return 0;

		phaseStarted = MeasureClock::now();
		rle::UF labels;
		labels.Reset(runCount);
		const int padding = use8Connectivity ? 2 : 1;
		for (int rowIndex = 1; rowIndex < roiHeight; ++rowIndex)
		{
			const int previousBegin = rowStart[static_cast<size_t>(rowIndex) - 1];
			const int previousEnd = rowStart[static_cast<size_t>(rowIndex)];
			const int currentBegin = rowStart[static_cast<size_t>(rowIndex)];
			const int currentEnd = rowStart[static_cast<size_t>(rowIndex) + 1];
			int previous = previousBegin;
			int current = currentBegin;
			while (previous < previousEnd && current < currentEnd)
			{
				const rle::Run& upper = runs[static_cast<size_t>(previous)];
				const rle::Run& lower = runs[static_cast<size_t>(current)];
				const int lowerLeft = lower.x0 - padding;
				const int lowerRight = lower.x1 + padding;
				if (upper.x1 < lowerLeft) { ++previous; continue; }
				if (upper.x0 > lowerRight) { ++current; continue; }
				labels.Union(upper.label, lower.label);
				if (upper.x1 <= lowerRight) ++previous;
				else ++current;
			}
		}
		timing.dLabelMergeMs = ElapsedMs(phaseStarted);

		phaseStarted = MeasureClock::now();
		std::vector<int> rootToIndex(static_cast<size_t>(runCount), -1);
		std::vector<int> indexToRoot;
		indexToRoot.reserve(static_cast<size_t>(runCount) / 4);
		std::vector<rle::Stat> stats;
		stats.reserve(static_cast<size_t>(runCount) / 4);
		std::vector<int> runBlobIndex(static_cast<size_t>(runCount));

		for (int index = 0; index < runCount; ++index)
		{
			const rle::Run& run = runs[static_cast<size_t>(index)];
			const int root = labels.Find(run.label);
			const int statIndex = detector.GetRootIndex(rootToIndex, indexToRoot, stats, root);
			runBlobIndex[static_cast<size_t>(index)] = statIndex;
			rle::Stat& stat = stats[static_cast<size_t>(statIndex)];
			const int length = run.x1 - run.x0 + 1;
			const double lengthDouble = static_cast<double>(length);
			const double y = static_cast<double>(run.y);
			const double sumX = 0.5 * static_cast<double>(run.x0 + run.x1) * lengthDouble;
			const double sumX2 = detector.SumSq_0ToN(static_cast<double>(run.x1)) -
				detector.SumSq_0ToN(static_cast<double>(run.x0) - 1.0);

			stat.area += length;
			stat.minx = std::min(stat.minx, run.x0);
			stat.maxx = std::max(stat.maxx, run.x1);
			stat.miny = std::min(stat.miny, run.y);
			stat.maxy = std::max(stat.maxy, run.y);
			stat.minv = std::min(stat.minv, run.minVal);
			stat.maxv = std::max(stat.maxv, run.maxVal);
			stat.M00 += lengthDouble;
			stat.M10 += sumX;
			stat.M01 += y * lengthDouble;
			stat.M20 += sumX2;
			stat.M02 += y * y * lengthDouble;
			stat.M11 += y * sumX;
		}

		const int blobCount = static_cast<int>(stats.size());
		if (blobCount == 0)
		{
			timing.dStatisticsMs = ElapsedMs(phaseStarted);
			return 0;
		}
		std::vector<std::vector<std::vector<std::pair<int, int>>>> blobRows(static_cast<size_t>(blobCount));
		for (int index = 0; index < blobCount; ++index)
			blobRows[static_cast<size_t>(index)].resize(static_cast<size_t>(roiHeight));
		for (int index = 0; index < runCount; ++index)
		{
			const rle::Run& run = runs[static_cast<size_t>(index)];
			blobRows[static_cast<size_t>(runBlobIndex[static_cast<size_t>(index)])][static_cast<size_t>(run.y)]
				.emplace_back(run.x0, run.x1);
		}
		timing.dStatisticsMs = ElapsedMs(phaseStarted);

		phaseStarted = MeasureClock::now();
		std::vector<int> order(static_cast<size_t>(blobCount));
		std::iota(order.begin(), order.end(), 0);
		std::sort(order.begin(), order.end(), [&](int lhs, int rhs)
		{
			return stats[static_cast<size_t>(lhs)].area > stats[static_cast<size_t>(rhs)].area;
		});
		const int selectedCount = std::min(detector.m_nMaxDefectCnt, blobCount);
		for (int index = 0; index < selectedCount; ++index)
		{
			const int blobIndex = order[static_cast<size_t>(index)];
			const rle::Stat& stat = stats[static_cast<size_t>(blobIndex)];
			if (stat.area <= detector.m_nNoiseFilterPixelCnt)
				break;
			rle::RleBlobStats defect;
			defect.eSurfaceType = data.eSurfaceType;
			defect.bUseWhiteImg = data.isWhiteImage;
			defect.nLane = data.nLane;
			defect.nMinBright = stat.minv;
			defect.nMaxBright = stat.maxv;
			defect.dSizeX = (stat.maxx - stat.minx + 1) * detector.m_dScaleX;
			defect.dSizeY = (stat.maxy - stat.miny + 1) * detector.m_dScaleY;
			defect.dArea = (stat.maxx - stat.minx + 1) * (stat.maxy - stat.miny + 1);
			defect.dAreaObj = static_cast<double>(stat.area);
			defect.rtPosCrop.SetRect(stat.minx, stat.miny, stat.maxx + 1, stat.maxy + 1);
			defect.rtPos.SetRect(stat.minx + static_cast<int>(roi.left), stat.miny + static_cast<int>(roi.top),
				stat.maxx + static_cast<int>(roi.left) + 1, stat.maxy + static_cast<int>(roi.top) + 1);
			defect.dRatioX = detector.GetRatioX(defect.dSizeX, defect.dSizeY);
			defect.dRatioY = detector.GetRatioY(defect.dSizeX, defect.dSizeY);
			defect.dAngle = detector.GetAngle(stat);
			const double area = static_cast<double>(stat.area) * detector.m_dScaleX * detector.m_dScaleY;
			const double perimeter = detector.ComputePerimeter4(blobRows[static_cast<size_t>(blobIndex)],
				roiHeight, detector.m_dScaleY);
			defect.dCompactness = detector.GetRoundness(area, perimeter);
			defects.emplace_back(defect);
		}
		timing.dFeatureMs = ElapsedMs(phaseStarted);
		return blobCount;
	}
}

void RunMeasuredBatch(CCL_RLE& detector, const uint8_t* whiteImage, const uint8_t* darkImage,
	int width, int height, const std::vector<rle::AVX_DATA>& jobs,
	std::vector<rle::RleBlobStats>& output, bool use8Connectivity,
	std::vector<bench::RoiTiming>& roiTiming)
{
	std::vector<std::vector<uint8_t>> debugBins(jobs.size());
	std::vector<std::future<void>> handles(jobs.size());
	roiTiming.assign(jobs.size(), bench::RoiTiming{});

	for (size_t i = 0; i < jobs.size(); ++i)
	{
		const rle::AVX_DATA data = jobs[i];
		const CRect roi = data.rtInspect;
		const uint8_t* source = data.isWhiteImage ? whiteImage : darkImage;
		handles[i] = detector.m_pJobPool->addJob([=, &detector, &output, &debugBins, &roiTiming]()
		{
			const auto started = std::chrono::high_resolution_clock::now();
			const auto bufferStarted = MeasureClock::now();
			if (roi.Width() > 0 && roi.Height() > 0)
				debugBins[i].assign(static_cast<size_t>(roi.Width()) * roi.Height(), 0);
			bench::RoiTiming timing;
			timing.nLane = data.nLane;
			timing.rtInspect = roi;
			timing.dBufferMs = ElapsedMs(bufferStarted);
			std::vector<rle::RleBlobStats> defects;
			const int blobCount = RunMeasuredOneRoi(detector, source, width, height, roi, data,
				use8Connectivity, defects, timing);

			const auto outputStarted = MeasureClock::now();
			if (!defects.empty())
			{
				std::lock_guard<std::mutex> lock(detector.m_mtxCandiDefect);
				output.insert(output.end(), defects.begin(), defects.end());
			}
			timing.dOutputMergeMs = ElapsedMs(outputStarted);
			timing.nBlobCount = blobCount;
			timing.nReturnedCount = defects.size();
			timing.dTimeMs = std::chrono::duration<double, std::milli>(
				std::chrono::high_resolution_clock::now() - started).count();
			roiTiming[i] = timing;
		});
	}

	for (auto& handle : handles)
		handle.wait();
}
