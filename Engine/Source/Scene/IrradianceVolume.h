#pragma once

#include "Core/Math/Math.h"

// 동적 GI 프로브 볼륨 (Phase 51 DDGI — 렌더링은 Renderer/DdgiRenderer.h, 식은 Renderer/DdgiMath.h):
//   상자(엔티티 월드 위치가 가운데, HalfExtents 반 크기, 월드 축 정렬 — 회전/스케일 무시)를 Spacing 칸으로 나눠 칸 가운데에 프로브를 놓는다
//   (상자 = 셰이딩 범위 — 방 벽·바닥 표면이 상자 안 FadeDistance보다 깊이 들어오게 방보다 조금 크게). 프레임마다 프로브 일부를 레이 트레이싱(인라인 RayQuery)으로 갱신해 조도 + 거리 모멘트(팔면체)를
//   시간 누적하고, 메시/지형/폴리지/반투명의 간접 확산광을 하늘 IBL 조도 대신 볼륨 표본(삼선형 + 체비셰프 가시성)으로 바꾼다.
//   경계 안쪽 FadeDistance에서 하늘 IBL 조도로 섞는다. 반사(스펙큘러)는 그대로 IBL/캡처/SSR/RT 반사. 레이 트레이싱(DXR 1.1)이 없으면 꺼진다
//   겹치면 Priority가 큰 볼륨이 먼저(같으면 작은 상자), 앞에서부터 남은 비중을 채운다
struct FIrradianceVolumeComponent
{
	FVector3 HalfExtents       = FVector3(500.0f, 500.0f, 200.0f); // cm 상자 반 크기
	float    Spacing           = 100.0f; // cm 목표 프로브 간격 (축마다 상자를 나누어떨어지게 맞춘다 — 축 2~32개)
	float    Intensity         = 1.0f;   // 간접 확산 배율
	float    FadeDistance      = 50.0f;  // cm 경계 안쪽 페이드 (바깥은 하늘 IBL 조도)
	float    NormalBias        = 10.0f;  // cm 셰이딩 점을 법선 쪽으로 (벽 뒤 프로브 누수·자기 가림 방지)
	float    ViewBias          = 30.0f;  // cm 셰이딩 점을 카메라 쪽으로
	float    Hysteresis        = 0.97f;  // 누적에서 이전 값 비중 (클수록 안정·느림, 급변하면 자동으로 빠르게)
	int32    RaysPerProbe      = 160;    // 프로브당 광선 (고정 32개 = 재배치·분류, 나머지 = 조도·거리)
	int32    ProbeUpdateBudget = 0;      // 프레임당 이 볼륨에서 갱신할 프로브 수 (0 = 전부, r.DDGI.ProbeBudget이 전체 상한)
	float    MaxRayDistance    = 10000.0f; // cm 광선 최대 거리 (빗나가면 하늘)
	bool     bRelocation       = true;   // 벽에 묻히거나 너무 가까운 프로브를 칸 안(간격 45%)에서 밀어낸다
	bool     bClassification   = true;   // 뒷면을 많이 본 프로브(벽 속)는 셰이딩에서 뺀다 (실내 누수 방지)
	int32    Priority          = 0;      // 겹치면 큰 값이 먼저
	int32    DebugProbes       = 0;      // 프로브 구 표시: 0 없음, 1 조도, 2 거리(평균), 3 상태 (초록 활성 / 빨강 비활성 / 파랑 아직 없음)
	float    DebugProbeRadius  = 8.0f;   // cm
};

void RegisterIrradianceVolumeTypes();
