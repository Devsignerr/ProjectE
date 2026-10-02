#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"

#include <vector>

// 3D 디버그 선 하나 (월드 cm). Color = 선형 RGBA8 (메모리 순서 R, G, B, A — 정점 형식 그대로)
struct FDebugLine
{
	FVector3 Start;
	FVector3 End;
	uint32   Color      = 0xFFFFFFFFu;
	float    Remaining  = 0.0f; // 남은 시간 (초). Tick에서 빼고 0 이하가 되면 지운다
	bool     bDepthTest = true; // false = 씬에 가려지지 않고 항상 위에
};

// 3D 디버그 그리기 저장소 (Phase 41-3, CPU 전용 — GPU 없이도 동작). 엔진 DLL 전역 하나(Get, 정의는 .cpp).
//   C++(엔진·게임 모듈): FDebugDraw::Get().DrawLine/DrawBox/DrawSphere/DrawArrow/DrawCapsule(..., 색, 지속 시간, 깊이 테스트)
//   Lua: Debug.DrawLine 등 (FScriptDebugDrawHooks → FGameWorld가 여기로 연결)
// 규칙
// - 색은 sRGB 0~1 (화면에 보이는 그대로, UI와 같음) → 저장할 때 선형 RGBA8로 바꾼다
// - 지속 시간(초): 0 = 한 번 그려지고 다음 Tick에 사라짐. Tick은 FGameWorld::TickGameplay 시작에 한 번 (게임 시간 —
//   에디터 일시정지 중에는 멈춰서 계속 보인다). 플레이 시작/끝(FGameWorld::BeginPlay/EndPlay)에 모두 지운다.
//   플레이 밖에서 그린 것은 Tick이 없으니 Clear할 때까지 남는다
// - 그리기는 앱이 FDebugDrawRenderer로 씬 렌더 뒤(톤매핑·TAA 뒤 오버레이 단계, 지터 없음)에 한다: 에디터 뷰포트(편집/플레이), 런타임.
//   GPU 없는 서버는 FGameWorld가 SetEnabled(false) → 그리기 호출은 무시된다
// - 상한 MaxLines (넘으면 새 선을 버리고 한 번 경고) — 프레임당 업로드 = 선 × 32바이트 (최대 1MB, 동적 업로드 버퍼 공유)
// - 메인 스레드 전용
class FDebugDraw
{
public:
	static constexpr size_t MaxLines        = 32768;
	static constexpr int32  DefaultSegments = 16; // 원 하나의 선분 수

	static FDebugDraw& Get();

	// sRGB 0~1 → 선형 RGBA8
	static uint32 PackColor(const FVector4& SrgbColor);

	void SetEnabled(bool bInEnabled);
	bool IsEnabled() const { return bEnabled; }

	void DrawLine(const FVector3& Start, const FVector3& End, const FVector4& Color = FVector4(1.0f, 1.0f, 1.0f, 1.0f), float Duration = 0.0f,
	              bool bDepthTest = true);
	// 화살표: 선 + 끝의 화살촉 (HeadSize <= 0이면 길이에 맞춰 자동)
	void DrawArrow(const FVector3& From, const FVector3& To, const FVector4& Color = FVector4(1.0f, 1.0f, 1.0f, 1.0f), float Duration = 0.0f,
	               bool bDepthTest = true, float HeadSize = 0.0f);
	// 상자: 가운데 + 반 크기 + 회전 (모서리 12개)
	void DrawBox(const FVector3& Center, const FVector3& HalfExtents, const FQuat& Rotation, const FVector4& Color = FVector4(1.0f, 1.0f, 1.0f, 1.0f),
	             float Duration = 0.0f, bool bDepthTest = true);
	// 구: 축 평면 원 3개
	void DrawSphere(const FVector3& Center, float Radius, const FVector4& Color = FVector4(1.0f, 1.0f, 1.0f, 1.0f), float Duration = 0.0f,
	                bool bDepthTest = true, int32 Segments = DefaultSegments);
	// 캡슐: 회전 전 +Z 축 (콜라이더/물리 질의와 같음), HalfHeight = 원기둥 절반
	void DrawCapsule(const FVector3& Center, float Radius, float HalfHeight, const FQuat& Rotation,
	                 const FVector4& Color = FVector4(1.0f, 1.0f, 1.0f, 1.0f), float Duration = 0.0f, bool bDepthTest = true,
	                 int32 Segments = DefaultSegments);

	// 수명 진행: 남은 시간을 줄이고 0 이하가 된 선을 지운다 (FGameWorld::TickGameplay 시작)
	void Tick(float DeltaSeconds);
	void Clear();

	const std::vector<FDebugLine>& GetLines() const { return Lines; }

private:
	void AddLine(const FVector3& Start, const FVector3& End, uint32 Color, float Duration, bool bDepthTest);
	void AddCircle(const FVector3& Center, const FVector3& AxisX, const FVector3& AxisY, float Radius, float StartAngle, float EndAngle, int32 Segments,
	               uint32 Color, float Duration, bool bDepthTest);

	std::vector<FDebugLine> Lines;
	bool                    bEnabled         = true;
	bool                    bOverflowWarned  = false;
};
