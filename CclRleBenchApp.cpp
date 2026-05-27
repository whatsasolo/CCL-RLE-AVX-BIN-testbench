#include "stdafx.h"
#include "MainFrame.h"

class CCclRleBenchApp : public CWinApp
{
public:
	BOOL InitInstance() override
	{
		CWinApp::InitInstance();
		if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
			return FALSE;

		auto* frame = new CMainFrame();
		if (!frame->GetSafeHwnd())
		{
			delete frame;
			CoUninitialize();
			return FALSE;
		}
		m_pMainWnd = frame;
		frame->ShowWindow(SW_SHOW);
		frame->UpdateWindow();
		return TRUE;
	}

	int ExitInstance() override
	{
		CoUninitialize();
		return CWinApp::ExitInstance();
	}
};

CCclRleBenchApp theApp;
