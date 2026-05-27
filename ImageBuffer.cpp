#include "stdafx.h"
#include "ImageBuffer.h"

namespace
{
	CString HResultText(const wchar_t* stage, HRESULT hr)
	{
		CString text;
		text.Format(L"%s failed. HRESULT=0x%08X", stage, static_cast<unsigned int>(hr));
		return text;
	}
}

bool CGrayImageBuffer::Load(const CString& path, CString& error)
{
	Clear();

	CComPtr<IWICImagingFactory> factory;
	HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&factory));
	if (FAILED(hr))
	{
		error = HResultText(L"Creating WIC factory", hr);
		return false;
	}

	CComPtr<IWICBitmapDecoder> decoder;
	hr = factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
	if (FAILED(hr))
	{
		error = HResultText(L"Opening image", hr);
		return false;
	}

	CComPtr<IWICBitmapFrameDecode> frame;
	hr = decoder->GetFrame(0, &frame);
	if (FAILED(hr))
	{
		error = HResultText(L"Reading image frame", hr);
		return false;
	}

	UINT width = 0;
	UINT height = 0;
	hr = frame->GetSize(&width, &height);
	if (FAILED(hr) || width == 0 || height == 0 ||
		width > static_cast<UINT>((std::numeric_limits<int>::max)()) ||
		height > static_cast<UINT>((std::numeric_limits<int>::max)()))
	{
		if (FAILED(hr))
			error = HResultText(L"Reading image size", hr);
		else
			error = L"Image dimensions are invalid.";
		return false;
	}

	const uint64_t pixelCount = static_cast<uint64_t>(width) * height;
	if (pixelCount > static_cast<uint64_t>((std::numeric_limits<size_t>::max)()) ||
		pixelCount > static_cast<uint64_t>((std::numeric_limits<UINT>::max)()))
	{
		error = L"Image is too large for a WIC 8-bit pixel buffer.";
		return false;
	}

	CComPtr<IWICFormatConverter> converter;
	hr = factory->CreateFormatConverter(&converter);
	if (SUCCEEDED(hr))
	{
		hr = converter->Initialize(frame, GUID_WICPixelFormat8bppGray, WICBitmapDitherTypeNone,
			nullptr, 0.0, WICBitmapPaletteTypeCustom);
	}
	if (FAILED(hr))
	{
		error = HResultText(L"Converting image to grayscale", hr);
		return false;
	}

	try
	{
		m_pixels.resize(static_cast<size_t>(pixelCount));
	}
	catch (const std::bad_alloc&)
	{
		error = L"Not enough memory to allocate the grayscale image buffer.";
		return false;
	}

	hr = converter->CopyPixels(nullptr, width, static_cast<UINT>(m_pixels.size()), m_pixels.data());
	if (FAILED(hr))
	{
		Clear();
		error = HResultText(L"Copying grayscale pixels", hr);
		return false;
	}

	m_width = static_cast<int>(width);
	m_height = static_cast<int>(height);
	m_path = path;
	InitializeBitmapInfo();
	return true;
}

void CGrayImageBuffer::Clear()
{
	m_width = 0;
	m_height = 0;
	m_path.Empty();
	m_pixels.clear();
	ZeroMemory(&m_bitmapInfo, sizeof(m_bitmapInfo));
}

void CGrayImageBuffer::InitializeBitmapInfo()
{
	ZeroMemory(&m_bitmapInfo, sizeof(m_bitmapInfo));
	m_bitmapInfo.header.biSize = sizeof(BITMAPINFOHEADER);
	m_bitmapInfo.header.biWidth = m_width;
	m_bitmapInfo.header.biHeight = -m_height;
	m_bitmapInfo.header.biPlanes = 1;
	m_bitmapInfo.header.biBitCount = 8;
	m_bitmapInfo.header.biCompression = BI_RGB;
	m_bitmapInfo.header.biClrUsed = 256;
	for (int i = 0; i < 256; ++i)
	{
		m_bitmapInfo.colors[i].rgbRed = static_cast<BYTE>(i);
		m_bitmapInfo.colors[i].rgbGreen = static_cast<BYTE>(i);
		m_bitmapInfo.colors[i].rgbBlue = static_cast<BYTE>(i);
	}
}

void CGrayImageBuffer::Draw(CDC& dc, const CRect& destination, const CRect& source) const
{
	if (!IsLoaded() || destination.IsRectEmpty() || source.IsRectEmpty())
		return;

	::SetStretchBltMode(dc.GetSafeHdc(), HALFTONE);
	::SetBrushOrgEx(dc.GetSafeHdc(), 0, 0, nullptr);
	::StretchDIBits(dc.GetSafeHdc(),
		destination.left, destination.top, destination.Width(), destination.Height(),
		source.left, source.top, source.Width(), source.Height(),
		m_pixels.data(), reinterpret_cast<const BITMAPINFO*>(&m_bitmapInfo),
		DIB_RGB_COLORS, SRCCOPY);
}
