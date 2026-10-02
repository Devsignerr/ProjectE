#pragma once

#include "Renderer/MaterialAsset.h"

#include <string>
#include <string_view>
#include <vector>

// 노드 머티리얼 편집기(Phase 51 사이드)의 순수 편집 연산 — ImGui 없이 FMaterialGraph/파라미터 목록만 고친다 (EditorTests가 검증).
// 화면(노드 편집기)은 MaterialGraphPanel이 그리고, 구조 변경은 모두 이 함수들을 거친다.
//
// 규칙
//   - 노드 위치는 FMaterialGraphNode::EditorPosition(왼쪽 위, 캔버스 좌표). (0,0)은 "위치 없음"(JSON에 쓰지 않는 값)으로 본다.
//     출력 노드 위치는 FMaterialGraph::OutputEditorPosition.
//   - 자동 배치(AutoLayout): 출력 노드를 오른쪽 끝(0, 출력 높이 가운데)에 두고 거꾸로 열 단위 — 열 = 출력까지 가장 긴 연결 경로 길이.
//     출력에서 닿지 않는 노드는 소비자가 없으면 열 1부터 같은 규칙. 열 안 순서 = 소비자의 순서(출력 핀 순서 → 앞 열 순서 → 핀 순서),
//     열마다 세로로 쌓아 가운데 정렬(노드 높이 추정 EstimateNodeSize). 순환이 있어도 끝난다(열 상한 = 노드 수).
//   - 새 노드 Id = 종류 이름(첫 글자 소문자) + 번호, 붙여넣기는 원래 Id 기준(끝 숫자를 떼고 번호). Id는 그래프 안에서만 쓰인다.
//   - 연결: 입력 핀 하나에 연결 하나(새 연결이 바꾼다). 순환을 만드는 연결은 거부.
//   - 노드 삭제: 그 노드로 가는 입력/출력 연결도 지운다(핀은 기본값으로). 파라미터 정의는 남긴다.
//   - 복사/붙여넣기: 클립보드 = JSON 텍스트 { "ProjectEMaterialNodes": 1, "Graph": {...}, "Parameters": [...] }.
//     고른 노드 사이 연결만 남고(밖으로 나가는 연결은 끊김), 참조한 파라미터 정의를 함께 싣는다(붙일 때 같은 이름이 없으면 추가).
//   - 그래프로 변환(ConvertToGraph): 고정 PBR 식(MaterialDefault.hlsli)과 같은 출력을 노드로 만든다 — 팩터는 파라미터, 텍스처는
//     비어 있지 않은 슬롯만 TextureSample(용도 = 슬롯 용도). 반투명 계열이 아니면 Opacity/OpacityMask를 연결하지 않는다.
namespace MaterialGraphEditing
{
	constexpr float NodeWidth     = 190.0f; // 노드 기본 폭 추정 (편집기 노드 최소 폭과 같게)
	constexpr float ColumnSpacing = 290.0f; // 자동 배치 열 간격 (왼쪽 모서리 기준)
	constexpr float RowGap        = 28.0f;  // 같은 열 노드 사이 세로 간격

	// 노드 화면 크기 추정 (자동 배치/겹침 검사용). 출력 노드는 OutputNodeSize
	FVector2 EstimateNodeSize(const FMaterialGraphNode& Node);
	FVector2 OutputNodeSize();

	// 위치가 없는((0,0)) 노드가 있는지
	bool HasMissingPositions(const FMaterialGraph& Graph);
	// bOnlyMissing이면 위치 없는 노드(와 출력 노드 위치가 없으면 출력 노드)만 옮긴다
	void AutoLayout(FMaterialGraph& Graph, bool bOnlyMissing);

	std::string MakeUniqueNodeId(const FMaterialGraph& Graph, std::string_view Base);
	std::string MakeUniqueParameterName(const std::vector<FMaterialParameter>& Parameters, std::string_view Base);

	// 종류별 기본 설정으로 노드 추가. 파라미터 노드(ScalarParameter/VectorParameter/StaticSwitch/TextureSample)는 새 파라미터를 만든다.
	// 알 수 없는 종류면 nullptr. 반환 포인터는 다음 Nodes 변경 전까지만 유효
	FMaterialGraphNode* AddNode(FMaterialGraph& Graph, std::vector<FMaterialParameter>& Parameters, const std::string& Type, const FVector2& Position);
	void                RemoveNodes(FMaterialGraph& Graph, const std::vector<std::string>& Ids);

	// From 노드의 결과가 (직간접적으로) To 노드에 쓰이게 연결하면 순환인지 (From == To 포함)
	bool WouldCreateCycle(const FMaterialGraph& Graph, std::string_view FromNode, std::string_view ToNode);
	// ToNode의 ToPin ← FromNode:FromOutput (ToNode == FMaterialGraphCompiler::OutputNodeId면 머티리얼 출력 핀). 순환/없는 노드면 false
	bool Connect(FMaterialGraph& Graph, const std::string& FromNode, uint32 FromOutput, const std::string& ToNode, const std::string& ToPin);
	bool Disconnect(FMaterialGraph& Graph, const std::string& ToNode, const std::string& ToPin);
	// 핀 입력 (연결 또는 상수) 찾기 — ToNode == OutputNodeId면 출력 목록
	const FMaterialGraphInput* FindInput(const FMaterialGraph& Graph, std::string_view ToNode, std::string_view ToPin);
	// 핀에 상수 지정 (기존 연결/상수를 바꾼다)
	void SetConstant(FMaterialGraph& Graph, const std::string& ToNode, const std::string& ToPin, const FVector4& Value, uint32 Width);

	// 파라미터 이름 바꾸기 (그 이름을 쓰는 노드도). 새 이름이 비었거나 이미 있으면 false
	bool   RenameParameter(FMaterialGraph& Graph, std::vector<FMaterialParameter>& Parameters, const std::string& OldName, const std::string& NewName);
	uint32 CountParameterUses(const FMaterialGraph& Graph, std::string_view Name);

	std::string              CopyNodes(const FMaterialGraph& Graph, const std::vector<FMaterialParameter>& Parameters, const std::vector<std::string>& Ids);
	bool                     IsClipboardText(std::string_view Text);
	// 붙여넣기: 새 Id 부여 + 내부 연결 재매핑, 붙인 노드 묶음의 왼쪽 위 = Anchor. 반환 = 새 노드 Id (실패면 빈 목록)
	std::vector<std::string> PasteNodes(FMaterialGraph& Graph, std::vector<FMaterialParameter>& Parameters, std::string_view Text, const FVector2& Anchor);

	// 그래프 없는(고정 PBR) 일반 머티리얼 → 같은 출력의 그래프 머티리얼. bAlwaysBaseColorTexture = 베이스 텍스처가 비어도 TextureSample을 둔다(새 머티리얼)
	void           ConvertToGraph(FMaterialAsset& Asset, bool bAlwaysBaseColorTexture);
	FMaterialAsset MakeDefaultGraphMaterial(const std::string& Name);
} // namespace MaterialGraphEditing
