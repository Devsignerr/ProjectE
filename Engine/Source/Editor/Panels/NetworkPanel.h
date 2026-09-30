#pragma once

struct FEditorContext;

// 네트워크 패널: 에디터 네트워크 플레이 설정(1인용/리슨/전용 서버, 클라이언트 수, 포트), 진행 중인 세션 상태,
// LAN 세션 찾기와 런타임 클라이언트로 접속
class FNetworkPanel
{
public:
	void Draw(FEditorContext& Context);
	bool bOpen = false; // 창 메뉴에서 연다 (기존 레이아웃에 자리가 없어 떠서 열리므로 기본은 닫힘)
};
