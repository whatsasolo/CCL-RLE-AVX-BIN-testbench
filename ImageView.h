#pragma once

#include "ImageBuffer.h"
#include "CCL_RLE.h"

constexpr UINT WM_APP_ROI_CHANGED = WM_APP + 10;

class CImageView : public CWnd
{
public:
	bool Create(CWnd* parent, UINT id);
	void SetImage(const CGrayImageBuffer* image);
	void SetRoi(const CRect& roi);
	void SetRois(const std::vector<CRect>& rois);
	void SetBlobs(const std::vector<rle::RleBlobStats>* blobs);
	void SetZoomRoi(bool zoomRoi);
	const CRect& Roi() const { return m_roi; }

private:
	CRect VisibleSource() const;
	CRect DestinationRect(const CRect& source) const;
	CPoint ImagePoint(CPoint clientPoint, const CRect& source, const CRect& destination) const;
	CPoint ClientPoint(CPoint imagePoint, const CRect& source, const CRect& destination) const;

	afx_msg void OnPaint();
	afx_msg void OnLButtonDown(UINT flags, CPoint point);
	afx_msg void OnMouseMove(UINT flags, CPoint point);
	afx_msg void OnLButtonUp(UINT flags, CPoint point);
	DECLARE_MESSAGE_MAP()

	const CGrayImageBuffer* m_image = nullptr;
	const std::vector<rle::RleBlobStats>* m_blobs = nullptr;
	CRect m_roi;
	std::vector<CRect> m_rois;
	CRect m_dragRoi;
	CPoint m_dragOrigin{};
	bool m_zoomRoi = false;
	bool m_dragging = false;
};
