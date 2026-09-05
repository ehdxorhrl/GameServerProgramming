#pragma once

class CGameloop
{
public:
	CGameloop();
	~CGameloop();
public:
	void Init(HWND hWnd);
	void Update();
	void Render();

public:
	HDC GetHDC() const { return hdc; }
	HWND GetHWND() const { return hwnd; }

	void SetServerSocket(SOCKET socket);

	std::queue<std::unique_ptr<Coord_Packet>> RecvQueue;
	CRITICAL_SECTION UpdateCS;

private:
	SOCKET serverSocket;
	HWND hwnd{};
	HDC hdc{};

	CImage imgboard, imgBlackKnight;
	short x, y;
};