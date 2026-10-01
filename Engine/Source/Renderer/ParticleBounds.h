#pragma once

#include "Core/Math/Math.h"

#include <vector>

struct FParticle;
struct FParticleEmitter;
struct FParticleGpuStep;
struct FParticleValue;

// 파티클 이미터 경계 (렌더 컬링 / GPU 계산 생략 판정용, 순수 함수 — RendererTests의 ParticleBoundsTests)
//   CPU 이미터: 실제 입자 위치의 AABB + 입자 크기 여유 (ComputeCpuBounds)
//   GPU 이미터: 에셋의 "고정 경계"(FParticleEmitter::bFixedBounds, 이미터 로컬)가 있으면 그것, 없으면 모듈 설정으로 보수적 추정
//     반경 = 생성 모양 반경 + 최대 속력 × 최대 수명 + ½ × 최대 가속 × 최대 수명² + 렌더 크기 여유 (EstimateLocalRadius, 이미터 로컬 단위).
//     감속/충돌은 줄이기만 하므로 무시(보수적). 입력은 상수/무작위/곡선의 성분별 최대 절댓값.
//   새 모듈이 입자를 더 멀리 보내면(속도/힘) EstimateLocalRadius에 함께 추가한다.
namespace ParticleBounds
{
	// 동적 입력의 성분별 최대 절댓값 (상수 = |A|, 무작위 = max(|A|, |B|), 곡선 = 키 값들)
	FVector4 MaxAbs(const FParticleValue& Value);
	// 입자 최대 수명 (초기화 모듈이 없으면 FParticle 기본값 1초)
	float MaxLifetime(const FParticleEmitter& Emitter);
	// 입자 하나가 그려지는 반경 / 입자 크기(가로·세로 최대) 비 — 렌더러 종류별 최댓값.
	// 스프라이트 = √2/2 (회전), 메시 = MeshRadius / 100 (크기 X = 100cm 기준 배율), 리본 = 폭 배율 / 2.
	// 속도 정렬 스프라이트의 늘이기는 OutStretchSeconds (= VelocityStretch / 2, 입자 속력 × 이 값만큼 더)
	float RenderSizeFactor(const FParticleEmitter& Emitter, float MaxMeshRadius, float& OutStretchSeconds);
	// GPU 이미터 보수적 반경 (이미터 로컬 원점 기준, 로컬 단위)
	float EstimateLocalRadius(const FParticleEmitter& Emitter, float MaxMeshRadius);
	// CPU 입자 월드 경계: 위치 × LocalToWorld (로컬 공간 이미터) ± (크기 × SizeFactor × WorldScale + 속력 × StretchSeconds)
	FBox ComputeCpuBounds(const std::vector<FParticle>& Particles, const FMatrix4x4& LocalToWorld, float SizeFactor, float StretchSeconds);
	// 행렬 3x3의 최대 축 배율 상한 (행 길이 최댓값 — 회전 + 축 배율일 때 정확)
	float MaxAxisScale(const FMatrix4x4& Matrix);

	// 화면 밖 GPU 이미터의 미룬 계산 요청 정리 (FParticleRenderer 머리 주석의 규칙). Steps/EndTimes는 시간 순, 같은 개수.
	//   1) 뒤에서부터 시간 합이 Lifetime 이상이 되는 지점보다 오래된 요청을 버린다 (결과가 계속 계산한 것과 같다)
	//   2) MaxSteps를 넘으면 이웃 둘씩 합친다 (시간 합, 생성 구간 이어 붙임 — 생성 수는 MaxParticles까지, 이미터 값은 뒤 요청)
	void TrimDeferredSteps(std::vector<FParticleGpuStep>& Steps, std::vector<float>& EndTimes, float Lifetime, size_t MaxSteps, uint32 MaxParticles);
} // namespace ParticleBounds
