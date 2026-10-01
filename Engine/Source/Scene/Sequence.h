#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// 컷신 시퀀스 (언리얼 시퀀서식). 에셋 .esequence (JSON v1, Content 기준) = 길이 + 트랙 목록.
//   트랙 대상(Target)은 엔티티 이름 경로: "" = 재생 엔티티 자신, "Door" = 이름, "Gate/Door" = 부모 이름/자식 이름 (끝부분 일치).
//   찾는 순서는 재생 엔티티의 하위 트리 → 씬 전체 (깊이 우선, 먼저 찾은 것). 프리팹 인스턴스 안 엔티티도 이름으로 찾는다.
// 트랙 종류:
//   Transform  — 키(시각, 위치, 회전 = Pitch/Yaw/Roll 도, 스케일)를 보간해 대상 트랜스폼(로컬)에 쓴다
//   Property   — 리플렉션 프로퍼티(Component.Property) 키. Float/Int32/Bool/Vector2/3/4(색). Int32/Bool은 항상 계단
//   CameraCut  — 컷(시각, 카메라 엔티티). 다음 컷까지 그 카메라를 주 카메라로 (bPrimary + 높은 Priority, 끝나면 원래 값). 시야각은 Property 트랙
//                (CameraComponent.FovYDegrees)
//   Animation  — 구간(시작~끝, 클립, 클립 시작 위치, 배속, 반복)을 모델 루트의 FAnimationComponent에 시각 지정으로 재생
//                (그 구간 동안 일시정지 + SetTime — 스크럽과 같은 경로라 결정적). 마지막 구간이 끝나면 끝 포즈 유지
//   Event      — 시각에 이름 이벤트 → 재생 엔티티(없으면 가장 가까운 조상)의 Lua OnSequenceEvent_<이름>()
// 보간 (키의 Interp = 그 키에서 다음 키까지): Constant 계단, Linear 직선, Smooth 3차 에르미트(이웃 키 기울기 — Catmull-Rom,
//   양 끝 키는 기울기 0 = 천천히 출발/도착). 회전은 오일러 각을 각각 보간한다 (언리얼과 같음 — 키를 넣을 때 이전 키와 가까운 각으로 펼친다).
// 순수 계산은 SequenceMath (단위 테스트 대상), 재생/적용은 Scene/SequencePlayer.h.

enum class ESequenceTrackType : uint8
{
	Transform,
	Property,
	CameraCut,
	Animation,
	Event,
};

enum class ESequenceInterp : uint8
{
	Constant,
	Linear,
	Smooth,
};

struct FSequenceTransformKey
{
	float           Time = 0.0f;
	FVector3        Position;
	FVector3        Rotation; // Pitch, Yaw, Roll (도)
	FVector3        Scale  = FVector3(1.0f);
	ESequenceInterp Interp = ESequenceInterp::Smooth;
};

struct FSequenceValueKey
{
	float           Time = 0.0f;
	FVector4        Value; // 성분 수는 프로퍼티 타입 (Float = X)
	ESequenceInterp Interp = ESequenceInterp::Linear;
};

struct FSequenceCameraCut
{
	float       Time = 0.0f;
	std::string Camera; // 엔티티 이름 경로
};

struct FSequenceAnimSection
{
	float       Start  = 0.0f;
	float       End    = 1.0f;
	std::string Clip;
	float       Offset = 0.0f; // 클립 시작 위치 (초)
	float       Rate   = 1.0f;
	bool        bLoop  = true;
};

struct FSequenceEventKey
{
	float       Time = 0.0f;
	std::string Name;
};

struct FSequenceTrack
{
	ESequenceTrackType Type = ESequenceTrackType::Transform;
	std::string        Name;      // 편집기 표시 이름 (비면 대상/종류로)
	std::string        Target;    // 엔티티 이름 경로 (CameraCut/Event는 쓰지 않음)
	std::string        Component; // Property: 리플렉션 컴포넌트 이름 ("PointLightComponent")
	std::string        Property;  // Property: 프로퍼티 이름 ("Intensity")
	bool               bMuted = false;

	std::vector<FSequenceTransformKey> TransformKeys; // 시각 오름차순
	std::vector<FSequenceValueKey>     ValueKeys;     // 〃
	std::vector<FSequenceCameraCut>    Cuts;          // 〃
	std::vector<FSequenceAnimSection>  Sections;      // 시작 오름차순
	std::vector<FSequenceEventKey>     Events;        // 시각 오름차순

	size_t GetKeyCount() const;
	float  GetKeyTime(size_t Index) const; // 종류별 키/컷/구간 시작/이벤트의 시각
	void   SortKeys();
};

struct FSequenceAsset
{
	static constexpr int32          Version   = 1;
	static constexpr const wchar_t* Extension = L".esequence";

	float                       Duration  = 5.0f;  // 초
	float                       FrameRate = 30.0f; // 편집기 눈금/스냅 (재생은 연속 시간)
	std::vector<FSequenceTrack> Tracks;

	static bool FromJsonString(const std::string& Text, FSequenceAsset& Out, std::string* OutError = nullptr);
	std::string ToJsonString() const;
	bool        SaveToFile(const std::filesystem::path& Path) const;
	void        SortKeys();

	static FSequenceAsset MakeDefault(); // 빈 이벤트 트랙 하나 + 5초
};

const char* ToString(ESequenceTrackType Type);
const char* ToString(ESequenceInterp Interp);

namespace SequenceMath
{
	// 키 열의 Time 시각 값 (Keys는 시각 오름차순, Get(키) → FVector4). 비면 Fallback. 첫 키 앞/마지막 키 뒤는 끝 값
	FVector4 EvaluateValueKeys(const std::vector<FSequenceValueKey>& Keys, float Time, const FVector4& Fallback, bool bStepOnly);
	// 트랜스폼 키 → 위치/회전/스케일. 키가 없으면 false
	bool EvaluateTransformKeys(const std::vector<FSequenceTransformKey>& Keys, float Time, FVector3& OutPosition, FQuat& OutRotation, FVector3& OutScale);
	// 키 두 개 사이 한 성분 보간 (테스트용 공개). P0~P3 = 이전/시작/끝/다음 값과 시각, 이웃이 없으면 bHasPrev/bHasNext false
	float Interpolate(ESequenceInterp Interp, float Alpha, float T0, float V0, float T1, float V1, bool bHasPrev, float TPrev, float VPrev, bool bHasNext,
	                  float TNext, float VNext);

	// Time에 보이는 컷 번호 (Time 이하인 마지막 컷). 첫 컷 전이면 -1
	int32 FindCameraCut(const std::vector<FSequenceCameraCut>& Cuts, float Time);
	// Time에 쓸 애니메이션 구간 (시작이 Time 이하인 마지막 구간, 끝을 지나면 끝 시각 고정) + 그 구간 기준 클립 시각 (반복 감기 전). 없으면 -1
	int32 FindAnimSection(const std::vector<FSequenceAnimSection>& Sections, float Time, float& OutClipTime);
	// (From, To] 구간 이벤트 번호 (bIncludeFrom이면 [From, To]). From > To면 없음
	void CollectEvents(const std::vector<FSequenceEventKey>& Events, float From, float To, bool bIncludeFrom, std::vector<int32>& OutIndices);

	// 쿼터니언 → 오일러 (도). 각 성분을 Reference에 가장 가까운 동치 각(±360°)으로 펼친다 (키 보간이 먼 길로 돌지 않게)
	FVector3 QuatToEulerNear(const FQuat& Rotation, const FVector3& Reference);
} // namespace SequenceMath

// .esequence 공유 캐시 (경로별 한 번 읽음, Content 기준 또는 절대). Invalidate가 세대를 올리면 재생 중 컴포넌트가 다시 읽는다
class FSequenceLibrary
{
public:
	static FSequenceLibrary& Get();

	std::shared_ptr<const FSequenceAsset> Load(const std::string& AssetPath); // 실패하면 nullptr (경고 로그)
	void                                  Invalidate();
	void                                  Invalidate(const std::string& AssetPath);
	uint32                                GetGeneration() const { return Generation; }

private:
	static std::wstring MakeKey(const std::string& AssetPath);

	std::unordered_map<std::wstring, std::shared_ptr<const FSequenceAsset>> Cache;
	uint32                                                                  Generation = 1;
};
