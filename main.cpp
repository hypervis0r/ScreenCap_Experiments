#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <Winsock2.h>
#include <wrl.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d3d11_2.h>
#include <d2d1_2.h>
#include <d2d1_2helper.h>
#include <vector>
#include <tchar.h>
#include <memory>
#include <compressapi.h>

#pragma comment(lib, "D3D11")
#pragma comment(lib, "ws2_32")
#pragma comment(lib, "Cabinet")

using namespace Microsoft::WRL;

struct ComException
{
	HRESULT result;
	ComException(HRESULT const value) :
		result(value)
	{}
};

// Failing HRESULTs will throw a C++ Exception for debugging
__forceinline void HR(HRESULT const result)
{
	if (S_OK != result)
	{
		throw ComException(result);
	}
}

// BGRA U8 Bitmap
struct Bitmap {
	LONG Width = 0;
	LONG Height = 0;
	std::vector<uint8_t> Buf;
};

class DesktopDuplication
{
private:
	D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
	ComPtr<ID3D11Device> pD3DDevice = nullptr;
	ComPtr<ID3D11DeviceContext> pImmediateContext = nullptr;
	ComPtr<IDXGIDevice> pDxgiDevice = nullptr;
	ComPtr<IDXGIAdapter> pDxgiAdapter = nullptr;
	ComPtr<IDXGIOutput> pDxgiOutput = nullptr;
	ComPtr<IDXGIOutput1> pDxgiOutput1 = nullptr;
	ComPtr<IDXGIOutputDuplication> pDxgiDesktopDupl = nullptr;
	
	HDESK hDesk = NULL;
	
	bool frameLock = false;

	HRESULT InitializeD3DApis()
	{
		HRESULT hr = 0;

		UINT createDeviceFlags = 0;
		D3D_FEATURE_LEVEL featureLevels[] =
		{
			D3D_FEATURE_LEVEL_11_1,
			D3D_FEATURE_LEVEL_11_0,
		};
		UINT numFeatureLevels = ARRAYSIZE(featureLevels);

		// Create the Direct3D 11 Device
		HR(D3D11CreateDevice(
			nullptr,
			D3D_DRIVER_TYPE_HARDWARE,
			nullptr,
			createDeviceFlags,
			featureLevels,
			numFeatureLevels,
			D3D11_SDK_VERSION,
			&pD3DDevice,
			&featureLevel,
			&pImmediateContext));

		// QI For the DXGI Device
		HR(pD3DDevice.As(&pDxgiDevice));

		// Retrieve the Device Adapter
		HR(pDxgiDevice->GetParent(__uuidof(IDXGIAdapter), (void**)pDxgiAdapter.GetAddressOf()));

		// Get the output specified by the output number
		HR(pDxgiAdapter->EnumOutputs(OutputNumber, pDxgiOutput.GetAddressOf()));

		// Get the output description
		HR(pDxgiOutput->GetDesc(&OutputDesc));

		// QI For IDXGIOutput1
		HR(pDxgiOutput.As(&pDxgiOutput1));

		// Duplicate the desktop screen output
		HR(pDxgiOutput1->DuplicateOutput(pD3DDevice.Get(), pDxgiDesktopDupl.GetAddressOf()));

		return S_OK;
	}

public:
	DXGI_OUTPUT_DESC OutputDesc = { 0 };
	UINT OutputNumber = 0;
	Bitmap LatestFrame;

	DesktopDuplication(UINT OutputNumber = 0)
	{
		this->OutputNumber = OutputNumber;

		// Initialize the Direct3D 11 APIs (including the Desktop Duplication APIs)
		HR(InitializeD3DApis());
	}

	HRESULT CaptureNextFrame()
	{
		ComPtr<IDXGIResource> dxgiRes = nullptr;
		DXGI_OUTDUPL_FRAME_INFO frameInfo = { 0 };

		// If we have a previous frame stored, lets release it
		if (frameLock)
		{
			frameLock = false;
			pDxgiDesktopDupl->ReleaseFrame();
		}

		try
		{
			// Acquire the next desktop frame
			HR(pDxgiDesktopDupl->AcquireNextFrame(0, &frameInfo, dxgiRes.GetAddressOf()));
		}
		catch (ComException& ex)
		{
			// If the result is DXGI_ERROR_WAIT_TIMEOUT, we should politely
			// inform the caller that theres no frames currently rendered.
			if (ex.result == DXGI_ERROR_WAIT_TIMEOUT)
				return DXGI_ERROR_WAIT_TIMEOUT;
			else
				throw ex;
		}
		
		// Set the frame lock
		frameLock = true;

		// Get the frame as a GPU Texture2D
		ComPtr<ID3D11Texture2D> d3dGpuTex2d = nullptr;
		HR(dxgiRes.As(&d3dGpuTex2d));

		// Get the desc for us to populate
		D3D11_TEXTURE2D_DESC desc = { 0 };
		d3dGpuTex2d->GetDesc(&desc);

		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.MiscFlags = 0;

		// Create the Texture2D for us to access on the CPU
		ComPtr<ID3D11Texture2D> d3dCpuTex2d = nullptr;
		HR(pD3DDevice->CreateTexture2D(&desc, nullptr, d3dCpuTex2d.GetAddressOf()));

		// Copy the frame from the GPU to the CPU
		pImmediateContext->CopyResource(d3dCpuTex2d.Get(), d3dGpuTex2d.Get());

		// Map the CPU Texture2D into our virtual memory space so we can copy it
		D3D11_MAPPED_SUBRESOURCE sr = { 0 };
		HR(pImmediateContext->Map(d3dCpuTex2d.Get(), 0, D3D11_MAP_READ, 0, &sr));

		// If the width or height have changed, we need to resize our buffer
		if (LatestFrame.Width != desc.Width || LatestFrame.Height != desc.Height) {
			LatestFrame.Width = desc.Width;
			LatestFrame.Height = desc.Height;
			LatestFrame.Buf.resize(desc.Width * desc.Height * 4);
		}
		
		// Blit the new frame into our frame buffer
		for (int y = 0; y < (int)desc.Height; y++)
			memcpy(LatestFrame.Buf.data() + y * desc.Width * 4, (uint8_t*)sr.pData + sr.RowPitch * y, desc.Width * 4);

		// Unmap the CPU Texture2D
		pImmediateContext->Unmap(d3dCpuTex2d.Get(), 0);

		return S_OK;
	}
};

std::unique_ptr<DesktopDuplication> g_DesktopDupl = nullptr;

void SendLatestFrame(SOCKET sock)
{
	// Create the compression engine
	COMPRESSOR_HANDLE compressor = NULL;
	CreateCompressor(COMPRESS_ALGORITHM_XPRESS, NULL, &compressor);

	std::vector<uint8_t> compressed;
	LONG width = 0;
	LONG height = 0;
	LONG compressed_size = 0;

	while (1)
	{
		// Capture the next desktop frame into memory
		g_DesktopDupl->CaptureNextFrame();

		// If the width or height has changed, we need to update the size of the compressed buffer
		if (g_DesktopDupl->LatestFrame.Width != width || g_DesktopDupl->LatestFrame.Height != height)
		{
			width = g_DesktopDupl->LatestFrame.Width;
			height = g_DesktopDupl->LatestFrame.Height;

			compressed.resize(width * height * 4);
		}

		// Compress the frame data into our compressed buffer
		Compress(
			compressor, 
			g_DesktopDupl->LatestFrame.Buf.data(), 
			g_DesktopDupl->LatestFrame.Width * g_DesktopDupl->LatestFrame.Height * 4, 
			compressed.data(),
			compressed.capacity(),
			(PSIZE_T)&compressed_size);


		// Send off the frame data (compressed size, width, height, compressed frame)
		send(sock, (char*)&compressed_size, sizeof(compressed_size), 0);

		send(sock, (char*)&g_DesktopDupl->LatestFrame.Width, sizeof(g_DesktopDupl->LatestFrame.Width), 0);
		send(sock, (char*)&g_DesktopDupl->LatestFrame.Height, sizeof(g_DesktopDupl->LatestFrame.Height), 0);

		send(sock, (char*)compressed.data(), compressed_size, 0);
	}
}

int main(void)
{
	// Initialize a smart pointer for the desktop duplication class
	g_DesktopDupl = std::make_unique<DesktopDuplication>(DesktopDuplication());

	WSADATA wsaData;

	// Initialize WinSock2
	WSAStartup(MAKEWORD(2, 2), &wsaData);

	// Create our TCP socket
	SOCKET ConnectSocket;
	ConnectSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	// Connect to specified address and port
	sockaddr_in clientService;
	clientService.sin_family = AF_INET;
	clientService.sin_addr.s_addr = inet_addr("127.0.0.1");
	clientService.sin_port = htons(20248);

	int iResult = 0;

	// Attempt to connect to the specified address
	do
	{
		iResult = connect(ConnectSocket, (SOCKADDR*)&clientService, sizeof(clientService));
		if (iResult == SOCKET_ERROR) 
		{
			wprintf(L"connect function failed with error: %ld\n", WSAGetLastError());
		}
	} while (iResult != 0);

	// Begin sending frames over TCP
	SendLatestFrame(ConnectSocket);
}