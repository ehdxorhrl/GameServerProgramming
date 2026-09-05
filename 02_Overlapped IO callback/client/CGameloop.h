#pragma once
#include <queue>
#include <memory>
#include <unordered_map>

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

    std::queue<std::unique_ptr<ServerPacket>> RecvQueue; // ServerPacket으로 변경
    CRITICAL_SECTION UpdateCS;

private:
    SOCKET serverSocket;
    HWND hwnd{};
    HDC hdc{};
    CImage imgboard, imgBlackKnight;
    std::unordered_map<long long, std::pair<short, short>> horses; // ID별 말 위치 저장
};