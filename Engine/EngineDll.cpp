// ProjectEEngine.dll 진입 단위. 엔진 모듈(Core/RHI/Scene/Renderer/Audio/Physics/Scripting)의 객체 파일이 이 DLL로 링크되고
// 함수는 자동 .def(sol2 내부·std 함수 제외, Tools/ExportFilter)로, 전역 데이터는 E_ENGINE_API로 내보낸다 (Core/EngineApi.h).

// 엔진 DLL 빌드 버전 (게임 모듈 호환성 검사용 — 형식이 바뀌면 올린다)
extern "C" __declspec(dllexport) unsigned int ProjectE_GetEngineApiVersion()
{
	return 1;
}
