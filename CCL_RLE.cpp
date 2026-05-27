#include "stdafx.h"
#include "CCL_RLE.h"


CCL_RLE::CCL_RLE()
{
	m_pJobPool = std::make_shared<gThreadPool>(30);
	m_nMaxDefectCnt = 30;
	m_nNoiseFilterPixelCnt = 1;
}


CCL_RLE::~CCL_RLE()
{
}

void CCL_RLE::RunBatch_ThreadPool_GrayCorrCCL(const uint8_t* pSrcGrayW, const uint8_t* pSrcGrayD, int W, int H, const std::vector<rle::AVX_DATA>& vtAvxData, std::vector<rle::RleBlobStats>& vecOut, bool bUse8Conn)
{
	auto start_time1 = std::chrono::high_resolution_clock::now();

	// 검사영역별 디버그용 버퍼
	std::vector<std::vector<uint8_t>> dbgBins(vtAvxData.size());

	std::vector<std::future<void>> vecHandles(vtAvxData.size());

	unsigned int nCnt = 0;

	for (int i = 0; i < (int)vtAvxData.size(); ++i)
	{
		const rle::AVX_DATA stAvxData = vtAvxData[(size_t)i];
		const CRect roi = vtAvxData[(size_t)i].rtInspect;

		// 검사영역 크기만큼만 할당한다.
		const int roiW = roi.Width();
		const int roiH = roi.Height();
		const bool isWhite = vtAvxData[(size_t)i].isWhiteImage;
		const uint8_t* pSrcGray = isWhite ? pSrcGrayW : pSrcGrayD;
		vecHandles[nCnt++] = m_pJobPool->addJob([=, &vecOut, &dbgBins]()
		{
				auto start_time = std::chrono::high_resolution_clock::now();
				if (roiW > 0 && roiH > 0)
					dbgBins[(size_t)i].assign((size_t)roiW * (size_t)roiH, 0);
				
				uint8_t* pDbg = dbgBins[(size_t)i].empty() ? nullptr : dbgBins[(size_t)i].data();
				std::vector<rle::RleBlobStats> vtDefectData{};
				int nBlobCnt = RunOneRoi_GrayCorrCCL(
					pSrcGray,
					W, H, roi,
					stAvxData, bUse8Conn,
					pDbg, vtDefectData, i
				);

				if(nBlobCnt > 0){
					m_mtxCandiDefect.lock();
					vecOut.insert(vecOut.end(), vtDefectData.begin(), vtDefectData.end());
					m_mtxCandiDefect.unlock();
				}
				auto end_time = std::chrono::high_resolution_clock::now();
				std::chrono::duration<double, std::milli> tack_time = end_time - start_time;
				double dTime = tack_time.count();
				CString str;
				str.Format("RLE - Type : %s(%d) - %dEA %.3fms\n", stAvxData.eSurfaceType == rle::SURFACE_TYPE::SURFACE_TYPE_COAT ? "Coat" : "Null", stAvxData.nLane, nBlobCnt, dTime);
				OutputDebugString(str);

		});
	}
	for (unsigned int i = 0; i < nCnt; i++)
	{
		vecHandles[i].wait();
	}
	auto end_time = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double, std::milli> tack_time = end_time - start_time1;
	double dTime = tack_time.count();
	CString str;
	str.Format("RLE - Type : Thread Wait %.3fms\n", dTime);
	OutputDebugString(str);
}

int CCL_RLE::RunOneRoi_GrayCorrCCL(const uint8_t* pSrcGray, int W, int H, const CRect& roi, rle::AVX_DATA stAvxData, bool bUse8Conn, uint8_t* pDbgBinRoi, std::vector<rle::RleBlobStats>& vtDefectData, int idx)
{
	rle::ColCorrPrecomp cc;
	if (!BuildColCorrPrecomp(pSrcGray, W, H, stAvxData.nTHB, stAvxData.nTHW, roi, cc))
		return 0;

	return RunCCL_RLE_AVX_GrayCorrThresh(
		pSrcGray,
		W, H, roi,
		cc,
		stAvxData,
		bUse8Conn,
		pDbgBinRoi, // nullptr이면 디버그 안 함
		vtDefectData,
		idx
	);
}

bool CCL_RLE::BuildColCorrPrecomp(const uint8_t* pSrcGray, int W, int H, int nThBlack, int nThWhite, const CRect& rtRoi, rle::ColCorrPrecomp& out)
{
	if (!pSrcGray || W <= 0 || H <= 0) return false;

	CRect r = rtRoi;
	r.left = std::max<LONG>(0, r.left);
	r.top = std::max<LONG>(0, r.top);
	r.right = std::min<LONG>(W, r.right);
	r.bottom = std::min<LONG>(H, r.bottom);

	const int nRoiW = r.Width();
	const int nRoiH = r.Height();
	if (nRoiW <= 0 || nRoiH <= 0) return false;

	out.m_RoiW = nRoiW;
	out.m_RoiH = nRoiH;
	out.m_vecOffset.assign((size_t)nRoiW, 0);

	// 열별 합계와 전체 합계를 계산한다.
	std::vector<uint64_t> colSum((size_t)nRoiW, 0ull);
	std::vector<uint64_t> colCnt((size_t)nRoiW, 0ull);
	uint64_t totalSum = 0ull;

	for (int y = r.top; y < r.bottom; ++y)
	{
		const uint8_t* row = pSrcGray + (size_t)y * (size_t)W;
		for (int x = r.left; x < r.right; ++x)
		{
			const uint8_t v = row[x];
			if (nThBlack <= v && nThWhite >= v) {
				colSum[(size_t)(x - r.left)] += v;
				colCnt[(size_t)(x - r.left)]++;
				totalSum += v;
			}
		}
	}

	const double dInvN = 1.0 / (double)((int64_t)nRoiW * (int64_t)nRoiH);
	const int nGlobalMean = (int)std::lround((double)totalSum * dInvN);
	out.m_nGlobalMean = std::max(0, std::min(nGlobalMean, 255));

	const double invH = 1.0 / (double)nRoiH;

	for (int dx = 0; dx < nRoiW; ++dx)
	{
		const int colMean = (int)std::lround((double)colSum[(size_t)dx] / std::max((int)colCnt[(size_t)dx],1));
		const int offset = colMean - out.m_nGlobalMean; // 부호 있는 보정값
		out.m_vecOffset[(size_t)dx] = (int16_t)std::max(-255, std::min(offset, 255));
	}

	return true;
}

inline int CCL_RLE::RunCCL_RLE_AVX_GrayCorrThresh(const uint8_t* pSrcGray, int W, int H, CRect rtRoi, const rle::ColCorrPrecomp& cc, rle::AVX_DATA stAvxData, bool bUse8Conn, uint8_t* pDbgBinRoi, std::vector<rle::RleBlobStats>& vtDefectData, int idx)
{
	//ScopeLog(INSP_LOG.get());
	//gTimeTracker::START("RunCCL", INSP_LOG.get());

	if (!pSrcGray || W <= 0 || H <= 0) return 0;

	CRect r = rtRoi;
	r.left = std::max<long>(0, r.left);
	r.top = std::max<long>(0, r.top);
	r.right = std::min<long>(W, r.right);
	r.bottom = std::min<long>(H, r.bottom);

	const int nRoiW = r.Width();
	const int nRoiH = r.Height();
	if (nRoiW <= 0 || nRoiH <= 0) return 0;
	if ((int)cc.m_vecOffset.size() < nRoiW) return 0;

	std::vector<rle::Run> runs;
	runs.reserve((size_t)nRoiH * 64); // 대략 40만개 정도 미리 예약
	std::vector<int> vecRowStart((size_t)nRoiH + 1, 0);

	for (int ry = 0; ry < nRoiH; ++ry)
	{
		vecRowStart[(size_t)ry] = (int)runs.size();

		const int yAbs = (int)r.top + ry;
		const uint8_t* row = pSrcGray + (size_t)yAbs * (size_t)W;

		uint8_t* dbgRow = nullptr;
		AppendRunsRow_GrayCorrThresh_AVX2<false>(
			row, (int)r.left, (int)r.right, ry, (int)r.left, cc.m_vecOffset.data(),
			stAvxData.nTHB, stAvxData.nTHW, stAvxData.mode, runs, nullptr
			);
	}
	//gTimeTracker::check("RunCCL", INSP_LOG.get(), _G_LOGGER_FILENAME, __LINE__, "done AppendRunsRow_GrayCorrThresh_AVX2");

	vecRowStart[(size_t)nRoiH] = (int)runs.size();

	const int runCount = (int)runs.size();
	if (runCount == 0) return 0;

	// ---------------- 합집합 찾기 병합 ----------------
	rle::UF uf;
	uf.Reset(runCount);

	const int pad = bUse8Conn ? 2 : 1;

	for (int ry = 1; ry < nRoiH; ++ry)
	{
		const int prvB = vecRowStart[(size_t)ry - 1];
		const int prvE = vecRowStart[(size_t)ry];
		const int curB = vecRowStart[(size_t)ry];
		const int curE = vecRowStart[(size_t)ry + 1];

		int i = prvB;
		int j = curB;

		while (i < prvE && j < curE)
		{
			const rle::Run& P = runs[(size_t)i];
			const rle::Run& C = runs[(size_t)j];

			const int CL = C.x0 - pad;
			const int CR = C.x1 + pad;

			if (P.x1 < CL) { ++i; continue; }
			if (P.x0 > CR) { ++j; continue; }

			uf.Union(P.label, C.label);

			if (P.x1 <= CR) ++i;
			else            ++j;
		}
	}
	//gTimeTracker::check("RunCCL", INSP_LOG.get(), _G_LOGGER_FILENAME, __LINE__, "done pad");

	// ---------------- 통계 계산 ----------------
	std::vector<int> rootToIdx((size_t)runCount, -1);
	std::vector<int> idxToRoot; idxToRoot.reserve((size_t)runCount / 4);
	std::vector<rle::Stat> stats; stats.reserve((size_t)runCount / 4);
	
	//stw 주석- 2026
	/*auto getIdx = [&](int root) -> int
	{
		int& v = rootToIdx[(size_t)root];
		if (v < 0)
		{
			v = (int)stats.size();
			stats.push_back(Stat{});
			idxToRoot.push_back(root);
		}
		return v;
	};*/

	//stw 주석- 2026
	//auto SumSq_0ToN = [](double n) -> double
	//{
	//	// 0부터 n까지의 제곱합
	//	return (n * (n + 1.0) * (2.0 * n + 1.0)) / 6.0;
	//};

	//stw 주석- 2026
	// 런 구간 기반 둘레 근사를 사용하여 선택 블롭의 원형도를 계산한다.
	/*auto OverlapLen = [](const std::vector<std::pair<int, int>>& A,
		const std::vector<std::pair<int, int>>& B) -> int
	{
		int i = 0, j = 0;
		int sum = 0;
		while (i < (int)A.size() && j < (int)B.size())
		{
			const int a0 = A[(size_t)i].first, a1 = A[(size_t)i].second;
			const int b0 = B[(size_t)j].first, b1 = B[(size_t)j].second;

			const int L = std::max(a0, b0);
			const int R = std::min(a1, b1);
			if (L <= R) sum += (R - L + 1);

			if (a1 < b1) ++i; else ++j;
		}
		return sum;
	};*/

	
	//stw 주석- 2026
	/*auto ComputePerimeter4 = [&](const std::vector<std::vector<std::pair<int, int>>>& rowsIn) -> double
	{
		double P = 0.0;

		std::vector<int> rowLen(nRoiH, 0);
		std::vector<int> rowRuns(nRoiH, 0);

		for (int y = 0; y < nRoiH; ++y)
		{
			for (const auto& p : rowsIn[y])
				rowLen[y] += (p.second - p.first + 1);

			rowRuns[y] = (int)rowsIn[y].size();
		}

		// 좌/우 경계
		for (int y = 0; y < nRoiH; ++y)
		{
			P += 2.0 * rowRuns[y] * m_fScaleY;
		}

		// 상단 경계만 계산 (중복 제거)
		for (int y = 0; y < nRoiH; ++y)
		{
			const int prevOverlap =
				(y > 0) ? OverlapLen(rowsIn[y], rowsIn[y - 1]) : 0;
			const int nextOverlap =
				(y + 1 < nRoiH) ? OverlapLen(rowsIn[y], rowsIn[y + 1]) : 0;

			const int topExposed = rowLen[y] - prevOverlap;
			const int btmExposed = rowLen[y] - nextOverlap;

			P += (double)(topExposed + btmExposed) * m_fScaleX;
		}

		return P;
	};*/


	// 통계와 루트 인덱스를 저장한다.
	std::vector<int> runBlobIdx((size_t)runCount);


	for (int k = 0; k < runCount; ++k)
	{
		const rle::Run& R = runs[(size_t)k];
		const int root = uf.Find(R.label);
		const int si = GetRootIndex(rootToIdx, idxToRoot, stats, root);
		runBlobIdx[k] = si;

		rle::Stat& s = stats[(size_t)si];

		const int len = R.x1 - R.x0 + 1;
		const int y = R.y;
		const int x0 = R.x0;
		const int x1 = R.x1;
		const int minv = R.minVal;
		const int maxv = R.maxVal;

		s.area += len;
		s.minx = std::min(s.minx, x0);
		s.maxx = std::max(s.maxx, x1);

		s.miny = std::min(s.miny, y);
		s.maxy = std::max(s.maxy, y);

		s.minv = std::min(s.minv, minv);
		s.maxv = std::max(s.maxv, maxv);

		const double dLen = (double)len;
		const double sumX = 0.5 * (double)(x0 + x1) * dLen;

		const double A = (double)x0;
		const double B = (double)x1;
		const double sumX2 = SumSq_0ToN(B) - SumSq_0ToN(A - 1.0);

		const double dy = (double)y;

		s.M00 += dLen;
		s.M10 += sumX;
		s.M01 += dy * dLen;
		s.M20 += sumX2;
		s.M02 += (dy * dy) * dLen;
		s.M11 += dy * sumX;
	}

	const int nBlobCount = (int)stats.size();
	if (nBlobCount == 0) return 0;


	// 블롭별로 행의 런을 분류한다. 런은 한 번만 순회한다.
	std::vector<std::vector<std::vector<std::pair<int, int>>>> blobRows(nBlobCount);
	for (int i = 0; i < nBlobCount; ++i)
		blobRows[i].resize(nRoiH);

	for (int k = 0; k < runCount; ++k)
	{
		const rle::Run& R = runs[k];
		blobRows[runBlobIdx[k]][R.y].emplace_back(R.x0, R.x1);
	}

	// 면적 기준으로 정렬하여 블롭을 선택한다.
	std::vector<int> vecIdx(nBlobCount);
	std::iota(vecIdx.begin(), vecIdx.end(), 0);

	std::sort(vecIdx.begin(), vecIdx.end(),
		[&](int a, int b)
	{
		return stats[a].area > stats[b].area;
	});

	const int nMaxDefect = m_nMaxDefectCnt;
	const int nPickCnt = std::min(nMaxDefect, nBlobCount); //std::min(Get_MaxDefectCnt(nRoiW, nRoiH), nBlobCount);
	const int nNoiseFilterPixelCnt = m_nNoiseFilterPixelCnt;
	// 결함 특징값을 계산한다.
	// 면적이 큰 결함부터 선택한다.
	for (int i = 0; i < nPickCnt; ++i)
	{
		const int nBlobIdx = vecIdx[i];
		const rle::Stat& s = stats[(size_t)nBlobIdx];
		const int root = idxToRoot[(size_t)nBlobIdx];
		
		if (s.area <= nNoiseFilterPixelCnt) break;

		rle::RleBlobStats stDefectData;

		// 경계 상자와 면적을 채우고 검사영역 상대 좌표를 절대 좌표로 변환한다.
		stDefectData.eSurfaceType = stAvxData.eSurfaceType;
		stDefectData.bUseWhiteImg = stAvxData.isWhiteImage;
		stDefectData.nLane = stAvxData.nLane;
		stDefectData.nMinBright = s.minv;
		stDefectData.nMaxBright = s.maxv;
		stDefectData.dSizeX = (s.maxx - s.minx + 1) * m_dScaleX;
		stDefectData.dSizeY = (s.maxy - s.miny + 1) * m_dScaleY;
		stDefectData.dArea = (s.maxx - s.minx + 1) * (s.maxy - s.miny + 1); // 현재는 너비와 높이의 곱을 사용하며 추후 다각형 면적 계산으로 변경이 필요하다.
		stDefectData.dAreaObj = (double)s.area;
		stDefectData.rtPosCrop.SetRect(s.minx, s.miny, s.maxx + 1, s.maxy + 1);
		stDefectData.rtPos.SetRect(s.minx + (int)r.left, s.miny + (int)r.top, s.maxx + (int)r.left + 1, s.maxy + (int)r.top + 1);
		stDefectData.dRatioX = GetRatioX(stDefectData.dSizeX, stDefectData.dSizeY);
		stDefectData.dRatioY = GetRatioY(stDefectData.dSizeX, stDefectData.dSizeY);

		// 모멘트 기반으로 각도를 계산한다.
		stDefectData.dAngle = GetAngle(s);

		auto& rows = blobRows[nBlobIdx];

		const double area = (double)s.area * m_dScaleX * m_dScaleY;
		const double perim = ComputePerimeter4(rows, nRoiH, m_dScaleY);

		double circ = GetRoundness(area, perim);

		stDefectData.dCompactness = circ;
		vtDefectData.emplace_back(stDefectData);
	}
	//gTimeTracker::check("RunCCL", INSP_LOG.get(), _G_LOGGER_FILENAME, __LINE__, "done set Defect Data");
	
	//gTimeTracker::END("RunCCL", INSP_LOG.get());

	return nBlobCount;
}

template<bool kDebugBin>
inline void CCL_RLE::AppendRunsRow_GrayCorrThresh_AVX2(const uint8_t* rowGray, int nXAbsBegin, int nXAbsEnd, int ry, int nRoiLeftAbs, const int16_t* nOffRoi, int nThBlack, int nThWhite, rle::EGrayBinMode mode, std::vector<rle::Run>& vecRuns, uint8_t* pDbgBinRow)
{
	// 현재 처리 중인 x의 절대 위치이다.
	int x = nXAbsBegin;
	const int16_t CorrMin = 4;    // 이 값 이하면 행 평균과 거의 같다고 판단한다.
	const int16_t CorrMax = 40;   // 이 값 이상이면 이상치로 판단한다.

								  // 흑색 임계값을 16개 원소 벡터로 확장한다.
	const int thBlackPlus1 = nThBlack + 1;
	const __m256i vThBlackP1 = _mm256_set1_epi16((short)thBlackPlus1);

	// 크거나 같음 비교를 초과 비교로 구현하기 위해 1을 뺀 값을 준비한다.
	const int thWhiteMinus1 = nThWhite - 1;
	const __m256i vThWhiteM1 = _mm256_set1_epi16((short)thWhiteMinus1);

	// 16비트의 모든 비트가 켜진 값을 준비한다.
	// 마스크를 반전할 때 배타적 논리합 연산에 사용한다.
	const __m256i vCorrMin = _mm256_set1_epi16(CorrMin);
	const __m256i vCorrMax = _mm256_set1_epi16(CorrMax);
	const __m256i vOne = _mm256_set1_epi16(1);
	const __m256i vAllOn = _mm256_set1_epi16((short)-1);

	// xAbs 위치부터 연속된 32픽셀을 읽는다.
	// 임계값 비교 결과를 32비트 마스크로 반환한다.
	auto buildMask32 = [&](int xAbs, uint8_t(&buf)[32]) -> uint32_t
	{
		// 정렬되지 않은 32바이트를 읽으며 픽셀 값 형식은 uint8이다.
		const __m256i g8 = _mm256_loadu_si256((const __m256i*)(rowGray + xAbs));

		// 이후 최대 밝기를 구할 때 사용할 값을 저장한다.
		_mm256_storeu_si256((__m256i*)buf, g8);

		// 256비트 값을 두 개의 128비트 묶음으로 분리한다.
		const __m128i gLo128 = _mm256_castsi256_si128(g8); // 0번부터 15번 픽셀
		const __m128i gHi128 = _mm256_extracti128_si256(g8, 1); // 16번부터 31번 픽셀
																// 이 명령은 128비트를 입력받으므로 두 묶음으로 나누어 처리한다.

																// uint8 값 16개를 각각 256비트의 uint16 값으로 확장한다.
																// 이 명령의 확장 형식이 부호 없는 8비트에서 16비트인지 확인이 필요하다.
		const __m256i g16Lo = _mm256_cvtepu8_epi16(gLo128);
		const __m256i g16Hi = _mm256_cvtepu8_epi16(gHi128);
		// --------------------------------------------------------------------------------------------------------- //
		// 검사영역 내부 좌표로 변환한다.
		const int dxBase = xAbs - nRoiLeftAbs;

		// 열 평균에서 전체 평균을 뺀 보정값을 읽고 같은 방식으로 확장한다.
		const __m256i o16Lo = _mm256_loadu_si256((const __m256i*)(nOffRoi + dxBase));
		const __m256i o16Hi = _mm256_loadu_si256((const __m256i*)(nOffRoi + dxBase + 16));

		// 보정값의 절대값을 계산한다.
		const __m256i oAbsLo = _mm256_abs_epi16(o16Lo);
		const __m256i oAbsHi = _mm256_abs_epi16(o16Hi);

		// 보정값의 절대값이 보정 적용 범위 안인지 검사한다.
		const __m256i geMinLo = _mm256_cmpgt_epi16(oAbsLo, _mm256_sub_epi16(vCorrMin, vOne));
		const __m256i geMinHi = _mm256_cmpgt_epi16(oAbsHi, _mm256_sub_epi16(vCorrMin, vOne));

		const __m256i leMaxLo = _mm256_cmpgt_epi16(_mm256_add_epi16(vCorrMax, vOne), oAbsLo);
		const __m256i leMaxHi = _mm256_cmpgt_epi16(_mm256_add_epi16(vCorrMax, vOne), oAbsHi);

		const __m256i maskUseCorrLo = _mm256_and_si256(geMinLo, leMaxLo);
		const __m256i maskUseCorrHi = _mm256_and_si256(geMinHi, leMaxHi);

		// 보정값은 원본 값에서 오프셋을 뺀 값이다.
		const __m256i vCorrLo = _mm256_sub_epi16(g16Lo, o16Lo);
		const __m256i vCorrHi = _mm256_sub_epi16(g16Hi, o16Hi);

		// 컬럼 보정 적용
		// 보정 = 원본 - 오프셋
		// 주의할 점은 오프셋이 너무 크면 음수가 됨.
		// 0부터 255까지의 범위가 보장되지 않으므로 임계값 비교에서 참이 될 수 있다.
		// 마스크 값이 1이면 보정값을 사용한다.
		// 마스크 값이 0이면 원본 픽셀값을 사용한다.
		const __m256i v16Lo = _mm256_blendv_epi8(g16Lo, vCorrLo, maskUseCorrLo);
		const __m256i v16Hi = _mm256_blendv_epi8(g16Hi, vCorrHi, maskUseCorrHi);

		// 픽셀의 켜짐 여부 마스크를 만든다.
		__m256i m16Lo, m16Hi;

		if (mode == rle::EGrayBinMode::BlackOnly)
		{	// 값이 흑색 임계값 이하이면 켜진 픽셀로 판단하여 어두운 결함을 탐색한다.
			// 값이 흑색 임계값보다 크면 모든 비트가 켜지고 아니면 0이 된다.
			//const __m256i gtLo = _mm256_cmpgt_epi16(v16Lo, vThBlack); stw
			//const __m256i gtHi = _mm256_cmpgt_epi16(v16Hi, vThBlack); stw

			// 배타적 논리합 연산으로 반전 마스크를 만든다.
			// 작거나 같음 비교는 초과 비교 결과를 반전하여 만든다.
			//m16Lo = _mm256_xor_si256(gtLo, vAllOn); stw
			//m16Hi = _mm256_xor_si256(gtHi, vAllOn); stw

			m16Lo = _mm256_cmpgt_epi16(vThBlackP1, v16Lo);
			m16Hi = _mm256_cmpgt_epi16(vThBlackP1, v16Hi);
		}
		else if (mode == rle::EGrayBinMode::WhiteOnly)
		{	// 값이 백색 임계값보다 크면 켜진 픽셀로 판단한다.
			m16Lo = _mm256_cmpgt_epi16(v16Lo, vThWhiteM1);
			m16Hi = _mm256_cmpgt_epi16(v16Hi, vThWhiteM1);
		}
		else
		{	// 어두운 결함과 밝은 결함을 모두 탐색한다.
			const __m256i leBLo = _mm256_cmpgt_epi16(vThBlackP1, v16Lo);
			const __m256i leBHi = _mm256_cmpgt_epi16(vThBlackP1, v16Hi);

			const __m256i geWLo = _mm256_cmpgt_epi16(v16Lo, vThWhiteM1);
			const __m256i geWHi = _mm256_cmpgt_epi16(v16Hi, vThWhiteM1);

			m16Lo = _mm256_or_si256(leBLo, geWLo);
			m16Hi = _mm256_or_si256(leBHi, geWHi);
		}

		// ----------------------------------------------------
		// 16비트 마스크를 8비트 값으로 묶어 32바이트로 만든다.
		//const __m256i packed8 = _mm256_packs_epi16(m16Lo, m16Hi);
		// AVX 묶기 연산은 128비트 단위로 동작한다.
		// m16Lo(0~15), m16Hi(16~31)을 넣으면 묶인 바이트 순서는
		// m16Lo의 하위 8개 값과 m16Hi의 하위 8개 값
		// m16Lo의 상위 8개 값과 m16Hi의 상위 8개 값
		// 즉 픽셀 순서가 [0~7, 16~23, 8~15, 24~31]로 섞일 수 있음.
		// 이 부분 디버깅 잘 해봐야 함
		//return (uint32_t)_mm256_movemask_epi8(packed8);
		// ----------------------------------------------------
		__m256i packed8 = _mm256_packs_epi16(m16Lo, m16Hi);
		packed8 = _mm256_permutevar8x32_epi32(
			packed8, _mm256_setr_epi32(0, 1, 4, 5, 2, 3, 6, 7)
		);
		return (uint32_t)_mm256_movemask_epi8(packed8);
	};


	// 최대 밝기를 구할 때 사용할 임시 버퍼이다.
	alignas(32) uint8_t tempRowBuf[32];

	// AVX2 블록 단위 처리
	for (; x + 32 <= nXAbsEnd; x += 32)
	{
		// x 위치부터 32픽셀을 처리한다.
		// 32픽셀의 임계값 비교 결과에서 켜짐 여부 마스크를 얻는다.
		const uint32_t m = buildMask32(x, tempRowBuf);

		// 디버그용 코드. 이진 결과를 외부 버퍼에 넣을지 말지 결정
		//{
		//    // 현재 32픽셀 블록의 검사영역 상대 시작 위치
		//    const int xRelBase = x - nRoiLeftAbs;
		//    StoreMask32ToBin(m, pDbgBinRow + xRelBase);
		//}

		// ---- 런 생성 ----
		// m이 0이면 32픽셀이 모두 검은색이므로 0이 아닐 때만 런을 생성한다.
		if (m)
		{
			const int xRelBase = x - nRoiLeftAbs;
			EmitRunsFromMask32(m, xRelBase, ry, tempRowBuf, vecRuns);
		}
	}

	// 32픽셀보다 적게 남은 꼬리 구간은 스칼라 방식으로 처리한다.
	for (; x < nXAbsEnd; ++x)
	{
		const int dx = x - nRoiLeftAbs;

		const int gray = (int)rowGray[x];
		const int off = (int)nOffRoi[dx];
		const int aoff = std::abs(off);

		int minVal = gray;
		int maxVal = gray;
		int v;
		// AVX와 동일한 보정 조건
		if (aoff >= CorrMin && aoff <= CorrMax)
			v = gray - off;
		else
			v = gray;

		bool on = false;
		if (mode == rle::EGrayBinMode::BlackOnly)        on = (v <= nThBlack);
		else if (mode == rle::EGrayBinMode::WhiteOnly)   on = (v >= nThWhite);
		else                                        on = (v <= nThBlack) || (v >= nThWhite);

		if (!on) continue;

		const int x0 = dx;
		int x1 = dx;

		++x;
		for (; x < nXAbsEnd; ++x)
		{
			const int ddx = x - nRoiLeftAbs;
			const int gray2 = (int)rowGray[x];
			const int off2 = (int)nOffRoi[ddx];
			const int aoff2 = std::abs(off2);

			int v2;
			if (aoff2 >= CorrMin && aoff2 <= CorrMax)
				v2 = gray2 - off2;
			else
				v2 = gray2;

			bool on2 = false;
			if (mode == rle::EGrayBinMode::BlackOnly)        on2 = (v2 <= nThBlack);
			else if (mode == rle::EGrayBinMode::WhiteOnly)   on2 = (v2 >= nThWhite);
			else                                        on2 = (v2 <= nThBlack) || (v2 >= nThWhite);

			if (!on2) break;
			x1 = ddx;
			minVal = std::min(minVal, gray2);
			maxVal = std::max(maxVal, gray2);
		}

		rle::Run r;
		r.x0 = x0; r.x1 = x1; r.y = ry; r.minVal = minVal; r.maxVal = maxVal; r.label = (int)vecRuns.size();
		vecRuns.push_back(r);
	}
}

inline void CCL_RLE::AppendRunsRow_Binary(uint8_t* row, int x0, int x1, int y, std::vector<rle::Run>& runs)
{
	int x = x0;
	while (x < x1)
	{
		while (x < x1 && row[x] == 0) ++x;
		if (x >= x1) break;

		int s = x;
		while (x < x1 && row[x] != 0) ++x;

		runs.push_back({ y, s, x - 1, (int)runs.size() });
	}
}

int CCL_RLE::Get_MaxDefectCnt(double dObjWidth, double dObjHeight)
{

	int nBase_w = 300;
	int nBase_h = 200;


	int nCnt_W = (dObjWidth / nBase_w) + 1;
	int nCnt_H = (dObjHeight / nBase_h) + 1;

	if (nCnt_W < 0)
	{
		nCnt_W = 1;
	}
	if (nCnt_H < 0)
	{
		nCnt_H = 1;
	}

	return nCnt_W * nCnt_H;
}


void CCL_RLE::SetNoiseFilterPixelCnt(int nPixel) {
	m_nNoiseFilterPixelCnt = nPixel;
}

void CCL_RLE::SetScale(double dScaleX, double dScaleY) {
	m_dScaleX = dScaleX;
	m_dScaleY = dScaleY;
}

void CCL_RLE::SetMaxDefectCnt(int nMaxDefectCnt) {
	m_nMaxDefectCnt = nMaxDefectCnt;
}
