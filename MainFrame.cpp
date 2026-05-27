#include "stdafx.h"
#include "MainFrame.h"

namespace
{
	enum ControlId : UINT
	{
		ID_OPEN_IMAGE = 1001,
		ID_RUN_ONCE,
		ID_BENCHMARK,
		ID_WHOLE_IMAGE,
		ID_SIX_FIXED_ROIS,
		ID_ZOOM_ROI,
		ID_USE_8_CONN,
		ID_WARMUP,
		ID_MODE,
		ID_BLACK_TH,
		ID_WHITE_TH,
		ID_NOISE,
		ID_MAX_BLOB,
		ID_REPEAT,
		ID_IMAGE_VIEW,
		ID_RESULT_LIST,
		ID_ROI_TIMING_LIST
	};

	const wchar_t* kDefaultImageName = L"20250214_094611_391_Cam1_white_1_100x100_400.jpg";

	struct FixedRoiDefinition
	{
		int left;
		int top;
		int right;
		int bottom;
	};

	// Paste six production m_rtInspArea rectangles as { left, top, right, bottom }.
	// If any rectangle is invalid, the temporary equal-width layout is used.
	constexpr std::array<FixedRoiDefinition, 6> kFixedRoiPreset =
	{{
		{ 5440, 0, 10040, 6144 }, // Lane 0: m_rtInspArea[TYPE_?][0]
		{ 10900, 0,15500, 6144 }, // Lane 1: m_rtInspArea[TYPE_?][1]
		{ 16300, 0, 20900, 6144 }, // Lane 2: m_rtInspArea[TYPE_?][2]
		{ 21700, 0, 26300, 6144 }, // Lane 3: m_rtInspArea[TYPE_?][3]
		{ 27150, 0, 31750, 6144 }, // Lane 4: m_rtInspArea[TYPE_?][4]
		{ 32550, 0, 37150, 6144 }  // Lane 5: m_rtInspArea[TYPE_?][5]
	}};

	int ReadEditInt(const CEdit& edit)
	{
		CString value;
		edit.GetWindowText(value);
		return _wtoi(value);
	}
}

BEGIN_MESSAGE_MAP(CMainFrame, CFrameWnd)
	ON_WM_CREATE()
	ON_WM_SIZE()
	ON_BN_CLICKED(ID_OPEN_IMAGE, &CMainFrame::OnOpenImage)
	ON_BN_CLICKED(ID_RUN_ONCE, &CMainFrame::OnRunOnce)
	ON_BN_CLICKED(ID_BENCHMARK, &CMainFrame::OnBenchmark)
	ON_BN_CLICKED(ID_WHOLE_IMAGE, &CMainFrame::OnWholeImage)
	ON_BN_CLICKED(ID_SIX_FIXED_ROIS, &CMainFrame::OnSixFixedRois)
	ON_BN_CLICKED(ID_ZOOM_ROI, &CMainFrame::OnZoomRoi)
	ON_MESSAGE(WM_APP_ROI_CHANGED, &CMainFrame::OnRoiChanged)
	ON_MESSAGE(WM_APP + 11, &CMainFrame::OnLoadDefaultImage)
END_MESSAGE_MAP()

CMainFrame::CMainFrame()
{
	Create(nullptr, L"CCL_RLE AVX Test Bench", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
		CRect(40, 40, 1540, 930));
}

int CMainFrame::OnCreate(LPCREATESTRUCT createStruct)
{
	if (CFrameWnd::OnCreate(createStruct) == -1)
		return -1;

	m_openButton.Create(L"Open Image...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, CRect(), this, ID_OPEN_IMAGE);
	m_runButton.Create(L"Run Once", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, CRect(), this, ID_RUN_ONCE);
	m_benchmarkButton.Create(L"Benchmark", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, CRect(), this, ID_BENCHMARK);
	m_wholeButton.Create(L"Use Whole Image", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, CRect(), this, ID_WHOLE_IMAGE);
	m_sixRoiButton.Create(L"Use 6 Fixed ROIs", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, CRect(), this, ID_SIX_FIXED_ROIS);
	m_zoomCheck.Create(L"Zoom to ROI", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, CRect(), this, ID_ZOOM_ROI);
	m_connCheck.Create(L"8-connected", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, CRect(), this, ID_USE_8_CONN);
	m_warmupCheck.Create(L"Warm-up before benchmark", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, CRect(), this, ID_WARMUP);
	m_connCheck.SetCheck(BST_CHECKED);
	m_warmupCheck.SetCheck(BST_CHECKED);

	m_modeCombo.Create(WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, CRect(), this, ID_MODE);
	m_modeCombo.AddString(L"Black only");
	m_modeCombo.AddString(L"White only");
	m_modeCombo.AddString(L"Black or white");
	m_modeCombo.SetCurSel(2);

	m_blackEdit.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER, CRect(), this, ID_BLACK_TH);
	m_whiteEdit.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER, CRect(), this, ID_WHITE_TH);
	m_noiseEdit.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER, CRect(), this, ID_NOISE);
	m_maxBlobEdit.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER, CRect(), this, ID_MAX_BLOB);
	m_repeatEdit.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER, CRect(), this, ID_REPEAT);
	m_blackEdit.SetWindowText(L"10");
	m_whiteEdit.SetWindowText(L"130");
	m_noiseEdit.SetWindowText(L"1");
	m_maxBlobEdit.SetWindowText(L"500");
	m_repeatEdit.SetWindowText(L"5");
	const wchar_t* parameterNames[] = { L"Detection mode", L"Black threshold", L"White threshold", L"Noise pixels >", L"Max returned blobs", L"Benchmark repeats" };
	for (size_t i = 0; i < m_parameterLabels.size(); ++i)
		AddLabel(parameterNames[i], m_parameterLabels[i]);

	AddLabel(L"No image loaded.", m_imageText);
	AddLabel(L"ROI: -", m_roiText);
	AddLabel(L"Batch timing: -", m_timeText);
	AddLabel(L"Returned candidates: -", m_countText);
	m_view.Create(this, ID_IMAGE_VIEW);

	m_resultList.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL,
		CRect(), this, ID_RESULT_LIST);
	m_resultList.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
	m_resultList.InsertColumn(0, L"#", LVCFMT_LEFT, 58);
	m_resultList.InsertColumn(1, L"Bounding box", LVCFMT_LEFT, 160);
	m_resultList.InsertColumn(2, L"Pixels", LVCFMT_RIGHT, 58);
	m_resultList.InsertColumn(3, L"Min/Max", LVCFMT_LEFT, 70);
	m_resultList.InsertColumn(4, L"Compact", LVCFMT_RIGHT, 66);

	m_timingList.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL,
		CRect(), this, ID_ROI_TIMING_LIST);
	m_timingList.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
	m_timingList.InsertColumn(0, L"ROI", LVCFMT_LEFT, 48);
	m_timingList.InsertColumn(1, L"Worker Total", LVCFMT_RIGHT, 92);
	m_timingList.InsertColumn(2, L"Buffer", LVCFMT_RIGHT, 74);
	m_timingList.InsertColumn(3, L"Column Corr.", LVCFMT_RIGHT, 94);
	m_timingList.InsertColumn(4, L"Thresh/Runs", LVCFMT_RIGHT, 100);
	m_timingList.InsertColumn(5, L"Label Merge", LVCFMT_RIGHT, 94);
	m_timingList.InsertColumn(6, L"Statistics", LVCFMT_RIGHT, 84);
	m_timingList.InsertColumn(7, L"Features", LVCFMT_RIGHT, 82);
	m_timingList.InsertColumn(8, L"Out Merge", LVCFMT_RIGHT, 82);
	m_timingList.InsertColumn(9, L"Returned", LVCFMT_RIGHT, 70);

	PostMessage(WM_APP + 11);
	return 0;
}

void CMainFrame::AddLabel(const wchar_t* text, CStatic& label)
{
	label.Create(text, WS_CHILD | WS_VISIBLE | SS_LEFT, CRect(), this);
}

void CMainFrame::OnSize(UINT type, int cx, int cy)
{
	CFrameWnd::OnSize(type, cx, cy);
	if (!m_view.GetSafeHwnd())
		return;

	const int margin = 12;
	const int panelWidth = 308;
	const int buttonH = 28;
	const int inputH = 23;
	const int viewLeft = panelWidth + margin * 2;
	const int listHeight = 188;
	int y = margin;

	auto place = [&](CWnd& wnd, int x, int top, int width, int height)
	{
		wnd.SetWindowPos(nullptr, x, top, width, height, SWP_NOZORDER);
	};

	place(m_openButton, margin, y, panelWidth, buttonH); y += 40;
	place(m_imageText, margin, y, panelWidth, 42); y += 49;
	place(m_roiText, margin, y, panelWidth, 36); y += 44;
	place(m_sixRoiButton, margin, y, 151, buttonH);
	place(m_wholeButton, margin + 157, y, 151, buttonH); y += 34;
	place(m_zoomCheck, margin, y + 5, 136, inputH); y += 37;

	CWnd* inputs[] = { &m_modeCombo, &m_blackEdit, &m_whiteEdit, &m_noiseEdit, &m_maxBlobEdit, &m_repeatEdit };
	for (size_t i = 0; i < m_parameterLabels.size(); ++i)
	{
		place(m_parameterLabels[i], margin, y + 3, 168, inputH);
		place(*inputs[i], margin + 178, y, 130, i == 0 ? 120 : inputH);
		y += 29;
	}
	y += 4;

	place(m_connCheck, margin, y, 130, inputH);
	place(m_warmupCheck, margin + 135, y, 170, inputH); y += 34;
	place(m_runButton, margin, y, 148, buttonH);
	place(m_benchmarkButton, margin + 160, y, 148, buttonH); y += 42;
	place(m_timeText, margin, y, panelWidth, 43); y += 47;
	place(m_countText, margin, y, panelWidth, inputH); y += 29;
	place(m_resultList, margin, y, panelWidth, std::max(80, cy - y - margin));

	const int viewWidth = std::max(1, cx - viewLeft - margin);
	const int viewHeight = std::max(1, cy - listHeight - margin * 3);
	place(m_view, viewLeft, margin, viewWidth, viewHeight);
	place(m_timingList, viewLeft, margin * 2 + viewHeight, viewWidth, listHeight);
	Invalidate();
}

void CMainFrame::OnOpenImage()
{
	CFileDialog dialog(TRUE, L"jpg", nullptr, OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST,
		L"Image Files (*.jpg;*.jpeg;*.png;*.bmp;*.tif)|*.jpg;*.jpeg;*.png;*.bmp;*.tif|All Files (*.*)|*.*||", this);
	if (dialog.DoModal() == IDOK)
		LoadImageFile(dialog.GetPathName());
}

bool CMainFrame::LoadImageFile(const CString& path)
{
	CWaitCursor waiting;
	CString error;
	if (!m_image.Load(path, error))
	{
		AfxMessageBox(error, MB_ICONERROR);
		return false;
	}

	m_roi.SetRect(0, 0, m_image.Width(), m_image.Height());
	m_view.SetImage(&m_image);
	ApplySixFixedRois();
	ClearResults();
	UpdateImageDetails();
	return true;
}

CString CMainFrame::DefaultImagePath() const
{
	wchar_t modulePath[MAX_PATH]{};
	GetModuleFileName(nullptr, modulePath, MAX_PATH);
	CString base(modulePath);
	const int slash = base.ReverseFind(L'\\');
	if (slash >= 0)
		base = base.Left(slash + 1);

	const CString candidates[] =
	{
		CString(kDefaultImageName),
		base + L"..\\..\\" + kDefaultImageName,
		base + L"..\\..\\..\\" + kDefaultImageName
	};
	for (const auto& candidate : candidates)
	{
		if (GetFileAttributes(candidate) != INVALID_FILE_ATTRIBUTES)
			return candidate;
	}
	return CString();
}

LRESULT CMainFrame::OnLoadDefaultImage(WPARAM, LPARAM)
{
	const CString path = DefaultImagePath();
	if (!path.IsEmpty())
		LoadImageFile(path);
	return 0;
}

bool CMainFrame::ReadParameters(rle::AVX_DATA& data, int& repeatCount)
{
	if (!m_image.IsLoaded())
	{
		AfxMessageBox(L"Load an image first.", MB_ICONINFORMATION);
		return false;
	}
	if (!IsAvx2Available())
	{
		AfxMessageBox(L"This CPU/OS does not report AVX2 support. The supplied detector requires AVX2.", MB_ICONERROR);
		return false;
	}

	const int black = ReadEditInt(m_blackEdit);
	const int white = ReadEditInt(m_whiteEdit);
	const int noise = ReadEditInt(m_noiseEdit);
	const int maxBlob = ReadEditInt(m_maxBlobEdit);
	repeatCount = ReadEditInt(m_repeatEdit);
	if (black < 0 || black > 255 || white < 0 || white > 255 || black > white ||
		noise < 0 || maxBlob < 1 || repeatCount < 1 || repeatCount > 1000)
	{
		AfxMessageBox(L"Check parameters: thresholds must be 0..255 (black <= white), noise >= 0, max blobs >= 1, repeats 1..1000.", MB_ICONWARNING);
		return false;
	}

	data.rtInspect = m_roi;
	data.nTHB = black;
	data.nTHW = white;
	data.nLane = 0;
	data.isWhiteImage = false;
	data.isLeft = true;
	switch (m_modeCombo.GetCurSel())
	{
	case 0:
		data.mode = rle::EGrayBinMode::BlackOnly;
		data.eSurfaceType = rle::SURFACE_TYPE::SURFACE_TYPE_NULL;
		break;
	case 2:
		data.mode = rle::EGrayBinMode::BlackOrWhite;
		data.eSurfaceType = rle::SURFACE_TYPE::SURFACE_TYPE_COAT;
		break;
	default:
		data.mode = rle::EGrayBinMode::WhiteOnly;
		data.eSurfaceType = rle::SURFACE_TYPE::SURFACE_TYPE_COAT;
		break;
	}

	m_detector.SetNoiseFilterPixelCnt(noise);
	m_detector.SetMaxDefectCnt(maxBlob);
	m_detector.SetScale(0.042, 0.028);
	return true;
}

bool CMainFrame::IsAvx2Available() const
{
	int regs[4]{};
	__cpuid(regs, 1);
	const bool osxsave = (regs[2] & (1 << 27)) != 0;
	const bool avx = (regs[2] & (1 << 28)) != 0;
	if (!osxsave || !avx || (_xgetbv(0) & 0x6) != 0x6)
		return false;
	__cpuidex(regs, 7, 0);
	return (regs[1] & (1 << 5)) != 0;
}

std::vector<rle::AVX_DATA> CMainFrame::BuildJobs(const rle::AVX_DATA& templateData) const
{
	std::vector<rle::AVX_DATA> jobs;
	jobs.reserve(m_rois.size());
	for (size_t i = 0; i < m_rois.size(); ++i)
	{
		rle::AVX_DATA data = templateData;
		data.rtInspect = m_rois[i];
		data.nLane = static_cast<int>(i);
		jobs.push_back(data);
	}
	return jobs;
}

double CMainFrame::ExecuteOne(const std::vector<rle::AVX_DATA>& jobs, std::vector<rle::RleBlobStats>& output,
	std::vector<bench::RoiTiming>& roiTiming)
{
	output.clear();
	const auto started = std::chrono::high_resolution_clock::now();
	RunMeasuredBatch(m_detector, m_image.Pixels(), m_image.Pixels(), m_image.Width(), m_image.Height(),
		jobs, output, m_connCheck.GetCheck() == BST_CHECKED, roiTiming);
	const auto ended = std::chrono::high_resolution_clock::now();
	return std::chrono::duration<double, std::milli>(ended - started).count();
}

void CMainFrame::OnRunOnce()
{
	rle::AVX_DATA data{};
	int ignoredRepeats = 0;
	if (!ReadParameters(data, ignoredRepeats))
		return;
	const std::vector<rle::AVX_DATA> jobs = BuildJobs(data);

	CWaitCursor waiting;
	try
	{
		std::vector<bench::RoiTiming> roiTiming;
		std::vector<double> timings{ ExecuteOne(jobs, m_blobs, roiTiming) };
		PresentRun(timings, m_blobs, { roiTiming });
	}
	catch (const std::exception& exception)
	{
		(void)exception;
		AfxMessageBox(L"Detector execution failed.", MB_ICONERROR);
	}
}

void CMainFrame::OnBenchmark()
{
	rle::AVX_DATA data{};
	int repeatCount = 0;
	if (!ReadParameters(data, repeatCount))
		return;
	const std::vector<rle::AVX_DATA> jobs = BuildJobs(data);

	CWaitCursor waiting;
	try
	{
		std::vector<rle::RleBlobStats> output;
		std::vector<bench::RoiTiming> roiTiming;
		if (m_warmupCheck.GetCheck() == BST_CHECKED)
			ExecuteOne(jobs, output, roiTiming);

		std::vector<double> timings;
		std::vector<std::vector<bench::RoiTiming>> roiTimings;
		timings.reserve(static_cast<size_t>(repeatCount));
		roiTimings.reserve(static_cast<size_t>(repeatCount));
		for (int i = 0; i < repeatCount; ++i)
		{
			timings.push_back(ExecuteOne(jobs, output, roiTiming));
			roiTimings.push_back(roiTiming);
		}
		m_blobs = std::move(output);
		PresentRun(timings, m_blobs, roiTimings);
	}
	catch (const std::exception& exception)
	{
		(void)exception;
		AfxMessageBox(L"Benchmark failed.", MB_ICONERROR);
	}
}

void CMainFrame::PresentRun(const std::vector<double>& timings, const std::vector<rle::RleBlobStats>& output,
	const std::vector<std::vector<bench::RoiTiming>>& roiTimings)
{
	const auto minmax = std::minmax_element(timings.begin(), timings.end());
	const double total = std::accumulate(timings.begin(), timings.end(), 0.0);
	const double average = total / timings.size();
	CString timeText;
	timeText.Format(L"Batch timing: avg %.3f ms   min %.3f ms   max %.3f ms  (%zu runs)",
		average, *minmax.first, *minmax.second, timings.size());
	m_timeText.SetWindowText(timeText);

	CString countText;
	countText.Format(L"Returned candidates: %zu from %zu ROI job(s)", output.size(), m_rois.size());
	m_countText.SetWindowText(countText);

	m_timingList.DeleteAllItems();
	if (!roiTimings.empty())
	{
		const size_t roiCount = roiTimings.back().size();
		for (size_t i = 0; i < roiCount; ++i)
		{
			std::array<double, 8> sums{};
			for (const auto& sample : roiTimings)
			{
				if (i < sample.size())
				{
					sums[0] += sample[i].dTimeMs;
					sums[1] += sample[i].dBufferMs;
					sums[2] += sample[i].dColumnCorrectionMs;
					sums[3] += sample[i].dThresholdRunsMs;
					sums[4] += sample[i].dLabelMergeMs;
					sums[5] += sample[i].dStatisticsMs;
					sums[6] += sample[i].dFeatureMs;
					sums[7] += sample[i].dOutputMergeMs;
				}
			}
			const double sampleCount = static_cast<double>(roiTimings.size());
			const auto& latest = roiTimings.back()[i];
			CString text;
			text.Format(L"L%d", latest.nLane);
			const int row = m_timingList.InsertItem(static_cast<int>(i), text);
			for (int column = 0; column < static_cast<int>(sums.size()); ++column)
			{
				text.Format(L"%.3f", sums[static_cast<size_t>(column)] / sampleCount);
				m_timingList.SetItemText(row, column + 1, text);
			}
			text.Format(L"%zu", latest.nReturnedCount);
			m_timingList.SetItemText(row, 9, text);
		}
	}

	m_resultList.DeleteAllItems();
	for (size_t i = 0; i < output.size(); ++i)
	{
		const auto& blob = output[i];
		CString text;
		text.Format(L"%d-%zu", blob.nLane, i + 1);
		const int row = m_resultList.InsertItem(static_cast<int>(i), text);
		text.Format(L"(%ld,%ld)-(%ld,%ld)", blob.rtPos.left, blob.rtPos.top, blob.rtPos.right, blob.rtPos.bottom);
		m_resultList.SetItemText(row, 1, text);
		text.Format(L"%.0f", blob.dAreaObj);
		m_resultList.SetItemText(row, 2, text);
		text.Format(L"%d / %d", blob.nMinBright, blob.nMaxBright);
		m_resultList.SetItemText(row, 3, text);
		text.Format(L"%.4f", blob.dCompactness);
		m_resultList.SetItemText(row, 4, text);
	}
	m_view.SetBlobs(&m_blobs);
}

void CMainFrame::OnWholeImage()
{
	if (!m_image.IsLoaded())
		return;
	m_roi.SetRect(0, 0, m_image.Width(), m_image.Height());
	m_rois.assign(1, m_roi);
	m_view.SetRoi(m_roi);
	UpdateRoiDetails();
	ClearResults();
}

void CMainFrame::OnSixFixedRois()
{
	if (!m_image.IsLoaded())
		return;
	ApplySixFixedRois();
	ClearResults();
}

void CMainFrame::OnZoomRoi()
{
	m_view.SetZoomRoi(m_zoomCheck.GetCheck() == BST_CHECKED);
}

LRESULT CMainFrame::OnRoiChanged(WPARAM, LPARAM)
{
	m_roi = m_view.Roi();
	m_rois.assign(1, m_roi);
	UpdateRoiDetails();
	ClearResults();
	return 0;
}

void CMainFrame::UpdateImageDetails()
{
	CString text;
	text.Format(L"Image: %d x %d pixels\r\nGray buffer: %.1f MB",
		m_image.Width(), m_image.Height(),
		static_cast<double>(m_image.Width()) * m_image.Height() / (1024.0 * 1024.0));
	m_imageText.SetWindowText(text);
}

void CMainFrame::UpdateRoiDetails()
{
	CString text;
	if (m_rois.size() == 6)
	{
		if (m_usingConfiguredFixedRois)
			text.Format(L"ROI mode: 6 fixed coordinates (L0-L5)\r\nEdit: kFixedRoiPreset in MainFrame.cpp");
		else
			text.Format(L"ROI mode: 6 provisional equal splits\r\nEdit: kFixedRoiPreset in MainFrame.cpp");
	}
	else
	{
		text.Format(L"ROI: (%ld, %ld) - (%ld, %ld)\r\nSize: %ld x %ld",
			m_roi.left, m_roi.top, m_roi.right, m_roi.bottom, m_roi.Width(), m_roi.Height());
	}
	m_roiText.SetWindowText(text);
}

void CMainFrame::ApplySixFixedRois()
{
	m_usingConfiguredFixedRois = TryApplyConfiguredFixedRois();
	if (!m_usingConfiguredFixedRois)
	{
		m_rois.clear();
		m_rois.reserve(6);
		for (int lane = 0; lane < 6; ++lane)
		{
			const int left = static_cast<int>((static_cast<int64_t>(m_image.Width()) * lane) / 6);
			const int right = static_cast<int>((static_cast<int64_t>(m_image.Width()) * (lane + 1)) / 6);
			m_rois.emplace_back(left, 0, right, m_image.Height());
		}
	}
	m_roi = m_rois.front();
	m_view.SetRois(m_rois);
	m_zoomCheck.SetCheck(BST_UNCHECKED);
	m_view.SetZoomRoi(false);
	UpdateRoiDetails();
}

bool CMainFrame::TryApplyConfiguredFixedRois()
{
	m_rois.clear();
	m_rois.reserve(6);
	for (const auto& definition : kFixedRoiPreset)
	{
		const CRect roi(definition.left, definition.top, definition.right, definition.bottom);
		if (roi.Width() <= 0 || roi.Height() <= 0 ||
			roi.left < 0 || roi.top < 0 ||
			roi.right > m_image.Width() || roi.bottom > m_image.Height())
		{
			m_rois.clear();
			return false;
		}
		m_rois.push_back(roi);
	}
	return true;
}

void CMainFrame::ClearResults()
{
	m_blobs.clear();
	m_timingList.DeleteAllItems();
	m_resultList.DeleteAllItems();
	m_timeText.SetWindowText(L"Batch timing: -");
	m_countText.SetWindowText(L"Returned candidates: -");
	m_view.SetBlobs(nullptr);
}
