# 05. NPC & Timer — Server-side AI Event

IOCP + AOI 구조에 **대량 NPC와 Timer 기반 행동 처리**를 추가한 단계입니다.

NPC마다 별도 Thread를 생성하지 않고, 행동 실행 시점을 Timer Queue에 예약한 뒤 시간이 된 이벤트를 IOCP Queue에 전달하여 기존 Worker Thread가 처리하도록 구성했습니다.

## NPC 관리

플레이어와 NPC를 별도의 자료구조로 분리하지 않고 하나의 `clients` 컨테이너에 함께 저장합니다.

```text
clients
 ├─ 0 ~ MAX_USER-1          : Player
 └─ MAX_USER ~ MAX_USER+N   : NPC
```

같은 `SESSION` 구조를 사용하기 때문에 위치, 시야 판정, View List, 패킷 전송 로직을 플레이어와 NPC가 공유할 수 있습니다.

## Timer Event 구조

```text
NPC Wake Up
    │
    ▼
Priority Queue
(wakeup_time)
    │
    ▼
Timer Thread
    │
    │ PostQueuedCompletionStatus
    ▼
IOCP Queue
    │
    ▼
Worker Thread
    │
    ▼
OP_NPC_MOVE
```

Timer Thread는 NPC의 실제 게임 로직을 직접 실행하지 않습니다.

실행 시간이 된 이벤트를 발견하면 `PostQueuedCompletionStatus()`를 이용해 `OP_NPC_MOVE` 작업을 IOCP에 넣고, 기존 Worker Thread가 `do_npc_random_move()`를 수행합니다.

NPC 이동이 끝나면 다음 이동 이벤트를 약 1초 뒤로 다시 예약합니다.

## NPC AOI

NPC 이동 시 전체 객체를 대상으로 패킷을 전송하지 않고 Spatial Map의 주변 Cell을 조사합니다.

NPC 이동 전후의 시야 목록을 비교해 플레이어에게 다음 패킷을 보냅니다.

- 새로 NPC를 보게 됨 → `ADD_OBJECT`
- 계속 보이는 NPC → `MOVE_OBJECT`
- NPC가 시야 밖으로 이동 → `REMOVE_OBJECT`

## 주요 코드

| 파일 | 내용 |
| --- | --- |
| [`AI_Timer.cpp`](./SERVER/SERVER/AI_Timer.cpp) | NPC, Timer Queue, IOCP 이벤트 및 Spatial Map |
| [`protocol.h`](./SERVER/SERVER/protocol.h) | Player/NPC 공용 객체 패킷 |
| [`stress_test`](./stress_test/) | 대량 접속 테스트 |

## 측정 자료

![NPC Timer 테스트](./타이머%20테스트.png)

## 성능 관점

NPC/Timer 단계부터는 네트워크 I/O 외에도 서버가 주기적으로 처리해야 할 게임 로직이 생깁니다.

활성 NPC가 늘어날수록 다음 비용이 추가됩니다.

```text
Timer Event
+ NPC 이동
+ 주변 Cell 탐색
+ View List 갱신
+ IOCP Worker 작업
```

따라서 순수 플레이어 IOCP/Sector 서버보다 처리 가능한 동시 접속 수가 낮아질 수 있으며, 이후 단계에서는 AI 로직의 유연한 변경을 위해 Lua Script를 결합합니다.

> Next: [06. Lua AI](../06_Lua/)
