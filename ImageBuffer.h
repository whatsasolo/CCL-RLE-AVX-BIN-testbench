#pragma once

#include "stdafx.h"

class CGrayImageBuffer
{
public:
	bool Load(const CString& path, CString& error);
	void Clear();
	void Draw(CDC& dc, const CRect& destination, const CRect& source) const;

	bool IsLoaded() const { return !m_pixels.empty(); }
	int Width() const { return m_width; }
	int Height() const { return m_height; }
	const uint8_t* Pixels() const { return m_pixels.data(); }
	const CString& Path() const { return m_path; }

private:
	struct GrayBitmapInfo
	{
		BITMAPINFOHEADER header{};
		RGBQUAD colors[256]{};
	};

	void InitializeBitmapInfo();

	int m_width = 0;
	int m_height = 0;
	CString m_path;
	std::vector<uint8_t> m_pixels;
	GrayBitmapInfo m_bitmapInfo{};
};

