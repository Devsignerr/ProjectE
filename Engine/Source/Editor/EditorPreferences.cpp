#include "Editor/EditorPreferences.h"

#include "Core/Settings/SettingsRegistry.h"

namespace
{
	constexpr const char* GEditorCategory = "에디터";
} // namespace

FEditorPreferences& FEditorPreferences::Get()
{
	static FEditorPreferences Preferences;
	return Preferences;
}

void FEditorPreferences::Initialize()
{
	if (bInitialized)
	{
		return;
	}
	bInitialized = true;

	FSettingsRegistry& Registry = FSettingsRegistry::Get();
	Registry.Register(General, { "EditorGeneral", "일반", GEditorCategory, "시작 씬과 자동 저장 (모든 프로젝트 공통)", ESettingsScope::EditorUser })
		.Property(&FEditorGeneralSettings::bLoadLastSceneOnStartup, "LoadLastSceneOnStartup", "시작 시 마지막 씬 열기")
		.Tooltip("에디터를 열 때 이 프로젝트에서 마지막으로 연 씬을 연다. 끄면 프로젝트 설정의 에디터 시작 맵")
		.Property(&FEditorGeneralSettings::bAutoSave, "AutoSave", "자동 저장")
		.Tooltip("저장 안 한 변경이 있으면 주기적으로 <Saved>/Autosaves/에 사본을 남긴다 (원본 씬 파일은 건드리지 않는다)")
		.Property(&FEditorGeneralSettings::AutoSaveIntervalMinutes, "AutoSaveIntervalMinutes", "자동 저장 간격 (분)").Range(1.0f, 120.0f, 1.0f)
		.Property(&FEditorGeneralSettings::AutoSaveKeepCount, "AutoSaveKeepCount", "자동 저장 보관 수").Range(1.0f, 50.0f).Tooltip("씬마다 최근 사본을 이만큼 남긴다");

	Registry.Register(Viewport, { "EditorViewport", "뷰포트", GEditorCategory, "편집 카메라와 기즈모 스냅 (모든 프로젝트 공통)", ESettingsScope::EditorUser })
		.Property(&FEditorViewportSettings::DefaultCameraSpeed, "DefaultCameraSpeed", "기본 카메라 속도 (cm/초)").Range(10.0f, 20000.0f, 10.0f)
		.Tooltip("저장된 편집 카메라가 없을 때. 휠로 바꾼 속도는 프로젝트별로 기억한다")
		.Property(&FEditorViewportSettings::MouseSensitivity, "MouseSensitivity", "마우스 감도 (도/픽셀)").Range(0.01f, 1.0f, 0.01f)
		.Property(&FEditorViewportSettings::FieldOfView, "FieldOfView", "시야각 (도)").Range(20.0f, 120.0f, 1.0f)
		.Property(&FEditorViewportSettings::bSnapEnabled, "SnapEnabled", "기즈모 스냅 켜기")
		.Property(&FEditorViewportSettings::TranslateSnap, "TranslateSnap", "이동 스냅 (cm)").Range(0.1f, 10000.0f, 1.0f)
		.Property(&FEditorViewportSettings::RotateSnap, "RotateSnap", "회전 스냅 (도)").Range(0.1f, 180.0f, 1.0f)
		.Property(&FEditorViewportSettings::ScaleSnap, "ScaleSnap", "스케일 스냅").Range(0.001f, 10.0f, 0.01f);

	Registry.Register(ProjectState, { "EditorPerProjectUserSettings", "프로젝트 상태", GEditorCategory, "", ESettingsScope::ProjectUser, false, true })
		.Property(&FEditorProjectState::LastOpenedScene, "LastOpenedScene", "마지막 씬");

	Registry.LoadAll(ESettingsScope::EditorUser);
	Registry.LoadAll(ESettingsScope::ProjectUser);
}

bool FEditorPreferences::SaveProjectState() const
{
	const FSettingsSection* Section = FSettingsRegistry::Get().Find("EditorPerProjectUserSettings");
	return Section != nullptr && Section->Save();
}

bool FEditorPreferences::SaveViewport() const
{
	const FSettingsSection* Section = FSettingsRegistry::Get().Find("EditorViewport");
	return Section != nullptr && Section->Save();
}
