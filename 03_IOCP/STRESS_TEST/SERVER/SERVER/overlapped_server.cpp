#include <iostream>
#include <concurrent_unordered_map.h>
#include <WS2tcpip.h>
#include <MSWSock.h>
#include <vector>
#include <unordered_set>
#include "protocol.h"

#pragma comment(lib, "WS2_32.lib")
#pragma comment(lib, "MSWSock.lib")
using namespace std;

enum COMP_TYPE { OP_ACCEPT, OP_RECV, OP_SEND };

class OVER_EXP {
public:
    WSAOVERLAPPED _over;
    WSABUF _wsabuf;
    char _send_buf[BUF_SIZE];
    COMP_TYPE _comp_type;
    OVER_EXP() {
        _wsabuf.len = BUF_SIZE;
        _wsabuf.buf = _send_buf;
        _comp_type = OP_RECV;
        ZeroMemory(&_over, sizeof(_over));
    }
    OVER_EXP(char* packet) {
        _wsabuf.len = packet[0];
        _wsabuf.buf = _send_buf;
        ZeroMemory(&_over, sizeof(_over));
        _comp_type = OP_SEND;
        memcpy(_send_buf, packet, packet[0]);
    }
};

void CALLBACK recv_callback(DWORD err, DWORD num_bytes, LPWSAOVERLAPPED over, DWORD flags);
void CALLBACK send_callback(DWORD err, DWORD num_bytes, LPWSAOVERLAPPED over, DWORD flags);

enum S_STATE { ST_FREE, ST_ALLOC, ST_INGAME };

class SESSION {
    OVER_EXP _recv_over;

public:
    S_STATE _state;
    bool _is_active;
    int _id;
    SOCKET _socket;
    short x, y;
    char _name[NAME_SIZE];
    int _prev_remain;
    int _last_move_time;

    SESSION() {
        _id = -1;
        _socket = 0;
        x = y = 0;
        _name[0] = 0;
        _state = ST_FREE;
        _is_active = false;
        _prev_remain = 0;
    }

    void do_recv() {
        DWORD recv_flag = 0;
        memset(&_recv_over._over, 0, sizeof(_recv_over._over));
        _recv_over._wsabuf.len = BUF_SIZE - _prev_remain;
        _recv_over._wsabuf.buf = _recv_over._send_buf + _prev_remain;
        long long lnum = _id;
        _recv_over._over.hEvent = reinterpret_cast<HANDLE>(lnum);
        WSARecv(_socket, &_recv_over._wsabuf, 1, 0, &recv_flag, &_recv_over._over, recv_callback);
    }

    void do_send(void* packet) {
        OVER_EXP* sdata = new OVER_EXP{ reinterpret_cast<char*>(packet) };
        long long lnum = _id;
        sdata->_over.hEvent = reinterpret_cast<HANDLE>(lnum);
        WSASend(_socket, &sdata->_wsabuf, 1, 0, 0, &sdata->_over, send_callback);
    }

    void send_login_info_packet() {
        SC_LOGIN_INFO_PACKET p;
        p.id = _id;
        p.size = sizeof(SC_LOGIN_INFO_PACKET);
        p.type = SC_LOGIN_INFO;
        p.x = x;
        p.y = y;
        do_send(&p);
    }

    void send_move_packet(int c_id);
    void send_add_player_packet(int c_id);
    void send_remove_player_packet(int c_id) {
        SC_REMOVE_PLAYER_PACKET p;
        p.id = c_id;
        p.size = sizeof(p);
        p.type = SC_REMOVE_PLAYER;
        do_send(&p);
    }
};

concurrency::concurrent_unordered_map<long long, shared_ptr<SESSION>> clients;

void SESSION::send_move_packet(int c_id) {
    auto it = clients.find(c_id);
    if (it == clients.end()) return;
    auto& s = it->second;
    SC_MOVE_PLAYER_PACKET p;
    p.id = c_id;
    p.size = sizeof(SC_MOVE_PLAYER_PACKET);
    p.type = SC_MOVE_PLAYER;
    p.x = s->x;
    p.y = s->y;
    p.move_time = s->_last_move_time;
    do_send(&p);
}

void SESSION::send_add_player_packet(int c_id) {
    auto it = clients.find(c_id);
    if (it == clients.end()) return;
    auto& s = it->second;
    SC_ADD_PLAYER_PACKET add_packet;
    add_packet.id = c_id;
    strcpy_s(add_packet.name, s->_name);
    add_packet.size = sizeof(add_packet);
    add_packet.type = SC_ADD_PLAYER;
    add_packet.x = s->x;
    add_packet.y = s->y;
    do_send(&add_packet);
}

int get_new_client_id() {
    for (int i = 0; i < MAX_USER; ++i) {
        if (clients.find(i) == clients.end())
            return i;
    }
    return -1;
}

void process_packet(int c_id, char* packet) {
    auto it = clients.find(c_id);
    if (it == clients.end()) return;
    auto session = it->second;

    switch (packet[1]) {
    case CS_LOGIN: {
        CS_LOGIN_PACKET* p = reinterpret_cast<CS_LOGIN_PACKET*>(packet);
        strcpy_s(session->_name, p->name);
        session->send_login_info_packet();
        session->_state = ST_INGAME;

        for (auto& pl : clients) {
            auto other = pl.second;
            if (ST_INGAME != other->_state) continue;
            if (other->_id == c_id) continue;
            other->send_add_player_packet(c_id);
            session->send_add_player_packet(other->_id);
        }
        break;
    }
    case CS_MOVE: {
        CS_MOVE_PACKET* p = reinterpret_cast<CS_MOVE_PACKET*>(packet);
        session->_last_move_time = p->move_time;
        short x = session->x;
        short y = session->y;
        switch (p->direction) {
        case 0: if (y > 0) y--; break;
        case 1: if (y < W_HEIGHT - 1) y++; break;
        case 2: if (x > 0) x--; break;
        case 3: if (x < W_WIDTH - 1) x++; break;
        }
        session->x = x;
        session->y = y;

        for (auto& cl : clients) {
            auto other = cl.second;
            if (other->_state != ST_INGAME) continue;
            other->send_move_packet(c_id);
        }
        break;
    }
    }
}

void disconnect(int c_id) {
    auto it = clients.find(c_id);
    if (it == clients.end()) return;
    auto session = it->second;

    cout << "[Disconnect] Client ID: " << c_id << " disconnected." << endl;

    for (auto& pl : clients) {
        auto other = pl.second;
        if (ST_INGAME != other->_state) continue;
        if (other->_id == c_id) continue;
        other->send_remove_player_packet(c_id);
    }
    closesocket(session->_socket);
    session->_state = ST_FREE;
}

void CALLBACK recv_callback(DWORD err, DWORD num_bytes, LPWSAOVERLAPPED over, DWORD flags) {
    long long key = reinterpret_cast<long long>(over->hEvent);
    auto it = clients.find(key);
    if (it == clients.end()) return;
    auto session = it->second;

    if (err) { disconnect(key); return; }

    OVER_EXP* ex_over = reinterpret_cast<OVER_EXP*>(over);
    int remain_data = num_bytes + session->_prev_remain;
    char* p = ex_over->_send_buf;
    while (remain_data > 0) {
        int packet_size = p[0];
        if (packet_size <= remain_data) {
            process_packet(key, p);
            p += packet_size;
            remain_data -= packet_size;
        }
        else break;
    }
    session->_prev_remain = remain_data;
    if (remain_data > 0) memcpy(ex_over->_send_buf, p, remain_data);
    session->do_recv();
}

void CALLBACK send_callback(DWORD err, DWORD num_bytes, LPWSAOVERLAPPED over, DWORD flags) {
    long long client_id = reinterpret_cast<long long>(over->hEvent);
    if (err) { disconnect(client_id); return; }
    delete reinterpret_cast<OVER_EXP*>(over);
}

int main() {
    WSADATA WSAData;
    WSAStartup(MAKEWORD(2, 2), &WSAData);
    SOCKET server = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    SOCKADDR_IN server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT_NUM);
    server_addr.sin_addr.S_un.S_addr = INADDR_ANY;
    bind(server, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr));
    listen(server, SOMAXCONN);
    SOCKADDR_IN cl_addr;
    int addr_size = sizeof(cl_addr);

    while (true) {
        SOCKET client = WSAAccept(server, reinterpret_cast<sockaddr*>(&cl_addr), &addr_size, NULL, NULL);
        int client_id = get_new_client_id();
        if (client_id != -1) {
            clients.insert({ client_id, make_shared<SESSION>() });
            auto it = clients.find(client_id);
            if (it != clients.end()) {
                auto session = it->second;
                session->_state = ST_INGAME;
                session->x = 0;
                session->y = 0;
                session->_id = client_id;
                session->_name[0] = 0;
                session->_prev_remain = 0;
                session->_socket = client;
                session->do_recv();
            }
        }
        else {
            cout << "Max user exceeded.\n";
        }
    }
    closesocket(server);
    WSACleanup();
}
