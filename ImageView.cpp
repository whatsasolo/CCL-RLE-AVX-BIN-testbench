#include "stdafx.h"
#include "ImageView.h"

BEGIN_MESSAGE_MAP(CImageView, CWnd)
	ON_WM_PAINT()
	ON_WM_LBUTTONDOWN()
	ON_WM_MOUSEMOVE()
	ON_WM_LBUTTONUP()
END_MESSAGE_MAP()

bool CImageView::Create(CWnd* parent, UINT id)
{
	return CWnd::CreateEx(WS_EX_CLIENTEDGE, AfxRegisterWndClass(CS_DBLCLKS, ::LoadCursor(nullptr, IDC_CROSS)),
		L"Image View", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, CRect(0, 0, 0, 0), parent, id) != FALSE;
}

void CImageView::SetImage(const CGrayImageBuffer* image)
{
	m_image = image;
	m_blobs = nullptr;
	if (m_image && m_image->IsLoaded())
	{
		m_roi.SetRect(0, 0, m_image->Width(), m_image->Height());
		m_rois.assign(1, m_roi);
	}
	else
	{
		m_roi.SetRectEmpty();
		m_rois.clear();
	}
	Invalidate();
}

void CImageView::SetRoi(const CRect& roi)
{
	m_roi = roi;
	m_rois.assign(1, roi);
	m_dragRoi.SetRectEmpty();
	Invalidate();
}

void CImageView::SetRois(const std::vector<CRect>& rois)
{
	m_rois = rois;
	m_roi = rois.empty() ? CRect() : rois.front();
	m_dragRoi.SetRectEmpty();
	Invalidate();
}

void CImageView::SetBlobs(const std::vector<rle::RleBlobStats>* blobs)
{
	m_blobs = blobs;
	Invalidate();
}

void CImageView::SetZoomRoi(bool zoomRoi)
{
	m_zoomRoi = zoomRoi;
	Invalidate();
}

CRect CImageView::VisibleSource() const
{
	if (!m_image || !m_image->IsLoaded())
		return CRect();
	if (m_zoomRoi && m_rois.size() == 1 && !m_roi.IsRectEmpty())
		return m_roi;
	return CRect(0, 0, m_image->Width(), m_image->Height());
}

CRect CImageView::DestinationRect(const CRect& source) const
{
	CRect client;
	GetClientRect(&client);
	client.DeflateRect(8, 8);
	if (source.IsRectEmpty() || client.IsRectEmpty())
		return CRect();

	const double scale = std::min(static_cast<double>(client.Width()) / source.Width(),
		static_cast<double>(client.Height()) / source.Height());
	const int width = std::max(1, static_cast<int>(source.Width() * scale));
	const int height = std::max(1, static_cast<int>(source.Height() * scale));
	const int left = client.left + (client.Width() - width) / 2;
	const int top = client.top + (client.Height() - height) / 2;
	return CRect(left, top, left + width, top + height);
}

CPoint CImageView::ImagePoint(CPoint point, const CRect& source, const CRect& destination) const
{
	point.x = std::max(destination.left, std::min(point.x, destination.right - 1));
	point.y = std::max(destination.top, std::min(point.y, destination.bottom - 1));
	const double fx = static_cast<double>(point.x - destination.left) / std::max(1, destination.Width());
	const double fy = static_cast<double>(point.y - destination.top) / std::max(1, destination.Height());
	return CPoint(source.left + static_cast<int>(fx * source.Width()),
		source.top + static_cast<int>(fy * source.Height()));
}

CPoint CImageView::ClientPoint(CPoint point, const CRect& source, const CRect& destination) const
{
	const double fx = static_cast<double>(point.x - source.left) / std::max(1, source.Width());
	const double fy = static_cast<double>(point.y - source.top) / std::max(1, source.Height());
	return CPoint(destination.left + static_cast<int>(fx * destination.Width()),
		destination.top + static_cast<int>(fy * destination.Height()));
}

void CImageView::OnPaint()
{
	CPaintDC dc(this);
	CRect client;
	GetClientRect(&client);
	dc.FillSolidRect(client, RGB(33, 33, 33));

	const CRect source = VisibleSource();
	const CRect destination = DestinationRect(source);
	if (!m_image || !m_image->IsLoaded() || source.IsRectEmpty())
	{
		dc.SetTextColor(RGB(210, 210, 210));
		dc.SetBkMode(TRANSPARENT);
		dc.DrawText(L"Open a grayscale image to begin.", client, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		return;
	}

	m_image->Draw(dc, destination, source);

	CPen roiPen(PS_SOLID, 1, RGB(0, 190, 255));
	CPen blobPen(PS_SOLID, 2, RGB(255, 60, 40));
	CPen dragPen(PS_DOT, 1, RGB(255, 230, 0));
	CBrush* oldBrush = static_cast<CBrush*>(dc.SelectStockObject(NULL_BRUSH));

	if (!m_zoomRoi && !m_rois.empty())
	{
		CPen* oldPen = dc.SelectObject(&roiPen);
		dc.SetTextColor(RGB(0, 220, 255));
		dc.SetBkMode(TRANSPARENT);
		for (size_t i = 0; i < m_rois.size(); ++i)
		{
			const CPoint tl = ClientPoint(m_rois[i].TopLeft(), source, destination);
			const CPoint br = ClientPoint(m_rois[i].BottomRight(), source, destination);
			dc.Rectangle(CRect(tl, br));
			if (m_rois.size() > 1)
			{
				CString name;
				name.Format(L"ROI %zu", i);
				dc.TextOut(tl.x + 3, tl.y + 3, name);
			}
		}
		dc.SelectObject(oldPen);
	}

	if (m_blobs)
	{
		CPen* oldPen = dc.SelectObject(&blobPen);
		for (const auto& blob : *m_blobs)
		{
			CRect clipped;
			if (!clipped.IntersectRect(blob.rtPos, source))
				continue;
			const CPoint tl = ClientPoint(clipped.TopLeft(), source, destination);
			const CPoint br = ClientPoint(clipped.BottomRight(), source, destination);
			dc.Rectangle(CRect(tl, br));
		}
		dc.SelectObject(oldPen);
	}

	if (m_dragging && !m_dragRoi.IsRectEmpty())
	{
		CPen* oldPen = dc.SelectObject(&dragPen);
		const CPoint tl = ClientPoint(m_dragRoi.TopLeft(), source, destination);
		const CPoint br = ClientPoint(m_dragRoi.BottomRight(), source, destination);
		dc.Rectangle(CRect(tl, br));
		dc.SelectObject(oldPen);
	}

	dc.SelectObject(oldBrush);
}

void CImageView::OnLButtonDown(UINT flags, CPoint point)
{
	const CRect source = VisibleSource();
	const CRect destination = DestinationRect(source);
	if (m_image && m_image->IsLoaded() && destination.PtInRect(point))
	{
		m_dragOrigin = ImagePoint(point, source, destination);
		m_dragRoi.SetRect(m_dragOrigin.x, m_dragOrigin.y, m_dragOrigin.x + 1, m_dragOrigin.y + 1);
		m_dragging = true;
		SetCapture();
		Invalidate();
	}
	CWnd::OnLButtonDown(flags, point);
}

void CImageView::OnMouseMove(UINT flags, CPoint point)
{
	if (m_dragging)
	{
		const CRect source = VisibleSource();
		const CRect destination = DestinationRect(source);
		const CPoint current = ImagePoint(point, source, destination);
		m_dragRoi.SetRect(std::min(m_dragOrigin.x, current.x), std::min(m_dragOrigin.y, current.y),
			std::max(m_dragOrigin.x, current.x) + 1, std::max(m_dragOrigin.y, current.y) + 1);
		Invalidate();
	}
	CWnd::OnMouseMove(flags, point);
}

void CImageView::OnLButtonUp(UINT flags, CPoint point)
{
	if (m_dragging)
	{
		ReleaseCapture();
		m_dragging = false;
		if (m_dragRoi.Width() > 1 && m_dragRoi.Height() > 1)
		{
			m_roi = m_dragRoi;
			m_rois.assign(1, m_roi);
			GetParent()->SendMessage(WM_APP_ROI_CHANGED);
		}
		Invalidate();
	}
	CWnd::OnLButtonUp(flags, point);
}
