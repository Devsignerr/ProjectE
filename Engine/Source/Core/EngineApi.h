#pragma once

// 엔진 DLL(ProjectEEngine.dll) 경계 표식.
//   함수/클래스 메서드는 CMake WINDOWS_EXPORT_ALL_SYMBOLS로 자동 내보내므로 표식이 필요 없다.
//   DLL 밖(에디터/런타임/게임 모듈/테스트)에서 접근하는 **전역 데이터**(로그 카테고리 등)만 E_ENGINE_API를 붙인다.
//   E_ENGINE_SHARED: 공유 빌드(엔진 모듈과 소비자 모두 정의), E_ENGINE_EXPORTS: 엔진 모듈 소스를 컴파일할 때만 정의
#if defined(E_ENGINE_SHARED)
	#if defined(E_ENGINE_EXPORTS)
		#define E_ENGINE_API __declspec(dllexport)
	#else
		#define E_ENGINE_API __declspec(dllimport)
	#endif
#else
	#define E_ENGINE_API
#endif
