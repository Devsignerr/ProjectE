#pragma once

struct FEditorContext;

// 씬 렌더러의 포스트 프로세싱 설정(톤매핑, 노출, 자동 노출, 블룸) 편집
class FPostProcessPanel
{
public:
	void Draw(FEditorContext& Context);

	bool bOpen = true;
};
