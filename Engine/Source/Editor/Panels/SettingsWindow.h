#pragma once

#include <set>
#include <string>

struct FEditorContext;
struct FSettingsSection;

// 언리얼식 설정 창 (프로젝트 설정 / 에디터 환경설정): 왼쪽 카테고리별 섹션 목록, 오른쪽 섹션 속성.
// 섹션은 FSettingsRegistry에서 범위로 걸러 나열하고, 속성은 리플렉션으로 그린다 (FPropertyWidgets — 인스펙터와 같은 위젯).
// 값이 바뀌면 섹션의 OnChanged를 바로 부르고, 조작이 끝나면(활성 위젯 없음) 그 섹션 파일을 저장한다 (언리얼과 같음).
class FSettingsWindow
{
public:
	enum class EKind
	{
		ProjectSettings,    // ESettingsScope::Project
		EditorPreferences,  // ESettingsScope::EditorUser
	};

	explicit FSettingsWindow(EKind InKind) : Kind(InKind) {}

	void Open(const std::string& SectionId = std::string()); // 비우면 이전/첫 섹션
	void Draw(FEditorContext& Context);

	bool bOpen = false;

private:
	bool IsListed(const FSettingsSection& Section) const;
	void DrawSectionList();
	// 섹션 속성 표 (Filter가 있으면 표시 이름에 들어간 것만). 반환: 그린 속성 수
	int  DrawSectionProperties(FEditorContext& Context, FSettingsSection& Section, const std::string& Filter);
	void SavePendingSections();
	bool DrawCustomSection(FSettingsSection& Section); // 사용자 정의 JSON 섹션 (반환: 바뀜)

	EKind                 Kind;
	std::string           SelectedId;
	char                  SearchText[128] = {};
	bool                  bFocusRequested = false;
	std::set<std::string> PendingSave; // 값이 바뀌어 저장을 기다리는 섹션 Id
};
