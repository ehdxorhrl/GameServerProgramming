#include "stdafx.h"  // "stdafx.cpp" 내용을 포함한다고 가정
#include <winsock2.h>
#include <ws2tcpip.h>
#include <memory>
#include <mutex>

void CALLBACK g_send_callback(DWORD err, DWORD num_bytes, LPWSAOVERLAPPED p_over, DWORD flag);
void CALLBACK g_recv_callback(DWORD err, DWORD num_bytes, LPWSAOVERLAPPED p_over, DWORD flag);

class SESSION;

// 전역 변수
std::unordered_map<long long, std::unique_ptr<SESSION>> g_users;
std::recursive_mutex g_users_mutex;

// 서버에서 클라이언트로 보내는 패킷 타입
enum class PacketType { Coord, Disconnect };

// 서버에서 클라이언트로 보내는 패킷 구조체
struct ServerPacket {
    PacketType type;    // 패킷 타입 (좌표 또는 접속 종료)
    long long id;       // 클라이언트 ID
    short x;            // X 좌표
    short y;            // Y 좌표
};

class EXP_OVER {
public:
    EXP_OVER(const ServerPacket& pkt) {
        ZeroMemory(&_send_over, sizeof(_send_over));
        memcpy(_send_buffer, &pkt, sizeof(ServerPacket));
        _send_wsabuf.buf = _send_buffer;
        _send_wsabuf.len = sizeof(ServerPacket);
    }

    WSAOVERLAPPED _send_over;
    char _send_buffer[sizeof(ServerPacket)];
    WSABUF _send_wsabuf;
};

class SESSION {
private:
    SOCKET _c_socket;
    long long _id;
    short x, y;

    WSAOVERLAPPED _recv_over;
    Input_Packet _recv_packet;
    WSABUF _recv_wsabuf;

    void do_recv() {
        DWORD recv_flag = 0;
        ZeroMemory(&_recv_over, sizeof(_recv_over));
        _recv_over.hEvent = reinterpret_cast<HANDLE>(_id);
        _recv_wsabuf.len = sizeof(Input_Packet);
        _recv_wsabuf.buf = reinterpret_cast<char*>(&_recv_packet);
        int ret = WSARecv(_c_socket, &_recv_wsabuf, 1, nullptr, &recv_flag, &_recv_over, g_recv_callback);
        if (ret != 0) {
            int err_no = WSAGetLastError();
            if (err_no != WSA_IO_PENDING) {
                print_error_message(err_no);
            }
        }
    }

public:
    SESSION(long long session_id, SOCKET s) : _id(session_id), _c_socket(s), x(0), y(0) {
        _recv_wsabuf.len = sizeof(Input_Packet);
        _recv_wsabuf.buf = reinterpret_cast<char*>(&_recv_packet);
        do_recv();
    }

    ~SESSION() {
        closesocket(_c_socket);
    }

    void recv_callback(DWORD err, DWORD num_bytes) {
        if (err != 0 || num_bytes == 0) {
            std::cout << "Client[" << _id << "] disconnected." << std::endl;
            std::lock_guard<std::recursive_mutex> lock(g_users_mutex);
            ServerPacket disc_pkt{ PacketType::Disconnect, _id, 0, 0 };
            for (auto& u : g_users) {
                if (u.first != _id) {
                    u.second->do_send(disc_pkt);
                }
            }
            g_users.erase(_id);
            return;
        }

        if (num_bytes == sizeof(Input_Packet)) {
            switch (_recv_packet.inputType) {
            case KT::Down:
                if (y < 700) y += 100;
                break;
            case KT::Up:
                if (y >= 100) y -= 100;
                break;
            case KT::Right:
                if (x < 700) x += 100;
                break;
            case KT::Left:
                if (x >= 100) x -= 100;
                break;
            }

            std::cout << "Client[" << _id << "] moved to (" << x << ", " << y << ")" << std::endl;

            ServerPacket pkt{ PacketType::Coord, _id, x, y };
            std::lock_guard<std::recursive_mutex> lock(g_users_mutex);
            for (auto& u : g_users) {
                //ServerPacket pkt_copy = pkt; // 패킷 복사
                u.second->do_send(pkt);
            }
        }
        do_recv();
    }

    void do_send(const ServerPacket& pkt) {
        EXP_OVER* o = new EXP_OVER(pkt);
        DWORD size_sent;
        WSASend(_c_socket, &o->_send_wsabuf, 1, &size_sent, 0, &o->_send_over, g_send_callback);
    }

    short get_x() const { return x; }
    short get_y() const { return y; }
};

void CALLBACK g_send_callback(DWORD err, DWORD num_bytes, LPWSAOVERLAPPED p_over, DWORD flag) {
    EXP_OVER* o = reinterpret_cast<EXP_OVER*>(p_over);
    delete o;
}

void CALLBACK g_recv_callback(DWORD err, DWORD num_bytes, LPWSAOVERLAPPED p_over, DWORD flag) {
    long long my_id = reinterpret_cast<long long>(p_over->hEvent);
    std::lock_guard<std::recursive_mutex> lock(g_users_mutex);
    auto it = g_users.find(my_id);
    if (it != g_users.end()) {
        it->second->recv_callback(err, num_bytes);
    }
}

int main() {
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 0), &wsa_data) != 0) {
        print_error_message(WSAGetLastError());
        return 1;
    }

    SOCKET s_socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (s_socket == INVALID_SOCKET) {
        print_error_message(WSAGetLastError());
        WSACleanup();
        return 1;
    }

    SOCKADDR_IN server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s_socket, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) == SOCKET_ERROR) {
        print_error_message(WSAGetLastError());
        closesocket(s_socket);
        WSACleanup();
        return 1;
    }

    if (listen(s_socket, SOMAXCONN) == SOCKET_ERROR) {
        print_error_message(WSAGetLastError());
        closesocket(s_socket);
        WSACleanup();
        return 1;
    }

    long long client_id = 0;

    while (true) {
        int addr_size = sizeof(server_addr);
        SOCKET c_socket = WSAAccept(s_socket, reinterpret_cast<sockaddr*>(&server_addr), &addr_size, nullptr, 0);
        if (c_socket == INVALID_SOCKET) {
            print_error_message(WSAGetLastError());
            continue;
        }

        std::lock_guard<std::recursive_mutex> lock(g_users_mutex);
        if (g_users.size() >= 10) {
            closesocket(c_socket);
            continue;
        }

        g_users.emplace(client_id, std::make_unique<SESSION>(client_id, c_socket));
        auto new_session = g_users[client_id].get();

        // 새 클라이언트에게 기존 클라이언트들의 위치 전송
        for (const auto& u : g_users) {
            ServerPacket pkt{ PacketType::Coord, u.first, u.second->get_x(), u.second->get_y() };
            new_session->do_send(pkt);
        }

        // 기존 클라이언트들에게 새 클라이언트의 위치 전송
        ServerPacket new_pkt{ PacketType::Coord, client_id, new_session->get_x(), new_session->get_y() };
        for (auto& u : g_users) {
            if (u.first != client_id) {
                u.second->do_send(new_pkt);
            }
        }
        client_id++;
    }

    closesocket(s_socket);
    WSACleanup();
    return 0;
}