#pragma once

#undef min
#undef max
#include <algorithm>
#include <vector>
#include <numeric>
#include "gThreadPool.h"

#pragma region CCL AVX
static inline int Ctz32(uint32_t x)
{
#if defined(_MSC_VER)
	unsigned long idx;
	_BitScanForward(&idx, x);
	return (int)idx;
#else
	return __builtin_ctz(x);
#endif
}


#define PI 3.141592653589793238



namespace rle {
	enum class SURFACE_TYPE
	{
		SURFACE_TYPE_NONE = 0,
		SURFACE_TYPE_SIDE,
		SURFACE_TYPE_COAT,
		SURFACE_TYPE_NULL,
		SURFACE_TYPE_INSU,
	};

	struct ColCorrPrecomp
	{
		int m_RoiW = 0;
		int m_RoiH = 0;
		int m_nGlobalMean = 0;                 // 범위는 0부터 255까지이다.
		std::vector<int16_t> m_vecOffset;        // offset[x]는 열 평균에서 전체 평균을 뺀 부호 있는 값이다.
	};

	enum class EGrayBinMode
	{
		BlackOnly,
		WhiteOnly,
		BlackOrWhite, // 어두운 결함과 밝은 결함을 모두 탐색한다.
	};

	struct AVX_DATA
	{
		CRect rtInspect{};

		int nTHW{};
		int nTHB{};
		int nLane{};

		EGrayBinMode mode{};
		SURFACE_TYPE eSurfaceType{};

		bool isWhiteImage{};
		bool isLeft{};
	};

	// 런 길이 기반 연결 요소 라벨링 알고리즘이다.
	struct Run
	{
		int y;
		int x0;
		int x1;     // 끝 좌표를 포함한다.
		int label;  // 합집합 찾기 구조에서 사용할 임시 라벨 인덱스이다.
		int minVal;
		int maxVal;
	};

	// 루트별 면적과 경계 상자 통계를 집계하고 최대 블롭을 선택한다.
	struct Stat
	{
		int area = 0;                 // 픽셀 면적
		int minx = 1 << 30;
		int miny = 1 << 30;
		int minv = 1 << 30;
		int maxx = -1;
		int maxy = -1;
		int maxv = -1;

		double M00 = 0; // 면적
		double M10 = 0; // x 좌표 합
		double M01 = 0; // y 좌표 합
		double M11 = 0; // x와 y의 곱의 합
		double M20 = 0; // x 제곱의 합
		double M02 = 0; // y 제곱의 합
	};

	// CCL 구조체
	struct UF
	{
		std::vector<int> parent, rnk;
		void Reset(int n)
		{
			parent.resize(n);
			rnk.assign(n, 0);
			for (int i = 0; i < n; ++i) parent[i] = i;
		}
		int Find(int x)
		{
			while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
			return x;
		}
		void Union(int a, int b)
		{
			a = Find(a); b = Find(b);
			if (a == b) return;
			if (rnk[a] < rnk[b]) std::swap(a, b);
			parent[b] = a;
			if (rnk[a] == rnk[b]) rnk[a]++;
		}
	};

	struct BlobMetrics
	{
		int nBlobCount = 0;
		int nMinX = 0, nMaxX = 0, nMinY = 0, nMaxY = 0;
		double dArea = 0.0;
		double dAngle = 0.0;
		double dCircularity = 0.0;
	};

	struct RleBlobStats
	{
		SURFACE_TYPE eSurfaceType{};
		bool bUseWhiteImg;
		int nLane;
		int nMinBright;
		int nMaxBright;
		double dSizeX;
		double dSizeY;
		double dArea;
		double dAreaObj;
		double dRatioX;
		double dRatioY;
		double dAngle;
		double dCompactness;
		CRect rtPosCrop;
		CRect rtPos;
	};
}

class CCL_RLE
{
public:
	CCL_RLE();
	~CCL_RLE();

private:

	static __forceinline void EmitRunsFromMask32(uint32_t m, int xRelBase, int ry, uint8_t(&buf)[32], std::vector<rle::Run>& runs)
	{
		// m의 i번째 비트가 1이면 xRelBase + i 위치의 픽셀이 켜진 상태이다. i의 범위는 0부터 31까지이다.
		while (m)
		{
			const uint32_t s = Ctz32(m);            // 시작 비트 위치
			uint32_t t = m >> s;                   // 시작 비트를 최하위 비트 위치로 이동한다.

												   // t의 최하위 비트부터 연속된 1의 개수를 센다.
												   // t가 모두 1인 경우에는 반전값이 0이므로 별도로 처리한다.
			uint32_t inv = ~t;
			uint32_t len = (inv == 0) ? (32u - s) : Ctz32(inv);

			const int x0 = xRelBase + (int)s;
			const int x1 = x0 + (int)len - 1;

			int minVal = 255;
			int maxVal = 0;

			#pragma unroll
			for (int x = s; x < s+ len; ++x)
			{
				minVal = std::min(minVal, (int)buf[x]);
				maxVal = std::max(maxVal, (int)buf[x]);
			}

			rle::Run r;
			r.x0 = x0;
			r.x1 = x1;
			r.y = ry;
			r.minVal = minVal;
			r.maxVal = maxVal;
			r.label = (int)runs.size();
			runs.push_back(r);

			// 현재 런의 끝까지 처리된 비트를 지운다.
			// s + len이 32인 경우에는 시프트 범위를 별도로 처리한다.
			const uint32_t end = s + len;
			if (end >= 32u) break;
			const uint32_t clearMask = (end == 32u) ? 0xFFFFFFFFu : ((1u << end) - 1u);
			m &= ~clearMask;
		}
	}

	static __forceinline int GetRootIndex(std::vector<int> &rootToIdx, std::vector<int>& idxToRoot, std::vector<rle::Stat> &stats, const int root) {
		int& v = rootToIdx[(size_t)root];
		if (v < 0)
		{
			v = (int)stats.size();
			stats.push_back(rle::Stat{});
			idxToRoot.push_back(root);
		}
		return v;
	}

	static __forceinline int OverlapLen(const std::vector<std::pair<int, int>>& A, const std::vector<std::pair<int, int>>& B) {
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
	};

	static __forceinline double SumSq_0ToN(double n){
		// 0부터 n까지의 제곱합을 계산한다.
		return (n * (n + 1.0) * (2.0 * n + 1.0)) / 6.0;
	};

	static __forceinline double ComputePerimeter4(const std::vector<std::vector<std::pair<int, int>>>& rowsIn, const int nRoiH, const double dScaleY) {
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
			P += 2.0 * rowRuns[y] * dScaleY;
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

			P += (double)(topExposed + btmExposed) * dScaleY;
		}

		return P;
	}

	static __forceinline double GetRoundness(const double area, const double perim) {
		double circ = 0.0;
		if (area > 0.0 && perim > 1e-12)
			circ = (4.0 * PI * area) / (perim * perim);

		circ = std::max(0.0, std::min(circ, 1.0));
		return circ;
	}

	static __forceinline double GetAngle(const rle::Stat &s) {
		double dAngle = 0.0;
		if (s.M00 > 0.0)
		{
			const double a = 2.0 * (s.M00 * s.M11 - s.M10 * s.M01);
			const double b = (s.M00 * s.M20 - s.M10 * s.M10) - (s.M00 * s.M02 - s.M01 * s.M01);
			dAngle = std::atan2(a, b) * 90.0 / PI;
		}
		return dAngle;
	}

	static __forceinline double GetRatioX(const double dSizeX, double dSizeY) {
		double dRatioX = dSizeX / dSizeY;
		return dRatioX;
	}

	static __forceinline double GetRatioY(const double dSizeX, double dSizeY) {
		double dRatioY = dSizeY / dSizeX;
		return dRatioY;
	}

	std::shared_ptr<gThreadPool> m_pJobPool;
	std::mutex m_mtxCandiDefect;

	int RunOneRoi_GrayCorrCCL(const uint8_t* pSrcGray,  int W, int H, const CRect& roi, rle::AVX_DATA stAvxData, bool bUse8Conn, uint8_t* pDbgBinRoi, std::vector<rle::RleBlobStats> &vtDefectData, int idx);
	bool BuildColCorrPrecomp(const uint8_t* pSrcGray, int W, int H, int nThBlack, int nThWhite, const CRect& rtRoi, rle::ColCorrPrecomp& out);
	int RunCCL_RLE_AVX_GrayCorrThresh(const uint8_t* pSrcGray, int W, int H, CRect rtRoi, const rle::ColCorrPrecomp& cc, rle::AVX_DATA stAvxData, bool bUse8Conn, uint8_t* pDbgBinRoi, std::vector<rle::RleBlobStats> &vtDefectData, int idx);

	template<bool kDebugBin>
	void AppendRunsRow_GrayCorrThresh_AVX2(const uint8_t* rowGray, int nXAbsBegin, int nXAbsEnd, int ry, int nRoiLeftAbs, const int16_t* nOffRoi, int nThBlack, int nThWhite, rle::EGrayBinMode mode, std::vector<rle::Run>& vecRuns, uint8_t* pDbgBinRow);
	void AppendRunsRow_Binary(uint8_t* row, int x0, int x1, int y, std::vector<rle::Run>& runs);

	int Get_MaxDefectCnt(double dObjWidth, double dObjHeight);
	
	int m_nNoiseFilterPixelCnt;
	double m_dScaleX;
	double m_dScaleY;
	int m_nMaxDefectCnt;

public:

	
	void RunBatch_ThreadPool_GrayCorrCCL(const uint8_t* pSrcGrayW, const uint8_t* pSrcGrayD, int W, int H, const std::vector<rle::AVX_DATA>& vtAvxData, std::vector<rle::RleBlobStats> &vtOut, bool bUse8Conn);

	void SetNoiseFilterPixelCnt(int nPixel);
	void SetScale(double dScaleX, double dScaleY);
	void SetMaxDefectCnt(int nMaxDefectCnt);
};

