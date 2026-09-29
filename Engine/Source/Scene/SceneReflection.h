#pragma once

// Scene 모듈의 컴포넌트 타입을 리플렉션 레지스트리에 등록한다. 여러 번 호출해도 한 번만 등록된다.
// FScene 생성자가 호출하므로 보통 직접 부를 필요는 없다.
void RegisterSceneTypes();
