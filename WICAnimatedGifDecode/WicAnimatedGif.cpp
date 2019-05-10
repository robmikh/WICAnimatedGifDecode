// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved

#include <windows.h>
#include <Unknwn.h>
#include <inspectable.h>
#include <winrt/base.h>

#include <wincodec.h>
#include <commdlg.h>
#include <d2d1.h>

#include "WicAnimatedGif.h"

using namespace winrt;

#define EXTRA_GIF_DELAY 0

const UINT DELAY_TIMER_ID = 1;    // Global ID for the timer, only one timer is used

// Utility inline functions

inline LONG RectWidth(RECT rc)
{
	return rc.right - rc.left;
}

inline LONG RectHeight(RECT rc)
{
	return rc.bottom - rc.top;
}


//                           Gif Animation Overview
// In order to play a gif animation, raw frames (which are compressed frames 
// directly retrieved from the image file) and image metadata are loaded 
// and used to compose the frames that are actually displayed in the animation 
// loop (which we call composed frames in this sample).  Composed frames have 
// the same sizes as the global gif image size, while raw frames can have their own sizes.
//
// At the highest level, a gif animation contains a fixed or infinite number of animation
// loops, in which the animation will be displayed repeatedly frame by frame; once all 
// loops are displayed, the animation will stop and the last frame will be displayed 
// from that point.
//
// In each loop, first the entire composed frame will be initialized with the background 
// color retrieved from the image metadata.  The very first raw frame then will be loaded 
// and directly overlaid onto the previous composed frame (i.e. in this case, the frame 
// cleared with background color) to produce the first  composed frame, and this frame 
// will then be displayed for a period that equals its delay.  For any raw frame after 
// the first raw frame (if there are any), the composed frame will first be disposed based 
// on the disposal method associated with the previous raw frame. Then the next raw frame 
// will be loaded and overlaid onto the result (i.e. the composed frame after disposal).  
// These two steps (i.e. disposing the previous frame and overlaying the current frame) together 
// 'compose' the next frame to be displayed.  The composed frame then gets displayed.  
// This process continues until the last frame in a loop is reached.
//
// An exception is the zero delay intermediate frames, which are frames with 0 delay 
// associated with them.  These frames will be used to compose the next frame, but the 
// difference is that the composed frame will not be displayed unless it's the last frame 
// in the loop (i.e. we move immediately to composing the next composed frame).


/******************************************************************
*                                                                 *
*  WinMain                                                        *
*                                                                 *
*  Application entrypoint                                         *
*                                                                 *
******************************************************************/

int WINAPI wWinMain(
	_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR pszCmdLine,
	_In_ int nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(pszCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);

	HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);

	check_hresult(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));

	DemoApp app;
	app.Initialize(hInstance);

	// Main message loop:
	MSG msg;
	while (GetMessage(&msg, nullptr, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	CoUninitialize();
	return 0;
}

/******************************************************************
*                                                                 *
*  DemoApp::DemoApp constructor                                   *
*                                                                 *
*  Initializes member data                                        *
*                                                                 *
******************************************************************/

DemoApp::DemoApp()
{
}

/******************************************************************
*                                                                 *
*  DemoApp::~DemoApp destructor                                   *
*                                                                 *
*  Tears down resources                                           *
*                                                                 *
******************************************************************/

DemoApp::~DemoApp()
{
}

/******************************************************************
*                                                                 *
*  DemoApp::Initialize                                            *
*                                                                 *
*  Creates application window and device-independent resources    *
*                                                                 *
******************************************************************/

void DemoApp::Initialize(HINSTANCE hInstance)
{
	// Register window class
	WNDCLASSEX wcex = {};
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = DemoApp::s_WndProc;
	wcex.cbClsExtra = 0;
	wcex.cbWndExtra = sizeof(LONG_PTR);
	wcex.hInstance = hInstance;
	wcex.hIcon = nullptr;
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcex.hbrBackground = nullptr;
	//wcex.lpszMenuName = MAKEINTRESOURCE(IDR_WICANIMATEDGIF);
	wcex.lpszClassName = L"WICANIMATEDGIF";
	wcex.hIconSm = nullptr;

	WINRT_VERIFY(RegisterClassEx(&wcex));

	// Create D2D factory
	check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.put()));

	// Create WIC factory
	check_hresult(CoCreateInstance(
		CLSID_WICImagingFactory,
		nullptr,
		CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(m_wicFactory.put())));

	// Create window
	m_hWnd = CreateWindow(
		L"WICANIMATEDGIF",
		L"WIC Animated Gif Sample",
		WS_OVERLAPPEDWINDOW | WS_VISIBLE,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		nullptr,
		nullptr,
		hInstance,
		this);
	WINRT_VERIFY(m_hWnd);

	SelectAndDisplayGif();
}

/******************************************************************
*                                                                 *
*  DemoApp::CreateDeviceResources                                 *
*                                                                 *
*  Creates a D2D hwnd render target for displaying gif frames     *
*  to users and a D2D bitmap render for composing frames.         *
*                                                                 *
******************************************************************/

void DemoApp::CreateDeviceResources()
{
	RECT rcClient;
	if (!GetClientRect(m_hWnd, &rcClient))
	{
		throw_last_error();
	}

	if (m_hwndRT == nullptr)
	{
		auto renderTargetProperties = D2D1::RenderTargetProperties();

		// Set the DPI to be the default system DPI to allow direct mapping
		// between image pixels and desktop pixels in different system DPI settings
		renderTargetProperties.dpiX = DEFAULT_DPI;
		renderTargetProperties.dpiY = DEFAULT_DPI;

		auto hwndRenderTargetproperties
			= D2D1::HwndRenderTargetProperties(m_hWnd,
				D2D1::SizeU(RectWidth(rcClient), RectHeight(rcClient)));

		check_hresult(m_d2dFactory->CreateHwndRenderTarget(
			renderTargetProperties,
			hwndRenderTargetproperties,
			m_hwndRT.put()));
	}
	else
	{
		// We already have a hwnd render target, resize it to the window size
		D2D1_SIZE_U size;
		size.width = RectWidth(rcClient);
		size.height = RectHeight(rcClient);
		check_hresult(m_hwndRT->Resize(size));
	}

	// Create a bitmap render target used to compose frames. Bitmap render 
	// targets cannot be resized, so we always recreate it.
	m_frameComposeRT = nullptr;
	check_hresult(m_hwndRT->CreateCompatibleRenderTarget(
		D2D1::SizeF(
			static_cast<float>(m_cxGifImage),
			static_cast<float>(m_cyGifImage)),
		m_frameComposeRT.put()));
}

/******************************************************************
*                                                                 *
*  DemoApp::OnRender                                              *
*                                                                 *
*  Called whenever the application needs to display the client    *
*  window.                                                        *
*                                                                 *
*  Renders the pre-composed frame by drawing it onto the hwnd     *
*  render target.                                                 *
*                                                                 *
******************************************************************/

void DemoApp::OnRender()
{
	com_ptr<ID2D1Bitmap> frameToRender;

	// Check to see if the render targets are initialized
	if (m_hwndRT && m_frameComposeRT)
	{
		// Only render when the window is not occluded
		if (!(m_hwndRT->CheckWindowState() & D2D1_WINDOW_STATE_OCCLUDED))
		{
			D2D1_RECT_F drawRect;
			CalculateDrawRectangle(drawRect);

			// Get the bitmap to draw on the hwnd render target
			check_hresult(m_frameComposeRT->GetBitmap(frameToRender.put()));

			// Draw the bitmap onto the calculated rectangle
			m_hwndRT->BeginDraw();

			m_hwndRT->Clear(D2D1::ColorF(D2D1::ColorF::Black));
			m_hwndRT->DrawBitmap(frameToRender.get(), drawRect);

			check_hresult(m_hwndRT->EndDraw());
		}
	}
}

/******************************************************************
*                                                                 *
*  DemoApp::GetFileOpen                                           *
*                                                                 *
*  Creates an open file dialog box and returns the filename       *
*  of the file selected(if any).                                  *
*                                                                 *
******************************************************************/

bool DemoApp::GetFileOpen(WCHAR* pszFileName, DWORD cchFileName)
{
	pszFileName[0] = L'\0';

	OPENFILENAME ofn;
	RtlZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = m_hWnd;
	ofn.lpstrFilter = L"*Gif Files\0*.gif\0";
	ofn.lpstrFile = pszFileName;
	ofn.nMaxFile = cchFileName;
	ofn.lpstrTitle = L"Select an image to display...";
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

	// Display the Open dialog box.
	return (GetOpenFileName(&ofn) == TRUE);
}

/******************************************************************
*                                                                 *
*  DemoApp::OnResize                                              *
*                                                                 *
*  If the application receives a WM_SIZE message, this method     *
*  will resize the render target appropriately.                   *
*                                                                 *
******************************************************************/

void DemoApp::OnResize(UINT uWidth, UINT uHeight)
{
	if (m_hwndRT)
	{
		D2D1_SIZE_U size;
		size.width = uWidth;
		size.height = uHeight;
		check_hresult(m_hwndRT->Resize(size));
	}
}

/******************************************************************
*                                                                 *
*  DemoApp::s_WndProc                                             *
*                                                                 *
*  Static window message handler used to initialize the           *
*  application object and call the object's member WndProc        *
*                                                                 *
******************************************************************/

LRESULT CALLBACK DemoApp::s_WndProc(
	HWND hWnd,
	UINT uMsg,
	WPARAM wParam,
	LPARAM lParam)
{
	DemoApp* pThis = nullptr;
	LRESULT lRet = 0;

	if (uMsg == WM_NCCREATE)
	{
		auto pcs = reinterpret_cast<LPCREATESTRUCT>(lParam);
		pThis = reinterpret_cast<DemoApp*>(pcs->lpCreateParams);

		SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR> (pThis));
		lRet = DefWindowProc(hWnd, uMsg, wParam, lParam);
	}
	else
	{
		pThis = reinterpret_cast<DemoApp*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
		if (pThis)
		{
			lRet = pThis->WndProc(hWnd, uMsg, wParam, lParam);
		}
		else
		{
			lRet = DefWindowProc(hWnd, uMsg, wParam, lParam);
		}
	}

	return lRet;
}

/******************************************************************
*                                                                 *
*  DemoApp::WndProc                                               *
*                                                                 *
*  Window message handler                                         *
*                                                                 *
******************************************************************/

LRESULT DemoApp::WndProc(
	HWND hWnd,
	UINT uMsg,
	WPARAM wParam,
	LPARAM lParam)
{
	try
	{
		switch (uMsg)
		{
			/*
			case WM_COMMAND:
			{
				// Parse the menu selections
				switch (LOWORD(wParam))
				{
				case IDM_FILE:
					hr = SelectAndDisplayGif();
					if (FAILED(hr))
					{
						MessageBox(hWnd, L"Load gif file failed. Exiting application.", L"Error", MB_OK);
						PostQuitMessage(1);
						return 1;
					}
					break;

				case IDM_EXIT:
					PostMessage(hWnd, WM_CLOSE, 0, 0);
					break;
				}
			}
			break;
			*/
		case WM_SIZE:
		{
			UINT uWidth = LOWORD(lParam);
			UINT uHeight = HIWORD(lParam);
			OnResize(uWidth, uHeight);
		}
		break;

		case WM_PAINT:
		{
			OnRender();
			ValidateRect(hWnd, nullptr);
		}
		break;

		case WM_DISPLAYCHANGE:
		{
			InvalidateRect(hWnd, nullptr, FALSE);
		}
		break;

		case WM_DESTROY:
		{
			PostQuitMessage(0);
			return 0;
		}
		break;

		case WM_TIMER:
		{
			// Timer expired, display the next frame and set a new timer
			// if needed
			ComposeNextFrame();
			InvalidateRect(hWnd, nullptr, FALSE);
		}
		break;

		default:
			return DefWindowProc(hWnd, uMsg, wParam, lParam);
		}
	}
	catch (const hresult_error& error)
	{
		// In case of a device loss, recreate all the resources and start playing
		// gif from the beginning
		//
		// In case of other errors from resize, paint, and timer event, we will
		// try our best to continue displaying the animation
		if (error.code() == D2DERR_RECREATE_TARGET)
		{
			try
			{
				RecoverDeviceResources();
			}
			catch (...)
			{
				MessageBox(hWnd, L"Device loss recovery failed. Exiting application.", L"Error", MB_OK);
				PostQuitMessage(1);
			}
		}
		else
		{
			throw;
		}
	}

	return 0;
}

/******************************************************************
*                                                                 *
*  DemoApp::GetGlobalMetadata()                                   *
*                                                                 *
*  Retrieves global metadata which pertains to the entire image.  *
*                                                                 *
******************************************************************/

void DemoApp::GetGlobalMetadata()
{
	PROPVARIANT propValue;
	PropVariantInit(&propValue);
	com_ptr<IWICMetadataQueryReader> metadataQueryReader;

	// Get the frame count
	check_hresult(m_decoder->GetFrameCount(&m_cFrames));

	// Create a MetadataQueryReader from the decoder
	check_hresult(m_decoder->GetMetadataQueryReader(
		metadataQueryReader.put()));

	// Get background color
	try
	{
		GetBackgroundColor(metadataQueryReader.get());
	}
	catch (...)
	{
		// Default to transparent if failed to get the color
		m_backgroundColor = D2D1::ColorF(0, 0.f);
	}

	// Get width
	check_hresult(metadataQueryReader->GetMetadataByName(
		L"/logscrdesc/Width",
		&propValue));
	WINRT_VERIFY(propValue.vt == VT_UI2);
	m_cxGifImage = propValue.uiVal;
	PropVariantClear(&propValue);

	// Get height
	check_hresult(metadataQueryReader->GetMetadataByName(
		L"/logscrdesc/Height",
		&propValue));
	WINRT_VERIFY(propValue.vt == VT_UI2);
	m_cyGifImage = propValue.uiVal;
	PropVariantClear(&propValue);

	// Get pixel aspect ratio
	check_hresult(metadataQueryReader->GetMetadataByName(
		L"/logscrdesc/PixelAspectRatio",
		&propValue));
	WINRT_VERIFY(propValue.vt == VT_UI1);
	UINT uPixelAspRatio = propValue.bVal;

	if (uPixelAspRatio != 0)
	{
		// Need to calculate the ratio. The value in uPixelAspRatio 
		// allows specifying widest pixel 4:1 to the tallest pixel of 
		// 1:4 in increments of 1/64th
		float pixelAspRatio = (uPixelAspRatio + 15.f) / 64.f;

		// Calculate the image width and height in pixel based on the
		// pixel aspect ratio. Only shrink the image.
		if (pixelAspRatio > 1.f)
		{
			m_cxGifImagePixel = m_cxGifImage;
			m_cyGifImagePixel = static_cast<unsigned int>(m_cyGifImage / pixelAspRatio);
		}
		else
		{
			m_cxGifImagePixel = static_cast<unsigned int>(m_cxGifImage * pixelAspRatio);
			m_cyGifImagePixel = m_cyGifImage;
		}
	}
	else
	{
		// The value is 0, so its ratio is 1
		m_cxGifImagePixel = m_cxGifImage;
		m_cyGifImagePixel = m_cyGifImage;
	}
	PropVariantClear(&propValue);

	// Get looping information
	// First check to see if the application block in the Application Extension
	// contains "NETSCAPE2.0" and "ANIMEXTS1.0", which indicates the gif animation
	// has looping information associated with it.
	// 
	// If we fail to get the looping information, loop the animation infinitely.
	if (SUCCEEDED(metadataQueryReader->GetMetadataByName(
		L"/appext/application",
		&propValue)) &&
		propValue.vt == (VT_UI1 | VT_VECTOR) &&
		propValue.caub.cElems == 11 &&  // Length of the application block
		(!memcmp(propValue.caub.pElems, "NETSCAPE2.0", propValue.caub.cElems) ||
			!memcmp(propValue.caub.pElems, "ANIMEXTS1.0", propValue.caub.cElems)))
	{
		PropVariantClear(&propValue);

		check_hresult(metadataQueryReader->GetMetadataByName(L"/appext/data", &propValue));
		//  The data is in the following format:
		//  byte 0: extsize (must be > 1)
		//  byte 1: loopType (1 == animated gif)
		//  byte 2: loop count (least significant byte)
		//  byte 3: loop count (most significant byte)
		//  byte 4: set to zero
		if (propValue.vt == (VT_UI1 | VT_VECTOR) &&
			propValue.caub.cElems >= 4 &&
			propValue.caub.pElems[0] > 0 &&
			propValue.caub.pElems[1] == 1)
		{
			m_uTotalLoopCount = MAKEWORD(propValue.caub.pElems[2],
				propValue.caub.pElems[3]);

			// If the total loop count is not zero, we then have a loop count
			// If it is 0, then we repeat infinitely
			if (m_uTotalLoopCount != 0)
			{
				m_fHasLoop = true;
			}
		}
	}

	PropVariantClear(&propValue);
}

/******************************************************************
*                                                                 *
*  DemoApp::GetRawFrame()                                         *
*                                                                 *
*  Decodes the current raw frame, retrieves its timing            *
*  information, disposal method, and frame dimension for          *
*  rendering.  Raw frame is the frame read directly from the gif  *
*  file without composing.                                        *
*                                                                 *
******************************************************************/

void DemoApp::GetRawFrame(UINT uFrameIndex)
{
	com_ptr<IWICFormatConverter> converter;
	com_ptr<IWICBitmapFrameDecode> wicFrame;
	com_ptr<IWICMetadataQueryReader> frameMetadataQueryReader;

	PROPVARIANT propValue;
	PropVariantInit(&propValue);

	// Retrieve the current frame
	check_hresult(m_decoder->GetFrame(uFrameIndex, wicFrame.put()));
	// Format convert to 32bppPBGRA which D2D expects
	check_hresult(m_wicFactory->CreateFormatConverter(converter.put()));

	check_hresult(converter->Initialize(
		wicFrame.get(),
		GUID_WICPixelFormat32bppPBGRA,
		WICBitmapDitherTypeNone,
		nullptr,
		0.f,
		WICBitmapPaletteTypeCustom));

	// Create a D2DBitmap from IWICBitmapSource
	m_rawFrame = nullptr;
	check_hresult(m_hwndRT->CreateBitmapFromWicBitmap(
		converter.get(),
		nullptr,
		m_rawFrame.put()));

	// Get Metadata Query Reader from the frame
	check_hresult(wicFrame->GetMetadataQueryReader(frameMetadataQueryReader.put()));

	// Get the Metadata for the current frame
	check_hresult(frameMetadataQueryReader->GetMetadataByName(L"/imgdesc/Left", &propValue));
	WINRT_VERIFY(propValue.vt == VT_UI2);
	m_framePosition.left = static_cast<float>(propValue.uiVal);
	PropVariantClear(&propValue);

	check_hresult(frameMetadataQueryReader->GetMetadataByName(L"/imgdesc/Top", &propValue));
	WINRT_VERIFY(propValue.vt == VT_UI2);
	m_framePosition.top = static_cast<float>(propValue.uiVal);
	PropVariantClear(&propValue);

	check_hresult(frameMetadataQueryReader->GetMetadataByName(L"/imgdesc/Width", &propValue));
	WINRT_VERIFY(propValue.vt == VT_UI2);
	m_framePosition.right = static_cast<float>(propValue.uiVal)
		+ m_framePosition.left;
	PropVariantClear(&propValue);

	check_hresult(frameMetadataQueryReader->GetMetadataByName(L"/imgdesc/Height", &propValue));
	WINRT_VERIFY(propValue.vt == VT_UI2);
	m_framePosition.bottom = static_cast<float>(propValue.uiVal)
		+ m_framePosition.top;
	PropVariantClear(&propValue);

	// Get delay from the optional Graphic Control Extension
	if (SUCCEEDED(frameMetadataQueryReader->GetMetadataByName(
		L"/grctlext/Delay",
		&propValue)))
	{
		WINRT_VERIFY(propValue.vt == VT_UI2);
		// Convert the delay retrieved in 10 ms units to a delay in 1 ms units
		check_hresult(UIntMult(propValue.uiVal, 10, &m_uFrameDelay));
		PropVariantClear(&propValue);
	}
	else
	{
		// Failed to get delay from graphic control extension. Possibly a
		// single frame image (non-animated gif)
		m_uFrameDelay = 0;
	}

#if EXTRA_GIF_DELAY
	// Insert an artificial delay to ensure rendering for gif with very small
	// or 0 delay.  This delay number is picked to match with most browsers' 
	// gif display speed.
	//
	// This will defeat the purpose of using zero delay intermediate frames in 
	// order to preserve compatibility. If this is removed, the zero delay 
	// intermediate frames will not be visible.
	if (m_uFrameDelay < 90)
	{
		m_uFrameDelay = 90;
	}
#endif

	if (SUCCEEDED(frameMetadataQueryReader->GetMetadataByName(
		L"/grctlext/Disposal",
		&propValue)))
	{
		WINRT_VERIFY(propValue.vt == VT_UI1);
		m_uFrameDisposal = propValue.bVal;
	}
	else
	{
		// Failed to get the disposal method, use default. Possibly a 
		// non-animated gif.
		m_uFrameDisposal = DM_UNDEFINED;
	}

	PropVariantClear(&propValue);
}

/******************************************************************
*                                                                 *
*  DemoApp::GetBackgroundColor()                                  *
*                                                                 *
*  Reads and stores the background color for gif.                 *
*                                                                 *
******************************************************************/

void DemoApp::GetBackgroundColor(
	IWICMetadataQueryReader* pMetadataQueryReader)
{
	DWORD dwBGColor;
	BYTE backgroundIndex = 0;
	WICColor rgColors[256];
	UINT cColorsCopied = 0;
	PROPVARIANT propVariant;
	PropVariantInit(&propVariant);
	com_ptr<IWICPalette> wicPalette;

	// If we have a global palette, get the palette and background color
	check_hresult(pMetadataQueryReader->GetMetadataByName(
		L"/logscrdesc/GlobalColorTableFlag",
		&propVariant));
	WINRT_VERIFY(propVariant.vt == VT_BOOL && propVariant.boolVal);
	PropVariantClear(&propVariant);

	// Background color index
	check_hresult(pMetadataQueryReader->GetMetadataByName(
		L"/logscrdesc/BackgroundColorIndex",
		&propVariant));
	WINRT_VERIFY(propVariant.vt == VT_UI1);
	backgroundIndex = propVariant.bVal;
	PropVariantClear(&propVariant);

	// Get the color from the palette
	check_hresult(m_wicFactory->CreatePalette(wicPalette.put()));

	// Get the global palette
	check_hresult(m_decoder->CopyPalette(wicPalette.get()));

	check_hresult(wicPalette->GetColors(
		ARRAYSIZE(rgColors),
		rgColors,
		&cColorsCopied));

	// Check whether background color is outside range 
	WINRT_VERIFY(backgroundIndex < cColorsCopied);

	// Get the color in ARGB format
	dwBGColor = rgColors[backgroundIndex];

	// The background color is in ARGB format, and we want to 
	// extract the alpha value and convert it to float
	float alpha = (dwBGColor >> 24) / 255.f;
	m_backgroundColor = D2D1::ColorF(dwBGColor, alpha);
}

/******************************************************************
*                                                                 *
*  DemoApp::CalculateDrawRectangle()                              *
*                                                                 *
*  Calculates a specific rectangular area of the hwnd             *
*  render target to draw a bitmap containing the current          *
*  composed frame.                                                *
*                                                                 *
******************************************************************/

void DemoApp::CalculateDrawRectangle(D2D1_RECT_F& drawRect)
{
	RECT rcClient;

	// Top and left of the client rectangle are both 0
	if (!GetClientRect(m_hWnd, &rcClient))
	{
		throw_last_error();
	}

	// Calculate the area to display the image
	// Center the image if the client rectangle is larger
	drawRect.left = (static_cast<float>(rcClient.right) - m_cxGifImagePixel) / 2.f;
	drawRect.top = (static_cast<float>(rcClient.bottom) - m_cyGifImagePixel) / 2.f;
	drawRect.right = drawRect.left + m_cxGifImagePixel;
	drawRect.bottom = drawRect.top + m_cyGifImagePixel;

	// If the client area is resized to be smaller than the image size, scale
	// the image, and preserve the aspect ratio
	auto aspectRatio = static_cast<float>(m_cxGifImagePixel) /
		static_cast<float>(m_cyGifImagePixel);

	if (drawRect.left < 0)
	{
		auto newWidth = static_cast<float>(rcClient.right);
		float newHeight = newWidth / aspectRatio;
		drawRect.left = 0;
		drawRect.top = (static_cast<float>(rcClient.bottom) - newHeight) / 2.f;
		drawRect.right = newWidth;
		drawRect.bottom = drawRect.top + newHeight;
	}

	if (drawRect.top < 0)
	{
		auto newHeight = static_cast<float>(rcClient.bottom);
		float newWidth = newHeight * aspectRatio;
		drawRect.left = (static_cast<float>(rcClient.right) - newWidth) / 2.f;
		drawRect.top = 0;
		drawRect.right = drawRect.left + newWidth;
		drawRect.bottom = newHeight;
	}
}

/******************************************************************
*                                                                 *
*  DemoApp::RestoreSavedFrame()                                   *
*                                                                 *
*  Copys the saved frame to the frame in the bitmap render        *
*  target.                                                        *
*                                                                 *
******************************************************************/

void DemoApp::RestoreSavedFrame()
{
	com_ptr<ID2D1Bitmap> frameToCopyTo = nullptr;

	WINRT_VERIFY(m_savedFrame.get());

	check_hresult(m_frameComposeRT->GetBitmap(frameToCopyTo.put()));

	// Copy the whole bitmap
	check_hresult(frameToCopyTo->CopyFromBitmap(nullptr, m_savedFrame.get(), nullptr));
}

/******************************************************************
*                                                                 *
*  DemoApp::ClearCurrentFrameArea()                               *
*                                                                 *
*  Clears a rectangular area equal to the area overlaid by the    *
*  current raw frame in the bitmap render target with background  *
*  color.                                                         *
*                                                                 *
******************************************************************/

void DemoApp::ClearCurrentFrameArea()
{
	m_frameComposeRT->BeginDraw();

	// Clip the render target to the size of the raw frame
	m_frameComposeRT->PushAxisAlignedClip(
		&m_framePosition,
		D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

	m_frameComposeRT->Clear(m_backgroundColor);

	// Remove the clipping
	m_frameComposeRT->PopAxisAlignedClip();

	check_hresult(m_frameComposeRT->EndDraw());
}

/******************************************************************
*                                                                 *
*  DemoApp::DisposeCurrentFrame()                                 *
*                                                                 *
*  At the end of each delay, disposes the current frame           *
*  based on the disposal method specified.                        *
*                                                                 *
******************************************************************/

void DemoApp::DisposeCurrentFrame()
{
	switch (m_uFrameDisposal)
	{
	case DM_UNDEFINED:
	case DM_NONE:
		// We simply draw on the previous frames. Do nothing here.
		break;
	case DM_BACKGROUND:
		// Dispose background
		// Clear the area covered by the current raw frame with background color
		ClearCurrentFrameArea();
		break;
	case DM_PREVIOUS:
		// Dispose previous
		// We restore the previous composed frame first
		RestoreSavedFrame();
		break;
	default:
		// Invalid disposal method
		check_hresult(E_FAIL);
	}
}

/******************************************************************
*                                                                 *
*  DemoApp::OverlayNextFrame()                                    *
*                                                                 *
*  Loads and draws the next raw frame into the composed frame     *
*  render target. This is called after the current frame is       *
*  disposed.                                                      *
*                                                                 *
******************************************************************/

void DemoApp::OverlayNextFrame()
{
	// Get Frame information
	GetRawFrame(m_uNextFrameIndex);
	// For disposal 3 method, we would want to save a copy of the current
	// composed frame
	if (m_uFrameDisposal == DM_PREVIOUS)
	{
		SaveComposedFrame();
	}

	// Start producing the next bitmap
	m_frameComposeRT->BeginDraw();

	// If starting a new animation loop
	if (m_uNextFrameIndex == 0)
	{
		// Draw background and increase loop count
		m_frameComposeRT->Clear(m_backgroundColor);
		m_uLoopNumber++;
	}

	// Produce the next frame
	m_frameComposeRT->DrawBitmap(
		m_rawFrame.get(),
		m_framePosition);

	check_hresult(m_frameComposeRT->EndDraw());

	// To improve performance and avoid decoding/composing this frame in the 
	// following animation loops, the composed frame can be cached here in system 
	// or video memory.

	// Increase the frame index by 1
	m_uNextFrameIndex = (++m_uNextFrameIndex) % m_cFrames;
}

/******************************************************************
*                                                                 *
*  DemoApp::SaveComposedFrame()                                   *
*                                                                 *
*  Saves the current composed frame in the bitmap render target   *
*  into a temporary bitmap. Initializes the temporary bitmap if   *
*  needed.                                                        *
*                                                                 *
******************************************************************/

void DemoApp::SaveComposedFrame()
{
	com_ptr<ID2D1Bitmap> frameToBeSaved;

	check_hresult(m_frameComposeRT->GetBitmap(frameToBeSaved.put()));
	// Create the temporary bitmap if it hasn't been created yet 
	if (m_savedFrame == nullptr)
	{
		auto bitmapSize = frameToBeSaved->GetPixelSize();
		D2D1_BITMAP_PROPERTIES bitmapProp;
		frameToBeSaved->GetDpi(&bitmapProp.dpiX, &bitmapProp.dpiY);
		bitmapProp.pixelFormat = frameToBeSaved->GetPixelFormat();

		check_hresult(m_frameComposeRT->CreateBitmap(
			bitmapSize,
			bitmapProp,
			m_savedFrame.put()));
	}

	// Copy the whole bitmap
	check_hresult(m_savedFrame->CopyFromBitmap(nullptr, frameToBeSaved.get(), nullptr));
}

/******************************************************************
*                                                                 *
*  DemoApp::SelectAndDisplayGif()                                 *
*                                                                 *
*  Opens a dialog and displays a selected image.                  *
*                                                                 *
******************************************************************/

void DemoApp::SelectAndDisplayGif()
{
	WCHAR szFileName[MAX_PATH];
	RECT rcClient = {};
	RECT rcWindow = {};

	// If the user cancels selection, then nothing happens
	if (GetFileOpen(szFileName, ARRAYSIZE(szFileName)))
	{
		// Reset the states
		m_uNextFrameIndex = 0;
		m_uFrameDisposal = DM_NONE;  // No previous frame, use disposal none
		m_uLoopNumber = 0;
		m_fHasLoop = false;
		m_savedFrame = nullptr;

		// Create a decoder for the gif file
		m_decoder = nullptr;
		check_hresult(m_wicFactory->CreateDecoderFromFilename(
			szFileName,
			nullptr,
			GENERIC_READ,
			WICDecodeMetadataCacheOnLoad,
			m_decoder.put()));
		GetGlobalMetadata();

		rcClient.right = m_cxGifImagePixel;
		rcClient.bottom = m_cyGifImagePixel;

		if (!AdjustWindowRect(&rcClient, WS_OVERLAPPEDWINDOW, TRUE))
		{
			throw_last_error();
		}

		// Get the upper left corner of the current window
		if (!GetWindowRect(m_hWnd, &rcWindow))
		{
			throw_last_error();
		}

		// Resize the window to fit the gif
		MoveWindow(
			m_hWnd,
			rcWindow.left,
			rcWindow.top,
			RectWidth(rcClient),
			RectHeight(rcClient),
			TRUE);

		CreateDeviceResources();

		// If we have at least one frame, start playing
		// the animation from the first frame
		if (m_cFrames > 0)
		{
			ComposeNextFrame();
			InvalidateRect(m_hWnd, nullptr, FALSE);
		}
	}
}

/******************************************************************
*                                                                 *
*  DemoApp::ComposeNextFrame()                                    *
*                                                                 *
*  Composes the next frame by first disposing the current frame   *
*  and then overlaying the next frame. More than one frame may    *
*  be processed in order to produce the next frame to be          *
*  displayed due to the use of zero delay intermediate frames.    *
*  Also, sets a timer that is equal to the delay of the frame.    *
*                                                                 *
******************************************************************/

void DemoApp::ComposeNextFrame()
{
	// Check to see if the render targets are initialized
	if (m_hwndRT && m_frameComposeRT)
	{
		// First, kill the timer since the delay is no longer valid
		KillTimer(m_hWnd, DELAY_TIMER_ID);

		// Compose one frame
		DisposeCurrentFrame();
		OverlayNextFrame();

		// Keep composing frames until we see a frame with delay greater than
		// 0 (0 delay frames are the invisible intermediate frames), or until
		// we have reached the very last frame.
		while (m_uFrameDelay == 0 && !IsLastFrame())
		{
			DisposeCurrentFrame();
			OverlayNextFrame();
		}

		// If we have more frames to play, set the timer according to the delay.
		// Set the timer regardless of whether we succeeded in composing a frame
		// to try our best to continue displaying the animation.
		if (!EndOfAnimation() && m_cFrames > 1)
		{
			// Set the timer according to the delay
			SetTimer(m_hWnd, DELAY_TIMER_ID, m_uFrameDelay, nullptr);
		}
	}
}

/******************************************************************
*                                                                 *
*  DemoApp::RecoverDeviceResources                                *
*                                                                 *
*  Discards device-specific resources and recreates them.         *
*  Also starts the animation from the beginning.                  *
*                                                                 *
******************************************************************/

void DemoApp::RecoverDeviceResources()
{
	m_hwndRT = nullptr;
	m_frameComposeRT = nullptr;
	m_savedFrame = nullptr;

	m_uNextFrameIndex = 0;
	m_uFrameDisposal = DM_NONE;  // No previous frames. Use disposal none.
	m_uLoopNumber = 0;

	CreateDeviceResources();
	if (m_cFrames > 0)
	{
		// Load the first frame
		ComposeNextFrame();
		InvalidateRect(m_hWnd, nullptr, FALSE);
	}
}