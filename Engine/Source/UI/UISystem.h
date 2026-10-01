#pragma once

#include "UI/UIAsset.h"
#include "UI/UIDrawList.h"
#include "UI/UIInput.h"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

class FInput;
class FScene;
class FUIInstance;
struct FUIComponent;

// .eui 파일 캐시 (경로별, 프로세스 전역). 파일이 바뀌었으면(수정 시각) 다시 읽는다 → 디자이너에서 저장하면 다음 인스턴스부터 반영.
class FUIAssetLibrary
{
public:
	static FUIAssetLibrary& Get();

	// 실패 시 nullptr (같은 파일 오류를 매번 로그하지 않도록 실패도 기억, 파일이 바뀌면 다시 시도)
	std::shared_ptr<const FUIAsset> Load(const std::filesystem::path& Path);
	void                            Clear() { Entries.clear(); }

private:
	struct FEntry
	{
		std::shared_ptr<const FUIAsset>  Asset;
		std::filesystem::file_time_type WriteTime;
	};
	std::unordered_map<std::wstring, FEntry> Entries; // 키: 정규화 경로 (소문자)
};

// 한 프레임의 UI 입력 (화면 픽셀 기준)
struct FUIFrameInput
{
	FUIRect         Viewport;     // UI를 놓을 화면 영역 (보통 출력 전체)
	bool            bHasPointer = false;
	FUIPointerInput Pointer;      // 위치는 화면 픽셀 (Viewport와 같은 좌표계)
	FUIKeyInput     Keys;
	float           DeltaSeconds = 0.0f; // 캐럿 깜빡임, UI 애니메이션
};

// UI가 이번 프레임에 가져간 입력 → 게임에는 FInput::WithoutMouseButtons / WithoutKeyboard 사본을 넘긴다
struct FUIInputResult
{
	bool bPointer  = false;
	bool bKeyboard = false; // 포커스된 텍스트 상자
	bool    bHasTextCaret = false; // 키보드를 가져간 텍스트 상자의 캐럿 (화면 픽셀, Viewport와 같은 좌표계) → 앱이 IME 후보 창 위치로 (FWindow::SetTextInput)
	FUIRect TextCaret;
};

// 씬의 UI 컴포넌트 처리 (상태는 컴포넌트 Runtime에 있다 — 시스템 자체는 상태 없음).
//   Update: 인스턴스 준비(에셋 경로가 바뀌면 다시) → 레이아웃 → 입력(Z 순서가 큰 것부터, 위 UI가 포인터를 가져가면 아래는 포인터 없음)
//           → Runtime.Events. 반환: UI가 포인터/키보드를 가져갔는지 (게임 입력에서 빼야 함)
//           키: 텍스트 편집은 모든 UI, Tab/Enter 버튼 탐색은 KeyboardFocus를 켠 UI만. 위 UI가 키보드를 가져가면 아래는 키 없음
//   Paint:  보이는 UI를 Z 순서 오름차순으로 그리기 목록에 쌓는다 (Update 뒤)
// 순서: FUISystem::Update → FGameWorld::TickGameplay(스크립트가 이벤트를 받음) → 렌더(Paint → FUIRenderer)
struct FUISystem
{
	static FUIInputResult Update(FScene& Scene, const FUIFrameInput& Input, const std::filesystem::path& ContentDirectory);
	static void Paint(FScene& Scene, FUIDrawList& Out);
	// 컴포넌트 인스턴스를 (필요하면) 만든다. 에셋이 없거나 읽지 못하면 nullptr
	static FUIInstance* EnsureInstance(FUIComponent& Component, const std::filesystem::path& ContentDirectory);

	// 엔진 입력 → UI 입력. PixelOffset: 입력 좌표 → 화면 픽셀 변환에 더할 값 (에디터 뷰포트 이미지 위치의 음수 등)
	static FUIPointerInput MakePointer(const FInput& Input, const FVector2& PixelOffset, bool bInside);
	static FUIKeyInput     MakeKeys(const FInput& Input);
};
