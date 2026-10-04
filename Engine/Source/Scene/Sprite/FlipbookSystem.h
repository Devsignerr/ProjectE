#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"

class FScene;

// FFlipbookComponent를 가진 엔티티를 매 프레임 진행한다 (FAnimationSystem처럼 정적 함수 — 상태는 컴포넌트 Runtime에만).
//   1) 플립북 경로/라이브러리 세대가 바뀌면 다시 읽는다 (경로가 바뀌면 StartTime부터 다시, 세대만 바뀌면 재생 위치 유지)
//   2) bPlaying이면 Time += dt × Speed, FlipbookMath::CollectEvents로 이벤트를 Runtime.PendingEvents에 쌓는다 (매 갱신 비우고 채움 —
//      첫 갱신은 시작 프레임 이벤트도). Once는 끝(정방향 T / 역방향 0)에 닿으면 bFinished + 그 자리에 멈춘다. Loop/PingPong은 Time을 주기로 감싼다
//   3) 같은 엔티티의 FSpriteComponent::Runtime.FlipbookAtlas/FlipbookSliceIndex에 현재 프레임을 쓴다 (플립북이 없거나 읽기 실패면 비움)
// 호출 위치: 게임 월드 표시 틱(TickPresentation, 애니메이션 단계 옆 — 트랜스폼은 쓰지 않는다). 연결은 World 쪽에서 (Phase 56 머지 때)
// 이벤트 배달(Lua OnFlipbookEvent_<이름>, 게임 모듈)도 World가 PendingEvents를 읽어 한다
class FFlipbookSystem
{
public:
	static void Update(FScene& Scene, float DeltaSeconds);

	// 처음부터 다시 (StartTime, 시작 프레임 이벤트 다시 냄, bFinished 해제). 컴포넌트가 없으면 false
	static bool Restart(FScene& Scene, FEntity Entity);
	// 재생 위치 지정 (초, 이벤트 없음 — 스크럽). 컴포넌트가 없으면 false
	static bool SetTime(FScene& Scene, FEntity Entity, float Seconds);
	// 현재 프레임 번호 (없으면 -1)
	static int32 GetFrame(FScene& Scene, FEntity Entity);
};
