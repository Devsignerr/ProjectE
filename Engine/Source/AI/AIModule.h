#pragma once

#include "Core/Log.h"

E_DECLARE_ENGINE_LOG_CATEGORY(LogAI)

// AI 컴포넌트(비헤이비어 트리, 내비메시 설정) 리플렉션 등록. 앱은 씬 로드 전에 호출한다 (여러 번 호출 안전)
void RegisterAITypes();
