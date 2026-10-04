#pragma once

#include "Core/Math/Math.h"

#include <cstring>

// 방향광 그림자 캐시·LOD 바이어스·작은 캐스터 컬링 규칙 (순수 함수 — RendererTests의 ShadowCacheTests)
//
// 정적 캐스터 캐시 (r.Shadow.Cache):
//  - 캐스케이드마다 정적 캐스터만 그린 깊이를 따로 보관하고, 프레임마다 캐시 → 섀도우 맵 복사 뒤 동적 캐스터만 그린다.
//  - 정적 캐스터 = 스킨이 아니고 월드 행렬이 r.Shadow.Cache.StaticFrames 프레임 연속 같은 인스턴스(FMeshInstance::bShadowStatic),
//    폴리지(배치 고정), 상태 해시를 알려 주는 추가 캐스터(지형 + 정적 2D 스프라이트·타일맵 — FShadowRenderer::ExtraCasterState). 그 밖은 모두 동적이다
//    (움직이는 2D 캐스터는 FShadowRenderer::ExtraDynamicCasters — FSpriteShadowRenderer).
//    새 그림자 캐스터 종류는 정적/동적을 반드시 알린다(알리지 않으면 동적 = 매 프레임 그림).
//  - 캐스케이드 키 = 해시(캐스케이드 뷰-투영 비트, 해상도·바이어스·LOD 바이어스·작은 캐스터 문턱, 정적 인스턴스 집합 해시,
//    추가 캐스터 상태 해시, 캐시 세대(셰이더 다시 로드·명시 무효화·LOD 설정 변경)). 키가 바뀌면(태양 이동, 캐스케이드 이동, 정적 캐스터
//    추가/삭제/이동/머티리얼 변경, 리소스 다시 로드) 캐시는 무효다.
//  - 메인 카메라 LOD(메시·지형 청크)는 키에 넣지 않는다 — 카메라가 움직일 때마다 캐시가 무효가 되지 않게. 캐시는 다시 그린 시점의 LOD로
//    남고 캐스케이드가 바뀌면(이동 중 칸 넘김) 맞춰진다.
//  - 카메라 이동 중 재사용(r.Shadow.Cache.Quantize): 양자화 캐스케이드(QuantizeFirstCascade부터)는 중심을 큰 격자에 맞춰
//    (ShadowMath::ComputeCascade) 칸 안에서는 뷰-투영이 비트 동일 → 칸을 넘는 프레임만 Direct, 다음 프레임 Rebuild.
//  - Decide: 키가 지난 프레임과 같고 캐시 키와도 같으면 Reuse, 지난 프레임과는 같지만 캐시가 없거나 낡았으면 Rebuild
//    (직전 캐시를 ImmediateRebuildReuseFrames 이상 재사용하다 키가 바뀌면 그 프레임에 바로 Rebuild — 이동 중 칸 넘김)
//    (정적을 캐시에 그림 → 복사 → 동적), 지난 프레임과 다르면 Direct(캐시 없이 전부 그림 — 태양이 매 프레임 움직이는 시간대 애니메이션은
//    복사 비용 없이 예전과 같다).
//
// LOD 바이어스 (r.Shadow.LodBias): 캐스케이드 c의 그림자 LOD = min(메인 LOD + floor(c × 바이어스), LOD 수 - 1). bFixedLod/스킨은 그대로.
// 작은 캐스터 (r.Shadow.MinCasterTexels): 경계 구 지름이 그 캐스케이드 텍셀 몇 개보다 작으면 그 캐스케이드에서 그리지 않는다.
namespace ShadowCacheMath
{
	enum class ECacheAction : uint8
	{
		Direct,  // 캐시 없이 모든 캐스터를 섀도우 맵에 그린다
		Rebuild, // 정적 캐스터를 캐시에 다시 그리고 복사한 뒤 동적 캐스터
		Reuse,   // 캐시 복사 뒤 동적 캐스터만
	};

	struct FCascadeCacheState
	{
		uint64 LastKey     = 0;
		uint64 CachedKey   = 0;
		uint32 ReuseFrames = 0; // 지금 캐시를 연속으로 재사용한 프레임 수
		bool   bHasLast    = false;
		bool   bCacheValid = false;
	};

	// 키가 바뀐 프레임에 바로 Rebuild하는 조건: 직전 캐시를 이만큼 재사용했다 (키가 가끔만 바뀜 — 이동 중 칸 넘김).
	// 매 프레임 바뀌는 키(움직이는 태양)는 재사용 0이라 Direct로 떨어진다
	constexpr uint32 ImmediateRebuildReuseFrames = 2;

	// 이번 프레임 키로 행동을 고르고 상태를 갱신한다
	inline ECacheAction Decide(FCascadeCacheState& State, uint64 Key, bool bEnabled)
	{
		ECacheAction Action = ECacheAction::Direct;
		if (!bEnabled)
		{
			State.bCacheValid = false;
		}
		else if (State.bCacheValid && State.CachedKey == Key)
		{
			Action = ECacheAction::Reuse;
		}
		else if ((State.bHasLast && State.LastKey == Key) || (State.bCacheValid && State.ReuseFrames >= ImmediateRebuildReuseFrames))
		{
			Action            = ECacheAction::Rebuild;
			State.CachedKey   = Key;
			State.bCacheValid = true;
		}
		else
		{
			State.bCacheValid = false; // 한 프레임 안정될 때까지 캐시를 만들지 않는다
		}
		State.ReuseFrames = Action == ECacheAction::Reuse ? State.ReuseFrames + 1 : 0;
		State.LastKey     = Key;
		State.bHasLast    = bEnabled;
		return Action;
	}

	// 섀도우 맵 장 되살리기 (캐시를 쓰는 캐스케이드 Rebuild/Reuse — 캐시 → 장): 장이 지난 프레임 끝에 어떤 상태였는지로 고른다.
	//   깨끗함 = 캐시 위에 아무것도 그리지 않았다 → 장은 이미 캐시 내용, 되살리기 없음 (이번에 위에 그리더라도 — 동적 캐스터가 들어온 첫 프레임)
	//   부분 = 위에 그린 것이 텍셀 사각형 Dirty 안뿐이다(동적 추가 캐스터만 — 2D 스프라이트 경계를 광원 공간에 투영) → 그 사각형만 캐시에서 다시 쓴다
	//          (FShadowRenderer: 캐시 깊이를 읽어 SV_Depth로 쓰는 시저 그리기 — D32는 서브리소스 일부 복사가 안 된다)
	//   그 밖(동적 메시·캐시에 넣지 않는 추가 캐스터가 그렸음, 처음, 캐시 키가 바뀜) → 장 전체 복사
	// 판정은 캐스케이드마다 따로. 직교 2D 카메라는 모든 캐스케이드가 같은 화면(평면)을 덮어 동적 2D 캐스터가 매 캐스케이드에 닿으므로,
	// 장 전체 복사 대신 부분 되살리기가 비용을 줄인다 (작은 캐릭터 = 작은 사각형).
	struct FTexelRect
	{
		int32 X0 = 0;
		int32 Y0 = 0;
		int32 X1 = 0; // 끝 (포함 안 함)
		int32 Y1 = 0;

		bool IsEmpty() const { return X1 <= X0 || Y1 <= Y0; }
		bool operator==(const FTexelRect& Other) const = default;
	};

	// 월드 AABB → 장 텍셀 사각형: 8꼭짓점을 광원 뷰-투영(행벡터 v × M)으로 NDC → 텍셀(u = (x+1)/2, v = (1-y)/2), 바깥으로 내림/올림 + Margin,
	// [0, Resolution]으로 자름. 꼭짓점이 w <= 0이면(직교 광원에서는 없음) 장 전체
	inline FTexelRect ComputeTexelRect(const FBox& Bounds, const FMatrix4x4& ViewProjection, uint32 Resolution, int32 Margin = 1)
	{
		const int32 Size = static_cast<int32>(Resolution);
		if (!Bounds.IsValid())
		{
			return {};
		}
		float MinX = 1.0e30f;
		float MinY = 1.0e30f;
		float MaxX = -1.0e30f;
		float MaxY = -1.0e30f;
		for (uint32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector3 P((Corner & 1) ? Bounds.Max.X : Bounds.Min.X, (Corner & 2) ? Bounds.Max.Y : Bounds.Min.Y, (Corner & 4) ? Bounds.Max.Z : Bounds.Min.Z);
			const FMatrix4x4& M = ViewProjection;
			const float X = P.X * M.M[0][0] + P.Y * M.M[1][0] + P.Z * M.M[2][0] + M.M[3][0];
			const float Y = P.X * M.M[0][1] + P.Y * M.M[1][1] + P.Z * M.M[2][1] + M.M[3][1];
			const float W = P.X * M.M[0][3] + P.Y * M.M[1][3] + P.Z * M.M[2][3] + M.M[3][3];
			if (W <= 1.0e-6f)
			{
				return { 0, 0, Size, Size };
			}
			const float U = (X / W * 0.5f + 0.5f) * static_cast<float>(Size);
			const float V = (0.5f - Y / W * 0.5f) * static_cast<float>(Size);
			MinX = FMath::Min(MinX, U);
			MinY = FMath::Min(MinY, V);
			MaxX = FMath::Max(MaxX, U);
			MaxY = FMath::Max(MaxY, V);
		}
		const auto Clamp = [Size](float Value) { return static_cast<int32>(FMath::Clamp(Value, 0.0f, static_cast<float>(Size))); };
		FTexelRect Rect;
		Rect.X0 = Clamp(FMath::Floor(MinX) - static_cast<float>(Margin));
		Rect.Y0 = Clamp(FMath::Floor(MinY) - static_cast<float>(Margin));
		Rect.X1 = Clamp(FMath::Ceil(MaxX) + static_cast<float>(Margin));
		Rect.Y1 = Clamp(FMath::Ceil(MaxY) + static_cast<float>(Margin));
		return Rect.IsEmpty() ? FTexelRect{} : Rect;
	}

	enum class ESliceRestore : uint8
	{
		None, // 장이 이미 캐시 내용 (또는 Direct — 지우고 전부 그림)
		Rect, // FSliceCopyState::Dirty 사각형만 캐시에서
		Copy, // 장 전체 복사
	};

	struct FSliceCopyState
	{
		uint64     Key      = 0;
		bool       bClean   = false;
		bool       bPartial = false; // 캐시와 다른 곳이 Dirty 안뿐
		FTexelRect Dirty;
	};

	// 이번 프레임 되살리기 (그리기 전 상태로 판정)
	inline ESliceRestore DecideSliceRestore(const FSliceCopyState& State, ECacheAction Action, uint64 CachedKey)
	{
		if (Action == ECacheAction::Direct)
		{
			return ESliceRestore::None;
		}
		if (State.Key == CachedKey && State.bClean)
		{
			return ESliceRestore::None;
		}
		if (State.Key == CachedKey && State.bPartial)
		{
			return ESliceRestore::Rect;
		}
		return ESliceRestore::Copy;
	}

	// 이번 프레임 끝 상태: bMeshOnTop = 위치를 모르는 것이 캐시 위에 그렸다(동적 메시 등), OnTopRect = 동적 추가 캐스터가 그린 텍셀 사각형 (비면 없음)
	inline void FinishSlice(FSliceCopyState& State, ECacheAction Action, uint64 CachedKey, bool bMeshOnTop, const FTexelRect& OnTopRect)
	{
		State.Key      = CachedKey;
		State.bClean   = Action != ECacheAction::Direct && !bMeshOnTop && OnTopRect.IsEmpty();
		State.bPartial = Action != ECacheAction::Direct && !bMeshOnTop && !OnTopRect.IsEmpty();
		State.Dirty    = State.bPartial ? OnTopRect : FTexelRect{};
	}

	// 캐스케이드 c의 LOD 바이어스 = floor(c × BiasPerCascade)
	inline uint32 ComputeCascadeLodBias(uint32 Cascade, float BiasPerCascade)
	{
		return BiasPerCascade > 0.0f ? static_cast<uint32>(FMath::Floor(static_cast<float>(Cascade) * BiasPerCascade + 1.0e-4f)) : 0u;
	}

	inline uint32 SelectShadowLod(uint32 MainLod, uint32 LodCount, uint32 CascadeBias, bool bKeepLod)
	{
		if (bKeepLod || LodCount <= 1)
		{
			return MainLod;
		}
		return FMath::Min(MainLod + CascadeBias, LodCount - 1);
	}

	// 경계 구 지름(2R)이 텍셀 MinTexels개보다 작으면 true (MinTexels <= 0 = 끔)
	inline bool IsCasterTooSmall(float BoundsRadius, float TexelWorldSize, float MinTexels)
	{
		return MinTexels > 0.0f && TexelWorldSize > 0.0f && 2.0f * BoundsRadius < MinTexels * TexelWorldSize;
	}

	// 위치가 Required 프레임 연속 같았으면 정적
	inline bool IsStatic(uint32 StableFrames, uint32 RequiredFrames)
	{
		return StableFrames >= RequiredFrames;
	}

	// ---- 해시 (곱셈 섞기). 정적 집합 해시는 인스턴스 해시의 합(순서 무관)
	constexpr uint64 HashSeed = 1469598103934665603ull;

	// 8바이트 단위로 섞는다 (남는 바이트는 하나씩). 같은 입력 → 같은 값 (실행·플랫폼 내 결정적)
	inline uint64 HashBytes(uint64 Hash, const void* Data, size_t Size)
	{
		const uint8* Bytes = static_cast<const uint8*>(Data);
		while (Size >= 8)
		{
			uint64 Word = 0;
			std::memcpy(&Word, Bytes, 8);
			Hash = (Hash ^ Word) * 0x9E3779B97F4A7C15ull;
			Hash ^= Hash >> 29;
			Bytes += 8;
			Size -= 8;
		}
		while (Size > 0)
		{
			Hash = (Hash ^ *Bytes) * 1099511628211ull;
			++Bytes;
			--Size;
		}
		return Hash;
	}

	template <typename T>
	inline uint64 HashValue(uint64 Hash, const T& Value)
	{
		return HashBytes(Hash, &Value, sizeof(T));
	}

	// 합으로 모을 인스턴스 해시가 고르게 퍼지도록 마지막에 섞는다 (splitmix64 마무리)
	inline uint64 Finalize(uint64 Hash)
	{
		Hash ^= Hash >> 30;
		Hash *= 0xbf58476d1ce4e5b9ull;
		Hash ^= Hash >> 27;
		Hash *= 0x94d049bb133111ebull;
		Hash ^= Hash >> 31;
		return Hash;
	}
} // namespace ShadowCacheMath
