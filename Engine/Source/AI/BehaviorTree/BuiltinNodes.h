#pragma once

class FBehaviorTreeNodeRegistry;

// 엔진 기본 노드 등록 (Owner = "Engine"). 레지스트리가 첫 Get()에서 호출한다.
//   컴포지트: Selector, Sequence, SimpleParallel(자식 0 = 주 태스크, 자식 1 = 배경 가지)
//   데코레이터: Blackboard(Key, Operation, Value, AbortMode), Cooldown(CooldownTime), Loop(NumLoops, 0 = 무한),
//              TimeLimit(TimeLimit), Inverter, ForceSuccess
//   태스크: Wait(WaitTime, RandomDeviation), SetBlackboard(Key, Value), Log(Message)
// Blackboard 데코레이터/SetBlackboard의 Value는 문자열이고 키 타입에 맞춰 해석한다 (BehaviorTreeTypes::ParseBlackboardValue).
// Blackboard 데코레이터: 키가 설정 안 됐으면 IsNotSet만 참. Less/Greater 계열은 Int/Float 키만 지원
void RegisterBuiltinBehaviorTreeNodes(FBehaviorTreeNodeRegistry& Registry);
