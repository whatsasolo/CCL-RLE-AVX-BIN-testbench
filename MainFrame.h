#pragma once

#include "CCL_RLE.h"
#include "MeasuredRunner.h"
#include "ImageBuffer.h"
#include "ImageView.h"

class CMainFrame : public CFrameWnd
{
public:
	CMainFrame();

protected:
	afx_msg int OnCreate(LPCREATESTRUCT createStruct);
	afx_msg void OnSize(UINT type, int cx, int cy);
	afx_msg void OnOpenImage();
	afx_msg void OnRunOnce();
	afx_msg void OnBenchmark();
	afx_msg void OnWholeImage();
	afx_msg void OnSixFixedRois();
	afx_msg void OnZoomRoi();
	afx_msg LRESULT OnRoiChanged(WPARAM, LPARAM);
	afx_msg LRESULT OnLoadDefaultImage(WPARAM, LPARAM);
	DECLARE_MESSAGE_MAP()

private:
	bool LoadImageFile(const CString& path);
	bool ReadParameters(rle::AVX_DATA& data, int& repeatCount);
	bool IsAvx2Available() const;
	double ExecuteOne(const std::vector<rle::AVX_DATA>& jobs, std::vector<rle::RleBlobStats>& output, std::vector<bench::RoiTiming>& roiTiming);
	std::vector<rle::AVX_DATA> BuildJobs(const rle::AVX_DATA& templateData) const;
	void ApplySixFixedRois();
	bool TryApplyConfiguredFixedRois();
	void PresentRun(const std::vector<double>& timings, const std::vector<rle::RleBlobStats>& output, const std::vector<std::vector<bench::RoiTiming>>& roiTimings);
	void UpdateImageDetails();
	void UpdateRoiDetails();
	void ClearResults();
	CString DefaultImagePath() const;
	void AddLabel(const wchar_t* text, CStatic& label);

	CCL_RLE m_detector;
	CGrayImageBuffer m_image;
	CRect m_roi;
	std::vector<CRect> m_rois;
	bool m_usingConfiguredFixedRois = false;
	std::vector<rle::RleBlobStats> m_blobs;

	CImageView m_view;
	CButton m_openButton;
	CButton m_runButton;
	CButton m_benchmarkButton;
	CButton m_wholeButton;
	CButton m_sixRoiButton;
	CButton m_zoomCheck;
	CButton m_connCheck;
	CButton m_warmupCheck;
	CComboBox m_modeCombo;
	CEdit m_blackEdit;
	CEdit m_whiteEdit;
	CEdit m_noiseEdit;
	CEdit m_maxBlobEdit;
	CEdit m_repeatEdit;
	std::array<CStatic, 6> m_parameterLabels;
	CStatic m_imageText;
	CStatic m_roiText;
	CStatic m_timeText;
	CStatic m_countText;
	CListCtrl m_timingList;
	CListCtrl m_resultList;
};
