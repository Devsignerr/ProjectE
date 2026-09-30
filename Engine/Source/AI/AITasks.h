#pragma once

class FBehaviorTreeNodeRegistry;

// 씬 연동 태스크 등록 (MoveTo/RotateTo/PlayAnimation, Owner = "Engine"). RegisterAITypes가 부른다.
// 이 태스크들은 FAISystem이 만든 트리(컨텍스트에 Scene/AI가 있음)에서만 동작하고, 없으면 Failure
void RegisterAITasks(FBehaviorTreeNodeRegistry& Registry);
