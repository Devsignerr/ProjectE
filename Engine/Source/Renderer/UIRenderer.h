#pragma once

#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "RHI/D3D12/D3D12RootSignature.h"
#include "Scene/ResourceHandles.h"

#include <filesystem>
#include <string>
#include <unordered_map>

class FD3D12RHI;
class FD3D12Texture;
class FResourceManager;
class FShaderLibrary;
class FUIFont;
struct FUIDrawList;
struct FUITextureRef;

// 게임 UI 그리기 (UI.hlsl): 그리기 목록의 사각형을 동적 업로드 버퍼에 올리고 묶음마다 시저 + 텍스처를 바꿔 인스턴스로 그린다.
// 파일 텍스처는 FResourceManager::LoadTexture(Content 기준 경로, 색상), 글꼴 아틀라스는 버전이 바뀔 때 R8 텍스처로 다시 만든다.
// 깊이 없이 출력 위에 알파 블렌드 — 씬/포스트 뒤, 에디터 ImGui 앞에 호출한다.
class FUIRenderer
{
public:
	~FUIRenderer();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary, FResourceManager& InResources, DXGI_FORMAT InColorFormat);
	void Shutdown();
	bool ReloadShaders(bool bForceRecompile);

	// Output: 렌더 타깃 상태의 출력 (이 함수가 바인딩한다). ContentDirectory: 텍스처 상대 경로 기준
	void Render(const FUIDrawList& DrawList, const FRenderOutput& Output, const std::filesystem::path& ContentDirectory);

	bool IsInitialized() const { return Rhi != nullptr; }

private:
	bool                 CreatePipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile);
	const FD3D12Texture& ResolveTexture(const FUITextureRef& Texture, const std::filesystem::path& ContentDirectory);
	FTextureHandle       GetFontTexture(const FUIFont& Font);

	struct FFontTexture
	{
		FTextureHandle Handle;
		uint32         Version = 0;
		uint32         Size    = 0;
	};

	FD3D12RHI*        Rhi           = nullptr;
	FShaderLibrary*   ShaderLibrary = nullptr;
	FResourceManager* Resources     = nullptr;
	uint32            ResourceRootProviderId = 0; // FResourceManager::AddRootProvider (FileTextures)
	DXGI_FORMAT       ColorFormat   = DXGI_FORMAT_UNKNOWN;

	FD3D12RootSignature RootSignature;
	FD3D12PipelineState Pipeline;

	std::unordered_map<const FUIFont*, FFontTexture> FontTextures;
	std::unordered_map<std::wstring, FTextureHandle> FileTextures; // 키: 절대 경로 (실패도 무효 핸들로 기억해 매 프레임 다시 읽지 않는다)
	bool                                             bWarnedBufferFull = false;
};
