#include <iostream>
#include <WS2tcpip.h>
#include <vector>
#include <concurrent_unordered_map.h>
#include "protocol.h"

#pragma comment(lib, "WS2_32.lib")
using namespace std;

enum S_STATE { ST_FREE, ST_ALLOC, ST_INGAME };

class SESSION {
public:
    S_STATE _state;
    int _id;
    SOCKET _socket;
    short x, y;
    char _name[NAME_SIZE];
    int _last_move_time;
    int _prev_remain;
    char _recv_buf[BUF_SIZE];

    SESSION() {
        _id = -1;
        _socket = 0;
        x = y = 0;
        _name[0] = 0;
        _state = ST_FREE;
        _last_move_time = 0;
        _prev_remain = 0;
    }

    void send_packet(void* packet) {
        if (_state != ST_INGAME) return;  
        if (_socket == INVALID_SOCKET) return;
        char* p = reinterpret_cast<char*>(packet);
        int total = 0;
        int size = p[0];
        while (total < size) {
            int sent = send(_socket, p + total, size - total, 0);
            if (sent == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
                else {
                    cout << "send error on client " << _id << endl;
                    return;
                }
            }
            total += sent;
        }
    }

    void send_login_info_packet() {
        SC_LOGIN_INFO_PACKET p;
        p.id = _id;
        p.size = sizeof(SC_LOGIN_INFO_PACKET);
        p.type = SC_LOGIN_INFO;
        p.x = x;
        p.y = y;
        send_packet(&p);
    }

    void send_move_packet(int c_id);
    void send_add_player_packet(int c_id);
    void send_remove_player_packet(int c_id);
};

concurrency::concurrent_unordered_map<int, shared_ptr<SESSION>> clients;

void SESSION::send_move_packet(int c_id) {
    auto it = clients.find(c_id);
    if (it == clients.end()) return;
    SC_MOVE_PLAYER_PACKET p;
    p.id = c_id;
    p.size = sizeof(SC_MOVE_PLAYER_PACKET);
    p.type = SC_MOVE_PLAYER;
    p.x = it->second->x;
    p.y = it->second->y;
    p.move_time = it->second->_last_move_time;
    send_packet(&p);
}

void SESSION::send_add_player_packet(int c_id) {
    auto it = clients.find(c_id);
    if (it == clients.end()) return;
    SC_ADD_PLAYER_PACKET p;
    p.id = c_id;
    strcpy_s(p.name, it->second->_name);
    p.size = sizeof(SC_ADD_PLAYER_PACKET);
    p.type = SC_ADD_PLAYER;
    p.x = it->second->x;
    p.y = it->second->y;
    send_packet(&p);
}

void SESSION::send_remove_player_packet(int c_id) {
    SC_REMOVE_PLAYER_PACKET p;
    p.id = c_id;
    p.size = sizeof(SC_REMOVE_PLAYER_PACKET);
    p.type = SC_REMOVE_PLAYER;
    send_packet(&p);
}

int get_new_client_id() {
    for (int i = 0; i < MAX_USER; ++i) {
        if (clients.find(i) == clients.end()) return i;
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
        for (auto& cl : clients) {
            auto other = cl.second;
            if (other->_state != ST_INGAME) continue;
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
    for (auto& cl : clients) {
        auto other = cl.second;
        if (other->_state != ST_INGAME) continue;
        if (other->_id == c_id) continue;
        other->send_remove_player_packet(c_id);
    }
    closesocket(session->_socket);
    it->second->_state = ST_FREE;
    cout << "Client " << c_id << " disconnected." << endl;
}

int main() {
    WSADATA WSAData;
    WSAStartup(MAKEWORD(2, 2), &WSAData);

    SOCKET g_s_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, 0);
    SOCKADDR_IN addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT_NUM);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(g_s_socket, (sockaddr*)&addr, sizeof(addr));
    listen(g_s_socket, SOMAXCONN);

    unsigned long mode = 1;
    ioctlsocket(g_s_socket, FIONBIO, &mode);

    while (true) {
        SOCKET client = accept(g_s_socket, NULL, NULL);
        if (client == INVALID_SOCKET) {
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
                cout << "accept error: " << WSAGetLastError() << endl;
            }
        }
        else {
            ioctlsocket(client, FIONBIO, &mode);
            int cid = get_new_client_id();
            if (cid != -1) {
                auto s = make_shared<SESSION>();
                s->_socket = client;
                s->_id = cid;
                s->_state = ST_INGAME;
                clients[cid] = s;
            }
            else {
                cout << "Max user exceeded." << endl;
                closesocket(client);
            }
        }

        for (auto& pair : clients) {
            auto s = pair.second;
            int recv_bytes = recv(s->_socket, s->_recv_buf + s->_prev_remain, BUF_SIZE - s->_prev_remain, 0);
            if (recv_bytes > 0) {
                int data_size = recv_bytes + s->_prev_remain;
                char* p = s->_recv_buf;
                while (data_size > 0) {
                    int packet_size = p[0];
                    if (packet_size <= data_size) {
                        process_packet(s->_id, p);
                        p += packet_size;
                        data_size -= packet_size;
                    }
                    else break;
                }
                s->_prev_remain = data_size;
                if (data_size > 0) memcpy(s->_recv_buf, p, data_size);
            }
            else if (recv_bytes == 0 || (recv_bytes == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)) {
                disconnect(s->_id);
            }
        }
    }

    closesocket(g_s_socket);
    WSACleanup();
}
