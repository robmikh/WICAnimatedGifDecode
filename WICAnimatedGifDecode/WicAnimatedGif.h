// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved


#pragma once

const float DEFAULT_DPI = 96.f;   // Default DPI that maps image resolution directly to screen resoltuion

/******************************************************************
*                                                                 *
*  DemoApp                                                        *
*                                                                 *
******************************************************************/

class DemoApp
{
public:

	DemoApp();
	~DemoApp();

	HRESULT Initialize(HINSTANCE hInstance);

private:

	enum DISPOSAL_METHODS
	{
		DM_UNDEFINED = 0,
		DM_NONE = 1,
		DM_BACKGROUND = 2,
		DM_PREVIOUS = 3
	};

	HRESULT CreateDeviceResources();
	HRESULT RecoverDeviceResources();

	HRESULT OnResize(UINT uWidth, UINT uHeight);
	HRESULT OnRender();

	bool    GetFileOpen(WCHAR* pszFileName, DWORD cchFileName);
	HRESULT SelectAndDisplayGif();

	HRESULT GetRawFrame(UINT uFrameIndex);
	HRESULT GetGlobalMetadata();
	HRESULT GetBackgroundColor(IWICMetadataQueryReader* pMetadataQueryReader);

	HRESULT ComposeNextFrame();
	HRESULT DisposeCurrentFrame();
	HRESULT OverlayNextFrame();

	HRESULT SaveComposedFrame();
	HRESULT RestoreSavedFrame();
	HRESULT ClearCurrentFrameArea();

	bool IsLastFrame()
	{
		return (m_uNextFrameIndex == 0);
	}

	bool EndOfAnimation()
	{
		return m_fHasLoop && IsLastFrame() && m_uLoopNumber == m_uTotalLoopCount + 1;
	}

	HRESULT CalculateDrawRectangle(D2D1_RECT_F& drawRect);

	LRESULT WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
	static LRESULT CALLBACK s_WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

private:

	HWND                        m_hWnd;

	winrt::com_ptr<ID2D1Factory> m_d2dFactory;
	winrt::com_ptr<ID2D1HwndRenderTarget> m_hwndRT;
	winrt::com_ptr<ID2D1BitmapRenderTarget> m_frameComposeRT;
	winrt::com_ptr<ID2D1Bitmap> m_rawFrame;
	winrt::com_ptr<ID2D1Bitmap> m_savedFrame;          // The temporary bitmap used for disposal 3 method
	D2D1_COLOR_F                m_backgroundColor;

	winrt::com_ptr<IWICImagingFactory> m_wicFactory;
	winrt::com_ptr<IWICBitmapDecoder> m_decoder;

	unsigned int    m_uNextFrameIndex;
	unsigned int    m_uTotalLoopCount;  // The number of loops for which the animation will be played
	unsigned int    m_uLoopNumber;      // The current animation loop number (e.g. 1 when the animation is first played)
	bool            m_fHasLoop;         // Whether the gif has a loop
	unsigned int    m_cFrames;
	unsigned int    m_uFrameDisposal;
	unsigned int    m_uFrameDelay;
	unsigned int    m_cxGifImage;
	unsigned int    m_cyGifImage;
	unsigned int    m_cxGifImagePixel;  // Width of the displayed image in pixel calculated using pixel aspect ratio
	unsigned int    m_cyGifImagePixel;  // Height of the displayed image in pixel calculated using pixel aspect ratio
	D2D1_RECT_F     m_framePosition;
};