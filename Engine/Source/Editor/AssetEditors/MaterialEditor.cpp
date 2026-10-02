#include "Editor/AssetEditors/MaterialEditor.h"

#include "Core/CommandLine.h"
#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Editor/AssetEditors/AssetEditorWidgets.h"
#include "Editor/AssetEditors/MaterialGraphEditing.h"
#include "Editor/AssetEditors/MaterialGraphPanel.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "RHI/D3D12/D3D12Texture.h"
#include "Renderer/ResourceManager.h"

#include <imgui.h>

#include <algorithm>
#include <format>

E_DECLARE_LOG_CATEGORY(LogEditor)

FMaterialEditor::FMaterialEditor(std::filesystem::path InPath)
	: FAssetEditor(std::move(InPath))
	, GraphPanel(std::make_unique<FMaterialGraphPanel>())
{
	// 자동 검증: 열 때 고를 노드 / 팔레트
	const FCommandLine& CommandLine = FCommandLine::FromProcess();
	if (const std::wstring Select = CommandLine.GetValue(L"--matgraph-select"); !Select.empty())
	{
		GraphPanel->SetAutoSelect(FStringConv::ToUtf8(Select));
	}
	if (CommandLine.HasFlag(L"--matgraph-palette"))
	{
		GraphPanel->RequestPalette();
	}
}

FMaterialEditor::~FMaterialEditor() = default;

void FMaterialEditor::LayoutMissingPositions(FMaterialGraph& Graph) const
{
	if (!MaterialGraphEditing::HasMissingPositions(Graph))
	{
		return;
	}
	// 노드 위치가 하나도 없으면 전체 자동 배치, 일부만 없으면 없는 것만
	const bool bAnyPositioned = std::any_of(Graph.Nodes.begin(), Graph.Nodes.end(), [](const FMaterialGraphNode& Node) {
		return Node.EditorPosition.X != 0.0f || Node.EditorPosition.Y != 0.0f;
	});
	MaterialGraphEditing::AutoLayout(Graph, bAnyPositioned);
}

bool FMaterialEditor::LoadAsset(FAssetEditorEnvironment& Env)
{
	FMaterialAsset Loaded;
	if (!Loaded.LoadFromFile(Path))
	{
		return false;
	}
	Asset = std::move(Loaded);
	if (Asset.IsGraphMaterial() && !Asset.IsInstance())
	{
		LayoutMissingPositions(Asset.Graph);
	}
	ApplyDelay = -1.0f;

	// 씬과 같은 머티리얼 리소스 (경로 캐시). 열 때/되돌릴 때 파일 내용으로 맞춘다
	if (!Env.Resources->GetMaterial(Material))
	{
		Material = Env.Resources->LoadMaterial(Path);
		if (!Material.IsValid())
		{
			return false;
		}
	}
	ApplyToMaterial(Env);
	GraphPanel->OnAssetReloaded(true);

	if (!Preview.GetScene().GetRegistry().IsValid(PreviewEntity))
	{
		PreviewEntity = Preview.GetScene().CreateEntity("PreviewMesh");
		Preview.GetScene().GetRegistry().Emplace<FStaticMeshComponent>(PreviewEntity);
		SetPreviewShape(Env, PreviewShape.c_str());
	}
	if (TextureFiles.empty() && Env.Editor != nullptr)
	{
		ScanFiles(Env.Editor->ContentDirectory);
	}
	return true;
}

bool FMaterialEditor::SaveAsset(FAssetEditorEnvironment& Env)
{
	// 디바운스 중인 반영이 있으면 먼저 (저장 후 파일 감시가 디스크 내용으로 다시 읽는다 — ReloadMaterialFile)
	if (ApplyDelay > 0.0f)
	{
		ApplyDelay = -1.0f;
		ApplyToMaterial(Env);
	}
	return Asset.SaveToFile(Path);
}

std::string FMaterialEditor::CaptureState() const
{
	return Asset.ToJsonString();
}

void FMaterialEditor::RestoreState(FAssetEditorEnvironment& Env, const std::string& State)
{
	if (Asset.FromJsonString(State))
	{
		ApplyDelay = -1.0f;
		ApplyToMaterial(Env);
		GraphPanel->OnAssetReloaded(false);
	}
}

void FMaterialEditor::ApplyToMaterial(FAssetEditorEnvironment& Env)
{
	// 해석(부모 체인) + 텍스처 변경 시에만 테이블 재작성은 리소스 관리자가 한다. 이 머티리얼을 부모로 둔 열린 인스턴스도 함께 갱신된다
	Env.Resources->ApplyMaterialAsset(Material, Asset, Path.parent_path());
	RefreshInherited(Env);
	RefreshGraphStatus(Env);
}

void FMaterialEditor::RefreshGraphStatus(FAssetEditorEnvironment& Env)
{
	if (Asset.IsInstance())
	{
		ResolvedGraph = FMaterialAsset{};
		Env.Resources->ResolveMaterialAsset(Asset, Path, ResolvedGraph);
		LayoutMissingPositions(ResolvedGraph.Graph); // 보기용 사본 (부모 파일에 위치가 없을 때)
	}
	else
	{
		ResolvedGraph = Asset;
	}
	bGraphMaterial = ResolvedGraph.IsGraphMaterial();
	GraphCompile   = FMaterialGraphCompileResult{};
	GraphAnalysis  = FMaterialGraphAnalysis{};
	if (bGraphMaterial)
	{
		GraphCompile  = FMaterialGraphCompiler::Compile(ResolvedGraph.Graph, ResolvedGraph.Parameters);
		GraphAnalysis = FMaterialGraphCompiler::Analyze(ResolvedGraph.Graph, ResolvedGraph.Parameters);
	}
}

void FMaterialEditor::CompileOwnGraph()
{
	ResolvedGraph = Asset;
	GraphCompile  = FMaterialGraphCompiler::Compile(Asset.Graph, Asset.Parameters);
	GraphAnalysis = FMaterialGraphCompiler::Analyze(Asset.Graph, Asset.Parameters);
}

void FMaterialEditor::OnGraphEdited(std::string_view Label, bool bSemantic)
{
	MarkEdited(Label);
	if (!bSemantic || Asset.IsInstance())
	{
		return;
	}
	CompileOwnGraph();
	if (!GraphCompile.bSuccess)
	{
		ApplyDelay = -1.0f; // 오류 그래프는 반영하지 않는다 (렌더러는 이전 셰이더 유지)
		return;
	}
	// 셰이더 해시가 그대로(값 조절)면 즉시, 바뀌면 조작이 멈춘 뒤 (새 셰이더 DXC 컴파일)
	const FMaterial* Live = DrawEnv != nullptr ? DrawEnv->Resources->GetMaterial(Material) : nullptr;
	if (Live != nullptr && Live->Shader != nullptr && Live->Shader->Hash == GraphCompile.Shader->Hash)
	{
		ApplyDelay = -1.0f;
		ApplyToMaterial(*DrawEnv);
	}
	else
	{
		ApplyDelay = 0.2f;
	}
}

void FMaterialEditor::Update(FAssetEditorEnvironment& Env, float DeltaSeconds)
{
	if (ApplyDelay > 0.0f)
	{
		ApplyDelay -= DeltaSeconds;
		if (ApplyDelay <= 0.0f)
		{
			ApplyDelay = -1.0f;
			ApplyToMaterial(Env);
		}
	}
	++UpdateFrames;
	VerifyEditStep(Env);
	// 자동 검증: 그래프가 몇 프레임 그려진 뒤(위치 동기화 후) 왕복 확인
	if (!bRoundTripVerified && UpdateFrames >= 10 && FCommandLine::FromProcess().HasFlag(L"--verify-matgraph-roundtrip"))
	{
		bRoundTripVerified = true;
		std::string Message;
		if (VerifyRoundTrip(Env, Message))
		{
			E_LOG(LogEditor, Display, "[머티리얼 그래프 왕복] {}: 성공 — {}", GetDisplayName(), Message);
		}
		else
		{
			E_LOG(LogEditor, Error, "[머티리얼 그래프 왕복] {}: 실패 — {}", GetDisplayName(), Message);
		}
	}
}

void FMaterialEditor::VerifyEditStep(FAssetEditorEnvironment& Env)
{
	// 자동 검증 --verify-matgraph-edit: 노드 추가 + 출력 연결(편집 알림 경로) → 디바운스 뒤 공유 머티리얼 셰이더가 바뀌는지 →
	// 실행 취소로 원래 셰이더로 돌아오는지. 일반 그래프 머티리얼만
	// --matgraph-convert: 그래프 없는 일반 머티리얼을 열자마자 "그래프로 변환" (저장하지 않음 — 화면 확인용)
	static const bool bConvert = FCommandLine::FromProcess().HasFlag(L"--matgraph-convert");
	if (bConvert && UpdateFrames == 5 && !Asset.IsInstance() && !Asset.IsGraphMaterial())
	{
		MaterialGraphEditing::ConvertToGraph(Asset, false);
		ApplyToMaterial(Env);
		MarkEdited("그래프로 변환");
		GraphPanel->OnAssetReloaded(true);
		E_LOG(LogEditor, Display, "자동 검증: {} 그래프로 변환 — 컴파일 {}", GetDisplayName(), GraphCompile.bSuccess ? "성공" : GraphCompile.JoinErrors());
	}
	static const bool bEnabled = FCommandLine::FromProcess().HasFlag(L"--verify-matgraph-edit");
	if (!bEnabled || VerifyEditStage < 0 || Asset.IsInstance() || !Asset.IsGraphMaterial())
	{
		return;
	}
	const auto LiveHash = [&]() -> uint64 {
		const FMaterial* Live = Env.Resources->GetMaterial(Material);
		return Live != nullptr && Live->Shader != nullptr ? Live->Shader->Hash : 0;
	};
	if (VerifyEditStage == 0 && UpdateFrames >= 15)
	{
		VerifyEditHash = LiveHash();
		FMaterialGraphNode* Node = MaterialGraphEditing::AddNode(Asset.Graph, Asset.Parameters, "Constant",
		                                                         Asset.Graph.OutputEditorPosition - FVector2(MaterialGraphEditing::ColumnSpacing, 260.0f));
		Node->Value      = FVector4(0.0f, 2.0f, 0.5f, 0.0f);
		Node->ValueWidth = 3;
		const std::string Id = Node->Id;
		MaterialGraphEditing::Connect(Asset.Graph, Id, 0, FMaterialGraphCompiler::OutputNodeId, "Emissive");
		DrawEnv = &Env;
		OnGraphEdited("자동 검증: 노드 추가", true);
		DrawEnv = nullptr;
		GraphPanel->OnAssetReloaded(false);
		CommitPendingEdit(false);
		VerifyEditStage = 1;
	}
	else if (VerifyEditStage == 1 && UpdateFrames >= 45)
	{
		const uint64 Edited = LiveHash();
		Undo(Env);
		const uint64 Restored = LiveHash();
		const bool   bOk      = GraphCompile.bSuccess && Edited != VerifyEditHash && Restored == VerifyEditHash;
		const std::string Message = std::format("{} — 셰이더 {:016x} → 편집 {:016x} → 실행 취소 {:016x}", GetDisplayName(), VerifyEditHash, Edited, Restored);
		if (bOk)
		{
			E_LOG(LogEditor, Display, "[머티리얼 그래프 편집 검증] 성공: {}", Message);
		}
		else
		{
			E_LOG(LogEditor, Error, "[머티리얼 그래프 편집 검증] 실패: {}", Message);
		}
		VerifyEditStage = -1;
	}
}

bool FMaterialEditor::VerifyRoundTrip(FAssetEditorEnvironment& Env, std::string& OutMessage) const
{
	// 파일 그대로 → 해석 → 컴파일 (기준)
	FMaterialAsset OnDisk;
	if (!OnDisk.LoadFromFile(Path))
	{
		OutMessage = "파일을 읽지 못했습니다";
		return false;
	}
	// 편집 상태 → 저장 형식(SaveToFile과 같은 문자열) → 다시 읽기
	const std::string Saved = Asset.ToJsonString();
	FMaterialAsset    Reloaded;
	if (!Reloaded.FromJsonString(Saved))
	{
		OutMessage = "저장 형식을 다시 읽지 못했습니다";
		return false;
	}
	const bool bIdempotent = Reloaded.ToJsonString() == Saved;
	const auto HashOf      = [&](const FMaterialAsset& Source, std::string& OutError) -> uint64 {
        FMaterialAsset Resolved;
        Env.Resources->ResolveMaterialAsset(Source, Path, Resolved);
        if (!Resolved.IsGraphMaterial())
        {
            return 0;
        }
        const FMaterialGraphCompileResult Result = FMaterialGraphCompiler::Compile(Resolved.Graph, Resolved.Parameters);
        OutError                                 = Result.JoinErrors();
        return Result.bSuccess ? Result.Shader->Hash : 0;
	};
	std::string  DiskError;
	std::string  ReloadError;
	const uint64 DiskHash   = HashOf(OnDisk, DiskError);
	const uint64 ReloadHash = HashOf(Reloaded, ReloadError);
	// 그래프 구조 (위치 제외): 노드 Id/종류/입력 수가 같아야 한다
	FMaterialAsset ResolvedDisk;
	FMaterialAsset ResolvedReload;
	Env.Resources->ResolveMaterialAsset(OnDisk, Path, ResolvedDisk);
	Env.Resources->ResolveMaterialAsset(Reloaded, Path, ResolvedReload);
	bool bSameStructure = ResolvedDisk.Graph.Nodes.size() == ResolvedReload.Graph.Nodes.size() &&
	                      ResolvedDisk.Graph.Outputs.size() == ResolvedReload.Graph.Outputs.size() &&
	                      ResolvedDisk.Parameters.size() == ResolvedReload.Parameters.size();
	for (size_t Index = 0; bSameStructure && Index < ResolvedDisk.Graph.Nodes.size(); ++Index)
	{
		const FMaterialGraphNode& A = ResolvedDisk.Graph.Nodes[Index];
		const FMaterialGraphNode& B = ResolvedReload.Graph.Nodes[Index];
		bSameStructure              = A.Id == B.Id && A.Type == B.Type && A.Inputs.size() == B.Inputs.size();
	}
	OutMessage = std::format("해시 {:016x} → {:016x}, 노드 {}개, JSON 재기록 {}, 구조 {}", DiskHash, ReloadHash, ResolvedReload.Graph.Nodes.size(),
	                         bIdempotent ? "같음" : "다름", bSameStructure ? "같음" : "다름");
	if (!DiskError.empty() || !ReloadError.empty())
	{
		OutMessage += " / 컴파일 오류: " + (DiskError.empty() ? ReloadError : DiskError);
	}
	return DiskHash == ReloadHash && bIdempotent && bSameStructure && DiskError.empty() && ReloadError.empty();
}

void FMaterialEditor::RefreshInherited(FAssetEditorEnvironment& Env)
{
	ParentError.clear();
	Inherited = FMaterialAsset{};
	if (!Asset.IsInstance())
	{
		return;
	}
	// 덮어쓰는 항목이 없는 사본을 해석 = 부모 체인 값
	FMaterialAsset ParentOnly = Asset;
	ParentOnly.OverrideMask   = 0;
	ParentOnly.Parameters.clear(); // 그래프 파라미터 덮어쓰기도 빼고
	Env.Resources->ResolveMaterialAsset(ParentOnly, Path, Inherited, nullptr, &ParentError);
}

void FMaterialEditor::SetPreviewShape(FAssetEditorEnvironment& Env, const char* PrimitiveName)
{
	PreviewShape               = PrimitiveName;
	FStaticMeshComponent& Mesh = Preview.GetScene().GetRegistry().Get<FStaticMeshComponent>(PreviewEntity);
	Mesh.Mesh                  = Env.Resources->GetOrCreatePrimitiveMesh(PreviewShape);
	Mesh.Material              = Material;
	Preview.GetScene().UpdateTransforms();
}

void FMaterialEditor::ScanFiles(const std::filesystem::path& ContentDirectory)
{
	TextureFiles = FAssetEditorWidgets::ScanImageFiles(ContentDirectory, Path.parent_path());

	// 부모 후보: Content 아래 .emat (자기 자신 제외)
	MaterialFiles.clear();
	std::error_code    ErrorCode;
	const std::wstring SelfKey = FMaterialAsset::MakePathKey(Path);
	for (auto It = std::filesystem::recursive_directory_iterator(ContentDirectory, ErrorCode); !ErrorCode && It != std::filesystem::recursive_directory_iterator();
	     It.increment(ErrorCode))
	{
		if (!It->is_regular_file(ErrorCode) || It->path().extension() != FMaterialAsset::Extension || FMaterialAsset::MakePathKey(It->path()) == SelfKey)
		{
			continue;
		}
		const std::filesystem::path Relative = std::filesystem::relative(It->path(), Path.parent_path(), ErrorCode);
		if (!ErrorCode)
		{
			MaterialFiles.push_back(FStringConv::ToUtf8(Relative.generic_wstring()));
		}
	}
	std::sort(MaterialFiles.begin(), MaterialFiles.end());
}

void FMaterialEditor::DrawPreviewToolbar(FAssetEditorEnvironment& Env)
{
	FAssetEditor::DrawPreviewToolbar(Env);
	ImGui::SameLine();
	ImGui::TextDisabled("|");
	for (const auto& [Name, Label] : { std::pair{ "sphere", "구" }, std::pair{ "cube", "큐브" }, std::pair{ "plane", "평면" } })
	{
		ImGui::SameLine();
		if (ImGui::RadioButton(Label, PreviewShape == Name))
		{
			SetPreviewShape(Env, Name);
			FramePreview(Env);
		}
	}
}

bool FMaterialEditor::DrawOverrideToggle(uint32 Field)
{
	if (!Asset.IsInstance())
	{
		return false;
	}
	ImGui::PushID(static_cast<int>(Field));
	bool       bOverride = (Asset.OverrideMask & Field) != 0;
	const bool bChanged  = ImGui::Checkbox("##Override", &bOverride);
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("덮어쓰기 (끄면 부모 값을 따른다)");
	}
	ImGui::PopID();
	ImGui::SameLine();
	if (bChanged)
	{
		Asset.OverrideMask = bOverride ? (Asset.OverrideMask | Field) : (Asset.OverrideMask & ~Field);
	}
	return bChanged;
}

bool FMaterialEditor::DrawTextureSlot(FAssetEditorEnvironment& Env, uint32 Slot, const char* Label)
{
	ImGui::PushID(static_cast<int>(Slot));
	bool         bChanged = DrawOverrideToggle(FMaterialAsset::GetTextureField(Slot));
	std::string& Current  = Asset.TexturePaths[Slot];

	// 작은 미리보기 (UI는 sRGB 텍스처를 선형으로 읽으므로 색상 텍스처가 약간 어둡게 보인다)
	const FMaterial* Live = Env.Resources->GetMaterial(Material);
	if (Live != nullptr && Live->Textures[Slot].IsValid())
	{
		const FD3D12Texture& Texture = Env.Resources->ResolveTexture(Live->Textures[Slot]);
		ImGui::Image(static_cast<ImTextureID>(Texture.GetSrv().Gpu.ptr), ImVec2(40.0f, 40.0f));
	}
	else
	{
		ImGui::Dummy(ImVec2(40.0f, 40.0f));
		const ImVec2 Min = ImGui::GetItemRectMin();
		const ImVec2 Max = ImGui::GetItemRectMax();
		ImGui::GetWindowDrawList()->AddRect(Min, Max, IM_COL32(90, 90, 100, 255));
	}
	ImGui::SameLine();
	ImGui::BeginGroup();
	ImGui::TextUnformatted(Label);
	ImGui::SetNextItemWidth(-FLT_MIN);
	if (FAssetEditorWidgets::TextureCombo("##Texture", Current, TextureFiles, "(없음)") || FAssetEditorWidgets::AcceptTextureDrop(Current, Path.parent_path()))
	{
		Asset.OverrideMask |= FMaterialAsset::GetTextureField(Slot);
		bChanged = true;
	}
	ImGui::EndGroup();
	ImGui::PopID();
	return bChanged;
}

bool FMaterialEditor::DrawParent(FAssetEditorEnvironment& Env)
{
	std::string Parent = Asset.Parent;
	if (!FAssetEditorWidgets::TextureCombo("부모", Parent, MaterialFiles, "(없음 — 일반 머티리얼)"))
	{
		return false;
	}
	if (Parent.empty())
	{
		// 인스턴스 → 일반: 지금 보이는(해석된) 값을 모두 자기 값으로
		FMaterialAsset Resolved;
		Env.Resources->ResolveMaterialAsset(Asset, Path, Resolved);
		const std::string Name = Asset.Name;
		Asset                  = Resolved;
		Asset.Name             = Name;
		LayoutMissingPositions(Asset.Graph);
		GraphPanel->OnAssetReloaded(true);
	}
	else
	{
		// 일반 → 인스턴스: 처음에는 덮어쓰는 항목 없음 (모두 부모 값). 인스턴스끼리 부모만 바꾸면 덮어쓰기는 유지
		if (!Asset.IsInstance())
		{
			Asset.OverrideMask = 0;
			Asset.Parameters.clear();
			Asset.bHasGraph = false;
			Asset.Graph     = FMaterialGraph{};
		}
		Asset.Parent = Parent;
		GraphPanel->OnAssetReloaded(true);
	}
	return true;
}

bool FMaterialEditor::DrawCommonSettings(FAssetEditorEnvironment& Env, bool& bOutTextures)
{
	// 인스턴스: 덮어쓰지 않은 항목은 부모 값을 보여 준다 (파일에는 쓰지 않음 — ToJsonString은 OverrideMask 항목만)
	if (Asset.IsInstance())
	{
		const uint32 Mask    = Asset.OverrideMask;
		const auto   Inherit = [&](uint32 Field, auto& Value, const auto& Source) {
            if ((Mask & Field) == 0)
            {
                Value = Source;
            }
		};
		Inherit(FMaterialAsset::Field_BaseColorFactor, Asset.Constants.BaseColorFactor, Inherited.Constants.BaseColorFactor);
		Inherit(FMaterialAsset::Field_EmissiveFactor, Asset.Constants.EmissiveFactor, Inherited.Constants.EmissiveFactor);
		Inherit(FMaterialAsset::Field_Metallic, Asset.Constants.Metallic, Inherited.Constants.Metallic);
		Inherit(FMaterialAsset::Field_Roughness, Asset.Constants.Roughness, Inherited.Constants.Roughness);
		Inherit(FMaterialAsset::Field_NormalScale, Asset.Constants.NormalScale, Inherited.Constants.NormalScale);
		Inherit(FMaterialAsset::Field_OcclusionStrength, Asset.Constants.OcclusionStrength, Inherited.Constants.OcclusionStrength);
		Inherit(FMaterialAsset::Field_AlphaCutoff, Asset.Constants.AlphaCutoff, Inherited.Constants.AlphaCutoff);
		Inherit(FMaterialAsset::Field_BlendMode, Asset.BlendMode, Inherited.BlendMode);
		Inherit(FMaterialAsset::Field_TwoSided, Asset.bTwoSided, Inherited.bTwoSided);
		for (uint32 Slot = 0; Slot < MaterialSlot_Count; ++Slot)
		{
			Inherit(FMaterialAsset::GetTextureField(Slot), Asset.TexturePaths[Slot], Inherited.TexturePaths[Slot]);
		}
	}

	bool       bChanged = false;
	const auto Edited   = [&](uint32 Field, bool bWidgetChanged) {
        if (bWidgetChanged)
        {
            Asset.OverrideMask |= Field;
            bChanged = true;
        }
	};

	ImGui::SeparatorText("기본");
	char NameBuffer[128];
	strncpy_s(NameBuffer, sizeof(NameBuffer), Asset.Name.c_str(), _TRUNCATE);
	if (ImGui::InputText("이름", NameBuffer, sizeof(NameBuffer)))
	{
		Asset.Name = NameBuffer;
		bChanged   = true;
	}
	if (DrawParent(Env))
	{
		bChanged     = true;
		bOutTextures = true;
	}
	if (!ParentError.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, FEditorTheme::Danger);
		ImGui::TextWrapped("%s", ParentError.c_str());
		ImGui::PopStyleColor();
	}
	else if (Asset.IsInstance())
	{
		FAssetEditorWidgets::Hint("인스턴스: 체크한 항목만 부모 값을 덮어쓴다. 값을 고치면 자동으로 체크된다.");
	}

	ImGui::SeparatorText("렌더 상태");
	bChanged |= DrawOverrideToggle(FMaterialAsset::Field_BlendMode);
	{
		static const char* const Labels[] = { "불투명 (Opaque)", "마스크 (Masked — 알파 테스트)", "반투명 (Translucent)", "가산 (Additive)" };
		int32                    Mode     = static_cast<int32>(Asset.BlendMode);
		if (ImGui::Combo("블렌드 모드", &Mode, Labels, IM_ARRAYSIZE(Labels)))
		{
			Asset.BlendMode = static_cast<EMaterialBlendMode>(Mode);
			Edited(FMaterialAsset::Field_BlendMode, true);
		}
	}
	if (Asset.BlendMode == EMaterialBlendMode::Masked)
	{
		bChanged |= DrawOverrideToggle(FMaterialAsset::Field_AlphaCutoff);
		Edited(FMaterialAsset::Field_AlphaCutoff, ImGui::SliderFloat("알파 컷오프", &Asset.Constants.AlphaCutoff, 0.0f, 1.0f));
	}
	bChanged |= DrawOverrideToggle(FMaterialAsset::Field_TwoSided);
	Edited(FMaterialAsset::Field_TwoSided, ImGui::Checkbox("양면 (컬링 없음)", &Asset.bTwoSided));
	return bChanged;
}

void FMaterialEditor::DrawProperties(FAssetEditorEnvironment& Env)
{
	DrawEnv = &Env;
	if (bGraphMaterial)
	{
		DrawGraphProperties(Env);
	}
	else
	{
		DrawFixedProperties(Env);
	}
	DrawEnv = nullptr;
}

void FMaterialEditor::DrawFixedProperties(FAssetEditorEnvironment& Env)
{
	bool bTextures = false;
	bool bChanged  = DrawCommonSettings(Env, bTextures);

	FMaterialConstants& Constants = Asset.Constants;
	const auto          Edited    = [&](uint32 Field, bool bWidgetChanged) {
        if (bWidgetChanged)
        {
            Asset.OverrideMask |= Field;
            bChanged = true;
        }
	};
	ImGui::SeparatorText("표면");
	bChanged |= DrawOverrideToggle(FMaterialAsset::Field_BaseColorFactor);
	Edited(FMaterialAsset::Field_BaseColorFactor, ImGui::ColorEdit4("베이스 컬러", &Constants.BaseColorFactor.X));
	bChanged |= DrawOverrideToggle(FMaterialAsset::Field_Metallic);
	Edited(FMaterialAsset::Field_Metallic, ImGui::SliderFloat("금속성", &Constants.Metallic, 0.0f, 1.0f));
	bChanged |= DrawOverrideToggle(FMaterialAsset::Field_Roughness);
	Edited(FMaterialAsset::Field_Roughness, ImGui::SliderFloat("거칠기", &Constants.Roughness, 0.0f, 1.0f));
	bChanged |= DrawOverrideToggle(FMaterialAsset::Field_EmissiveFactor);
	Edited(FMaterialAsset::Field_EmissiveFactor, ImGui::ColorEdit3("발광", &Constants.EmissiveFactor.X, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float));
	bChanged |= DrawOverrideToggle(FMaterialAsset::Field_NormalScale);
	Edited(FMaterialAsset::Field_NormalScale, ImGui::DragFloat("노멀 강도", &Constants.NormalScale, 0.01f, 0.0f, 4.0f));
	bChanged |= DrawOverrideToggle(FMaterialAsset::Field_OcclusionStrength);
	Edited(FMaterialAsset::Field_OcclusionStrength, ImGui::SliderFloat("AO 강도", &Constants.OcclusionStrength, 0.0f, 1.0f));

	ImGui::SeparatorText("텍스처");
	bTextures |= DrawTextureSlot(Env, MaterialSlot_BaseColor, "베이스 컬러 (sRGB, A = 알파)");
	bTextures |= DrawTextureSlot(Env, MaterialSlot_MetallicRoughness, "금속/거칠기 (G=거칠기, B=금속)");
	bTextures |= DrawTextureSlot(Env, MaterialSlot_Normal, "노멀");
	bTextures |= DrawTextureSlot(Env, MaterialSlot_Occlusion, "AO (R)");
	bTextures |= DrawTextureSlot(Env, MaterialSlot_Emissive, "발광 (sRGB)");
	if (ImGui::SmallButton("파일 목록 새로 고침") && Env.Editor != nullptr)
	{
		ScanFiles(Env.Editor->ContentDirectory);
	}

	if (!Asset.IsInstance())
	{
		ImGui::SeparatorText("노드 그래프");
		if (ImGui::Button(ICON_FA_DIAGRAM_PROJECT " 그래프로 변환"))
		{
			// 같은 출력의 노드 그래프 (팩터 = 파라미터, 텍스처 = TextureSample)
			MaterialGraphEditing::ConvertToGraph(Asset, false);
			bChanged  = true;
			bTextures = true;
			GraphPanel->OnAssetReloaded(true);
		}
		ImGui::SetItemTooltip("현재 값/텍스처를 같은 결과의 노드로 바꿉니다 (실행 취소 가능)");
	}

	if (bChanged || bTextures)
	{
		ApplyToMaterial(Env);
		MarkEdited(bTextures ? "텍스처 변경" : "머티리얼 값 변경");
	}

	ImGui::Spacing();
	FAssetEditorWidgets::Hint("열린 씬에 바로 반영됩니다(이 머티리얼을 부모로 둔 인스턴스 포함). 저장하지 않고 닫으면 원래대로 돌아갑니다.");
}

void FMaterialEditor::DrawPreviewArea(FAssetEditorEnvironment& Env)
{
	if (!bGraphMaterial)
	{
		FAssetEditor::DrawPreviewArea(Env);
		return;
	}
	DrawEnv              = &Env;
	const bool bReadOnly = Asset.IsInstance();

	// 도구 줄: 컴파일 상태 + 정렬/전체 보기
	if (GraphCompile.bSuccess)
	{
		ImGui::TextColored(FEditorTheme::Success, ICON_FA_CIRCLE_CHECK " 컴파일 성공");
		ImGui::SetItemTooltip("셰이더 %016llx · 노드 %zu개 · 상수 레지스터 %u · 텍스처 %u", static_cast<unsigned long long>(GraphCompile.Shader->Hash),
		                      ResolvedGraph.Graph.Nodes.size(), GraphCompile.Shader->Layout.ConstantRegisters, GraphCompile.Shader->Layout.TextureCount);
	}
	else
	{
		ImGui::TextColored(FEditorTheme::Danger, ICON_FA_TRIANGLE_EXCLAMATION " 컴파일 오류 %zu개 (렌더러는 이전 셰이더 유지)", GraphCompile.Errors.size());
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%s", GraphCompile.JoinErrors().c_str());
		}
	}
	ImGui::SameLine();
	if (ImGui::SmallButton(ICON_FA_EXPAND " 전체 보기"))
	{
		GraphPanel->RequestFrame();
	}
	if (!bReadOnly)
	{
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_TABLE_COLUMNS " 자동 정렬"))
		{
			MaterialGraphEditing::AutoLayout(Asset.Graph, false);
			GraphPanel->OnAssetReloaded(true);
			MarkEdited("자동 정렬");
		}
	}
	ImGui::SameLine();
	if (bReadOnly)
	{
		ImGui::TextColored(FEditorTheme::Warning, ICON_FA_LOCK " 인스턴스 — 부모 그래프 (읽기 전용, 파라미터만 덮어쓰기)");
	}
	else
	{
		ImGui::TextDisabled("우클릭/Space: 노드 · 핀 끌기: 연결 · Del: 삭제 · Ctrl+C/V/D");
	}
	// 그래프 안이 아닌 곳에 포커스가 있을 때도 F로 전체 보기
	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false) &&
	    !ImGui::GetIO().KeyCtrl && !Preview.IsHovered())
	{
		GraphPanel->RequestFrame();
	}

	FMaterialGraphPanel::FContext Context;
	Context.Graph          = bReadOnly ? &ResolvedGraph.Graph : &Asset.Graph;
	Context.Parameters     = bReadOnly ? &ResolvedGraph.Parameters : &Asset.Parameters;
	Context.bReadOnly      = bReadOnly;
	Context.Compile        = &GraphCompile;
	Context.Analysis       = &GraphAnalysis;
	Context.TextureFiles   = &TextureFiles;
	Context.AssetDirectory = Path.parent_path();
	Context.OnEdited       = [this](std::string_view Label, bool bSemantic) { OnGraphEdited(Label, bSemantic); };
	GraphPanel->Draw(Context);
	DrawEnv = nullptr;
}

void FMaterialEditor::DrawGraphProperties(FAssetEditorEnvironment& Env)
{
	// 미리보기 (위) + 탭 (아래)
	DrawPreviewToolbar(Env);
	const float Width = ImGui::GetContentRegionAvail().x;
	Preview.DrawViewport(FVector2(Width, FMath::Clamp(Width * 0.75f, 120.0f, ImGui::GetContentRegionAvail().y * 0.45f)));
	if (Preview.IsHovered() && ImGui::IsKeyPressed(ImGuiKey_F) && !ImGui::GetIO().KeyCtrl)
	{
		FramePreview(Env);
	}

	static const std::wstring TabArg   = FCommandLine::FromProcess().GetValue(L"--matgraph-tab");
	const bool                bFirst   = UpdateFrames < 3;
	const auto                TabFlags = [&](const wchar_t* Name) { return bFirst && TabArg == Name ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None; };
	if (!ImGui::BeginTabBar("##MaterialGraphTabs"))
	{
		return;
	}
	FMaterialGraphPanel::FContext Context;
	const bool                    bReadOnly = Asset.IsInstance();
	Context.Graph                           = bReadOnly ? &ResolvedGraph.Graph : &Asset.Graph;
	Context.Parameters                      = bReadOnly ? &ResolvedGraph.Parameters : &Asset.Parameters;
	Context.bReadOnly                       = bReadOnly;
	Context.Compile                         = &GraphCompile;
	Context.Analysis                        = &GraphAnalysis;
	Context.TextureFiles                    = &TextureFiles;
	Context.AssetDirectory                  = Path.parent_path();
	Context.OnEdited                        = [this](std::string_view Label, bool bSemantic) { OnGraphEdited(Label, bSemantic); };

	if (ImGui::BeginTabItem(ICON_FA_SLIDERS " 상세", nullptr, TabFlags(L"details")))
	{
		if (ImGui::BeginChild("##Details"))
		{
			if (!GraphPanel->DrawSelectionDetails(Context))
			{
				// 선택 없음: 머티리얼 설정
				bool bTextures = false;
				if (DrawCommonSettings(Env, bTextures))
				{
					ApplyToMaterial(Env);
					MarkEdited("머티리얼 설정");
					if (!Asset.IsInstance() && !Asset.IsGraphMaterial())
					{
						GraphPanel->OnAssetReloaded(true);
					}
				}
				ImGui::Spacing();
				FAssetEditorWidgets::Hint("그래프에서 노드를 고르면 그 노드의 설정과 입력이 여기에 보입니다. 표면은 그래프가 만들고, 블렌드 모드/양면/컷오프는 여기서 정합니다.");
			}
		}
		ImGui::EndChild();
		ImGui::EndTabItem();
	}
	if (ImGui::BeginTabItem(ICON_FA_LIST " 파라미터", nullptr, TabFlags(L"params")))
	{
		if (ImGui::BeginChild("##Parameters"))
		{
			if (bReadOnly)
			{
				bool bTextures = false;
				if (DrawParameterOverrides(bTextures))
				{
					ApplyToMaterial(Env);
					MarkEdited(bTextures ? "텍스처 변경" : "파라미터 덮어쓰기");
				}
			}
			else
			{
				DrawParameterList(Env);
			}
		}
		ImGui::EndChild();
		ImGui::EndTabItem();
	}
	if (ImGui::BeginTabItem(ICON_FA_CODE " HLSL", nullptr, TabFlags(L"hlsl")))
	{
		DrawHlslTab();
		ImGui::EndTabItem();
	}
	ImGui::EndTabBar();
}

bool FMaterialEditor::DrawParameterOverrides(bool& bOutTextures)
{
	bool bChanged = false;
	FAssetEditorWidgets::Hint("체크한 파라미터만 부모 값을 덮어씁니다. 끄면 부모 값(흐리게)을 따릅니다.");
	for (const FMaterialParameter& Shown : ResolvedGraph.Parameters)
	{
		ImGui::PushID(Shown.Name.c_str());
		FMaterialParameter* Target    = Asset.FindParameter(Shown.Name);
		bool                bOverride = Target != nullptr;
		if (ImGui::Checkbox("##Override", &bOverride))
		{
			if (bOverride)
			{
				Asset.Parameters.push_back(Shown);
			}
			else
			{
				std::erase_if(Asset.Parameters, [&](const FMaterialParameter& Parameter) { return Parameter.Name == Shown.Name; });
			}
			bChanged = true;
			bOutTextures |= Shown.Type == EMaterialParameterType::Texture;
			ImGui::PopID();
			continue;
		}
		ImGui::SetItemTooltip("덮어쓰기 (끄면 부모 값을 따른다)");
		ImGui::SameLine();
		// 부모 값 (덮어쓰지 않으면 이 값이 쓰인다)
		const FMaterialParameter* ParentValue = nullptr;
		for (const FMaterialParameter& Parameter : Inherited.Parameters)
		{
			ParentValue = Parameter.Name == Shown.Name ? &Parameter : ParentValue;
		}
		FMaterialParameter  ShownCopy = ParentValue != nullptr ? *ParentValue : Shown;
		FMaterialParameter& Edit      = Target != nullptr ? *Target : ShownCopy;
		Edit.Type                     = Shown.Type; // 인스턴스 값의 타입은 부모 것
		Edit.Usage                    = Shown.Usage;
		ImGui::BeginDisabled(Target == nullptr);
		bool bTexture = false;
		ImGui::BeginGroup();
		if (Shown.Type == EMaterialParameterType::Texture)
		{
			ImGui::TextUnformatted(Shown.Name.c_str());
		}
		const char* Label = Shown.Type == EMaterialParameterType::Texture ? "##Texture" : Shown.Name.c_str();
		if (FMaterialGraphPanel::DrawParameterValue(Edit, Label, &TextureFiles, Path.parent_path(), bTexture, false))
		{
			bChanged = true;
			bOutTextures |= bTexture;
		}
		ImGui::EndGroup();
		ImGui::EndDisabled();
		ImGui::PopID();
	}
	if (ResolvedGraph.Parameters.empty())
	{
		ImGui::TextDisabled("(파라미터 없음)");
	}
	return bChanged;
}

void FMaterialEditor::DrawParameterList(FAssetEditorEnvironment& Env)
{
	(void)Env;
	int32 RemoveIndex = -1;
	for (size_t Index = 0; Index < Asset.Parameters.size(); ++Index)
	{
		FMaterialParameter& Parameter = Asset.Parameters[Index];
		ImGui::PushID(static_cast<int>(Index));
		const uint32 Uses = MaterialGraphEditing::CountParameterUses(Asset.Graph, Parameter.Name);

		// 이름 (입력이 끝날 때 적용 — 그래프 노드 참조도 함께 바뀐다)
		char Buffer[128];
		strncpy_s(Buffer, (ParameterNameEditIndex == static_cast<int32>(Index) ? ParameterNameEdit : Parameter.Name).c_str(), _TRUNCATE);
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
		if (ImGui::InputText("##Name", Buffer, sizeof(Buffer)))
		{
			ParameterNameEdit      = Buffer;
			ParameterNameEditIndex = static_cast<int32>(Index);
		}
		if (ImGui::IsItemDeactivatedAfterEdit() && ParameterNameEditIndex == static_cast<int32>(Index))
		{
			if (MaterialGraphEditing::RenameParameter(Asset.Graph, Asset.Parameters, Parameter.Name, ParameterNameEdit))
			{
				OnGraphEdited("파라미터 이름", true);
			}
			ParameterNameEditIndex = -1;
		}
		ImGui::SameLine();
		ImGui::TextDisabled("%s · %s", GetMaterialParameterTypeName(Parameter.Type), Uses == 0 ? "안 쓰임" : std::format("노드 {}", Uses).c_str());
		ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 24.0f);
		if (ImGui::SmallButton(ICON_FA_TRASH))
		{
			RemoveIndex = static_cast<int32>(Index);
		}
		ImGui::SetItemTooltip(Uses > 0 ? "삭제 (이 파라미터를 쓰는 노드는 컴파일 오류가 됩니다)" : "삭제");

		bool bTexture = false;
		if (FMaterialGraphPanel::DrawParameterValue(Parameter, Parameter.Type == EMaterialParameterType::Texture ? "##Texture" : "##Value", &TextureFiles,
		                                            Path.parent_path(), bTexture))
		{
			OnGraphEdited(bTexture ? "텍스처 변경" : "파라미터 값", true);
		}
		ImGui::Separator();
		ImGui::PopID();
	}
	if (RemoveIndex >= 0)
	{
		Asset.Parameters.erase(Asset.Parameters.begin() + RemoveIndex);
		OnGraphEdited("파라미터 삭제", true);
	}
	if (ImGui::Button(ICON_FA_PLUS " 파라미터 추가"))
	{
		ImGui::OpenPopup("##AddParameter");
	}
	if (ImGui::BeginPopup("##AddParameter"))
	{
		for (uint32 Type = 0; Type < static_cast<uint32>(EMaterialParameterType::Count); ++Type)
		{
			const EMaterialParameterType ParameterType = static_cast<EMaterialParameterType>(Type);
			if (ImGui::MenuItem(GetMaterialParameterTypeName(ParameterType)))
			{
				FMaterialParameter Parameter;
				Parameter.Type  = ParameterType;
				Parameter.Name  = MaterialGraphEditing::MakeUniqueParameterName(Asset.Parameters, GetMaterialParameterTypeName(ParameterType));
				Parameter.Value = ParameterType == EMaterialParameterType::Vector ? FVector4::OneVector : FVector4(1.0f, 0.0f, 0.0f, 0.0f);
				Asset.Parameters.push_back(std::move(Parameter));
				OnGraphEdited("파라미터 추가", true);
			}
		}
		ImGui::EndPopup();
	}
	ImGui::Spacing();
	FAssetEditorWidgets::Hint("파라미터는 머티리얼 인스턴스가 덮어쓸 수 있는 값입니다. 정적 스위치는 값마다 다른 셰이더가 됩니다. 텍스처는 콘텐츠 브라우저에서 끌어 놓을 수 있습니다.");
}

void FMaterialEditor::DrawHlslTab()
{
	if (!GraphCompile.bSuccess)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, FEditorTheme::Danger);
		ImGui::TextWrapped("컴파일 오류:\n%s", GraphCompile.JoinErrors().c_str());
		ImGui::PopStyleColor();
		return;
	}
	std::string Hlsl = GraphCompile.Shader->Hlsl; // 읽기 전용 위젯용 사본
	ImGui::Text("셰이더 %016llx", static_cast<unsigned long long>(GraphCompile.Shader->Hash));
	ImGui::SameLine();
	if (ImGui::SmallButton(ICON_FA_COPY " 복사"))
	{
		ImGui::SetClipboardText(Hlsl.c_str());
	}
	ImGui::InputTextMultiline("##Hlsl", Hlsl.data(), Hlsl.size() + 1, ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_ReadOnly);
}

void FMaterialEditor::CollectResourceRoots(FResourceRoots& Roots)
{
	FAssetEditor::CollectResourceRoots(Roots);
	Roots.Add(Material); // 편집 중인 공유 머티리얼 (미리보기 모양을 바꾸는 중에도)
}
