#pragma once

// 머티리얼 그래프/파라미터 JSON 읽기·쓰기 (Renderer 모듈 내부 — MaterialAsset.cpp가 .emat 안에서 쓴다).
// 형식은 Renderer/MaterialGraph.h 머리 주석이 기준.

#include "Renderer/MaterialGraph.h"

#include <json.hpp>

#include <string>
#include <vector>

namespace MaterialGraphJson
{
	// 실패한 항목은 건너뛰고 OutWarnings에 사유를 남긴다 (그래프 의미 오류는 컴파일러가 따로 검사)
	void ReadParameters(const nlohmann::json& Array, std::vector<FMaterialParameter>& Out, std::vector<std::string>& OutWarnings);
	void ReadGraph(const nlohmann::json& Object, FMaterialGraph& Out, std::vector<std::string>& OutWarnings);

	// bWriteType = false: 인스턴스 덮어쓰기 (이름 + 값만 — 타입은 부모 것)
	nlohmann::json WriteParameters(const std::vector<FMaterialParameter>& Parameters, bool bWriteType = true);
	nlohmann::json WriteGraph(const FMaterialGraph& Graph);
} // namespace MaterialGraphJson
