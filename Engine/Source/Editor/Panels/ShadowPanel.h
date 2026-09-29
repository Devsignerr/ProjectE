#pragma once

struct FEditorContext;

class FShadowPanel
{
public:
	void Draw(FEditorContext& Context);
	bool bOpen = true;
};