#include <iostream>
#include <array>
#include <WS2tcpip.h>
#include <MSWSock.h>
#include <thread>
#include <vector>
#include <mutex>
#include <unordered_set>
#include <chrono>
#include <queue>
#include <concurrent_unordered_map.h>
#include <concurrent_priority_queue.h>
#include <memory>
#include <sqltypes.h>
#include <sqlext.h>
#include "protocol.h"

#include "include/lua.hpp"


#pragma comment(lib, "WS2_32.lib")
#pragma comment(lib, "MSWSock.lib")
#pragma comment(lib, "lua54.lib")

using namespace std;
using namespace concurrency;
using namespace chrono;

constexpr int VIEW_RANGE = 5;

constexpr int CELL_SIZE = 20;
const int GRID_W = W_WIDTH / CELL_SIZE;
const int GRID_H = W_HEIGHT / CELL_SIZE;

unordered_set<int> spatial_map[GRID_W][GRID_H];
mutex spatial_lock[GRID_W][GRID_H];

atomic<long long> g_next_user_id = 0;

bool check_user_exists(int user_id);
bool get_user_position(int user_id, short& x, short& y);
void store_user_position(int user_id, short x, short y);

enum COMP_TYPE { OP_ACCEPT, OP_RECV, OP_SEND, OP_NPC_MOVE, OP_AI_HELLO };
class OVER_EXP {
public:
	WSAOVERLAPPED _over;
	WSABUF _wsabuf;
	char _send_buf[BUF_SIZE];
	COMP_TYPE _comp_type;
	int _ai_target_obj;
	OVER_EXP()
	{
		_wsabuf.len = BUF_SIZE;
		_wsabuf.buf = _send_buf;
		_comp_type = OP_RECV;
		ZeroMemory(&_over, sizeof(_over));
	}
	OVER_EXP(char* packet)
	{
		_wsabuf.len = packet[0];
		_wsabuf.buf = _send_buf;
		ZeroMemory(&_over, sizeof(_over));
		_comp_type = OP_SEND;
		memcpy(_send_buf, packet, packet[0]);
	}
};

enum EVENT_TYPE { EV_RANDOM_MOVE };
enum S_STATE { ST_FREE, ST_ALLOC, ST_INGAME };

struct TIMER_EVENT {
	int obj_id;
	chrono::high_resolution_clock::time_point wakeup_time;
	EVENT_TYPE event_id;
	int target_id;

	constexpr bool operator < (const TIMER_EVENT& _Left) const
	{
		return (wakeup_time > _Left.wakeup_time);
	}
};

concurrency::concurrent_priority_queue<TIMER_EVENT> timer_queue;

class SESSION {
	OVER_EXP _recv_over;

public:
	mutex _s_lock;
	S_STATE _state;
	atomic_bool	_is_active;
	atomic_bool ai_active = false;
	int _id;
	int _uid;
	SOCKET _socket;
	short	x, y;
	char	_name[NAME_SIZE];
	int		_prev_remain;
	unordered_set <int> _view_list;
	mutex	_vl;
	long long		last_move_time;
	short _prev_x = 0, _prev_y = 0;
	lua_State* _L;
	mutex	_ll;

public:
	SESSION()
	{
		_id = -1;
		_socket = 0;
		x = y = 0;
		_name[0] = 0;
		_state = ST_FREE;
		_prev_remain = 0;
	}

	~SESSION() {}

	void do_recv()
	{
		DWORD recv_flag = 0;
		memset(&_recv_over._over, 0, sizeof(_recv_over._over));
		_recv_over._wsabuf.len = BUF_SIZE - _prev_remain;
		_recv_over._wsabuf.buf = _recv_over._send_buf + _prev_remain;
		WSARecv(_socket, &_recv_over._wsabuf, 1, 0, &recv_flag,
			&_recv_over._over, 0);
	}

	void do_send(void* packet)
	{
		OVER_EXP* sdata = new OVER_EXP{ reinterpret_cast<char*>(packet) };
		WSASend(_socket, &sdata->_wsabuf, 1, 0, 0, &sdata->_over, 0);
	}
	void send_login_info_packet()
	{
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
	void send_chat_packet(int c_id, const char* mess);
	void send_remove_player_packet(int c_id)
	{
		_vl.lock();
		if (_view_list.count(c_id))
			_view_list.erase(c_id);
		else {
			_vl.unlock();
			return;
		}
		_vl.unlock();
		SC_REMOVE_OBJECT_PACKET p;
		p.id = c_id;
		p.size = sizeof(p);
		p.type = SC_REMOVE_OBJECT;
		do_send(&p);
	}

	void update_cell()
	{
		int old_cx = _prev_x / CELL_SIZE;
		int old_cy = _prev_y / CELL_SIZE;
		int new_cx = x / CELL_SIZE;
		int new_cy = y / CELL_SIZE;

		if (old_cx == new_cx && old_cy == new_cy) return;

		{
			lock_guard<mutex> lg(spatial_lock[old_cx][old_cy]);
			spatial_map[old_cx][old_cy].erase(_id);
		}

		{
			lock_guard<mutex> lg(spatial_lock[new_cx][new_cy]);
			spatial_map[new_cx][new_cy].insert(_id);
		}

		_prev_x = x;
		_prev_y = y;
	}
};

HANDLE h_iocp;
concurrency::concurrent_unordered_map<long long, shared_ptr<SESSION>> clients;

// NPC 구현 첫번째 방법
//  NPC클래스를 별도 제작, NPC컨테이너를 따로 생성한다.
//  장점 : 깔끔하다, 군더더기가 없다.
//  단점 : 플레이어와 NPC가 따로논다. 똑같은 역할을 수행하는 함수를 여러개씩 중복 작성해야 한다.
//         예) bool can_see(int from, int to)
//                 => bool can_see_p2p()
//				    bool can_see_p2n()
//					bool can_see_n2n()

// NPC 구현 두번째 방법  <===== 실습에서 사용할 방법.
//   clients 컨테이너에 NPC도 추가한다.
//   장점 : 플레이어와 NPC를 동일하게 취급할 수 있어서, 프로그래밍 작성 부하가 줄어든다.
//   단점 : 사용하지 않는 멤버들로 인한 메모리 낭비.

// NPC 구현 세번째 방법  (실제로 많이 사용되는 방법)
//   클래스 상속기능을 사용한다.
//     SESSION은 NPC클래스를 상속받아서 네트워크 관련 기능을 추가한 형태로 정의한다.
//       clients컨테이너를 objects컨테이너로 변경하고, 컨테이너는 NPC의 pointer를 저장한다.
//      장점 : 메모리 낭비가 없다, 함수의 중복작성이 필요없다.
//          (포인터로 관리되므로 player id의 중복사용 방지를 구현하기 쉬워진다 => Data Race 방지를 위한 추가 구현이 필요)
//      단점 : 포인터가 사용되고, reinterpret_cast가 필요하다. (별로 단점이 안니다).

SOCKET g_s_socket, g_c_socket;
OVER_EXP g_a_over;

//void wakeup()
//{
//	if (false == _is_active) {
//		_is_active = true;
//		timer_lock.lock();
//		timer_queue.emplace(TIMER_EVENT{ _id, high_resolution_clock::now() + 1s, EV_MOVE, 0 });
//		timer_lock.unlock();
//	}
//
//}

void WakeUpNPC(int npc_id, int waker)
{
	OVER_EXP* exover = new OVER_EXP;
	exover->_comp_type = OP_AI_HELLO;
	exover->_ai_target_obj = waker;
	PostQueuedCompletionStatus(h_iocp, 1, npc_id, &exover->_over);

	if (clients[npc_id]->_is_active) return;
	bool old_state = false;
	if (false == atomic_compare_exchange_strong(&clients[npc_id]->_is_active, &old_state, true))
		return;
	TIMER_EVENT ev{ npc_id, chrono::high_resolution_clock::now(), EV_RANDOM_MOVE, 0 };
	timer_queue.push(ev);
}

bool is_pc(int object_id)
{
	return object_id < MAX_USER;
}

bool is_npc(int object_id)
{
	return !is_pc(object_id);
}

bool can_see(const SESSION* from, const SESSION* to)
{
	const int dx = from->x - to->x;
	if (dx > VIEW_RANGE || dx < -VIEW_RANGE)
		return false;

	const int dy = from->y - to->y;
	return (dy >= -VIEW_RANGE && dy <= VIEW_RANGE);
}

bool can_see_fast(int from_id, int to_id)
{
	auto it_from = clients.find(from_id);
	if (it_from == clients.end()) return false;

	auto it_to = clients.find(to_id);
	if (it_to == clients.end()) return false;

	return can_see(it_from->second.get(), it_to->second.get());
}

void SESSION::send_move_packet(int c_id)
{
	auto it = clients.find(c_id);
	if (it == clients.end()) return;

	auto& other = it->second;

	SC_MOVE_OBJECT_PACKET p;
	p.id = c_id;
	p.size = sizeof(SC_MOVE_OBJECT_PACKET);
	p.type = SC_MOVE_OBJECT;
	p.x = other->x;
	p.y = other->y;
	p.move_time = other->last_move_time;
	do_send(&p);
}

void SESSION::send_add_player_packet(int c_id)
{
	auto it = clients.find(c_id);
	if (it == clients.end()) return;
	auto& other = it->second;

	SC_ADD_OBJECT_PACKET add_packet;
	add_packet.id = c_id;
	strcpy_s(add_packet.name, other->_name);
	add_packet.x = other->x;
	add_packet.y = other->y;
	add_packet.uid = other->_uid;
	add_packet.size = sizeof(add_packet);
	add_packet.type = SC_ADD_OBJECT;

	_vl.lock();
	_view_list.insert(c_id);
	_vl.unlock();
	do_send(&add_packet);
}

void SESSION::send_chat_packet(int p_id, const char* mess)
{
	SC_CHAT_PACKET packet;
	packet.id = p_id;
	packet.size = sizeof(packet);
	packet.type = SC_CHAT;
	strcpy_s(packet.mess, mess);
	do_send(&packet);
}

long long get_new_client_id()
{
	while (true) {
		long long id = g_next_user_id.fetch_add(1);
		if (id >= MAX_USER) return -1;
		if (clients.find(id) == clients.end()) return id;
	}
}

void disconnect(int c_id)
{
	auto it = clients.find(c_id);
	if (it == clients.end()) return;
	shared_ptr<SESSION> self = it->second;

	self->_vl.lock();
	unordered_set<int> vl = self->_view_list;
	self->_vl.unlock();

	for (int p_id : vl) {
		if (is_npc(p_id)) continue;
		auto pit = clients.find(p_id);
		if (pit == clients.end()) continue;

		shared_ptr<SESSION> pl = pit->second;
		{
			lock_guard<mutex> ll(pl->_s_lock);
			if (ST_INGAME != pl->_state) continue;
		}
		if (pl->_id == c_id) continue;
		pl->send_remove_player_packet(c_id);
	}

	store_user_position(self->_uid, self->x, self->y);

	closesocket(self->_socket);

	lock_guard<mutex> ll(self->_s_lock);
	self->_state = ST_FREE;
}

void process_packet(int c_id, char* packet)
{
	auto it = clients.find(c_id);
	if (it == clients.end()) return;

	shared_ptr<SESSION> self = it->second;

	switch (packet[1]) {
	case CS_LOGIN: {
		CS_LOGIN_PACKET* p = reinterpret_cast<CS_LOGIN_PACKET*>(packet);

		if (!check_user_exists(p->id)) {
			SC_LOGIN_FAIL_PACKET fail;
			fail.size = sizeof(fail);
			fail.type = SC_LOGIN_FAIL;
			self->do_send(&fail);
			disconnect(c_id);
			return;
		}

		short x{}, y{};
		get_user_position(p->id, x, y);
		strcpy_s(self->_name, p->name);
		{
			lock_guard<mutex> ll{ self->_s_lock };
			self->x = x;
			self->y = y;
			self->_state = ST_INGAME;
			self->_uid = p->id;
		}
		self->send_login_info_packet();

		auto self = clients[c_id];

		for (auto& [id, pl] : clients) {
			if (id == c_id) continue;

			if (pl->_state != ST_INGAME) continue;

			if (!can_see(self.get(), pl.get())) continue;

			if (is_pc(id)) {
				pl->_vl.lock();
				pl->_view_list.insert(c_id);
				pl->_vl.unlock();

				pl->send_add_player_packet(c_id);
			}
			else {
				WakeUpNPC(id, c_id);
			}

			self->_vl.lock();
			self->_view_list.insert(id);
			self->_vl.unlock();

			self->send_add_player_packet(id);
		}
		break;
	}

	case CS_MOVE: {
		CS_MOVE_PACKET* p = reinterpret_cast<CS_MOVE_PACKET*>(packet);
		self->last_move_time = p->move_time;

		short x = self->x;
		short y = self->y;

		switch (p->direction) {
		case 0: if (y > 0) y--; break;
		case 1: if (y < W_HEIGHT - 1) y++; break;
		case 2: if (x > 0) x--; break;
		case 3: if (x < W_WIDTH - 1) x++; break;
		}

		self->x = x;
		self->y = y;
		self->update_cell();

		unordered_set<int> near_list;

		self->_vl.lock();
		unordered_set<int> old_vlist = self->_view_list;
		self->_vl.unlock();

		auto self = clients[c_id];
		for (auto& [id, cl] : clients) {
			if (cl->_state != ST_INGAME) continue;
			if (id == c_id) continue;
			if (can_see(self.get(), cl.get()))
				near_list.insert(id);
		}

		self->send_move_packet(c_id);

		for (int id : near_list) {
			auto& cpl = clients[id];
			if (is_pc(id)) {
				cpl->_vl.lock();
				if (cpl->_view_list.count(c_id)) {
					cpl->_vl.unlock();
					cpl->send_move_packet(c_id);
				}
				else {
					cpl->_vl.unlock();
					cpl->send_add_player_packet(c_id);
				}
			}
			else {
				WakeUpNPC(id, c_id);
			}

			if (old_vlist.count(id) == 0)
				self->send_add_player_packet(id);
		}

		for (int id : old_vlist) {
			if (near_list.count(id) == 0) {
				self->send_remove_player_packet(id);
				if (is_pc(id))
					clients[id]->send_remove_player_packet(c_id);
			}
		}
		break;
	}
	}
}


void do_npc_random_move(int npc_id)
{
	shared_ptr<SESSION> npc = clients[npc_id];
	unordered_set<int> old_vl;

	int cx = npc->x / CELL_SIZE;
	int cy = npc->y / CELL_SIZE;

	for (int dx = -1; dx <= 1; ++dx) {
		for (int dy = -1; dy <= 1; ++dy) {
			int nx = cx + dx;
			int ny = cy + dy;
			if (nx < 0 || ny < 0 || nx >= GRID_W || ny >= GRID_H) continue;

			lock_guard<mutex> lg(spatial_lock[nx][ny]);
			for (int id : spatial_map[nx][ny]) {
				if (id == npc->_id) continue;
				auto& obj = clients[id];
				if (obj->_state != ST_INGAME) continue;
				if (is_npc(obj->_id)) continue;
				if (can_see(npc.get(), obj.get()))
					old_vl.insert(id);
			}
		}
	}

	int x = npc->x;
	int y = npc->y;
	switch (rand() % 4) {
	case 0: if (x < (W_WIDTH - 1)) x++; break;
	case 1: if (x > 0) x--; break;
	case 2: if (y < (W_HEIGHT - 1)) y++; break;
	case 3:if (y > 0) y--; break;
	}
	npc->x = x;
	npc->y = y;
	npc->update_cell();

	unordered_set<int> new_vl;

	cx = npc->x / CELL_SIZE;
	cy = npc->y / CELL_SIZE;

	for (int dx = -1; dx <= 1; ++dx) {
		for (int dy = -1; dy <= 1; ++dy) {
			int nx = cx + dx;
			int ny = cy + dy;
			if (nx < 0 || ny < 0 || nx >= GRID_W || ny >= GRID_H) continue;

			lock_guard<mutex> lg(spatial_lock[nx][ny]);
			for (int id : spatial_map[nx][ny]) {
				if (id == npc->_id) continue;
				auto& obj = clients[id];
				if (obj->_state != ST_INGAME) continue;
				if (is_npc(obj->_id)) continue;
				if (can_see(npc.get(), obj.get()))
					new_vl.insert(id);
			}
		}
	}

	for (auto pl_id : new_vl) {
		if (0 == old_vl.count(pl_id)) {
			clients[pl_id]->send_add_player_packet(npc->_id);
		}
		else {
			clients[pl_id]->send_move_packet(npc->_id);
		}
	}

	for (auto pl_id : old_vl) {
		if (0 == new_vl.count(pl_id)) {
			auto& player = clients[pl_id];
			player->_vl.lock();
			if (player->_view_list.count(npc->_id)) {
				player->_vl.unlock();
				player->send_remove_player_packet(npc->_id);
			}
			else {
				player->_vl.unlock();
			}
		}
	}

	using namespace chrono;
	long long current_time = duration_cast<milliseconds>
		(system_clock::now().time_since_epoch()).count();

	clients[npc_id]->last_move_time = current_time;
	
	if (npc->ai_active) {
		clients[npc_id]->_ll.lock();
		lua_getglobal(npc->_L, "event_npc_move");
		if (lua_pcall(npc->_L, 0, 0, 0) != LUA_OK) {
			cerr << "Lua error: " << lua_tostring(npc->_L, -1) << endl;
			lua_pop(npc->_L, 1);
		}
		npc->ai_active = false;
		clients[npc_id]->_ll.unlock();
	}
	
}

void worker_thread(HANDLE h_iocp)
{
	while (true) {
		DWORD num_bytes;
		ULONG_PTR key;
		WSAOVERLAPPED* over = nullptr;
		BOOL ret = GetQueuedCompletionStatus(h_iocp, &num_bytes, &key, &over, INFINITE);
		OVER_EXP* ex_over = reinterpret_cast<OVER_EXP*>(over);
		if (FALSE == ret) {
			if (ex_over->_comp_type == OP_ACCEPT) cout << "Accept Error";
			else {
				cout << "GQCS Error on client[" << key << "]\n";
				disconnect(static_cast<int>(key));
				if (ex_over->_comp_type == OP_SEND) delete ex_over;
				continue;
			}
		}

		if ((0 == num_bytes) && ((ex_over->_comp_type == OP_RECV) || (ex_over->_comp_type == OP_SEND))) {
			disconnect(static_cast<int>(key));
			if (ex_over->_comp_type == OP_SEND) delete ex_over;
			continue;
		}

		switch (ex_over->_comp_type) {
		case OP_ACCEPT: {
			int client_id = get_new_client_id();
			if (client_id != -1) {
				auto new_session = make_shared<SESSION>();
				{
					lock_guard<mutex> ll(new_session->_s_lock);
					new_session->_state = ST_ALLOC;
				}
				new_session->x = 0;
				new_session->y = 0;
				new_session->_id = client_id;
				new_session->_name[0] = 0;
				new_session->_prev_remain = 0;
				new_session->_socket = g_c_socket;
				clients[client_id] = new_session;
				CreateIoCompletionPort(reinterpret_cast<HANDLE>(g_c_socket), h_iocp, client_id, 0);
				new_session->do_recv();
				g_c_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
			}
			else {
				cout << "Max user exceeded.\n";
			}
			ZeroMemory(&g_a_over._over, sizeof(g_a_over._over));
			int addr_size = sizeof(SOCKADDR_IN);
			AcceptEx(g_s_socket, g_c_socket, g_a_over._send_buf, 0, addr_size + 16, addr_size + 16, 0, &g_a_over._over);
			break;
		}
		case OP_RECV: {
			int remain_data = num_bytes + clients[key]->_prev_remain;
			char* p = ex_over->_send_buf;
			while (remain_data > 0) {
				int packet_size = p[0];
				if (packet_size <= remain_data) {
					process_packet(static_cast<int>(key), p);
					p = p + packet_size;
					remain_data = remain_data - packet_size;
				}
				else break;
			}
			clients[key]->_prev_remain = remain_data;
			if (remain_data > 0) {
				memcpy(ex_over->_send_buf, p, remain_data);
			}
			clients[key]->do_recv();
			break;
		}
		case OP_SEND:
			delete ex_over;
			break;
		case OP_NPC_MOVE: {
			using namespace chrono;
			int npc_id = static_cast<int>(key);
			do_npc_random_move(npc_id);
			timer_queue.push(TIMER_EVENT{ npc_id, high_resolution_clock::now() + 1s, EV_RANDOM_MOVE, 0 });
			delete ex_over;
			break;
		}
		case OP_AI_HELLO: {
			clients[key]->_ll.lock();
			auto L = clients[key]->_L;
			lua_getglobal(L, "event_player_move");
			lua_pushnumber(L, ex_over->_ai_target_obj);
			lua_pcall(L, 1, 0, 0);
			//lua_pop(L, 1);
			clients[key]->_ll.unlock();
			clients[key]->ai_active = true;
			delete ex_over;
			break;
		}					

		}
	}
}

int API_get_x(lua_State* L)
{
	int user_id =
		(int)lua_tointeger(L, -1);
	lua_pop(L, 2);
	int x = clients[user_id]->x;
	lua_pushnumber(L, x);
	return 1;
}

int API_get_y(lua_State* L)
{
	int user_id =
		(int)lua_tointeger(L, -1);
	lua_pop(L, 2);
	int y = clients[user_id]->y;
	lua_pushnumber(L, y);
	return 1;
}

int API_SendMessage(lua_State* L)
{
	int my_id = (int)lua_tointeger(L, -3);
	int user_id = (int)lua_tointeger(L, -2);
	char* mess = (char*)lua_tostring(L, -1);

	lua_pop(L, 4);

	clients[user_id]->send_chat_packet(my_id, mess);
	return 0;
}

void InitializeNPC()
{
	using namespace chrono;
	cout << "NPC initialize begin.\n";
	for (int i = MAX_USER; i < MAX_USER + MAX_NPC; ++i) {
		auto npc = make_shared<SESSION>();
		npc->x = rand() % W_WIDTH;
		npc->y = rand() % W_HEIGHT;
		npc->_id = i;
		sprintf_s(npc->_name, "NPC%d", i);
		npc->_state = ST_INGAME;
		npc->_is_active = false;
		int cx = npc->x / CELL_SIZE;
		int cy = npc->y / CELL_SIZE;
		spatial_map[cx][cy].insert(npc->_id);
		clients[i] = npc;

		auto L = clients[i]->_L = luaL_newstate();
		luaL_openlibs(L);
		luaL_loadfile(L, "npc.lua");
		lua_pcall(L, 0, 0, 0);

		lua_getglobal(L, "set_uid");
		lua_pushnumber(L, i);
		lua_pcall(L, 1, 0, 0);
		// lua_pop(L, 1);// eliminate set_uid from stack after call

		lua_register(L, "API_SendMessage", API_SendMessage);
		lua_register(L, "API_get_x", API_get_x);
		lua_register(L, "API_get_y", API_get_y);
	}
	cout << "NPC initialize end.\n";
}

void do_timer()
{
	while (true) {
		TIMER_EVENT ev;
		auto current_time = chrono::high_resolution_clock::now();
		if (true == timer_queue.try_pop(ev)) {
			if (ev.wakeup_time > current_time) {
				timer_queue.push(ev);
				this_thread::sleep_for(1ms);
				continue;
			}
			switch (ev.event_id) {
			case EV_RANDOM_MOVE:
				OVER_EXP* ov = new OVER_EXP;
				ov->_comp_type = OP_NPC_MOVE;
				PostQueuedCompletionStatus(h_iocp, 1, ev.obj_id, &ov->_over);
				break;
			}
			continue;

		}

		this_thread::sleep_for(1ms);
	}
}

int main()
{
	WSADATA WSAData;
	WSAStartup(MAKEWORD(2, 2), &WSAData);
	g_s_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	SOCKADDR_IN server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(PORT_NUM);
	server_addr.sin_addr.S_un.S_addr = INADDR_ANY;
	bind(g_s_socket, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr));
	listen(g_s_socket, SOMAXCONN);
	SOCKADDR_IN cl_addr;
	int addr_size = sizeof(cl_addr);

	InitializeNPC();

	h_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 0);
	CreateIoCompletionPort(reinterpret_cast<HANDLE>(g_s_socket), h_iocp, 9999, 0);
	g_c_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	g_a_over._comp_type = OP_ACCEPT;
	AcceptEx(g_s_socket, g_c_socket, g_a_over._send_buf, 0, addr_size + 16, addr_size + 16, 0, &g_a_over._over);

	vector <thread> worker_threads;
	int num_threads = std::thread::hardware_concurrency();
	for (int i = 0; i < num_threads; ++i)
		worker_threads.emplace_back(worker_thread, h_iocp);

	thread timer_thread{ do_timer };
	timer_thread.join();
	for (auto& th : worker_threads)
		th.join();
	closesocket(g_s_socket);
	WSACleanup();
}

bool check_user_exists(int user_id) {
	SQLHENV henv = nullptr;
	SQLHDBC hdbc = nullptr;
	SQLHSTMT hstmt = nullptr;
	SQLRETURN retcode;
	int exists = 0;

	cout << "[DEBUG] check_user_exists(" << user_id << ") 호출됨\n";

	do {
		// 환경 핸들
		if (SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv) != SQL_SUCCESS) break;
		if (SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0) != SQL_SUCCESS) break;

		// 연결 핸들
		if (SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc) != SQL_SUCCESS) break;

		cout << "[DEBUG] DB 연결 시도 중...\n";
		SQLSetConnectAttr(hdbc, SQL_LOGIN_TIMEOUT, (SQLPOINTER)5, 0);
		retcode = SQLConnect(hdbc, (SQLWCHAR*)L"2025_Game_2020182029", SQL_NTS, (SQLWCHAR*)NULL, 0, NULL, 0);
		if (!SQL_SUCCEEDED(retcode)) {
			cout << "[ERROR] DB 연결 실패\n";
			break;
		}
		cout << "[DEBUG] DB 연결 성공\n";

		// 문장 핸들
		if (SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt) != SQL_SUCCESS) break;

		cout << "[DEBUG] 파라미터 바인딩 중...\n";
		SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &user_id, 0, nullptr);

		cout << "[DEBUG] 쿼리 실행 중...\n";
		retcode = SQLPrepare(hstmt, (SQLWCHAR*)L"SELECT dbo.user_exists(?)", SQL_NTS);
		if (!SQL_SUCCEEDED(retcode)) {
			cout << "[ERROR] SQLPrepare 실패\n";
			break;
		}

		if (SQLExecute(hstmt) != SQL_SUCCESS) {
			cout << "[ERROR] SQLExecute 실패\n";
			break;
		}

		if (SQLBindCol(hstmt, 1, SQL_C_LONG, &exists, 0, nullptr) != SQL_SUCCESS) break;
		if (SQLFetch(hstmt) != SQL_SUCCESS) {
			cout << "[ERROR] SQLFetch 실패\n";
			break;
		}

		cout << "[DEBUG] user_exists 결과: " << exists << "\n";
	} while (false); // 단일 루프로 break 사용

	// 리소스 해제
	if (hstmt) SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
	if (hdbc) {
		SQLDisconnect(hdbc);
		SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
	}
	if (henv) SQLFreeHandle(SQL_HANDLE_ENV, henv);

	return exists == 1;
}

bool get_user_position(int user_id, short& x, short& y)
{
	SQLHENV henv = nullptr;
	SQLHDBC hdbc = nullptr;
	SQLHSTMT hstmt = nullptr;
	SQLRETURN retcode;

	x = -1; y = -1; // 기본값

	do {
		if (SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv) != SQL_SUCCESS) break;
		if (SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0) != SQL_SUCCESS) break;
		if (SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc) != SQL_SUCCESS) break;

		SQLSetConnectAttr(hdbc, SQL_LOGIN_TIMEOUT, (SQLPOINTER)5, 0);
		retcode = SQLConnect(hdbc, (SQLWCHAR*)L"2025_Game_2020182029", SQL_NTS, NULL, 0, NULL, 0);
		if (!SQL_SUCCEEDED(retcode)) {
			std::cout << "[ERROR] DB 연결 실패 (get_user_position)\n";
			break;
		}

		if (SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt) != SQL_SUCCESS) break;

		// 쿼리 준비 및 파라미터 바인딩
		SQLPrepare(hstmt, (SQLWCHAR*)L"SELECT x, y FROM get_user_position(?)", SQL_NTS);
		SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &user_id, 0, NULL);

		if (SQLExecute(hstmt) != SQL_SUCCESS) {
			std::cout << "[ERROR] SQLExecute 실패\n";
			break;
		}

		SQLINTEGER x_val = 0, y_val = 0;
		SQLBindCol(hstmt, 1, SQL_C_LONG, &x_val, 0, NULL);
		SQLBindCol(hstmt, 2, SQL_C_LONG, &y_val, 0, NULL);

		if (SQLFetch(hstmt) == SQL_SUCCESS) {
			x = static_cast<short>(x_val);
			y = static_cast<short>(y_val);
			std::cout << "[DEBUG] get_user_position 성공: (" << x << ", " << y << ")\n";
			SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
			SQLDisconnect(hdbc);
			SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
			SQLFreeHandle(SQL_HANDLE_ENV, henv);
			return true;
		}
		else {
			std::cout << "[DEBUG] get_user_position: 해당 ID 없음\n";
		}

	} while (false);

	if (hstmt) SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
	if (hdbc) {
		SQLDisconnect(hdbc);
		SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
	}
	if (henv) SQLFreeHandle(SQL_HANDLE_ENV, henv);

	return false;
}

void store_user_position(int user_id, short x, short y)
{
	SQLHENV henv = nullptr;
	SQLHDBC hdbc = nullptr;
	SQLHSTMT hstmt = nullptr;
	SQLRETURN ret;

	do {
		// 환경 핸들
		if (SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv) != SQL_SUCCESS) break;
		if (SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0) != SQL_SUCCESS) break;

		// 연결 핸들
		if (SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc) != SQL_SUCCESS) break;

		SQLSetConnectAttr(hdbc, SQL_LOGIN_TIMEOUT, (SQLPOINTER)5, 0);
		ret = SQLConnect(hdbc, (SQLWCHAR*)L"2025_Game_2020182029", SQL_NTS, nullptr, 0, nullptr, 0);
		if (!SQL_SUCCEEDED(ret)) {
			std::cout << "[ERROR] DB 연결 실패 (store_user_position)\n";
			break;
		}

		// 스테이트먼트 핸들
		if (SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt) != SQL_SUCCESS) break;

		// SQL 준비 및 바인딩
		SQLPrepare(hstmt, (SQLWCHAR*)L"EXEC store_user_pos ?, ?, ?", SQL_NTS);
		SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &user_id, 0, nullptr);
		SQLBindParameter(hstmt, 2, SQL_PARAM_INPUT, SQL_C_SHORT, SQL_SMALLINT, 0, 0, &x, 0, nullptr);
		SQLBindParameter(hstmt, 3, SQL_PARAM_INPUT, SQL_C_SHORT, SQL_SMALLINT, 0, 0, &y, 0, nullptr);

		ret = SQLExecute(hstmt);
		if (!SQL_SUCCEEDED(ret)) {
			std::cout << "[ERROR] store_user_pos 실행 실패\n";
			break;
		}

		std::cout << "[DEBUG] 위치 저장 성공: user_id=" << user_id << ", x=" << x << ", y=" << y << "\n";

	} while (false);

	// 자원 해제
	if (hstmt) SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
	if (hdbc) {
		SQLDisconnect(hdbc);
		SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
	}
	if (henv) SQLFreeHandle(SQL_HANDLE_ENV, henv);
}
