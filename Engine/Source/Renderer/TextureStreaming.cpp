#include "Renderer/TextureStreaming.h"

#include "Core/Console/Console.h"
#include "Core/FileSystem.h"
#include "Core/Log.h"
#include "Core/Profiling.h"
#include "Core/StringConv.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12Texture.h"
#include "Renderer/MeshInstancing.h"
#include "Renderer/RendererConsoleVariables.h"
#include "Renderer/ResourceCollector.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/StaticMesh.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <unordered_map>

E_DECLARE_LOG_CATEGORY(LogRenderer)

FTextureStreamingState::FEntry::FEntry()                                    = default;
FTextureStreamingState::FEntry::~FEntry()                                   = default;
FTextureStreamingState::FEntry::FEntry(FEntry&&) noexcept                   = default;
FTextureStreamingState::FEntry& FTextureStreamingState::FEntry::operator=(FEntry&&) noexcept = default;

namespace
{
	using namespace TextureStreamingMath;

	constexpr float  DeterministicDeltaSeconds = 1.0f / 60.0f; // 자동 검증/동기 로딩: 실제 시간 대신 고정 (결정적)
	constexpr float  ShadowOnlyPriorityScale   = 0.1f;         // 메인 뷰 밖 그림자 캐스터
	constexpr float  FullResidencyPriority     = 1.0e6f;       // 지형·데칼 (메시 인스턴스 밖)
	constexpr uint32 MaxConcurrentRequests     = 32;           // 비동기: 동시에 읽기·업로드 중인 요청 상한
	constexpr uint64 Megabyte                  = 1024ull * 1024ull;

	DXGI_FORMAT ToDxgiFormat(ETextureFormat Format, bool bSRGB)
	{
		switch (Format)
		{
		case ETextureFormat::BC7: return bSRGB ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
		case ETextureFormat::BC5: return DXGI_FORMAT_BC5_UNORM;
		case ETextureFormat::BC4: return DXGI_FORMAT_BC4_UNORM;
		default:                  return bSRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
		}
	}

	// 가장 큰 축 스케일 (행벡터 규약: 행 0~2 길이 = 축 스케일). 늘어난 축에서 UV 밀도가 가장 낮다 → 가장 세밀한 밉이 필요
	float GetMaxAxisScale(const FMatrix4x4& World)
	{
		float MaxScale = 0.0f;
		for (int32 Row = 0; Row < 3; ++Row)
		{
			const float Length = std::sqrt(World.M[Row][0] * World.M[Row][0] + World.M[Row][1] * World.M[Row][1] + World.M[Row][2] * World.M[Row][2]);
			MaxScale           = std::max(MaxScale, Length);
		}
		return MaxScale;
	}
} // namespace

// ---------------------------------------------------------------- 통계 문자열

std::vector<std::string> TextureStreaming::FormatStats(const FTextureStreamingStats& Stats)
{
	std::vector<std::string> Lines;
	if (!Stats.bActive)
	{
		Lines.push_back("텍스처 스트리밍: 비활성 (비동기 로딩을 켜지 않은 앱)");
		return Lines;
	}
	Lines.push_back(std::format("텍스처 스트리밍 {}: 텍스처 {} (줄어듦 {}, 고정 {}{})", Stats.bEnabled ? "켬" : "끔", Stats.StreamingTextures, Stats.ReducedTextures,
	                            Stats.PinnedTextures, Stats.BrokenSources > 0 ? std::format(", 읽기 실패 {}", Stats.BrokenSources) : std::string()));
	Lines.push_back(std::format("풀 {} / 예산 {}{}  (필요 {}, 전체 밉이면 {})", ResourceGc::FormatBytes(Stats.ResidentBytes), ResourceGc::FormatBytes(Stats.PoolBytes),
	                            Stats.bOverBudget ? " 초과!" : "", ResourceGc::FormatBytes(Stats.WantedBytes), ResourceGc::FormatBytes(Stats.FullBytes)));
	Lines.push_back(std::format("요청 대기 {}  업로드 {:.1f} MB/s (누적 {})  고정 {}", Stats.PendingRequests, Stats.UploadMBPerSecond,
	                            ResourceGc::FormatBytes(Stats.UploadedBytes), ResourceGc::FormatBytes(Stats.PinnedBytes)));
	return Lines;
}

// ---------------------------------------------------------------- 상태 / 등록

bool FResourceManager::IsStreamingEnabled() const
{
	return bAsyncLoadingEnabled && RendererCVars::Streaming.Get();
}

bool FResourceManager::IsStreamingDeterministic() const
{
	return GetLoadMode() != EResourceLoadMode::Async;
}

uint64 FResourceManager::GetStreamingPoolBytes() const
{
	const int32 PoolMb = RendererCVars::StreamingPoolSizeMB.Get();
	if (PoolMb > 0)
	{
		return static_cast<uint64>(PoolMb) * Megabyte;
	}
	// 자동: VRAM 예산의 40% (렌더 타깃·메시·그림자·다른 프로그램 몫을 남긴다), 256MB~4GB
	uint64                         Budget = 0;
	FD3D12Device::FVideoMemoryInfo Info;
	if (Rhi != nullptr && Rhi->GetDevice().QueryVideoMemory(Info) && Info.LocalBudget > 0)
	{
		Budget = Info.LocalBudget * 2 / 5;
	}
	return std::clamp<uint64>(Budget, 256 * Megabyte, 4096 * Megabyte);
}

uint32 FResourceManager::GetStreamingPendingCount() const
{
	uint32 Count = 0;
	for (const auto& [Id, Entry] : Streaming.Entries)
	{
		Count += Entry.PendingTop != InvalidMip ? 1u : 0u;
	}
	return Count;
}

uint32 FResourceManager::GetTextureResidentTopMip(FTextureHandle Handle) const
{
	const auto Found = Streaming.Entries.find(Handle.ToId());
	return Found != Streaming.Entries.end() ? Found->second.ResidentTop : 0u;
}

FTextureHandle FResourceManager::CreateStreamingTexture(const FCompressedTexture& Texture, const FTextureStreamSource& Source, const std::wstring& DebugName)
{
	if (!Source.IsValid() || !bAsyncLoadingEnabled || !Texture.IsValid())
	{
		return CreateTexture(Texture, DebugName);
	}
	const uint32         MipCount = static_cast<uint32>(Texture.Mips.size());
	const FPayloadLayout Layout   = ComputePayloadLayout(Texture.Format, Texture.bSRGB, Texture.GetWidth(), Texture.GetHeight(), MipCount);
	const uint32         Tail     = ComputeTailTopMip(Texture.Format, Texture.GetWidth(), Texture.GetHeight(), MipCount);
	if (!Layout.IsValid() || Tail == 0)
	{
		return CreateTexture(Texture, DebugName); // 작은 텍스처: 항상 전체
	}
	// 대화형 비동기 + 스트리밍: 꼬리만 올리고 보고에 따라 올린다. 결정적 모드/끔: 전체 (첫 보고에서 내림)
	const uint32   Top = IsStreamingEnabled() && !IsStreamingDeterministic() ? Tail : 0u;
	FTextureHandle Handle;
	if (Top == 0)
	{
		Handle = CreateTexture(Texture, DebugName);
	}
	else
	{
		FCompressedTexture Partial;
		Partial.Format = Texture.Format;
		Partial.bSRGB  = Texture.bSRGB;
		Partial.Mips.assign(Texture.Mips.begin() + Top, Texture.Mips.end());
		Handle = CreateTexture(Partial, DebugName);
	}
	if (Handle.IsValid())
	{
		RegisterStreamingTexture(Handle, Source, Layout, Top, false, DebugName);
	}
	return Handle;
}

void FResourceManager::RegisterStreamingTexture(FTextureHandle Handle, const FTextureStreamSource& Source, const FPayloadLayout& Layout, uint32 ResidentTop,
                                                bool bPinned, const std::wstring& DebugName)
{
	if (!bAsyncLoadingEnabled || !Source.IsValid() || !Layout.IsValid() || !Textures.IsValid(Handle))
	{
		return;
	}
	const uint32 Tail = ComputeTailTopMip(Layout.Format, Layout.Width, Layout.Height, Layout.MipCount);
	if (Tail == 0)
	{
		return; // 작은 텍스처 / 줄일 수 없는 크기: 항상 전체
	}
	FTextureStreamingState::FEntry& Entry = Streaming.Entries[Handle.ToId()];
	if (Entry.PendingTexture)
	{
		Entry.PendingTexture->ShutdownDeferred(*Rhi);
	}
	Entry              = FTextureStreamingState::FEntry{};
	Entry.Handle       = Handle;
	Entry.Source       = Source;
	Entry.Layout       = Layout;
	Entry.TailTop      = Tail;
	Entry.ResidentTop  = std::min(ResidentTop, Tail);
	Entry.TargetTop    = Entry.ResidentTop;
	Entry.bPinned      = bPinned;
	Entry.bInitialFull = Entry.ResidentTop == 0 && IsStreamingDeterministic();
	Entry.DebugName    = DebugName;
	Entry.RangeBytes.resize(Layout.MipCount + 1, 0);
	for (uint32 Mip = Layout.MipCount; Mip-- > 0;)
	{
		Entry.RangeBytes[Mip] = Entry.RangeBytes[Mip + 1] + Layout.Mips[Mip].DataSize;
	}
}

void FResourceManager::UnregisterStreamingTexture(FTextureHandle Handle)
{
	const auto Found = Streaming.Entries.find(Handle.ToId());
	if (Found == Streaming.Entries.end())
	{
		return;
	}
	if (Found->second.PendingTexture && Rhi != nullptr)
	{
		Found->second.PendingTexture->ShutdownDeferred(*Rhi); // 복사 큐가 아직 쓸 수 있다 (지연 해제가 프레임 펜스를 기다림)
	}
	Streaming.Entries.erase(Found); // 읽는 중인 작업은 완료 콜백이 항목이 없어 버린다
}

void FResourceManager::PinStreamingTexture(FTextureHandle Handle)
{
	const auto Found = Streaming.Entries.find(Handle.ToId());
	if (Found == Streaming.Entries.end() || Found->second.bPinned)
	{
		return;
	}
	// 다음 보고/갱신에서 전체로 올린다 (완료 콜백 안에서도 불릴 수 있어 여기서 기다리지 않는다)
	Found->second.bPinned   = true;
	Found->second.TargetTop = 0;
}

void FResourceManager::ShutdownStreaming()
{
	if (Streaming.bOwnsConsole)
	{
		FConsoleManager::Get().UnregisterCommand("r.Streaming.Dump");
		Streaming.bOwnsConsole = false;
	}
	for (auto& [Id, Entry] : Streaming.Entries)
	{
		if (Entry.PendingTexture)
		{
			Entry.PendingTexture->Shutdown(); // 호출자가 GPU 완료를 기다린 뒤
		}
	}
	Streaming.Entries.clear();
	Streaming.bReportedThisFrame = false;
}

void FResourceManager::InitStreamingConsole()
{
	FConsoleManager& Console = FConsoleManager::Get();
	if (Console.FindCommand("r.Streaming.Dump") != nullptr)
	{
		return; // 다른 리소스 관리자가 이미 가졌다
	}
	Console.RegisterCommand({ "r.Streaming.Dump", "스트리밍 텍스처마다 상주/목표/꼬리 밉과 크기를 로그로 (r.Streaming.Dump [이름 필터])",
	                          [this](const std::vector<std::string>& Args, const FConsoleOutput& Output) {
		                          const std::string Filter = Args.empty() ? std::string() : Args.front();
		                          for (const std::string& Line : TextureStreaming::FormatStats(GetTextureStreamingStats()))
		                          {
			                          Output.Print(Line);
		                          }
		                          for (const auto& [Id, Entry] : Streaming.Entries)
		                          {
			                          const std::string Name = FStringConv::ToUtf8(Entry.DebugName);
			                          if (!Filter.empty() && Name.find(Filter) == std::string::npos)
			                          {
				                          continue;
			                          }
			                          const FMipRecord& Top = Entry.Layout.Mips[Entry.ResidentTop];
			                          Output.Printf("  {} {}x{}: 상주 밉 {} ({}x{}, {}) 목표 {} 꼬리 {}{}{}", Name, Entry.Layout.Width, Entry.Layout.Height, Entry.ResidentTop,
			                                        Top.Width, Top.Height, ResourceGc::FormatBytes(Entry.RangeBytes[Entry.ResidentTop]), Entry.TargetTop, Entry.TailTop,
			                                        Entry.bPinned ? " 고정" : "", Entry.bBroken ? " 읽기 실패" : "");
		                          }
	                          } });
	Streaming.bOwnsConsole = true;
}

// ---------------------------------------------------------------- 필요 밉 보고

void FResourceManager::ReportTextureStreamingView(const FTextureStreamingView& View)
{
	if (!bAsyncLoadingEnabled || Streaming.Entries.empty() || View.Instances == nullptr || View.ScreenHeight == 0)
	{
		return;
	}
	E_PROFILE_SCOPE("텍스처 스트리밍 보고");
	Streaming.bReportedThisFrame = true;

	// 머티리얼마다 log2(UV/픽셀) 최솟값 (가장 세밀한 요구)과 우선순위 (화면 크기 최댓값)
	struct FMaterialNeed
	{
		float MinLog2  = std::numeric_limits<float>::infinity();
		float Priority = 0.0f;
	};
	std::unordered_map<const FMaterial*, FMaterialNeed> Needs;
	Needs.reserve(256);
	const float NegativeInfinity = -std::numeric_limits<float>::infinity();
	for (const FMeshInstance& Instance : View.Instances->GetInstances())
	{
		if (Instance.Material == nullptr || Instance.Mesh == nullptr)
		{
			continue;
		}
		const bool bMain = !View.IsInMainView || View.IsInMainView(Instance.WorldBounds);
		// 메인 뷰 밖: 그림자 패스만 그린다 — 텍스처를 읽는 것은 Masked(알파 테스트)뿐
		if (!bMain && (!Instance.IsMasked() || !View.IsShadowCaster || !View.IsShadowCaster(Instance.WorldBounds)))
		{
			continue;
		}
		const FVector3 Center = Instance.WorldBounds.GetCenter();
		const FVector3 Extent = Instance.WorldBounds.GetExtent();
		float          CmPerPixel;
		float          ScreenSize;
		if (View.bOrthographic)
		{
			CmPerPixel = ComputeOrthographicCmPerPixel(View.OrthoHeight, View.ScreenHeight);
			ScreenSize = View.OrthoHeight > 0.0f ? 2.0f * Extent.Length() / View.OrthoHeight : 1.0f;
		}
		else
		{
			// 경계 상자의 가장 가까운 시야 깊이 (원근 픽셀 크기는 깊이에 비례)
			const FVector3& F     = View.CameraForward;
			const float     Depth = FVector3::Dot(Center - View.CameraPosition, F) -
			                    (std::fabs(F.X) * Extent.X + std::fabs(F.Y) * Extent.Y + std::fabs(F.Z) * Extent.Z);
			const float NearDepth = std::max(Depth, View.NearZ);
			CmPerPixel            = ComputePerspectiveCmPerPixel(NearDepth, View.TanHalfFovY, View.ScreenHeight);
			ScreenSize            = View.TanHalfFovY > 0.0f ? Extent.Length() / (NearDepth * View.TanHalfFovY) : 1.0f;
		}
		const float Scale   = Instance.IsSkinned() ? 1.0f : GetMaxAxisScale(Instance.World);
		const float Density = Scale > 1.0e-6f ? Instance.Mesh->GetUvDensity() / Scale : 0.0f;
		const float Log2    = ComputeLog2UvPerPixel(Density, CmPerPixel); // 알 수 없음 = -무한대 → 밉 0

		FMaterialNeed& Need = Needs[Instance.Material];
		Need.MinLog2        = std::min(Need.MinLog2, Log2);
		Need.Priority       = std::max(Need.Priority, ScreenSize * (bMain ? 1.0f : ShadowOnlyPriorityScale));
	}
	for (const FMaterialHandle Handle : View.FullResidencyMaterials)
	{
		if (const FMaterial* Material = Materials.Get(Handle))
		{
			FMaterialNeed& Need = Needs[Material];
			Need.MinLog2        = NegativeInfinity;
			Need.Priority       = FullResidencyPriority;
		}
	}

	const int32 Margin = std::max(0, RendererCVars::StreamingMipMargin.Get());
	for (const auto& [Material, Need] : Needs)
	{
		const uint32 Count = std::min(std::max(Material->TextureCount, static_cast<uint32>(MaterialSlot_Count)), MaterialTextureMax);
		for (uint32 Slot = 0; Slot < Count; ++Slot)
		{
			const FTextureHandle Texture = Material->Textures[Slot];
			if (!Texture.IsValid())
			{
				continue;
			}
			const auto Found = Streaming.Entries.find(Texture.ToId());
			if (Found == Streaming.Entries.end())
			{
				continue;
			}
			FTextureStreamingState::FEntry& Entry  = Found->second;
			const float                     Tiling = Material->TextureUvTiling[Slot];
			float                           Log2   = Need.MinLog2;
			if (Tiling < 0.0f)
			{
				Log2 = NegativeInfinity; // 계산된 UV: 항상 전체
			}
			else if (Tiling > 0.0f && std::isfinite(Log2))
			{
				Log2 += std::log2(Tiling);
			}
			const uint32 MaxSize = std::max(Entry.Layout.Width, Entry.Layout.Height);
			const uint32 Top     = std::min(ComputeRequiredTopMip(MaxSize, Log2, View.MipBias, Margin, Entry.Layout.MipCount), Entry.TailTop);
			Entry.FrameWantedTop = Entry.FrameWantedTop == InvalidMip ? Top : std::min(Entry.FrameWantedTop, Top);
			Entry.FramePriority  = std::max(Entry.FramePriority, Need.Priority);
		}
	}

	// 결정적 모드: 더 세밀한 밉이 필요해진 텍스처를 지금 채운다 (이 뷰의 그래프 실행 전 — 같은 프레임에 그림). 내리는 것은 다음 BeginFrame
	if (IsStreamingEnabled() && IsStreamingDeterministic())
	{
		UpdateStreamingTargets(0.0f, false);
		IssueStreamingRequests(false, true);
		FinishStreamingNow();
	}
}

// ---------------------------------------------------------------- 목표 / 요청

void FResourceManager::UpdateStreamingTargets(float DeltaSeconds, bool bFinal)
{
	const bool  bEnabled  = IsStreamingEnabled();
	const float DropDelay = std::max(0.0f, RendererCVars::StreamingDropDelay.Get());

	std::vector<FBudgetItem>                     Items;
	std::vector<FTextureStreamingState::FEntry*> ItemEntries;
	Items.reserve(Streaming.Entries.size());
	ItemEntries.reserve(Streaming.Entries.size());
	uint64 WantedBytes = 0;
	for (auto& [Id, Entry] : Streaming.Entries)
	{
		if (Entry.bPinned || !bEnabled)
		{
			Entry.TargetTop = 0; // 고정 / 스트리밍 끔: 전체
			continue;
		}
		const bool   bSeen = Entry.FrameWantedTop != InvalidMip;
		const uint32 Want  = bSeen ? Entry.FrameWantedTop : Entry.TailTop; // 아무 뷰도 쓰지 않음 = 꼬리만
		uint32       Held  = Want;
		if (bFinal)
		{
			if (Entry.bInitialFull)
			{
				// 결정적 모드에서 전체로 올린 텍스처: 첫 보고에서 기다리지 않고 필요 밉으로
				Entry.Hysteresis   = FHysteresisState{ Want, 0.0f };
				Entry.bInitialFull = false;
			}
			else
			{
				Held = UpdateHysteresis(Entry.Hysteresis, Want, DeltaSeconds, DropDelay);
			}
		}
		else if (Entry.Hysteresis.HeldTop != InvalidMip)
		{
			// 보고 중간: 이번 프레임 보고로 더 세밀해진 것만 반영 (내림·시간 진행 없음)
			Held = bSeen ? std::min(Entry.Hysteresis.HeldTop, Entry.FrameWantedTop) : Entry.Hysteresis.HeldTop;
		}
		else if (!bSeen)
		{
			Held = Entry.ResidentTop; // 아직 첫 갱신 전이고 이번 프레임에 안 보임: 그대로
		}
		WantedBytes += Entry.RangeBytes[std::min(Want, Entry.TailTop)];
		Items.push_back({ Held, Entry.TailTop, Entry.FramePriority, Entry.RangeBytes.data() });
		ItemEntries.push_back(&Entry);
	}

	Streaming.PoolBytes        = GetStreamingPoolBytes();
	const FBudgetResult Result = FitToBudget(Items, Streaming.PoolBytes);
	for (size_t Index = 0; Index < ItemEntries.size(); ++Index)
	{
		ItemEntries[Index]->TargetTop = Result.Tops[Index];
	}
	if (!bFinal)
	{
		return;
	}
	Streaming.LastWantedBytes = WantedBytes;
	Streaming.bLastOverBudget = Result.bOverBudget;
	if (Result.bOverBudget && !Streaming.bWarnedOverBudget)
	{
		E_LOG(LogRenderer, Warning, "텍스처 스트리밍 예산 초과: 모든 텍스처를 꼬리 밉까지 줄여도 {} > 예산 {} (r.Streaming.PoolSizeMB)",
		      ResourceGc::FormatBytes(Result.TotalBytes), ResourceGc::FormatBytes(Streaming.PoolBytes));
	}
	Streaming.bWarnedOverBudget = Result.bOverBudget;
	// 진단: 항목 수가 바뀐 첫 갱신마다 한 줄 (씬 로드 뒤 필요/전체 크기)
	if (Streaming.Entries.size() != Streaming.LastLoggedEntryCount)
	{
		Streaming.LastLoggedEntryCount = Streaming.Entries.size();
		uint32 Pinned = 0, Unseen = 0;
		uint64 Full   = 0;
		for (const auto& [Id, Entry] : Streaming.Entries)
		{
			Pinned += Entry.bPinned ? 1u : 0u;
			Unseen += !Entry.bPinned && Entry.FrameWantedTop == InvalidMip ? 1u : 0u;
			Full += Entry.bPinned ? 0u : Entry.RangeBytes[0];
		}
		E_LOG(LogRenderer, Log, "[텍스처 스트리밍] 항목 {} (고정 {}, 이번 보고에 없음 {}): 필요 {} / 전체 밉이면 {}, 예산 {}", Streaming.Entries.size(), Pinned, Unseen,
		      ResourceGc::FormatBytes(WantedBytes), ResourceGc::FormatBytes(Full), ResourceGc::FormatBytes(Streaming.PoolBytes));
	}
	for (auto& [Id, Entry] : Streaming.Entries)
	{
		Entry.FrameWantedTop = InvalidMip;
		Entry.FramePriority  = 0.0f;
	}
}

void FResourceManager::IssueStreamingRequests(bool bAllowDrops, bool bUnlimited)
{
	if (Rhi == nullptr)
	{
		return;
	}
	struct FCandidate
	{
		FTextureStreamingState::FEntry* Entry;
		bool                            bDrop;
		float                           Score;
	};
	std::vector<FCandidate> Candidates;
	uint32                  InFlight = 0;
	for (auto& [Id, Entry] : Streaming.Entries)
	{
		if (Entry.PendingTop != InvalidMip)
		{
			++InFlight;
			continue;
		}
		const FD3D12Texture* Live = Textures.Get(Entry.Handle);
		if (Entry.bBroken || Entry.TargetTop == Entry.ResidentTop || Live == nullptr || !Live->IsReady())
		{
			continue;
		}
		const bool bDrop = Entry.TargetTop > Entry.ResidentTop;
		if (bDrop && !bAllowDrops)
		{
			continue;
		}
		const float Levels = static_cast<float>(bDrop ? Entry.TargetTop - Entry.ResidentTop : Entry.ResidentTop - Entry.TargetTop);
		Candidates.push_back({ &Entry, bDrop, Levels * (Entry.bPinned ? FullResidencyPriority : std::max(Entry.FramePriority, 1.0e-3f)) });
	}
	if (Candidates.empty())
	{
		return;
	}
	// 내리기 먼저 (메모리를 비우고 읽기가 작다), 그다음 올리기: 부족한 밉 수 × 우선순위 큰 순. 같으면 번호 순 (안정 정렬)
	std::stable_sort(Candidates.begin(), Candidates.end(), [](const FCandidate& A, const FCandidate& B) {
		if (A.bDrop != B.bDrop)
		{
			return A.bDrop;
		}
		return A.Score > B.Score;
	});
	const uint64 FrameCap    = static_cast<uint64>(std::max(1.0f, RendererCVars::StreamingMaxUploadMBPerFrame.Get()) * static_cast<float>(Megabyte));
	uint64       IssuedBytes = 0;
	uint32       Issued      = 0;
	for (const FCandidate& Candidate : Candidates)
	{
		FTextureStreamingState::FEntry& Entry = *Candidate.Entry;
		const uint64                    Bytes = Entry.RangeBytes[Entry.TargetTop];
		if (!bUnlimited && (InFlight + Issued >= MaxConcurrentRequests || (Issued > 0 && IssuedBytes + Bytes > FrameCap)))
		{
			break; // 다음 프레임에 이어서 (끊김 방지)
		}
		IssueStreamingRequest(Entry, Entry.TargetTop);
		IssuedBytes += Bytes;
		++Issued;
	}
}

void FResourceManager::IssueStreamingRequest(FTextureStreamingState::FEntry& Entry, uint32 NewTop)
{
	struct FReadResult
	{
		std::vector<FTextureMip> Mips;
		bool                     bOk = false;
	};
	auto         Result = std::make_shared<FReadResult>();
	const uint32 Serial = Streaming.NextSerial++;
	Entry.RequestSerial = Serial;
	Entry.PendingTop    = NewTop;
	const uint64 Id     = Entry.Handle.ToId();
	LoadJobs.Submit(
		[Result, Source = Entry.Source, Layout = Entry.Layout, NewTop] {
			E_PROFILE_SCOPE("텍스처 밉 읽기");
			std::vector<uint8> Bytes;
			Result->bOk = FFileSystem::ReadFileRange(Source.File, Source.PayloadOffset + Layout.GetRangeReadOffset(NewTop), Layout.GetRangeReadSize(NewTop), Bytes) &&
			              ParseMipRange(Layout, NewTop, Bytes.data(), Bytes.size(), Result->Mips);
		},
		[this, Result, Id, Serial] {
			const auto Found = Streaming.Entries.find(Id);
			if (Found == Streaming.Entries.end() || Found->second.RequestSerial != Serial)
			{
				return; // 그사이 삭제·재등록됨
			}
			FTextureStreamingState::FEntry& Entry = Found->second;
			if (!Result->bOk || Result->Mips.empty())
			{
				E_LOG(LogRenderer, Warning, "텍스처 밉을 다시 읽지 못했습니다 (쿠킹 파일이 바뀌었거나 손상 — 이 텍스처는 더 스트리밍하지 않음): {}",
				      FStringConv::ToUtf8(Entry.DebugName));
				Entry.bBroken    = true;
				Entry.PendingTop = InvalidMip;
				return;
			}
			std::vector<FD3D12Texture::FMipData> Mips;
			Mips.reserve(Result->Mips.size());
			uint64 Bytes = 0;
			for (const FTextureMip& Mip : Result->Mips)
			{
				Mips.push_back({ Mip.Data.data(), Mip.Data.size() });
				Bytes += Mip.Data.size();
			}
			auto Texture = std::make_unique<FD3D12Texture>();
			if (!Texture->Init2DFromMipsAsync(Rhi->GetDevice(), Rhi->GetUploadQueue(), Rhi->GetSrvAllocator(), Result->Mips[0].Width, Result->Mips[0].Height,
			                                  ToDxgiFormat(Entry.Layout.Format, Entry.Layout.bSRGB), Mips.data(), static_cast<uint32>(Mips.size()),
			                                  Entry.DebugName.c_str()))
			{
				Entry.bBroken    = true;
				Entry.PendingTop = InvalidMip;
				return;
			}
			Entry.PendingFence   = Texture->GetUploadFence();
			Entry.PendingTexture = std::move(Texture);
			Streaming.UploadedBytes += Bytes;
			Streaming.WindowBytes += Bytes;
		});
}

void FResourceManager::CompleteStreamUploads(uint64 FinalizedFence)
{
	std::vector<FTextureHandle> Swapped;
	for (auto& [Id, Entry] : Streaming.Entries)
	{
		if (!Entry.PendingTexture || Entry.PendingFence > FinalizedFence)
		{
			continue;
		}
		FD3D12Texture* Live = Textures.Get(Entry.Handle);
		Entry.PendingTexture->MarkUploadComplete();
		if (Live != nullptr)
		{
			// 같은 핸들 뒤의 리소스/SRV를 새 밉 범위로 (이전 것은 진행 중인 프레임이 읽을 수 있어 지연 해제)
			Live->SwapContents(*Entry.PendingTexture);
			E_LOG(LogRenderer, Verbose, "[텍스처 스트리밍] {}: 밉 {} → {} ({}x{})", FStringConv::ToUtf8(Entry.DebugName), Entry.ResidentTop, Entry.PendingTop,
			      Live->GetWidth(), Live->GetHeight());
			Entry.ResidentTop = Entry.PendingTop;
			Swapped.push_back(Entry.Handle);
		}
		Entry.PendingTexture->ShutdownDeferred(*Rhi);
		Entry.PendingTexture.reset();
		Entry.PendingTop   = InvalidMip;
		Entry.PendingFence = 0;
	}
	if (Swapped.empty())
	{
		return;
	}
	{
		const FTextureStreamingStats Stats = GetTextureStreamingStats();
		E_LOG(LogRenderer, Log, "[텍스처 스트리밍] 교체 {}개 → 상주 {} / 전체 밉이면 {} (줄어듦 {}/{})", Swapped.size(), ResourceGc::FormatBytes(Stats.ResidentBytes),
		      ResourceGc::FormatBytes(Stats.FullBytes), Stats.ReducedTextures, Stats.StreamingTextures);
	}
	// 그 텍스처를 쓰는 머티리얼 테이블을 다시 만든다 (RefreshMaterialTextures 경로 — DXR 바인드리스 등 테이블 사용자 공통)
	std::vector<FMaterialHandle> ToRefresh;
	Materials.ForEach([&](FMaterialHandle Handle, FMaterial& Material) {
		for (const FTextureHandle& Slot : Material.Textures)
		{
			if (Slot.IsValid() && std::find(Swapped.begin(), Swapped.end(), Slot) != Swapped.end())
			{
				ToRefresh.push_back(Handle);
				break;
			}
		}
	});
	for (const FMaterialHandle Handle : ToRefresh)
	{
		RefreshMaterialTextures(Handle);
	}
}

void FResourceManager::FinishStreamingNow()
{
	E_PROFILE_SCOPE("텍스처 스트리밍 비우기");
	while (GetStreamingPendingCount() > 0)
	{
		LoadJobs.WaitIdle();
		LoadJobs.PumpCompletions();
		Rhi->FlushUploads();
		CompletePendingUploads(Rhi->GetUploadQueue().GetFinalizedFence());
		CompleteStreamUploads(Rhi->GetUploadQueue().GetFinalizedFence());
	}
}

void FResourceManager::ProcessTextureStreaming()
{
	if (!bAsyncLoadingEnabled || Rhi == nullptr)
	{
		return;
	}
	E_PROFILE_SCOPE("텍스처 스트리밍 갱신");
	CompleteStreamUploads(Rhi->GetUploadQueue().GetFinalizedFence());

	// 업로드 속도 (1초 창)
	const auto Now = std::chrono::steady_clock::now();
	if (!Streaming.bWindowStarted)
	{
		Streaming.WindowStart    = Now;
		Streaming.bWindowStarted = true;
	}
	const float WindowSeconds = std::chrono::duration<float>(Now - Streaming.WindowStart).count();
	if (WindowSeconds >= 1.0f)
	{
		Streaming.UploadMBPerSecond = static_cast<float>(static_cast<double>(Streaming.WindowBytes) / static_cast<double>(Megabyte)) / WindowSeconds;
		Streaming.WindowBytes       = 0;
		Streaming.WindowStart       = Now;
	}

	// 측정용 주기 로그 (r.Streaming.LogStats 초)
	if (const float Interval = RendererCVars::StreamingLogStats.Get(); Interval > 0.0f)
	{
		if (!Streaming.bStatsLogStarted || std::chrono::duration<float>(Now - Streaming.LastStatsLog).count() >= Interval)
		{
			Streaming.bStatsLogStarted = true;
			Streaming.LastStatsLog     = Now;
			const FResourceMemoryStats Memory = GetMemoryStats();
			std::string                Line;
			for (const std::string& Part : TextureStreaming::FormatStats(GetTextureStreamingStats()))
			{
				Line += (Line.empty() ? "" : " | ") + Part;
			}
			E_LOG(LogRenderer, Display, "[텍스처 스트리밍 통계] 텍스처 {}개 {} | VRAM {} | {}", Memory.Textures, ResourceGc::FormatBytes(Memory.TextureBytes),
			      ResourceGc::FormatBytes(Memory.LocalUsage), Line);
		}
	}

	const bool bDeterministic = IsStreamingDeterministic();
	float      DeltaSeconds   = DeterministicDeltaSeconds;
	if (!bDeterministic)
	{
		DeltaSeconds = Streaming.bHasLastUpdate ? std::clamp(std::chrono::duration<float>(Now - Streaming.LastUpdate).count(), 0.0f, 0.5f) : 0.0f;
	}
	Streaming.LastUpdate     = Now;
	Streaming.bHasLastUpdate = true;
	if (Streaming.Entries.empty())
	{
		Streaming.bReportedThisFrame = false;
		return;
	}

	// 보고가 없는 프레임(창 최소화·씬 렌더 없음)은 목표를 유지한다. 스트리밍을 끄면 바로 전체로
	if (Streaming.bReportedThisFrame || !IsStreamingEnabled())
	{
		UpdateStreamingTargets(DeltaSeconds, true);
	}
	Streaming.bReportedThisFrame = false;
	IssueStreamingRequests(true, bDeterministic);
	if (bDeterministic)
	{
		FinishStreamingNow();
	}
}

FTextureStreamingStats FResourceManager::GetTextureStreamingStats() const
{
	FTextureStreamingStats Stats;
	Stats.bActive           = bAsyncLoadingEnabled;
	Stats.bEnabled          = IsStreamingEnabled();
	Stats.PoolBytes         = Streaming.PoolBytes > 0 ? Streaming.PoolBytes : GetStreamingPoolBytes();
	Stats.WantedBytes       = Streaming.LastWantedBytes;
	Stats.UploadMBPerSecond = Streaming.UploadMBPerSecond;
	Stats.UploadedBytes     = Streaming.UploadedBytes;
	Stats.bOverBudget       = Streaming.bLastOverBudget;
	for (const auto& [Id, Entry] : Streaming.Entries)
	{
		const uint64 Resident = Entry.RangeBytes[Entry.ResidentTop];
		if (Entry.bPinned)
		{
			++Stats.PinnedTextures;
			Stats.PinnedBytes += Resident;
		}
		else
		{
			++Stats.StreamingTextures;
			Stats.ResidentBytes += Resident;
			Stats.FullBytes += Entry.RangeBytes[0];
			Stats.ReducedTextures += Entry.ResidentTop > 0 ? 1u : 0u;
		}
		Stats.PendingRequests += Entry.PendingTop != InvalidMip ? 1u : 0u;
		Stats.BrokenSources += Entry.bBroken ? 1u : 0u;
	}
	return Stats;
}
