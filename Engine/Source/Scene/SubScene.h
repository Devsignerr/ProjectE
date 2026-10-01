#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"

#include <string>

// 서브 씬 스트리밍 (Phase 31-2). 메인 씬이 다른 .escene을 루트 엔티티 하나 아래로 불러오고 내린다.
// 불러오기/내리기·판정은 FGameWorld(World/GameWorldStreaming.cpp 머리 주석), 파일 → 엔티티는 FSceneSerializer::ParseFile/AppendDocument

// 스트리밍 볼륨: 스트리밍 기준이 상자 안에 들어오면 SubScene을 불러오고, 모든 기준이 상자 + UnloadMargin 밖이면 내린다.
// 상자 = 이 엔티티의 월드 위치 ± HalfExtents (축 정렬, 회전·스케일 무시). 서버/Standalone이 판정하고 클라이언트는 서버를 따른다
struct FSubSceneVolumeComponent
{
	std::string SubScene;                                       // Content 기준 .escene (작성한 좌표 그대로 불러온다)
	FVector3    HalfExtents  = FVector3(1000.0f, 1000.0f, 500.0f); // cm
	float       UnloadMargin = 500.0f;                          // cm — 경계에서 불러오기/내리기가 반복되지 않게
};

// 스트리밍 기준 표시: 이 엔티티 위치로 볼륨을 판정한다. 주 카메라와 캐릭터 이동 컴포넌트(플레이어 폰)는 표시 없이도 기준이다
struct FStreamingSourceComponent
{
	uint8 Unused = 0;
};

// 런타임 전용: 불러온 서브 씬의 루트 (리플렉션 미등록 — 저장/복제/플레이 복제 대상 아님. 루트에 FTransientComponent도 붙여 저장에서 뺀다)
struct FSubSceneRootComponent
{
	std::string Asset;
	uint32      InstanceId = 0; // 서버가 정한 번호 (NetId 구간 = NetReplication::GetSubSceneNetIdBase)
};
