#include "Editor/EditorTheme.h"

#include "Core/Log.h"
#include "Core/StringConv.h"

#include <filesystem>
#include <format>

E_DECLARE_LOG_CATEGORY(LogEditor)

extern const unsigned char GFontAwesomeSolidData[];
extern const unsigned int  GFontAwesomeSolidSize;

namespace
{
	ImFont* GBoldFont = nullptr;

	constexpr ImVec4 Gray(float Value, float Alpha = 1.0f) { return ImVec4(Value, Value, Value, Alpha); }

	// 기본 글꼴 + 아이콘 글꼴 합치기
	ImFont* AddFontWithIcons(const std::filesystem::path& TextFont, float Size, float Scale)
	{
		ImGuiIO& IO   = ImGui::GetIO();
		ImFont*  Font = nullptr;
		if (std::filesystem::exists(TextFont))
		{
			ImFontConfig Config;
			Config.OversampleH = 2;
			Font = IO.Fonts->AddFontFromFileTTF(FStringConv::ToUtf8(TextFont.wstring()).c_str(), Size, &Config, IO.Fonts->GetGlyphRangesKorean());
		}
		if (Font == nullptr)
		{
			ImFontConfig Config;
			Config.SizePixels = Size;
			Font              = IO.Fonts->AddFontDefault(&Config);
		}

		// 아이콘은 글자보다 약간 작게, 폭을 고정해 목록에서 줄이 맞도록
		static const ImWchar IconRanges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };
		ImFontConfig         IconConfig;
		IconConfig.MergeMode            = true;
		IconConfig.PixelSnapH           = true;
		IconConfig.FontDataOwnedByAtlas = false; // 정적 배열
		IconConfig.GlyphMinAdvanceX     = Size * 1.15f;
		IconConfig.GlyphOffset          = ImVec2(0.0f, 1.0f * Scale);
		IO.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(GFontAwesomeSolidData), static_cast<int>(GFontAwesomeSolidSize), Size * 0.85f,
		                               &IconConfig, IconRanges);
		return Font;
	}
} // namespace

void FEditorTheme::Apply(float Scale)
{
	ImGuiStyle& Style = ImGui::GetStyle();
	Style             = ImGuiStyle{};

	// 크기: 촘촘하고 각진 배치 (언리얼 5 풍)
	Style.WindowPadding            = ImVec2(8.0f, 8.0f);
	Style.FramePadding             = ImVec2(7.0f, 4.0f);
	Style.CellPadding              = ImVec2(6.0f, 3.0f);
	Style.ItemSpacing              = ImVec2(6.0f, 5.0f);
	Style.ItemInnerSpacing         = ImVec2(5.0f, 4.0f);
	Style.IndentSpacing            = 14.0f;
	Style.ScrollbarSize            = 11.0f;
	Style.GrabMinSize              = 9.0f;
	Style.WindowRounding           = 0.0f;
	Style.ChildRounding            = 0.0f;
	Style.FrameRounding            = 2.0f;
	Style.PopupRounding            = 3.0f;
	Style.ScrollbarRounding        = 2.0f;
	Style.GrabRounding             = 2.0f;
	Style.TabRounding              = 2.0f;
	Style.WindowBorderSize         = 1.0f;
	Style.ChildBorderSize          = 1.0f;
	Style.PopupBorderSize          = 1.0f;
	Style.FrameBorderSize          = 1.0f;
	Style.TabBorderSize            = 0.0f;
	Style.TabBarBorderSize         = 1.0f;
	Style.TabBarOverlineSize       = 2.0f;
	Style.SeparatorTextBorderSize  = 1.0f;
	Style.SeparatorTextPadding     = ImVec2(14.0f, 4.0f);
	Style.WindowTitleAlign         = ImVec2(0.0f, 0.5f);
	Style.WindowMenuButtonPosition = ImGuiDir_None;
	Style.DockingSeparatorSize     = 3.0f;

	ImVec4* Colors                           = Style.Colors;
	Colors[ImGuiCol_Text]                    = Gray(0.86f);
	Colors[ImGuiCol_TextDisabled]            = TextDim;
	Colors[ImGuiCol_WindowBg]                = Gray(0.137f); // #232323 패널
	Colors[ImGuiCol_ChildBg]                 = Gray(0.137f, 0.0f);
	Colors[ImGuiCol_PopupBg]                 = Gray(0.10f, 0.98f);
	Colors[ImGuiCol_Border]                  = Gray(0.055f);
	Colors[ImGuiCol_BorderShadow]            = Gray(0.0f, 0.0f);
	Colors[ImGuiCol_FrameBg]                 = Gray(0.059f); // 입력칸은 패널보다 어둡게
	Colors[ImGuiCol_FrameBgHovered]          = Gray(0.09f);
	Colors[ImGuiCol_FrameBgActive]           = Gray(0.11f);
	Colors[ImGuiCol_TitleBg]                 = Gray(0.078f);
	Colors[ImGuiCol_TitleBgActive]           = Gray(0.078f);
	Colors[ImGuiCol_TitleBgCollapsed]        = Gray(0.078f);
	Colors[ImGuiCol_MenuBarBg]               = Gray(0.082f);
	Colors[ImGuiCol_ScrollbarBg]             = Gray(0.10f, 0.0f);
	Colors[ImGuiCol_ScrollbarGrab]           = Gray(0.26f);
	Colors[ImGuiCol_ScrollbarGrabHovered]    = Gray(0.33f);
	Colors[ImGuiCol_ScrollbarGrabActive]     = Gray(0.40f);
	Colors[ImGuiCol_CheckMark]               = AccentHover;
	Colors[ImGuiCol_SliderGrab]              = Accent;
	Colors[ImGuiCol_SliderGrabActive]        = AccentHover;
	Colors[ImGuiCol_Button]                  = Gray(0.22f);
	Colors[ImGuiCol_ButtonHovered]           = Gray(0.29f);
	Colors[ImGuiCol_ButtonActive]            = Accent;
	Colors[ImGuiCol_Header]                  = ImVec4(0.00f, 0.36f, 0.72f, 0.65f); // 선택 행
	Colors[ImGuiCol_HeaderHovered]           = Gray(0.25f);
	Colors[ImGuiCol_HeaderActive]            = Accent;
	Colors[ImGuiCol_Separator]               = Gray(0.055f);
	Colors[ImGuiCol_SeparatorHovered]        = Accent;
	Colors[ImGuiCol_SeparatorActive]         = AccentHover;
	Colors[ImGuiCol_ResizeGrip]              = Gray(0.0f, 0.0f);
	Colors[ImGuiCol_ResizeGripHovered]       = Accent;
	Colors[ImGuiCol_ResizeGripActive]        = AccentHover;
	Colors[ImGuiCol_InputTextCursor]         = Gray(0.95f);
	Colors[ImGuiCol_Tab]                     = Gray(0.078f);
	Colors[ImGuiCol_TabHovered]              = Gray(0.20f);
	Colors[ImGuiCol_TabSelected]             = Gray(0.137f);
	Colors[ImGuiCol_TabSelectedOverline]     = Accent;
	Colors[ImGuiCol_TabDimmed]               = Gray(0.078f);
	Colors[ImGuiCol_TabDimmedSelected]       = Gray(0.137f);
	Colors[ImGuiCol_TabDimmedSelectedOverline] = Gray(0.30f);
	Colors[ImGuiCol_DockingPreview]          = ImVec4(Accent.x, Accent.y, Accent.z, 0.55f);
	Colors[ImGuiCol_DockingEmptyBg]          = Gray(0.06f);
	Colors[ImGuiCol_PlotLines]               = Gray(0.6f);
	Colors[ImGuiCol_PlotHistogram]           = Accent;
	Colors[ImGuiCol_TableHeaderBg]           = Gray(0.18f);
	Colors[ImGuiCol_TableBorderStrong]       = Gray(0.055f);
	Colors[ImGuiCol_TableBorderLight]        = Gray(0.09f);
	Colors[ImGuiCol_TableRowBg]              = Gray(0.0f, 0.0f);
	Colors[ImGuiCol_TableRowBgAlt]           = Gray(1.0f, 0.025f);
	Colors[ImGuiCol_TextSelectedBg]          = ImVec4(Accent.x, Accent.y, Accent.z, 0.45f);
	Colors[ImGuiCol_DragDropTarget]          = Warning;
	Colors[ImGuiCol_NavCursor]               = Accent;
	Colors[ImGuiCol_ModalWindowDimBg]        = Gray(0.0f, 0.55f);

	Style.ScaleAllSizes(Scale);
}

void FEditorTheme::LoadFonts(float Scale)
{
	const float Size = 15.0f * Scale;
	AddFontWithIcons(L"C:\\Windows\\Fonts\\malgun.ttf", Size, Scale);
	GBoldFont = AddFontWithIcons(L"C:\\Windows\\Fonts\\malgunbd.ttf", Size, Scale);
	if (!std::filesystem::exists(L"C:\\Windows\\Fonts\\malgun.ttf"))
	{
		E_LOG(LogEditor, Warning, "한글 폰트(malgun.ttf)를 찾지 못해 기본 폰트를 사용합니다");
	}
}

ImFont* FEditorTheme::GetBoldFont()
{
	return GBoldFont != nullptr ? GBoldFont : ImGui::GetFont();
}

std::string FEditorTheme::PanelTitle(const char* Icon, std::string_view Label, std::string_view Id)
{
	return std::format("{}  {}###{}", Icon, Label, Id);
}

FEditorTheme::FAssetStyle FEditorTheme::GetAssetStyle(std::string_view Extension, bool bDirectory)
{
	if (bDirectory)
	{
		return { ICON_FA_FOLDER, IM_COL32(222, 178, 82, 255), "폴더" };
	}
	if (Extension == ".glb" || Extension == ".gltf" || Extension == ".fbx")
	{
		return { ICON_FA_CUBE, IM_COL32(64, 170, 255, 255), "모델" };
	}
	if (Extension == ".emat")
	{
		return { ICON_FA_PALETTE, IM_COL32(96, 200, 110, 255), "머티리얼" };
	}
	if (Extension == ".eparticle")
	{
		return { ICON_FA_FIRE, IM_COL32(255, 128, 64, 255), "파티클" };
	}
	if (Extension == ".eprefab")
	{
		return { ICON_FA_BOXES_STACKED, IM_COL32(115, 184, 255, 255), "프리팹" };
	}
	if (Extension == ".escene")
	{
		return { ICON_FA_MOUNTAIN_SUN, IM_COL32(230, 110, 90, 255), "씬" };
	}
	if (Extension == ".png" || Extension == ".jpg" || Extension == ".jpeg" || Extension == ".tga" || Extension == ".bmp")
	{
		return { ICON_FA_IMAGE, IM_COL32(190, 120, 230, 255), "텍스처" };
	}
	if (Extension == ".lua")
	{
		return { ICON_FA_FILE_CODE, IM_COL32(80, 200, 200, 255), "스크립트" };
	}
	if (Extension == ".wav" || Extension == ".ogg" || Extension == ".mp3" || Extension == ".flac")
	{
		return { ICON_FA_MUSIC, IM_COL32(235, 110, 170, 255), "오디오" };
	}
	return {};
}

const char* FEditorTheme::GetComponentIcon(std::string_view TypeName)
{
	struct FEntry
	{
		std::string_view Name;
		const char*      Icon;
	};
	static constexpr FEntry Entries[] = {
		{ "TransformComponent", ICON_FA_UP_DOWN_LEFT_RIGHT },
		{ "StaticMeshComponent", ICON_FA_CUBE },
		{ "ModelComponent", ICON_FA_CUBES },
		{ "DirectionalLightComponent", ICON_FA_SUN },
		{ "AnimationComponent", ICON_FA_PERSON_RUNNING },
		{ "CameraComponent", ICON_FA_VIDEO },
		{ "ScriptComponent", ICON_FA_FILE_CODE },
		{ "ParticleSystemComponent", ICON_FA_FIRE },
		{ "SocketAttachmentComponent", ICON_FA_LINK },
		{ "RigidBodyComponent", ICON_FA_WEIGHT_HANGING },
		{ "BoxColliderComponent", ICON_FA_BOX },
		{ "SphereColliderComponent", ICON_FA_CIRCLE },
		{ "CapsuleColliderComponent", ICON_FA_CAPSULES },
		{ "AudioSourceComponent", ICON_FA_VOLUME_HIGH },
	};
	for (const FEntry& Entry : Entries)
	{
		if (Entry.Name == TypeName)
		{
			return Entry.Icon;
		}
	}
	return ICON_FA_PUZZLE_PIECE;
}

bool FEditorTheme::ToolButton(const char* Icon, const char* Tooltip, bool bActive)
{
	if (bActive)
	{
		ImGui::PushStyleColor(ImGuiCol_Button, Accent);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentHover);
	}
	const bool bClicked = ImGui::Button(Icon);
	if (bActive)
	{
		ImGui::PopStyleColor(2);
	}
	if (Tooltip != nullptr)
	{
		ImGui::SetItemTooltip("%s", Tooltip);
	}
	return bClicked;
}
