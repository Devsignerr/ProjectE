#pragma once

#include "Core/Reflection/PropertyType.h"

struct FPropertyInfo;

// 리플렉션 프로퍼티 값 위젯 (인스펙터와 설정 창 공용). ImGui 프레임 안에서만 부른다
class FPropertyWidgets
{
public:
	// Bool/Int32(enum이면 콤보)/UInt32/Float/String/Vector2/3/4. Quat/Entity/ResourceHandle은 호출자가 그린다
	static bool IsValueType(EPropertyType Type);

	// 값 위젯 하나. Label이 nullptr이면 DisplayName. 툴팁이 있으면 마우스 오버에 표시. 반환: 이번 프레임에 값이 바뀜
	static bool DrawValue(const FPropertyInfo& Property, void* Object, const char* Label = nullptr);

	// 에셋 경로 문자열(AssetFilter가 있는 String): 직접 입력 + 콘텐츠 브라우저 드롭(Content 기준 경로).
	// 확장자가 맞지 않는 드롭이면 bOutRejected = true (값은 그대로)
	static bool DrawAssetPath(const FPropertyInfo& Property, void* Object, const char* Label, bool* bOutRejected = nullptr);
};
