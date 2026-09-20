#pragma once
#include <windows.h>
#include <unknwn.h>
MIDL_INTERFACE("45D64A29-A63E-4CB6-B498-5781D298CB4F")
ICoreWindowInterop : public IUnknown
{
	virtual HRESULT STDMETHODCALLTYPE get_WindowHandle(HWND *hwnd) = 0;
	virtual HRESULT STDMETHODCALLTYPE put_MessageHandled(unsigned char value) = 0;
};
__CRT_UUID_DECL(ICoreWindowInterop, 0x45D64A29, 0xA63E, 0x4CB6, 0xB4, 0x98, 0x57, 0x81, 0xD2, 0x98, 0xCB, 0x4F)
