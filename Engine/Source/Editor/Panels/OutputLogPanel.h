#pragma once

#include "Core/Log.h"

#include <deque>

struct FEditorContext;

class FOutputLogPanel
{
public:
	void Draw(FEditorContext& Context);
	bool bOpen = true;

private:
	std::deque<FLogMessage> Messages;
	uint64 LastSequence = 0;
	char Search[256] = {};
	bool bAutoScroll = true;
};
