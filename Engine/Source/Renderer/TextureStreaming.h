#pragma once

#include "Core/Math/Math.h"
#include "Renderer/TextureStreamingMath.h"
#include "Scene/ResourceHandles.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class FD3D12Texture;
class FMeshInstanceList;

// ---------------------------------------------------------------- 텍스처 밉 스트리밍 (Phase 53)
//
// 대상: 쿠킹 파일에서 다시 읽을 수 있는 텍스처 중 머티리얼만 쓰는 것 —
//   경로 텍스처를 머티리얼이 부른 것(.emat 고정 PBR/그래프, LoadTexture 내부 경로)과 모델 이미지(.emodel 안 본문, CreateStreamingTexture).
//   공개 LoadTexture(UI·파티클·썸네일 등 머티리얼 밖)로 한 번이라도 부른 텍스처는 "고정"(항상 전체 밉).
//   큰 변 256 이하, 그래프에서 계산된 UV로 읽는 텍스처, 지형 레이어·데칼 머티리얼 텍스처(씬 렌더러가 매 프레임 전체로 보고)도 전체.
// 상주 방식 (a): 텍스처마다 상주 밉 범위 [ResidentTop..끝). 범위를 바꿀 때는 그 범위의 새 D3D12 텍스처를 만들고(쿠킹 파일에서 밉 단위로
//   다시 읽어 복사 큐로 업로드) 업로드가 끝나면 풀 슬롯의 내용을 바꾼다(FD3D12Texture::SwapContents — 핸들 유지) → 그 텍스처를 쓰는
//   머티리얼 테이블을 RefreshMaterialTextures로 다시 만든다(DXR 바인드리스도 같은 경로). 이전 리소스/SRV/테이블은 지연 해제.
//   밉 k를 밉 0으로 하는 텍스처는 크기가 2^k로 나누어떨어질 때만 만든다 → 하드웨어 LOD가 정확히 k만큼 줄어 같은 밉을 읽는다
//   (필요한 밉이 모두 상주하면 전체 밉 텍스처와 비트 동일).
// 필요 밉: 씬 렌더러가 뷰마다 ReportTextureStreamingView (메시 인스턴스 + 카메라, 식은 TextureStreamingMath 머리 주석).
//   메인 프러스텀 밖 그림자 캐스터는 우선순위만 낮게(같은 식). 한 프레임 안 여러 뷰(에디터 뷰포트·미리보기·썸네일)는 최솟값.
// 갱신 (BeginFrame — ProcessAsyncLoads): 지난 프레임 보고 → 내릴 때 늦게(r.Streaming.DropDelay초) → 예산(r.Streaming.PoolSizeMB) 배분
//   → 올리기/내리기 요청(작업 스레드에서 FFileSystem 범위 읽기 → 메인에서 복사 큐 업로드, 프레임당 r.Streaming.MaxUploadMBPerFrame).
//   보고가 없는 프레임(창 최소화 등)은 갱신하지 않는다.
// 처음 로드: 비동기(대화형)는 꼬리 밉만 올리고 보고에 따라 올린다. 자동 검증/동기 로딩(AsyncDrain/Sync)은 전체 밉으로 올린 뒤
//   첫 보고에서 바로(지연 없이) 필요 밉으로 내리고, 이후 더 세밀한 밉이 필요해지면 그 뷰의 보고 안에서 바로 읽고 바꿔(CPU 대기)
//   같은 프레임에 그린다 → 결정적이고, 필요 밉 식이 보수적이면 스트리밍을 끈 화면과 비트 동일.
// r.Streaming 0: 새 텍스처는 전체 밉, 이미 줄어든 텍스처는 전체로 되돌린다 (등록은 계속 — 다시 켜면 줄인다).
// 리소스 수거와의 관계: 수거는 텍스처 전체를 해제(DestroyTexture → 스트리밍 항목·진행 중 요청 제거), 스트리밍은 살아 있는 텍스처의 밉만 조절한다.
// 스트리밍은 EnableAsyncLoading을 부른 앱에서만 (테스트·도구는 항상 전체 밉).

// 쿠킹 파일 안 텍스처 본문 위치 (.etex = FAssetCache::TexturePayloadOffset, .emodel = 이미지마다)
struct FTextureStreamSource
{
	std::filesystem::path File;
	uint64                PayloadOffset = 0;

	bool IsValid() const { return !File.empty() && PayloadOffset > 0; }
};

// 씬 렌더러 → 리소스 관리자: 한 뷰의 스트리밍 보고
struct FTextureStreamingView
{
	const FMeshInstanceList* Instances = nullptr;
	FVector3                 CameraPosition;
	FVector3                 CameraForward = FVector3(1.0f, 0.0f, 0.0f);
	bool                     bOrthographic = false;
	float                    TanHalfFovY   = 0.0f;
	float                    OrthoHeight   = 0.0f;
	float                    NearZ         = 1.0f;
	uint32                   ScreenHeight  = 0;    // 씬 내부 해상도 높이
	float                    MipBias       = 0.0f; // MaterialMipBias (TAAU)
	std::function<bool(const FBox&)> IsInMainView;   // 없으면 모두 메인 뷰
	std::function<bool(const FBox&)> IsShadowCaster; // 메인 뷰 밖이면 이것만 낮은 우선순위로
	std::vector<FMaterialHandle>     FullResidencyMaterials; // 지형 레이어·데칼 머티리얼 (메시 인스턴스 밖에서 그려짐 → 전체 밉)
};

// 통계 (stat streaming, 통계 창)
struct FTextureStreamingStats
{
	bool   bEnabled          = false;
	bool   bActive           = false; // EnableAsyncLoading 됨
	uint32 StreamingTextures = 0;     // 스트리밍 항목 (고정 제외)
	uint32 PinnedTextures    = 0;     // 항목 중 고정(전체 밉)
	uint32 ReducedTextures   = 0;     // 전체보다 적게 상주
	uint64 PoolBytes         = 0;     // 예산
	uint64 ResidentBytes     = 0;     // 스트리밍 항목 상주 바이트 (고정 제외)
	uint64 WantedBytes       = 0;     // 지난 갱신의 필요 밉 합 (예산 적용 전)
	uint64 FullBytes         = 0;     // 스트리밍 항목이 전체 밉이면 (절약 비교)
	uint64 PinnedBytes       = 0;
	uint32 PendingRequests   = 0;     // 읽기·업로드 중
	float  UploadMBPerSecond = 0.0f;
	uint64 UploadedBytes     = 0;     // 누적
	uint32 BrokenSources     = 0;     // 쿠킹 파일이 바뀌어 더 읽지 않는 항목
	bool   bOverBudget       = false;
};

namespace TextureStreaming
{
	std::vector<std::string> FormatStats(const FTextureStreamingStats& Stats);
}

// FResourceManager 안 상태 (구현은 TextureStreaming.cpp — FResourceManager 멤버 함수)
struct FTextureStreamingState
{
	struct FEntry
	{
		FTextureHandle                     Handle;
		FTextureStreamSource               Source;
		TextureStreamingMath::FPayloadLayout Layout;
		std::vector<uint64>                RangeBytes; // [k] = [k..) 데이터 바이트 (예산)
		uint32                             TailTop     = 0;
		uint32                             ResidentTop = 0;
		uint32                             TargetTop   = 0; // 지난 갱신 결과
		TextureStreamingMath::FHysteresisState Hysteresis;
		bool                               bPinned      = false;
		bool                               bInitialFull = false; // 결정적 모드: 처음 전체로 올림 → 첫 보고에서 바로 내린다
		bool                               bBroken      = false;
		// 이번 프레임 보고 (뷰들의 최솟값)
		uint32 FrameWantedTop = TextureStreamingMath::InvalidMip;
		float  FramePriority  = 0.0f;
		// 진행 중 요청 (읽기 → 업로드)
		uint32                         RequestSerial = 0;
		uint32                         PendingTop    = TextureStreamingMath::InvalidMip; // 읽는 중이거나 업로드 중
		std::unique_ptr<FD3D12Texture> PendingTexture; // 업로드 중 (펜스 완료 후 교체)
		uint64                         PendingFence = 0;
		std::wstring                   DebugName;

		FEntry();
		~FEntry();
		FEntry(FEntry&&) noexcept;
		FEntry& operator=(FEntry&&) noexcept;
	};

	std::map<uint64, FEntry>           Entries; // 키: FTextureHandle::ToId (정렬 — 예산 배분·요청 순서가 결정적)
	bool                               bReportedThisFrame = false;
	uint32                             NextSerial         = 1;
	uint64                             PoolBytes          = 0; // 지난 갱신에 쓴 예산
	size_t                             LastLoggedEntryCount = 0;
	double                             LastUpdate = 0.0; // FFrameTime 누적 시간 (내림 지연은 앱 프레임 시간으로)
	bool                               bHasLastUpdate = false;
	uint64                             LastWantedBytes = 0;
	bool                               bLastOverBudget = false;
	bool                               bWarnedOverBudget = false;
	// 업로드 속도 (1초 창)
	uint64                                UploadedBytes      = 0;
	uint64                                WindowBytes        = 0;
	float                                 UploadMBPerSecond  = 0.0f;
	std::chrono::steady_clock::time_point WindowStart;
	bool                                  bWindowStarted = false;
	std::chrono::steady_clock::time_point LastStatsLog; // r.Streaming.LogStats
	bool                                  bStatsLogStarted = false;
	bool                                  bOwnsConsole   = false;
};
