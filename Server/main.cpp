#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <WinSock2.h>
#include <stdio.h>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <memory>
#include <tchar.h>
#include <compressapi.h>

#pragma comment(lib, "ws2_32")
#pragma comment(lib, "Cabinet")

// BGRA U8 Bitmap
struct Bitmap {
	LONG Width = 0;
	LONG Height = 0;
	std::vector<uint8_t> Buf;
};

struct Buffer
{
	std::mutex frameLock;
	std::atomic<std::shared_ptr<Bitmap>> front;
	std::atomic<std::shared_ptr<Bitmap>> back;

	Buffer()
	{
		front = std::make_unique<Bitmap>();
		back = std::make_unique<Bitmap>();
	}

	void SwapBuffersAtomic()
	{
		// Lock the mutex for the buffer exchange
		this->frameLock.lock();

		// Swap the front and back buffers
		auto temp = this->front.load();
		this->front.exchange(this->back);
		this->back.exchange(temp);

		// Unlock the mutex
		this->frameLock.unlock();
	}
};

Buffer g_LatestFrame;


void ReceiveFrame(SOCKET sock)
{
	UINT32 width = 0;
	UINT32 height = 0;
	UINT32 compressed_size = 0;
	std::vector<uint8_t> compressed;
	SIZE_T decompressed_size = 0;

	// Create the decompressor engine
	DECOMPRESSOR_HANDLE decompressor = NULL;
	CreateDecompressor(COMPRESS_ALGORITHM_XPRESS, NULL, &decompressor);

	while (1)
	{
		width = 0;
		height = 0;

		// Get the address to the backbuffer
		auto backBuffer = g_LatestFrame.back.load();

		// Receive the size of the compressed data
		recv(sock, (char*)&compressed_size, sizeof(compressed_size), 0);

		// If the compressed size is larger than the capacity, we need to resize
		if (compressed.capacity() < compressed_size)
		{
			compressed.resize(compressed_size);
		}

		// Get the width and the height of the screen
		recv(sock, (char*)&width, sizeof(width), 0);
		recv(sock, (char*)&height, sizeof(height), 0);

		// If the width or height has changed, we need to resize our frame
		if (backBuffer->Width != width || backBuffer->Height != height)
		{
			backBuffer->Width = width;
			backBuffer->Height = height;

			backBuffer->Buf.resize(width * height * 4);
		}

		int totalToReceive = compressed_size;

		// Receive all of the compressed data
		do
		{
			totalToReceive -= recv(sock, (char*)compressed.data(), totalToReceive, 0);
		} while (totalToReceive > 0);

		// Decompress the data into the frame buffer
		Decompress(decompressor, compressed.data(), compressed_size, backBuffer->Buf.data(), width * height * 4, &decompressed_size);

		// Swap the front and back frame buffers
		g_LatestFrame.SwapBuffersAtomic();
	}
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_TIMER:
		// TODO: Maybe make this not in the timer
		InvalidateRect(hWnd, nullptr, FALSE);
		break;
	case WM_PAINT:
	{
		PAINTSTRUCT ps = { 0 };
		BITMAPINFO  inf = { 0 };
		void* bits = nullptr;

		// Lock the buffer mutex so we don't get any buffers
		// switched on us mid-render
		g_LatestFrame.frameLock.lock();

		// Load the front buffer pointer
		auto frame = g_LatestFrame.front.load();

		// Populate the bitmap information for GDI
		inf.bmiHeader.biSize = sizeof(inf.bmiHeader);
		inf.bmiHeader.biWidth = frame->Width;
		inf.bmiHeader.biHeight = -frame->Height;
		inf.bmiHeader.biPlanes = 1;
		inf.bmiHeader.biBitCount = 32;
		inf.bmiHeader.biCompression = BI_RGB;

		HDC hdc = BeginPaint(hWnd, &ps);

		// Set stretch mode to HALFTONE for best quality
		SetStretchBltMode(hdc, HALFTONE);

		// Get window size
		RECT cl_rect = { 0 };
		GetClientRect(hWnd, &cl_rect);

		// Stretch the frame bits into the HDC to render
		StretchDIBits(hdc, 0, 0, cl_rect.right, cl_rect.bottom, 0, 0, frame->Width, frame->Height, frame->Buf.data(), &inf, DIB_RGB_COLORS, SRCCOPY);

		EndPaint(hWnd, &ps);

		// Unlock the frame mutex
		g_LatestFrame.frameLock.unlock();

		// IDK why this doesnt work
		InvalidateRect(hWnd, nullptr, FALSE);

		break;
	}
	case WM_DESTROY:
		PostQuitMessage(0);
		break;
	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
	return 0;
}

HWND CreateWin32Window()
{
	WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_HREDRAW | CS_VREDRAW, WndProc, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, _T("Test Window"), NULL };
	RegisterClassEx(&wc);

	// Create the new window
	HWND hWnd = CreateWindowExW(0, wc.lpszClassName, _T("Test Window"), WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800, NULL, NULL, NULL, NULL);
	if (!hWnd)
	{
		printf("fail: %d\n", GetLastError());
	}

	return hWnd;
}

int main(void)
{
	WSADATA wsaData;

	// Initialize WinSock2
	WSAStartup(MAKEWORD(2, 2), &wsaData);

	sockaddr_in local;

	// Configure the binding address and port
	local.sin_family = AF_INET; //Address family
	local.sin_addr.s_addr = INADDR_ANY; //Wild card IP address
	local.sin_port = htons((u_short)20248); //port to use

	// Create the server socket
	SOCKET server = socket(AF_INET, SOCK_STREAM, 0);

	// Bind address to the server socket
	if (bind(server, (sockaddr*)&local, sizeof(local)) != 0)
	{
		return -1;
	}

	// Listen for connections
	if (listen(server, SOMAXCONN) != 0)
	{
		return -1;
	}

	SOCKET client; 
	sockaddr_in from;
	int fromlen = sizeof(from);

	// Accept our incoming client connection
	client = accept(server,
		(struct sockaddr*)&from, &fromlen);

	// We don't need the server socket anymore
	closesocket(server);

	// Spawn the thread that will receive the incoming frames from the client
	std::thread recvThread = std::thread(ReceiveFrame, client);

	// Create a window to render frames to
	HWND wnd = CreateWin32Window();

	// Set timer to tell window to render the new frames
	SetTimer(wnd, 1, 1, nullptr);

	MSG msg;

	// Main window message loop
	while (1)
	{
		while (PeekMessage(&msg, wnd, 0U, 0U, PM_REMOVE) > 0)
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

	// This is redundant but whatever
	recvThread.join();
}