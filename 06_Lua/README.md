# 06. Lua — Scriptable NPC AI

C++ 서버 코드에 직접 작성되어 있던 NPC 행동을 **Lua Script로 분리**하여 게임 서버 로직과 AI 정책을 분리한 단계입니다.

네트워크, IOCP, Timer, Spatial Map은 C++에서 처리하고, NPC가 플레이어를 만났을 때 어떤 행동을 할지는 `npc.lua`에서 결정합니다.

## 구조

```text
C++ Server
 ├─ Network / IOCP
 ├─ Timer
 ├─ Spatial Map
 ├─ NPC Position
 │
 └─ Lua API
      │
      ▼
   npc.lua
   ├─ event_player_move()
   └─ event_npc_move()
```

## Lua State

각 NPC Session은 자신의 `lua_State*`를 보유합니다.

NPC 초기화 과정에서:

1. `luaL_newstate()`로 Lua State 생성
2. Lua 기본 Library 초기화
3. `npc.lua` Load 및 실행
4. NPC ID를 `set_uid()`로 전달
5. C++ 함수를 Lua API로 등록

등록한 C++ API:

| Lua API | 역할 |
| --- | --- |
| `API_get_x(id)` | 객체의 X 좌표 조회 |
| `API_get_y(id)` | 객체의 Y 좌표 조회 |
| `API_SendMessage(from, to, msg)` | NPC가 플레이어에게 메시지 전송 |

## AI Event

### 플레이어가 NPC 주변에서 이동

C++에서 `OP_AI_HELLO` 이벤트를 IOCP Queue에 넣고 Worker Thread가 Lua의 다음 함수를 호출합니다.

```lua
event_player_move(player)
```

Lua Script는 플레이어와 NPC의 좌표를 확인하고 같은 위치에서 처음 만난 플레이어에게 `HELLO` 메시지를 보냅니다.

### NPC가 이동

NPC 이동이 끝난 뒤:

```lua
event_npc_move()
```

를 호출합니다.

`HELLO`를 전송했던 플레이어별 이동 횟수를 기록하고 NPC가 3번 이동하면 `BYE`를 보낸 뒤 목록에서 제거합니다.

## C++ ↔ Lua 호출 흐름

```text
Player Move
    │
    ▼
WakeUpNPC()
    │
    ▼
OP_AI_HELLO
    │
    ▼
Lua event_player_move()
    │
    ├─ API_get_x / API_get_y
    └─ API_SendMessage
```

Lua State는 여러 Worker Thread에서 동시에 접근하지 않도록 NPC별 `mutex`로 보호합니다.

Timer Queue는 이 단계에서 `concurrent_priority_queue`를 사용합니다.

## 주요 코드

| 파일 | 내용 |
| --- | --- |
| [`AI_Timer.cpp`](./SERVER/SERVER/AI_Timer.cpp) | C++ / Lua 연동, IOCP 및 Timer |
| [`npc.lua`](./SERVER/SERVER/npc.lua) | NPC 행동 Script |
| [`include`](./SERVER/SERVER/include/) | Lua Header |
| [`stress_test`](./stress_test/) | 부하 테스트 |

## 측정 자료

### NPC / Timer 단계

![과제 5 성능](./과제5%20성능.png)

### Lua 적용 단계

![과제 6 성능](./과제6%20성능.png)

### Lua AI 측정

![Lua AI 성능](./LUA_AI%20성능.png)

## Trade-off

Lua를 사용하면 NPC 행동을 C++ 서버를 다시 컴파일하지 않고 Script 중심으로 분리할 수 있다는 장점이 있습니다.

반면 현재 구현은 **NPC마다 별도의 Lua State를 생성**하고 C++ ↔ Lua 호출 및 Lua State Lock이 추가되기 때문에 NPC 수가 많아질수록 CPU와 메모리 비용이 증가할 수 있습니다.

이 단계에서는 성능뿐 아니라 **서버 핵심 로직과 콘텐츠/AI 로직을 분리하는 구조적 장단점**을 함께 확인했습니다.

> Next: [07. Database](../07_DB/)
