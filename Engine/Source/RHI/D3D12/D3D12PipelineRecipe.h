#pragma once

#include "RHI/D3D12/D3D12Common.h"

#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// PSO 캐시 (Phase 48) 순수 부분: PSO 설명을 포인터 없는 "레시피"로 직렬화해 키(64비트 해시)를 만들고, 다시 D3D12 설명으로 되살린다.
//   - 레시피 = 루트 시그니처 블롭 해시 + 셰이더 바이트코드 해시/크기 + 고정 기능 상태(필드별 기록 — 구조체 패딩을 해시하지 않는다)
//   - 키 = FNV-1a 64(레시피 바이트). 같은 설명 + 같은 바이트코드 → 같은 키, 바이트코드가 바뀌면(셰이더 핫 리로드·머티리얼 변형) 새 키
//   - 레시피 파일(.epso)은 레시피 + 블롭(루트 시그니처·셰이더)을 담아 드라이버와 무관 → 프로젝트에 넣어 패키지에 포함할 수 있다
//   - 드라이버 캐시(ID3D12PipelineLibrary 직렬화)는 어댑터·드라이버마다 다르므로 FPipelineLibraryHeader로 무효화한다
//   지원하지 않는 설명(HS/DS/GS, 스트림 출력, 캐시된 PSO 블롭)은 캐시를 거치지 않는다 (IsCacheable = false)
namespace PipelineCache
{
	uint64 HashBytes(const void* Data, size_t Size, uint64 Seed = 0xcbf29ce484222325ull);

	enum class EPipelineType : uint8
	{
		Graphics = 0,
		Compute  = 1,
	};

	struct FInputElement
	{
		std::string                SemanticName;
		uint32                     SemanticIndex = 0;
		DXGI_FORMAT                Format        = DXGI_FORMAT_UNKNOWN;
		uint32                     InputSlot     = 0;
		uint32                     AlignedByteOffset = 0;
		D3D12_INPUT_CLASSIFICATION Classification    = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
		uint32                     InstanceDataStepRate = 0;
	};

	struct FShaderRef
	{
		uint64 Hash = 0;
		uint32 Size = 0; // 0 = 없음
	};

	struct FRecipe
	{
		EPipelineType Type              = EPipelineType::Graphics;
		uint64        RootSignatureHash = 0;
		FShaderRef    Shaders[2];        // 그래픽스: VS, PS / 계산: CS, -
		// 그래픽스 고정 기능 상태
		D3D12_BLEND_DESC                    Blend{};
		uint32                              SampleMask = 0;
		D3D12_RASTERIZER_DESC               Rasterizer{};
		D3D12_DEPTH_STENCIL_DESC            DepthStencil{};
		std::vector<FInputElement>          InputLayout;
		D3D12_INDEX_BUFFER_STRIP_CUT_VALUE  StripCut      = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
		D3D12_PRIMITIVE_TOPOLOGY_TYPE       TopologyType  = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
		uint32                              NumRenderTargets = 0;
		DXGI_FORMAT                         RtvFormats[8] = {};
		DXGI_FORMAT                         DsvFormat     = DXGI_FORMAT_UNKNOWN;
		DXGI_SAMPLE_DESC                    SampleDesc{ 1, 0 };
		uint32                              NodeMask = 0;
		D3D12_PIPELINE_STATE_FLAGS          Flags    = D3D12_PIPELINE_STATE_FLAG_NONE;
	};

	bool IsCacheable(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& Desc);
	bool IsCacheable(const D3D12_COMPUTE_PIPELINE_STATE_DESC& Desc);

	// 설명 → 레시피 (루트 시그니처 해시는 호출자가: 등록된 블롭 해시)
	FRecipe FromDesc(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& Desc, uint64 RootSignatureHash);
	FRecipe FromDesc(const D3D12_COMPUTE_PIPELINE_STATE_DESC& Desc, uint64 RootSignatureHash);

	// 레시피 ↔ 바이트 (필드별, 리틀 엔디언 그대로). Deserialize 실패(잘림·형식) = false
	std::vector<uint8> Serialize(const FRecipe& Recipe);
	bool               Deserialize(std::span<const uint8> Bytes, FRecipe& OutRecipe);
	uint64             ComputeKey(const FRecipe& Recipe); // HashBytes(Serialize)
	std::wstring       MakeLibraryName(uint64 Key);       // ID3D12PipelineLibrary 항목 이름

	// 레시피 → D3D12 설명 (입력 배치 저장소는 OutElements가 소유 — 설명보다 오래 살아야 한다)
	D3D12_GRAPHICS_PIPELINE_STATE_DESC ToGraphicsDesc(const FRecipe& Recipe, ID3D12RootSignature* RootSignature, D3D12_SHADER_BYTECODE Vertex,
	                                                  D3D12_SHADER_BYTECODE Pixel, std::vector<D3D12_INPUT_ELEMENT_DESC>& OutElements);
	D3D12_COMPUTE_PIPELINE_STATE_DESC  ToComputeDesc(const FRecipe& Recipe, ID3D12RootSignature* RootSignature, D3D12_SHADER_BYTECODE Compute);

	// ---- 레시피 파일 (.epso): 블롭(루트 시그니처·셰이더, 해시로 중복 제거) + 레시피 + 마지막 사용 실행 번호
	struct FRecipeEntry
	{
		FRecipe Recipe;
		uint32  LastUsedRun = 0;
	};
	struct FRecipeFile
	{
		static constexpr uint32 Magic   = 0x4F535045; // "EPSO"
		static constexpr uint32 Version = 1;

		uint32                                          RunCounter = 0; // 이 파일을 쓴 실행 번호 (사용자 캐시가 실행마다 1 올림)
		std::unordered_map<uint64, std::vector<uint8>>  Blobs;          // 해시 → 바이트
		std::unordered_map<uint64, FRecipeEntry>        Recipes;        // 키 → 레시피

		std::vector<uint8> Write() const;
		bool               Read(std::span<const uint8> Bytes); // 실패하면 비운다

		// 블롭이 모두 있는 레시피인가 (워밍 가능)
		bool HasBlobs(const FRecipe& Recipe) const;
		// 오래 안 쓴 레시피 제거: CurrentRun - LastUsedRun > MaxAgeRuns. 쓰이지 않는 블롭도 제거. 반환 = 지운 레시피 수
		uint32 Prune(uint32 CurrentRun, uint32 MaxAgeRuns);
		// 다른 파일의 레시피/블롭을 합친다 (같은 키는 LastUsedRun 큰 쪽)
		void Merge(const FRecipeFile& Other);
	};

	// ---- 드라이버 캐시(파이프라인 라이브러리 직렬화) 파일 머리: 어댑터·드라이버·엔진 형식이 같아야 쓴다
	struct FLibraryHeader
	{
		static constexpr uint32 Magic   = 0x4C535045; // "EPSL"
		static constexpr uint32 Version = 3; // 2: 라이브러리 블롭 뒤 저장 키 목록, 3: 동시 PSO 작업으로 저장됐을 수 있는 캐시 버림 (D3D12PipelineCache.h (c))

		uint32 FileMagic     = Magic;
		uint32 FileVersion   = Version;
		uint32 VendorId      = 0;
		uint32 DeviceId      = 0;
		uint32 SubSysId      = 0;
		uint32 Revision      = 0;
		uint64 DriverVersion = 0; // IDXGIAdapter::CheckInterfaceSupport UMD 버전
		uint64 DataSize      = 0; // 뒤따르는 라이브러리 바이트 수
	};
	static_assert(sizeof(FLibraryHeader) == 40);
	bool IsLibraryCompatible(const FLibraryHeader& Stored, const FLibraryHeader& Current);
} // namespace PipelineCache
