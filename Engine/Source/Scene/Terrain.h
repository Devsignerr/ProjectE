#pragma once

#include "Core/CoreTypes.h"
#include "Core/ECS/Entity.h"
#include "Core/Log.h"
#include "Core/Math/Math.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

E_DECLARE_ENGINE_LOG_CATEGORY(LogTerrain)

class FScene;
struct FTransformComponent;

// ---- 지형 (Phase 34) ----------------------------------------------------------------------------------------------
// 규칙:
//   - 지형은 엔티티 월드 위치만 따른다 (회전/스케일 무시). 위치 = 지형 가운데, 높이 0(16비트 32768) = 위치 Z
//   - 높이맵 정점 격자 Resolution x Resolution (셀 = Resolution - 1), X = 열(+X 앞), Y = 행(+Y 오른쪽)
//   - 높이 Z = 위치 Z + (h / 65535 - 0.5) * HeightRange, 레이어 가중치는 정점마다 RGBA8 (합 255, 레이어 0~3)
//   - 데이터는 .eterrain(JSON + base64) 하나에 담고 FTerrainLibrary가 경로별로 공유한다 (렌더러/물리/에디터 공용)
//   - 편집 되돌리기: 컴포넌트 EditRevision(씬에 저장)과 데이터 Revision이 다르면 에디터가 변경 기록으로 맞춘다
struct FTerrainComponent
{
	std::string Asset;                                  // .eterrain (Content 기준)
	FVector2    Size        = FVector2(10000.0f, 10000.0f); // cm (가로 X, 세로 Y)
	float       HeightRange = 5000.0f;                  // cm: 16비트 높이 전체 범위 (가운데 = 위치 Z)
	std::string Layer0Material;                         // .emat (레이어 0 = 바탕)
	std::string Layer1Material;
	std::string Layer2Material;
	std::string Layer3Material;
	float       Layer0Tiling = 400.0f; // cm: 텍스처 한 장이 덮는 크기
	float       Layer1Tiling = 400.0f;
	float       Layer2Tiling = 400.0f;
	float       Layer3Tiling = 400.0f;
	bool        bCastShadows = true;
	bool        bCollision   = true;  // 플레이 중 높이맵 충돌 (정적 바디)
	uint32      EditRevision = 0;     // 편집 되돌리기용 데이터 버전 (에디터만 바꾼다)
};

constexpr uint32 TerrainMaxLayers = 4;

// 정점 격자 사각형 (양 끝 포함). 비었으면 MinX > MaxX
struct FTerrainRect
{
	int32 MinX = 0;
	int32 MinY = 0;
	int32 MaxX = -1;
	int32 MaxY = -1;

	bool  IsEmpty() const { return MinX > MaxX || MinY > MaxY; }
	int32 GetWidth() const { return IsEmpty() ? 0 : MaxX - MinX + 1; }
	int32 GetHeight() const { return IsEmpty() ? 0 : MaxY - MinY + 1; }
	void  Add(const FTerrainRect& Other);
	FTerrainRect Clipped(int32 Resolution) const;
	static FTerrainRect Full(int32 Resolution) { return { 0, 0, Resolution - 1, Resolution - 1 }; }
};

// 높이맵 + 레이어 가중치. 데이터를 바꾼 코드는 MarkChanged로 알린다 (렌더러/물리가 바뀐 영역만 다시 올린다)
struct FTerrainData
{
	static constexpr uint16 DefaultHeight = 32768;

	uint32              Resolution = 0;
	std::vector<uint16> Heights; // Resolution²
	std::vector<uint32> Weights; // Resolution², 바이트 i = 레이어 i 가중치 (합 255)
	uint32              Revision = 0; // 저장된 편집 버전 (컴포넌트 EditRevision과 비교)

	// ---- 런타임 (저장 안 함)
	uint64 ChangeCounter = 1; // MarkChanged마다 증가
	bool   bUnsaved      = false;

	void Initialize(uint32 InResolution); // 평평한 지형 (높이 32768, 레이어 0 = 255)
	bool IsValid() const { return Resolution >= 2 && Heights.size() == static_cast<size_t>(Resolution) * Resolution && Weights.size() == Heights.size(); }

	uint16 GetHeight(int32 X, int32 Y) const { return Heights[static_cast<size_t>(Y) * Resolution + X]; }
	uint32 GetWeight(int32 X, int32 Y) const { return Weights[static_cast<size_t>(Y) * Resolution + X]; }

	void MarkChanged(const FTerrainRect& Rect);
	// Counter 이후 바뀐 영역 합. 기록이 모자라면(오래됨) 전체. 바뀐 것이 없으면 false
	bool GetChangesSince(uint64 Counter, FTerrainRect& OutRect) const;

private:
	struct FChange
	{
		uint64       Counter = 0;
		FTerrainRect Rect;
	};
	std::vector<FChange> RecentChanges; // 최근 몇 개만 (오래된 것은 버림)
};

// 엔티티 트랜스폼 + 컴포넌트 → 월드 좌표 변환 정보 (순수 계산)
struct FTerrainFrame
{
	FVector3 Origin;          // 정점 (0, 0)의 월드 XY + 높이 0(16비트 0)의 Z
	FVector2 CellSize;        // cm
	float    HeightScale = 0.0f; // cm / 16비트 단위
	int32    Resolution  = 0;

	static FTerrainFrame Make(const FVector3& WorldPosition, const FTerrainComponent& Terrain, uint32 Resolution);

	FVector2 WorldToGrid(float WorldX, float WorldY) const;
	FVector3 GridToWorld(float GridX, float GridY, float Height16) const;
	float    HeightToWorldZ(float Height16) const { return Origin.Z + Height16 * HeightScale; }
	float    WorldZToHeight(float WorldZ) const { return HeightScale > 0.0f ? (WorldZ - Origin.Z) / HeightScale : 0.0f; }
	FBox     GetBounds(const FTerrainData& Data) const; // 실제 높이 최소/최대 기준
	FBox     GetFullBounds() const;                     // 높이 범위 전체 (가장 보수적)
};

// 순수 함수 (단위 테스트 대상)
namespace TerrainMath
{
	// 격자 좌표(실수)의 높이(16비트 단위, 실수). 셀 삼각형 두 개(대각선 (0,0)-(1,1))로 보간 — 렌더/충돌과 같은 면
	float SampleHeight(const FTerrainData& Data, float GridX, float GridY);
	// 월드 Z (격자 밖이면 가장자리로 자름)
	float SampleWorldHeight(const FTerrainData& Data, const FTerrainFrame& Frame, float WorldX, float WorldY);
	// 정점 중심 차분 월드 법선 (+Z 위)
	FVector3 ComputeNormal(const FTerrainData& Data, const FTerrainFrame& Frame, float GridX, float GridY);
	// 레이캐스트 (지형 경계 안을 걸음 + 이분 탐색). 맞으면 true + 거리(cm)
	bool Raycast(const FTerrainData& Data, const FTerrainFrame& Frame, const FVector3& Origin, const FVector3& Direction, float MaxDistance,
	             float& OutDistance);

	// 브러시 감쇠: 중심 거리 / 반경 (0~1) → 세기 배율. Falloff = 가장자리에서 줄어드는 폭 비율 (0 = 단단함, 1 = 중심부터 부드럽게)
	float BrushFalloff(float NormalizedDistance, float Falloff);

	uint8 GetLayerWeight(uint32 Packed, uint32 Layer);
	// Layer 가중치를 Target(0~255)로 Alpha만큼 다가가게 하고 나머지 레이어를 비율대로 줄여 합 255를 맞춘다
	uint32 PaintWeight(uint32 Packed, uint32 Layer, float Alpha);
	uint32 NormalizeWeight(uint32 Packed); // 합이 0이면 레이어 0 = 255

	// 2D 값 잡음 (0~1, 결정적) — 노이즈 브러시
	float ValueNoise(float X, float Y, uint32 Seed);
}

enum class ETerrainBrushOp : int32
{
	Raise = 0,
	Lower,
	Flatten,
	Smooth,
	Noise,
	Paint, // 레이어 칠하기 (Layer)
};

struct FTerrainBrush
{
	ETerrainBrushOp Op       = ETerrainBrushOp::Raise;
	float           Radius   = 500.0f; // cm
	float           Strength = 0.5f;   // 0~1
	float           Falloff  = 0.5f;   // 0~1
	uint32          Layer    = 1;      // Paint
	float           FlattenHeight = 0.0f; // Flatten 목표 (월드 Z, 보통 스트로크 시작점)
	float           NoiseScale    = 800.0f; // cm: 노이즈 무늬 크기
	uint32          Seed          = 1;
};

// 브러시 한 번 적용 (DeltaSeconds 만큼). 반환: 바뀐 정점 영역 (비었으면 변화 없음). MarkChanged는 부르지 않는다
FTerrainRect ApplyTerrainBrush(FTerrainData& Data, const FTerrainFrame& Frame, const FTerrainBrush& Brush, const FVector2& WorldCenter, float DeltaSeconds);

// 영역 복사본 (되돌리기 기록용)
struct FTerrainRegion
{
	FTerrainRect        Rect;
	std::vector<uint16> Heights;
	std::vector<uint32> Weights;

	static FTerrainRegion Capture(const FTerrainData& Data, const FTerrainRect& Rect);
	// 같은 크기 전체 버퍼(Resolution²)에서 영역만 떼어 낸다 (스트로크 시작 스냅샷 → 바뀐 영역)
	static FTerrainRegion CaptureFrom(const std::vector<uint16>& Heights, const std::vector<uint32>& Weights, uint32 Resolution, const FTerrainRect& Rect);
	void Apply(FTerrainData& Data) const; // MarkChanged는 부르지 않는다
	size_t GetMemorySize() const { return Heights.size() * sizeof(uint16) + Weights.size() * sizeof(uint32); }
};

// .eterrain 입출력 + 높이맵 이미지 (RAW16 / PNG16 쓰기)
namespace TerrainIO
{
	constexpr int32 Version = 1;

	std::string ToJsonString(const FTerrainData& Data);
	bool        FromJsonString(std::string_view Json, FTerrainData& OutData, std::string* OutError);
	bool        SaveToFile(const FTerrainData& Data, const std::filesystem::path& Path);
	bool        LoadFromFile(const std::filesystem::path& Path, FTerrainData& OutData, std::string* OutError);

	// 정사각형 16비트 높이 배열 → 지형 해상도로 리샘플 (쌍선형)
	std::vector<uint16> ResampleHeights(const std::vector<uint16>& Source, uint32 SourceWidth, uint32 SourceHeight, uint32 Resolution);
	// RAW16 (리틀 엔디언, 정사각형): 크기에서 한 변 길이를 구한다
	bool ReadRaw16(const std::filesystem::path& Path, std::vector<uint16>& OutHeights, uint32& OutSize);
	bool WriteRaw16(const std::filesystem::path& Path, const FTerrainData& Data);
	// 16비트 회색조 PNG (무압축 deflate)
	std::vector<uint8> EncodePng16(const std::vector<uint16>& Pixels, uint32 Width, uint32 Height);
	bool               WritePng16(const std::filesystem::path& Path, const FTerrainData& Data);

	std::string EncodeBase64(const uint8* Data, size_t Size);
	bool        DecodeBase64(std::string_view Text, std::vector<uint8>& OutBytes);
}

// 경로별 공유 지형 데이터 (엔진 전역). 경로는 Content 기준 (절대 경로도 허용)
class FTerrainLibrary
{
public:
	static constexpr const wchar_t* Extension = L".eterrain";

	static FTerrainLibrary& Get();

	void                  SetContentDirectory(const std::filesystem::path& Directory);
	std::filesystem::path ResolveAssetPath(const std::string& Asset) const;

	// 캐시된 데이터 (없으면 파일에서 로드). 실패면 nullptr (같은 경로 재시도는 Invalidate 전까지 하지 않는다)
	std::shared_ptr<FTerrainData> Load(const std::string& Asset);
	std::shared_ptr<FTerrainData> Find(const std::string& Asset) const;
	// 새 평평한 지형을 만들어 저장하고 캐시에 넣는다
	std::shared_ptr<FTerrainData> Create(const std::string& Asset, uint32 Resolution);
	bool Save(const std::string& Asset);
	void SaveAllUnsaved();
	// 에셋 파일/폴더 이동 후 캐시 키를 새 경로로 (절대 경로). 저장 안 한 편집도 그대로 따라간다
	void OnAssetMoved(const std::filesystem::path& From, const std::filesystem::path& To);
	std::string MakeAssetPath(const std::filesystem::path& AbsolutePath) const; // Content 안이면 '/' 상대 경로
	void Invalidate(const std::string& Asset);
	void Clear();

	template <typename TFunc>
	void ForEach(TFunc&& Func) const
	{
		for (const auto& [Asset, Data] : Cache)
		{
			if (Data)
			{
				Func(Asset, *Data);
			}
		}
	}

private:
	std::filesystem::path                                          ContentDirectory;
	std::unordered_map<std::string, std::shared_ptr<FTerrainData>> Cache; // 키: 에셋 문자열 그대로
};

// 씬의 지형 하나 (질의용 묶음): 엔티티 지형 컴포넌트 + 공유 데이터 + 변환
struct FTerrainInstance
{
	FEntity                  Entity;
	const FTerrainComponent* Component = nullptr;
	FTerrainData*            Data      = nullptr;
	FTerrainFrame            Frame;
};

// 씬의 모든 지형 (트랜스폼 WorldMatrix 기준 — UpdateTransforms 뒤). 데이터를 못 읽은 지형은 뺀다
void GatherTerrains(FScene& Scene, std::vector<FTerrainInstance>& OutTerrains);

// 리플렉션 등록 (RegisterSceneTypes 끝에서 부른다)
void RegisterTerrainTypes();
