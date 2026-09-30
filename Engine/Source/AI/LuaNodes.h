#pragma once

class FBehaviorTreeNodeRegistry;

// Lua 비헤이비어 트리 노드 등록 (LuaTask/LuaDecorator/LuaService, Owner = "Engine"). RegisterAITypes가 부른다.
// 파라미터 Script(.lua, Content 기준) + Properties(스크립트 Properties 오버라이드 JSON, 스크립트 컴포넌트와 같은 형식).
// 스크립트 형식 (스크립트 컴포넌트처럼 클래스 테이블 반환, self.entity/self.Properties 사용):
//   태스크:     OnExecute(self) → "Running"/"Success"/"Failure" 또는 true/false (nil/없음 = Success)
//               OnTick(self, dt) → 같은 값 (nil = 계속 Running, 없음 = Success), OnAbort(self)
//   데코레이터: CanExecute(self) → true/"Success"면 통과 (없음 = 통과, 오류 = 실패). ObservedKeys(쉼표 구분)가 바뀌면 AbortMode로 재평가
//   서비스:     OnBecomeRelevant(self), OnTick(self, dt), OnCeaseRelevant(self)
// 스크립트 객체는 노드가 처음 쓰일 때 만들어진다 (플레이 중 FAISystem이 만든 트리에서만. 아니면 태스크/데코레이터는 실패)
void RegisterLuaBehaviorTreeNodes(FBehaviorTreeNodeRegistry& Registry);
