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

	// ROI별 디버그용 버퍼
	std::vector<std::vector<uint8_t>> dbgBins(vtAvxData.size());

	std::vector<std::future<void>> vecHandles(vtAvxData.size());

	unsigned int nCnt = 0;

	for (int i = 0; i < (int)vtAvxData.size(); ++i)
	{
		const rle::AVX_DATA stAvxData = vtAvxData[(size_t)i];
		const CRect roi = vtAvxData[(size_t)i].rtInspect;

		// ROI 크기만큼만 할당
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

	// colSum[x] + totalSum
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
		const int offset = colMean - out.m_nGlobalMean; // signed
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

	// ---------------- UF merge ----------------
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

	// ---------------- Stats ----------------
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
	//	// sum_{k=0..n} k^2
	//	return (n * (n + 1.0) * (2.0 * n + 1.0)) / 6.0;
	//};

	//stw 주석- 2026
	// Circularity (4πA/P²) using your run-interval perimeter approximation (best blob only)
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


	// Stats + root index 저장
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


	// Blob별 row run 분류 (run 1회만)
	std::vector<std::vector<std::vector<std::pair<int, int>>>> blobRows(nBlobCount);
	for (int i = 0; i < nBlobCount; ++i)
		blobRows[i].resize(nRoiH);

	for (int k = 0; k < runCount; ++k)
	{
		const rle::Run& R = runs[k];
		blobRows[runBlobIdx[k]][R.y].emplace_back(R.x0, R.x1);
	}

	// Blob 선택 (면적 기준 정렬)
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
	// defect feature 뽑음
	// pick defect (max area부터)
	for (int i = 0; i < nPickCnt; ++i)
	{
		const int nBlobIdx = vecIdx[i];
		const rle::Stat& s = stats[(size_t)nBlobIdx];
		const int root = idxToRoot[(size_t)nBlobIdx];
		
		if (s.area <= nNoiseFilterPixelCnt) break;

		rle::RleBlobStats stDefectData;

		// Fill bbox/area (convert ROI-relative to absolute)
		stDefectData.eSurfaceType = stAvxData.eSurfaceType;
		stDefectData.bUseWhiteImg = stAvxData.isWhiteImage;
		stDefectData.nLane = stAvxData.nLane;
		stDefectData.nMinBright = s.minv;
		stDefectData.nMaxBright = s.maxv;
		stDefectData.dSizeX = (s.maxx - s.minx + 1) * m_dScaleX;
		stDefectData.dSizeY = (s.maxy - s.miny + 1) * m_dScaleY;
		stDefectData.dArea = (s.maxx - s.minx + 1) * (s.maxy - s.miny + 1); // 일단 x * y -> polygon area 구하도록 변경 필요
		stDefectData.dAreaObj = (double)s.area;
		stDefectData.rtPosCrop.SetRect(s.minx, s.miny, s.maxx + 1, s.maxy + 1);
		stDefectData.rtPos.SetRect(s.minx + (int)r.left, s.miny + (int)r.top, s.maxx + (int)r.left + 1, s.maxy + (int)r.top + 1);
		stDefectData.dRatioX = GetRatioX(stDefectData.dSizeX, stDefectData.dSizeY);
		stDefectData.dRatioY = GetRatioY(stDefectData.dSizeX, stDefectData.dSizeY);

		// Angle (moment-based, Chain-like)
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
	// 현재 처리중인 X의 절대 위치
	int x = nXAbsBegin;
	const int16_t CorrMin = 4;    // 이 이하면 row 평균과 거의 같다고 판단
	const int16_t CorrMax = 40;   // 이 이상은 이상치

								  // Black Th를 16개 벡터에 브로드캐스트
	const int thBlackPlus1 = nThBlack + 1;
	const __m256i vThBlackP1 = _mm256_set1_epi16((short)thBlackPlus1);

	// >= 에서 >를 구현하기 위해 -1 값을 준비
	const int thWhiteMinus1 = nThWhite - 1;
	const __m256i vThWhiteM1 = _mm256_set1_epi16((short)thWhiteMinus1);

	// 16비트 모든 값을 0xFFFF로 준비
	// ~mask를 만들고 싶을 때 XOR 뒤집기 위해 사용
	const __m256i vCorrMin = _mm256_set1_epi16(CorrMin);
	const __m256i vCorrMax = _mm256_set1_epi16(CorrMax);
	const __m256i vOne = _mm256_set1_epi16(1);
	const __m256i vAllOn = _mm256_set1_epi16((short)-1);

	// xAbs 위치에서 연속된 32픽셀 읽고
	// Threshold를 먹인 후 결과를 32 bit mask로 리턴
	auto buildMask32 = [&](int xAbs, uint8_t(&buf)[32]) -> uint32_t
	{
		// 32바이트 unaligned load - 픽셀 값은 uint8
		const __m256i g8 = _mm256_loadu_si256((const __m256i*)(rowGray + xAbs));

		// 나중에 피크밝기를 구할때 사용될 정보
		_mm256_storeu_si256((__m256i*)buf, g8);

		// 256을 128 두 묶음으로 분리
		const __m128i gLo128 = _mm256_castsi256_si128(g8); // 0 ~ 15
		const __m128i gHi128 = _mm256_extracti128_si256(g8, 1); // 16 ~31
																// _mm256_cvtepu8_epi16는 입력을 128을 받기 때문에 굳이 128 두 묶음으로 나눔

																// uint8 16개 -> uint 16개로 확장(각각 256-bit) 
																// 근데 _mm256_cvtepu8_epi16가 정확히 uint8->uint16인지 uint8->int16인지 알아봐야함
		const __m256i g16Lo = _mm256_cvtepu8_epi16(gLo128);
		const __m256i g16Hi = _mm256_cvtepu8_epi16(gHi128);
		// --------------------------------------------------------------------------------------------------------- //
		// ROI 내부 좌표로 변환
		const int dxBase = xAbs - nRoiLeftAbs;

		// 보정값 로드 (col mean - global mean)해서 똑같이 확장
		const __m256i o16Lo = _mm256_loadu_si256((const __m256i*)(nOffRoi + dxBase));
		const __m256i o16Hi = _mm256_loadu_si256((const __m256i*)(nOffRoi + dxBase + 16));

		// abs(offset)
		const __m256i oAbsLo = _mm256_abs_epi16(o16Lo);
		const __m256i oAbsHi = _mm256_abs_epi16(o16Hi);

		// CorrMin <= |off| <= CorrMax
		const __m256i geMinLo = _mm256_cmpgt_epi16(oAbsLo, _mm256_sub_epi16(vCorrMin, vOne));
		const __m256i geMinHi = _mm256_cmpgt_epi16(oAbsHi, _mm256_sub_epi16(vCorrMin, vOne));

		const __m256i leMaxLo = _mm256_cmpgt_epi16(_mm256_add_epi16(vCorrMax, vOne), oAbsLo);
		const __m256i leMaxHi = _mm256_cmpgt_epi16(_mm256_add_epi16(vCorrMax, vOne), oAbsHi);

		const __m256i maskUseCorrLo = _mm256_and_si256(geMinLo, leMaxLo);
		const __m256i maskUseCorrHi = _mm256_and_si256(geMinHi, leMaxHi);

		// 보정값 = 원본 - offset
		const __m256i vCorrLo = _mm256_sub_epi16(g16Lo, o16Lo);
		const __m256i vCorrHi = _mm256_sub_epi16(g16Hi, o16Hi);

		// 컬럼 보정 적용
		// 보정 = 원본 - 오프셋
		// 주의할 점은 오프셋이 너무 크면 음수가 됨.
		// 0~255이 확실하게 보장이 안되므로 th 비교에서 true가 될 수 있음.
		// mask == 1 → 보정값 사용
		// mask == 0 → 원본 g16 사용
		const __m256i v16Lo = _mm256_blendv_epi8(g16Lo, vCorrLo, maskUseCorrLo);
		const __m256i v16Hi = _mm256_blendv_epi8(g16Hi, vCorrHi, maskUseCorrHi);

		// 픽셀 On/Off 마스크 만들기
		__m256i m16Lo, m16Hi;

		if (mode == rle::EGrayBinMode::BlackOnly)
		{	// v <= nThBlack 이면 On (기준치보다 낮은 defect 탐색)
			// v16Lo > vThBlack 이면 0xFFFF 아니면 0
			//const __m256i gtLo = _mm256_cmpgt_epi16(v16Lo, vThBlack); stw
			//const __m256i gtHi = _mm256_cmpgt_epi16(v16Hi, vThBlack); stw

			// xor로 ~m 을 만듦
			// a <= b == !(a > b) 이므로 !를 만들기 위해 xor로 ~m을 만듦
			//m16Lo = _mm256_xor_si256(gtLo, vAllOn); stw
			//m16Hi = _mm256_xor_si256(gtHi, vAllOn); stw

			m16Lo = _mm256_cmpgt_epi16(vThBlackP1, v16Lo);
			m16Hi = _mm256_cmpgt_epi16(vThBlackP1, v16Hi);
		}
		else if (mode == rle::EGrayBinMode::WhiteOnly)
		{	// v > thWhite이면 On
			m16Lo = _mm256_cmpgt_epi16(v16Lo, vThWhiteM1);
			m16Hi = _mm256_cmpgt_epi16(v16Hi, vThWhiteM1);
		}
		else
		{	// Black White 둘 다
			const __m256i leBLo = _mm256_cmpgt_epi16(vThBlackP1, v16Lo);
			const __m256i leBHi = _mm256_cmpgt_epi16(vThBlackP1, v16Hi);

			const __m256i geWLo = _mm256_cmpgt_epi16(v16Lo, vThWhiteM1);
			const __m256i geWHi = _mm256_cmpgt_epi16(v16Hi, vThWhiteM1);

			m16Lo = _mm256_or_si256(leBLo, geWLo);
			m16Hi = _mm256_or_si256(leBHi, geWHi);
		}

		// ----------------------------------------------------
		// 16bit 마스크를 8bit로 패킹 -> 32 바이트
		//const __m256i packed8 = _mm256_packs_epi16(m16Lo, m16Hi);
		// AVX packs는 128bit 단위로 동작함.
		// m16Lo(0~15), m16Hi(16~31)을 넣으면 packed8 바이트 순서는
		// m16Lo의 low 8 + m16Hi의 low 8
		// m16Lo의 high 8 + m16Hi의 high 8
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


	// 피크밝기 구하는데 사용될 변수
	alignas(32) uint8_t tempRowBuf[32];

	// AVX2 blocks
	for (; x + 32 <= nXAbsEnd; x += 32)
	{
		// x 위치에서 32픽셀 처리
		// 32픽셀의 Th 비교 결과로 On/Off 마스크를 얻음
		const uint32_t m = buildMask32(x, tempRowBuf);

		// 디버그용 코드. 이진 결과를 외부 버퍼에 넣을지 말지 결정
		//{
		//    // ROI-relative base index for this 32-block
		//    const int xRelBase = x - nRoiLeftAbs;
		//    StoreMask32ToBin(m, pDbgBinRow + xRelBase);
		//}

		// ---- RUN EMIT ----
		// m이 0이면 32픽셀 모두 블랙이라는 뜻이니까 0이 아닐때만 Run 생성함.
		if (m)
		{
			const int xRelBase = x - nRoiLeftAbs;
			EmitRunsFromMask32(m, xRelBase, ry, tempRowBuf, vecRuns);
		}
	}

	// tail scalar, 32픽셀보다 작은 남은 부분 픽셀 처리
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
