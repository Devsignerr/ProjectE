#include "Renderer/MaterialGraph.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <map>
#include <unordered_map>

// 머티리얼 그래프 → HLSL (규칙은 MaterialGraph.h 머리 주석이 기준, 생성 함수가 쓰는 공용 식/매크로는 Engine/Shaders/MaterialCommon.hlsli)

namespace
{
	// 식 하나: HLSL 코드 + 너비 (0 = 오류)
	struct FValue
	{
		std::string Code;
		uint32      Width = 0;
	};

	// 핀 기본값 (연결하지 않았을 때): Required / Constant(표의 상수) / TexCoord(In.UV0) / Time(E_MATERIAL_TIME) / WorldNormal(In.WorldNormal)
	using EPinDefault = EMaterialPinDefault;

	struct FPinDesc
	{
		const char* Name;
		EPinDefault Default       = EPinDefault::Required;
		FVector4    Constant      = FVector4::ZeroVector;
		uint32      ConstantWidth = 1;
	};

	class FCompiler;
	// 입력(핀 순서, 기본값 적용됨) → 출력들. 실패하면 Compiler.Error 후 false
	using FEmitFunction = std::function<bool(FCompiler& Compiler, const FMaterialGraphNode& Node, const std::vector<FValue>& Inputs, std::vector<FValue>& Outputs)>;

	struct FNodeDesc
	{
		const char*            Type;
		std::vector<FPinDesc>  Pins;
		uint32                 OutputCount = 1;
		FEmitFunction          Emit;
		uint32                 Settings      = MaterialNodeSetting_None; // 편집기 상세 패널이 보일 설정 (EMaterialNodeSetting)
		EMaterialParameterType ParameterType = EMaterialParameterType::Scalar;
		const char*            Category      = ""; // 편집기 팔레트 범주 (GetNodeTable의 Section이 채운다)
	};

	const std::vector<FNodeDesc>& GetNodeTable();

	const FNodeDesc* FindNodeDesc(const std::string& Type)
	{
		for (const FNodeDesc& Desc : GetNodeTable())
		{
			if (Type == Desc.Type)
			{
				return &Desc;
			}
		}
		return nullptr;
	}

	std::string FloatType(uint32 Width) { return Width <= 1 ? std::string("float") : std::format("float{}", Width); }

	// 결정적 실수 리터럴 (가장 짧은 왕복 표기 + f)
	std::string FormatFloat(float Value)
	{
		if (!std::isfinite(Value))
		{
			Value = 0.0f;
		}
		if (Value == 0.0f)
		{
			Value = 0.0f; // -0 → 0
		}
		std::string Text = std::format("{}", Value);
		if (Text.find_first_of(".eE") == std::string::npos)
		{
			Text += ".0";
		}
		return Text + "f";
	}

	std::string FormatConstant(const FVector4& Value, uint32 Width)
	{
		if (Width <= 1)
		{
			return FormatFloat(Value.X);
		}
		std::string Code = FloatType(Width) + "(";
		for (uint32 Index = 0; Index < Width; ++Index)
		{
			Code += (Index == 0 ? "" : ", ") + FormatFloat((&Value.X)[Index]);
		}
		return Code + ")";
	}

	const char* SwizzleOf(uint32 Count) // 앞 Count 성분
	{
		static const char* const Swizzles[] = { "", "x", "xy", "xyz", "xyzw" };
		return Swizzles[std::min(Count, 4u)];
	}

	class FCompiler
	{
	public:
		FCompiler(const FMaterialGraph& InGraph, const std::vector<FMaterialParameter>& InParameters)
			: Graph(InGraph)
			, Parameters(InParameters)
		{
		}

		FMaterialGraphCompileResult Run();
		// 편집기 분석: 모든 노드를 평가해 출력 너비와 오류를 모은다 (코드는 쓰지 않음)
		FMaterialGraphAnalysis Analyze();

		// ---- 노드 코드 생성 도우미
		void Error(const FMaterialGraphNode* Node, const std::string& Message)
		{
			Errors.push_back(Node != nullptr ? std::format("[{}] {}", Node->Id, Message) : Message);
			ErrorNodes.push_back(Node != nullptr ? Node->Id : NullNodeContext);
		}

		// 지역 변수로 남긴다 (같은 키 = 같은 식이면 이전 변수 재사용)
		FValue Local(const std::string& Key, uint32 Width, const std::string& Expression)
		{
			const std::string FullKey = std::format("{}#{}={}", Key, Width, Expression);
			if (const auto Found = LocalByKey.find(FullKey); Found != LocalByKey.end())
			{
				return { Found->second, Width };
			}
			const std::string Name = std::format("Local{}", LocalCount++);
			Lines += std::format("\tconst {} {} = {};\n", FloatType(Width), Name, Expression);
			LocalByKey.emplace(FullKey, Name);
			return { Name, Width };
		}

		// 너비 변환: 같으면 그대로, float1은 확장, bTruncate면 앞 성분 자르기. 안 되면 오류
		bool Convert(const FMaterialGraphNode* Node, const char* Pin, const FValue& Value, uint32 Width, bool bTruncate, std::string& OutCode)
		{
			if (Value.Width == Width)
			{
				OutCode = Value.Code;
				return true;
			}
			if (Value.Width == 1)
			{
				OutCode = std::format("(({}){})", FloatType(Width), Value.Code);
				return true;
			}
			if (bTruncate && Value.Width > Width)
			{
				OutCode = std::format("({}).{}", Value.Code, SwizzleOf(Width));
				return true;
			}
			Error(Node, std::format("입력 {}: float{}를 float{}로 쓸 수 없습니다 (float1만 확장, 그 밖의 너비 차이는 오류)", Pin, Value.Width, Width));
			return false;
		}

		// 두 입력의 공통 너비 (같거나 한쪽이 float1)
		bool CommonWidth(const FMaterialGraphNode& Node, const FValue& A, const FValue& B, uint32& OutWidth)
		{
			if (A.Width == B.Width || B.Width == 1)
			{
				OutWidth = A.Width;
				return true;
			}
			if (A.Width == 1)
			{
				OutWidth = B.Width;
				return true;
			}
			Error(&Node, std::format("입력 너비가 맞지 않습니다 (float{}와 float{} — float1만 확장)", A.Width, B.Width));
			return false;
		}

		const FMaterialParameter* FindParameter(const FMaterialGraphNode& Node, EMaterialParameterType Type)
		{
			for (uint32 Index = 0; Index < static_cast<uint32>(Parameters.size()); ++Index)
			{
				if (Parameters[Index].Name == Node.Name)
				{
					if (Parameters[Index].Type != Type)
					{
						Error(&Node, std::format("파라미터 {}의 타입이 {}가 아닙니다", Node.Name, GetMaterialParameterTypeName(Type)));
						return nullptr;
					}
					return &Parameters[Index];
				}
			}
			Error(&Node, std::format("파라미터 {}가 없습니다", Node.Name.empty() ? std::string("(이름 없음)") : Node.Name));
			return nullptr;
		}

		// 파라미터 참조 자리표시 (레이아웃은 모두 모은 뒤 선언 순서로 정해 바꿔 넣는다)
		std::string ParameterToken(const FMaterialParameter& Parameter)
		{
			const uint32 Index = static_cast<uint32>(&Parameter - Parameters.data());
			UsedParameters.push_back(Index);
			return std::format("\x01{}\x01", Index);
		}

		const FMaterialParameter* GetStaticSwitch(const FMaterialGraphNode& Node) { return FindParameter(Node, EMaterialParameterType::StaticSwitch); }

	private:
		bool CheckStructure();
		bool Evaluate(uint32 NodeIndex, uint32 Output, const FMaterialGraphNode* Consumer, FValue& OutValue);
		bool EvaluateInput(const FMaterialGraphNode* Node, const FMaterialGraphInput& Input, FValue& OutValue);
		bool EvaluatePin(const FMaterialGraphNode& Node, const FPinDesc& Pin, FValue& OutValue);
		// 머티리얼 출력 핀 대입문 (출력 핀 순서). 오류는 Errors에
		std::string EvaluateOutputs();
		std::string Finalize(const std::string& Body, FMaterialParameterLayout& OutLayout);

		const FMaterialGraph&                  Graph;
		const std::vector<FMaterialParameter>& Parameters;
		std::vector<std::string>               Errors;
		std::vector<std::string>               ErrorNodes;
		std::string                            NullNodeContext; // 노드 없는 오류의 위치 (머티리얼 출력 평가 중 = OutputNodeId)
		std::unordered_map<std::string, uint32> NodeIndexById;
		std::vector<std::vector<FValue>>       NodeOutputs; // 노드별 계산 결과 (계산 전 비어 있음)
		std::vector<uint8>                     NodeFailed;
		std::unordered_map<std::string, std::string> LocalByKey;
		std::string                            Lines;
		uint32                                 LocalCount = 0;
		std::vector<uint32>                    UsedParameters;
	};

	bool FCompiler::CheckStructure()
	{
		// 중복 Id, 종류, 핀 이름, 연결 대상
		for (uint32 Index = 0; Index < static_cast<uint32>(Graph.Nodes.size()); ++Index)
		{
			const FMaterialGraphNode& Node = Graph.Nodes[Index];
			if (Node.Id.empty())
			{
				Error(nullptr, std::format("노드 {}에 Id가 없습니다", Index));
				continue;
			}
			if (!NodeIndexById.emplace(Node.Id, Index).second)
			{
				Error(&Node, "Id가 중복됩니다");
			}
		}
		for (const FMaterialGraphNode& Node : Graph.Nodes)
		{
			const FNodeDesc* Desc = FindNodeDesc(Node.Type);
			if (Desc == nullptr)
			{
				Error(&Node, std::format("알 수 없는 노드 종류: {}", Node.Type));
				continue;
			}
			for (const FMaterialGraphInput& Input : Node.Inputs)
			{
				const bool bKnownPin = std::any_of(Desc->Pins.begin(), Desc->Pins.end(), [&](const FPinDesc& Pin) { return Input.Pin == Pin.Name; });
				if (!bKnownPin)
				{
					Error(&Node, std::format("{} 노드에 {} 입력이 없습니다", Node.Type, Input.Pin));
				}
				if (Input.IsLink() && !NodeIndexById.contains(Input.Node))
				{
					Error(&Node, std::format("입력 {}: 없는 노드 {}", Input.Pin, Input.Node));
				}
			}
		}
		NullNodeContext = FMaterialGraphCompiler::OutputNodeId;
		for (const FMaterialGraphInput& Output : Graph.Outputs)
		{
			bool bKnown = false;
			for (uint32 Index = 0; Index < static_cast<uint32>(EMaterialOutput::Count); ++Index)
			{
				bKnown |= Output.Pin == GetMaterialOutputName(static_cast<EMaterialOutput>(Index));
			}
			if (!bKnown)
			{
				Error(nullptr, std::format("알 수 없는 머티리얼 출력: {}", Output.Pin));
			}
			if (Output.IsLink() && !NodeIndexById.contains(Output.Node))
			{
				Error(nullptr, std::format("출력 {}: 없는 노드 {}", Output.Pin, Output.Node));
			}
		}
		NullNodeContext.clear();
		if (!Errors.empty())
		{
			return false;
		}

		// 순환 (쓰지 않는 노드 포함 전체): 반복 DFS, 상태 0 = 미방문, 1 = 방문 중, 2 = 완료
		std::vector<uint8> State(Graph.Nodes.size(), 0);
		for (uint32 Start = 0; Start < static_cast<uint32>(Graph.Nodes.size()); ++Start)
		{
			if (State[Start] != 0)
			{
				continue;
			}
			std::vector<std::pair<uint32, uint32>> Stack = { { Start, 0u } }; // (노드, 다음 입력 번호)
			State[Start]                                 = 1;
			while (!Stack.empty())
			{
				auto& [NodeIndex, Next] = Stack.back();
				const FMaterialGraphNode& Node = Graph.Nodes[NodeIndex];
				if (Next >= Node.Inputs.size())
				{
					State[NodeIndex] = 2;
					Stack.pop_back();
					continue;
				}
				const FMaterialGraphInput& Input = Node.Inputs[Next++];
				if (!Input.IsLink())
				{
					continue;
				}
				const uint32 Target = NodeIndexById.at(Input.Node);
				if (State[Target] == 1)
				{
					Error(&Node, std::format("순환 연결: {} → {}", Node.Id, Input.Node));
					return false;
				}
				if (State[Target] == 0)
				{
					State[Target] = 1;
					Stack.emplace_back(Target, 0u);
				}
			}
		}
		return true;
	}

	bool FCompiler::EvaluateInput(const FMaterialGraphNode* Node, const FMaterialGraphInput& Input, FValue& OutValue)
	{
		if (!Input.IsLink())
		{
			const uint32 Width = std::clamp(Input.ConstantWidth, 1u, 4u);
			OutValue           = { FormatConstant(Input.Constant, Width), Width };
			return true;
		}
		return Evaluate(NodeIndexById.at(Input.Node), Input.Output, Node, OutValue);
	}

	bool FCompiler::EvaluatePin(const FMaterialGraphNode& Node, const FPinDesc& Pin, FValue& OutValue)
	{
		if (const FMaterialGraphInput* Input = Node.FindInput(Pin.Name))
		{
			return EvaluateInput(&Node, *Input, OutValue);
		}
		switch (Pin.Default)
		{
		case EPinDefault::Constant:    OutValue = { FormatConstant(Pin.Constant, Pin.ConstantWidth), Pin.ConstantWidth }; return true;
		case EPinDefault::TexCoord:    OutValue = { "In.UV0", 2 }; return true;
		case EPinDefault::Time:        OutValue = { "E_MATERIAL_TIME", 1 }; return true;
		case EPinDefault::WorldNormal: OutValue = { "In.WorldNormal", 3 }; return true;
		default:
			Error(&Node, std::format("입력 {}이(가) 연결되지 않았습니다", Pin.Name));
			return false;
		}
	}

	bool FCompiler::Evaluate(uint32 NodeIndex, uint32 Output, const FMaterialGraphNode* Consumer, FValue& OutValue)
	{
		const FMaterialGraphNode& Node = Graph.Nodes[NodeIndex];
		const FNodeDesc*          Desc = FindNodeDesc(Node.Type);
		if (NodeFailed[NodeIndex] != 0)
		{
			return false;
		}
		if (NodeOutputs[NodeIndex].empty())
		{
			std::vector<FValue> Outputs;
			bool                bOk = true;
			if (Node.Type == "StaticSwitch")
			{
				// 정적 스위치: 고른 쪽 입력만 따라간다 (다른 쪽 노드·파라미터는 코드/레이아웃에 들어가지 않는다)
				const FMaterialParameter* Switch = GetStaticSwitch(Node);
				FValue                    Chosen;
				bOk = Switch != nullptr && EvaluatePin(Node, Desc->Pins[Switch->GetBool() ? 0 : 1], Chosen);
				Outputs.push_back(Chosen);
			}
			else
			{
				std::vector<FValue> Inputs(Desc->Pins.size());
				for (size_t Pin = 0; Pin < Desc->Pins.size() && bOk; ++Pin)
				{
					bOk = EvaluatePin(Node, Desc->Pins[Pin], Inputs[Pin]);
				}
				bOk = bOk && Desc->Emit(*this, Node, Inputs, Outputs);
			}
			if (!bOk)
			{
				NodeFailed[NodeIndex] = 1;
				return false;
			}
			NodeOutputs[NodeIndex] = std::move(Outputs);
		}
		const std::vector<FValue>& Results = NodeOutputs[NodeIndex];
		if (Output >= Results.size() || Results[Output].Width == 0)
		{
			Error(Consumer, std::format("{} 노드({})에 출력 {}이(가) 없습니다", Node.Type, Node.Id, Output));
			return false;
		}
		OutValue = Results[Output];
		return true;
	}

	std::string FCompiler::Finalize(const std::string& Body, FMaterialParameterLayout& OutLayout)
	{
		// 쓰인 파라미터를 선언 순서로: 벡터(레지스터 하나씩) → 스칼라(4개씩 묶음), 텍스처는 칸 0부터
		std::sort(UsedParameters.begin(), UsedParameters.end());
		UsedParameters.erase(std::unique(UsedParameters.begin(), UsedParameters.end()), UsedParameters.end());
		std::map<uint32, std::string> Replacement;
		uint32                        Register = 0;
		for (const uint32 Index : UsedParameters)
		{
			if (Parameters[Index].Type == EMaterialParameterType::Vector)
			{
				OutLayout.Slots.push_back({ Parameters[Index].Name, EMaterialParameterType::Vector, Register, 0, ETextureUsage::Color });
				Replacement[Index] = std::format("E_MATERIAL_PARAM({})", Register++);
			}
		}
		uint32 ScalarCount = 0;
		for (const uint32 Index : UsedParameters)
		{
			if (Parameters[Index].Type == EMaterialParameterType::Scalar)
			{
				const uint32 ScalarRegister  = Register + ScalarCount / 4;
				const uint32 ScalarComponent = ScalarCount % 4;
				OutLayout.Slots.push_back({ Parameters[Index].Name, EMaterialParameterType::Scalar, ScalarRegister, ScalarComponent, ETextureUsage::Color });
				Replacement[Index] = std::format("E_MATERIAL_PARAM({}).{}", ScalarRegister, "xyzw"[ScalarComponent]);
				++ScalarCount;
			}
		}
		OutLayout.ConstantRegisters = Register + (ScalarCount + 3) / 4;
		uint32 TextureSlot          = 0;
		for (const uint32 Index : UsedParameters)
		{
			if (Parameters[Index].Type == EMaterialParameterType::Texture)
			{
				OutLayout.Slots.push_back({ Parameters[Index].Name, EMaterialParameterType::Texture, TextureSlot, 0, Parameters[Index].Usage });
				Replacement[Index] = std::format("E_MATERIAL_TEXTURE({})", TextureSlot++);
			}
		}
		OutLayout.TextureCount = TextureSlot;

		std::string Resolved;
		Resolved.reserve(Body.size());
		for (size_t Position = 0; Position < Body.size();)
		{
			if (Body[Position] != '\x01')
			{
				Resolved.push_back(Body[Position++]);
				continue;
			}
			const size_t End   = Body.find('\x01', Position + 1);
			const uint32 Index = static_cast<uint32>(std::stoul(Body.substr(Position + 1, End - Position - 1)));
			Resolved += Replacement[Index];
			Position = End + 1;
		}

		// 리소스 선언 + 함수
		std::string Hlsl;
		Hlsl += "// ProjectE 머티리얼 그래프 생성 코드 (FMaterialGraphCompiler — 손으로 고치지 않는다). 사용법: MaterialCommon.hlsli 머리 주석\n";
		Hlsl += std::format("#define E_MATERIAL_GRAPH_CONSTANT_REGISTERS {}\n", OutLayout.ConstantRegisters);
		Hlsl += std::format("#define E_MATERIAL_GRAPH_TEXTURE_COUNT {}\n", OutLayout.TextureCount);
		Hlsl += "#ifndef E_MATERIAL_CUSTOM_RESOURCES\n";
		Hlsl += "cbuffer MaterialGraphParameters : register(E_MATERIAL_CBUFFER_REGISTER)\n{\n";
		Hlsl += "\tfloat4 MaterialHeader; // FMaterialGraphHeader: x = 시간(초), y = 알파 컷오프\n";
		if (OutLayout.ConstantRegisters > 0)
		{
			Hlsl += std::format("\tfloat4 MaterialParams[{}];\n", OutLayout.ConstantRegisters);
		}
		Hlsl += "};\n";
		for (const FMaterialParameterSlot& Slot : OutLayout.Slots)
		{
			if (Slot.Type == EMaterialParameterType::Texture)
			{
				Hlsl += std::format("Texture2D MaterialTexture{0} : register(t{0}, E_MATERIAL_TEXTURE_SPACE); // {1}\n", Slot.Register, GetTextureUsageName(Slot.Usage));
			}
		}
		Hlsl += "#endif\n\n";
		Hlsl += "void EvaluateMaterial(in FMaterialPixelInputs In, out FMaterialSurface Out)\n{\n";
		Hlsl += Resolved;
		Hlsl += "}\n";
		return Hlsl;
	}

	std::string FCompiler::EvaluateOutputs()
	{
		// 출력 핀 순서대로 (결정적). 연결 안 된 출력은 기본값
		static const FVector4 Defaults[] = { FVector4(0.5f, 0.5f, 0.5f, 0.0f), FVector4::ZeroVector, FVector4(0.5f, 0.0f, 0.0f, 0.0f),
			                                 FVector4(0.0f, 0.0f, 1.0f, 0.0f), FVector4::OneVector,  FVector4::ZeroVector,
			                                 FVector4::OneVector,                FVector4::OneVector };
		NullNodeContext = FMaterialGraphCompiler::OutputNodeId;
		std::string Assignments;
		for (uint32 Index = 0; Index < static_cast<uint32>(EMaterialOutput::Count); ++Index)
		{
			const EMaterialOutput Output = static_cast<EMaterialOutput>(Index);
			const char*           Name   = GetMaterialOutputName(Output);
			const uint32          Width  = GetMaterialOutputWidth(Output);
			std::string           Code   = FormatConstant(Defaults[Index], Width);
			for (const FMaterialGraphInput& Input : Graph.Outputs)
			{
				FValue Value;
				if (Input.Pin == Name && EvaluateInput(nullptr, Input, Value))
				{
					std::string Converted;
					if (Convert(nullptr, Name, Value, Width, true, Converted))
					{
						Code = Converted;
					}
				}
			}
			Assignments += std::format("\tOut.{} = {};\n", Name, Code);
		}
		NullNodeContext.clear();
		return Assignments;
	}

	FMaterialGraphAnalysis FCompiler::Analyze()
	{
		FMaterialGraphAnalysis Result;
		for (const FMaterialGraphNode& Node : Graph.Nodes)
		{
			Result.NodeIds.push_back(Node.Id);
		}
		Result.OutputWidths.resize(Graph.Nodes.size());
		if (CheckStructure())
		{
			NodeOutputs.assign(Graph.Nodes.size(), {});
			NodeFailed.assign(Graph.Nodes.size(), 0);
			// 출력부터 (출력에서 닿는 노드의 오류가 먼저) → 나머지 노드
			EvaluateOutputs();
			for (uint32 Index = 0; Index < static_cast<uint32>(Graph.Nodes.size()); ++Index)
			{
				FValue Ignored;
				if (NodeOutputs[Index].empty() && NodeFailed[Index] == 0)
				{
					Evaluate(Index, 0, nullptr, Ignored);
				}
				for (const FValue& Value : NodeOutputs[Index])
				{
					Result.OutputWidths[Index].push_back(Value.Width);
				}
			}
		}
		Result.Errors     = std::move(Errors);
		Result.ErrorNodes = std::move(ErrorNodes);
		return Result;
	}

	FMaterialGraphCompileResult FCompiler::Run()
	{
		FMaterialGraphCompileResult Result;
		if (!CheckStructure())
		{
			Result.Errors     = std::move(Errors);
			Result.ErrorNodes = std::move(ErrorNodes);
			return Result;
		}
		NodeOutputs.assign(Graph.Nodes.size(), {});
		NodeFailed.assign(Graph.Nodes.size(), 0);

		const std::string Assignments = EvaluateOutputs();
		if (!Errors.empty())
		{
			Result.Errors     = std::move(Errors);
			Result.ErrorNodes = std::move(ErrorNodes);
			return Result;
		}

		auto Shader    = std::make_shared<FMaterialShader>();
		Shader->Hlsl   = Finalize(Lines + Assignments, Shader->Layout);
		Shader->Hash   = FMaterialGraphCompiler::HashText(Shader->Hlsl);
		if (Shader->Layout.TextureCount > MaterialTextureMax)
		{
			Result.Errors.push_back(std::format("텍스처 파라미터가 너무 많습니다 ({} > {})", Shader->Layout.TextureCount, MaterialTextureMax));
			Result.ErrorNodes.emplace_back();
			return Result;
		}
		Result.Shader   = std::move(Shader);
		Result.bSuccess = true;
		return Result;
	}

	// ---- 노드 표
	FEmitFunction Binary(const char* Operator)
	{
		return [Operator](FCompiler& C, const FMaterialGraphNode& Node, const std::vector<FValue>& In, std::vector<FValue>& Out) {
			uint32      Width = 0;
			std::string A;
			std::string B;
			if (!C.CommonWidth(Node, In[0], In[1], Width) || !C.Convert(&Node, "A", In[0], Width, false, A) || !C.Convert(&Node, "B", In[1], Width, false, B))
			{
				return false;
			}
			Out.push_back(C.Local(Node.Type, Width, std::format("{} {} {}", A, Operator, B)));
			return true;
		};
	}

	FEmitFunction BinaryCall(const char* Function)
	{
		return [Function](FCompiler& C, const FMaterialGraphNode& Node, const std::vector<FValue>& In, std::vector<FValue>& Out) {
			uint32      Width = 0;
			std::string A;
			std::string B;
			if (!C.CommonWidth(Node, In[0], In[1], Width) || !C.Convert(&Node, "A", In[0], Width, false, A) || !C.Convert(&Node, "B", In[1], Width, false, B))
			{
				return false;
			}
			Out.push_back(C.Local(Node.Type, Width, std::vformat(Function, std::make_format_args(A, B))));
			return true;
		};
	}

	// 같은 너비 단항 (Format의 {}에 입력)
	FEmitFunction Unary(const char* Format)
	{
		return [Format](FCompiler& C, const FMaterialGraphNode& Node, const std::vector<FValue>& In, std::vector<FValue>& Out) {
			Out.push_back(C.Local(Node.Type, In[0].Width, std::vformat(Format, std::make_format_args(In[0].Code))));
			return true;
		};
	}

	// 텍스처 샘플/정점 색 출력: 0 = RGBA, 1 = RGB, 2~5 = R, G, B, A
	void PushColorOutputs(const FValue& Rgba, std::vector<FValue>& Out)
	{
		Out.push_back(Rgba);
		Out.push_back({ Rgba.Code + ".rgb", 3 });
		for (const char* Channel : { ".r", ".g", ".b", ".a" })
		{
			Out.push_back({ Rgba.Code + Channel, 1 });
		}
	}

	FPinDesc Pin(const char* Name) { return { Name }; }
	FPinDesc PinDefault(const char* Name, float Value) { return { Name, EPinDefault::Constant, FVector4(Value, 0.0f, 0.0f, 0.0f), 1 }; }

	// 표 범주: Section(범주, 노드들)로 묶어 넣는다 (편집기 팔레트가 이 범주와 순서로 보인다)
	void Section(std::vector<FNodeDesc>& Table, const char* Category, std::vector<FNodeDesc> Nodes)
	{
		for (FNodeDesc& Node : Nodes)
		{
			Node.Category = Category;
			Table.push_back(std::move(Node));
		}
	}

	std::vector<FNodeDesc> BuildNodeTable()
	{
		std::vector<FNodeDesc> Table;
		Section(Table, "상수·파라미터", {
			{ "Constant", {}, 1,
			  [](FCompiler&, const FMaterialGraphNode& Node, const std::vector<FValue>&, std::vector<FValue>& Out) {
				  const uint32 Width = std::clamp(Node.ValueWidth, 1u, 4u);
				  Out.push_back({ FormatConstant(Node.Value, Width), Width });
				  return true;
			  }, MaterialNodeSetting_Value },
			{ "ScalarParameter", {}, 1,
			  [](FCompiler& C, const FMaterialGraphNode& Node, const std::vector<FValue>&, std::vector<FValue>& Out) {
				  const FMaterialParameter* Parameter = C.FindParameter(Node, EMaterialParameterType::Scalar);
				  if (Parameter == nullptr)
				  {
					  return false;
				  }
				  Out.push_back({ C.ParameterToken(*Parameter), 1 });
				  return true;
			  }, MaterialNodeSetting_Parameter, EMaterialParameterType::Scalar },
			{ "VectorParameter", {}, 6,
			  [](FCompiler& C, const FMaterialGraphNode& Node, const std::vector<FValue>&, std::vector<FValue>& Out) {
				  const FMaterialParameter* Parameter = C.FindParameter(Node, EMaterialParameterType::Vector);
				  if (Parameter == nullptr)
				  {
					  return false;
				  }
				  const FValue Value{ C.ParameterToken(*Parameter), 4 };
				  PushColorOutputs(Value, Out); // 0 = RGBA, 1 = RGB, 2~5 = 성분
				  return true;
			  }, MaterialNodeSetting_Parameter, EMaterialParameterType::Vector },
			{ "StaticSwitch", { Pin("True"), Pin("False") }, 1, nullptr, MaterialNodeSetting_Parameter, EMaterialParameterType::StaticSwitch }, // Evaluate가 직접 처리 (고른 쪽만)
		});
		Section(Table, "텍스처·입력", {
			{ "TextureSample", { { "UV", EPinDefault::TexCoord } }, 6,
			  [](FCompiler& C, const FMaterialGraphNode& Node, const std::vector<FValue>& In, std::vector<FValue>& Out) {
				  const FMaterialParameter* Parameter = C.FindParameter(Node, EMaterialParameterType::Texture);
				  std::string               Uv;
				  if (Parameter == nullptr || !C.Convert(&Node, "UV", In[0], 2, true, Uv))
				  {
					  return false;
				  }
				  const char* Sampler = "E_MATERIAL_SAMPLER_WRAP";
				  if (Node.Option == "Clamp")
				  {
					  Sampler = "E_MATERIAL_SAMPLER_CLAMP";
				  }
				  else if (!Node.Option.empty() && Node.Option != "Wrap")
				  {
					  C.Error(&Node, std::format("알 수 없는 Sampler {} (Wrap|Clamp)", Node.Option));
					  return false;
				  }
				  std::string Sample = std::format("MATERIAL_SAMPLE({}, {}, {})", C.ParameterToken(*Parameter), Sampler, Uv);
				  if (Parameter->Usage == ETextureUsage::Normal)
				  {
					  Sample = std::format("MaterialDecodeNormal({})", Sample); // 탄젠트 노멀 (XY → Z 재구성), A = 1
				  }
				  PushColorOutputs(C.Local(Node.Type, 4, Sample), Out);
				  return true;
			  }, MaterialNodeSetting_Parameter | MaterialNodeSetting_Sampler, EMaterialParameterType::Texture },
			{ "TexCoord", {}, 1,
			  [](FCompiler& C, const FMaterialGraphNode& Node, const std::vector<FValue>&, std::vector<FValue>& Out) {
				  if (Node.Index != 0)
				  {
					  C.Error(&Node, std::format("UV 채널 {}은 없습니다 (정점 형식의 UV는 채널 0 하나)", Node.Index));
					  return false;
				  }
				  if (Node.Value.X == 1.0f && Node.Value.Y == 1.0f)
				  {
					  Out.push_back({ "In.UV0", 2 });
				  }
				  else
				  {
					  Out.push_back(C.Local(Node.Type, 2, std::format("In.UV0 * {}", FormatConstant(Node.Value, 2))));
				  }
				  return true;
			  }, MaterialNodeSetting_Tiling },
			{ "VertexColor", {}, 6,
			  [](FCompiler&, const FMaterialGraphNode&, const std::vector<FValue>&, std::vector<FValue>& Out) {
				  PushColorOutputs({ "In.VertexColor", 4 }, Out);
				  return true;
			  } },
			{ "WorldPosition", {}, 1,
			  [](FCompiler&, const FMaterialGraphNode&, const std::vector<FValue>&, std::vector<FValue>& Out) {
				  Out.push_back({ "In.WorldPosition", 3 });
				  return true;
			  } },
			{ "WorldNormal", {}, 1,
			  [](FCompiler&, const FMaterialGraphNode&, const std::vector<FValue>&, std::vector<FValue>& Out) {
				  Out.push_back({ "In.WorldNormal", 3 });
				  return true;
			  } },
			{ "CameraVector", {}, 1,
			  [](FCompiler&, const FMaterialGraphNode&, const std::vector<FValue>&, std::vector<FValue>& Out) {
				  Out.push_back({ "In.CameraVector", 3 });
				  return true;
			  } },
			{ "Time", {}, 1,
			  [](FCompiler&, const FMaterialGraphNode&, const std::vector<FValue>&, std::vector<FValue>& Out) {
				  Out.push_back({ "E_MATERIAL_TIME", 1 });
				  return true;
			  } },
		});
		Section(Table, "산술", {
			{ "Add",{ Pin("A"), Pin("B") }, 1, Binary("+") },
			{ "Subtract", { Pin("A"), Pin("B") }, 1, Binary("-") },
			{ "Multiply", { Pin("A"), Pin("B") }, 1, Binary("*") },
			{ "Divide", { Pin("A"), Pin("B") }, 1, Binary("/") },
			{ "Min", { Pin("A"), Pin("B") }, 1, BinaryCall("min({}, {})") },
			{ "Max", { Pin("A"), Pin("B") }, 1, BinaryCall("max({}, {})") },
			{ "Power", { Pin("A"), Pin("B") }, 1, BinaryCall("pow(max(abs({}), 0.000001f), {})") }, // 음수/0 밑 NaN 방지 (UE ClampedPow)
			{ "Saturate", { Pin("A") }, 1, Unary("saturate({})") },
			{ "Abs", { Pin("A") }, 1, Unary("abs({})") },
			{ "Frac", { Pin("A") }, 1, Unary("frac({})") },
			{ "Floor", { Pin("A") }, 1, Unary("floor({})") },
			{ "OneMinus", { Pin("A") }, 1, Unary("1.0f - {}") },
			{ "Sine", { Pin("A") }, 1, Unary("sin({})") },
			{ "Cosine", { Pin("A") }, 1, Unary("cos({})") },
			{ "Normalize", { Pin("A") }, 1, Unary("normalize({})") },
			{ "Length", { Pin("A") }, 1,
			  [](FCompiler& C, const FMaterialGraphNode& Node, const std::vector<FValue>& In, std::vector<FValue>& Out) {
				  Out.push_back(C.Local(Node.Type, 1, std::format("length({})", In[0].Code)));
				  return true;
			  } },
			{ "Clamp", { Pin("A"), PinDefault("Min", 0.0f), PinDefault("Max", 1.0f) }, 1,
			  [](FCompiler& C, const FMaterialGraphNode& Node, const std::vector<FValue>& In, std::vector<FValue>& Out) {
				  std::string Min;
				  std::string Max;
				  if (!C.Convert(&Node, "Min", In[1], In[0].Width, false, Min) || !C.Convert(&Node, "Max", In[2], In[0].Width, false, Max))
				  {
					  return false;
				  }
				  Out.push_back(C.Local(Node.Type, In[0].Width, std::format("clamp({}, {}, {})", In[0].Code, Min, Max)));
				  return true;
			  } },
			{ "Lerp", { Pin("A"), Pin("B"), Pin("Alpha") }, 1,
			  [](FCompiler& C, const FMaterialGraphNode& Node, const std::vector<FValue>& In, std::vector<FValue>& Out) {
				  uint32      Width = 0;
				  std::string A;
				  std::string B;
				  std::string Alpha;
				  if (!C.CommonWidth(Node, In[0], In[1], Width) || !C.Convert(&Node, "A", In[0], Width, false, A) ||
				      !C.Convert(&Node, "B", In[1], Width, false, B) || !C.Convert(&Node, "Alpha", In[2], Width, false, Alpha))
				  {
					  return false;
				  }
				  Out.push_back(C.Local(Node.Type, Width, std::format("lerp({}, {}, {})", A, B, Alpha)));
				  return true;
			  } },
		});
		Section(Table, "벡터", {
			{ "Dot",{ Pin("A"), Pin("B") }, 1,
			  [](FCompiler& C, const FMaterialGraphNode& Node, const std::vector<FValue>& In, std::vector<FValue>& Out) {
				  uint32      Width = 0;
				  std::string A;
				  std::string B;
				  if (!C.CommonWidth(Node, In[0], In[1], Width) || !C.Convert(&Node, "A", In[0], Width, false, A) || !C.Convert(&Node, "B", In[1], Width, false, B))
				  {
					  return false;
				  }
				  Out.push_back(C.Local(Node.Type, 1, std::format("dot({}, {})", A, B)));
				  return true;
			  } },
			{ "Cross", { Pin("A"), Pin("B") }, 1,
			  [](FCompiler& C, const FMaterialGraphNode& Node, const std::vector<FValue>& In, std::vector<FValue>& Out) {
				  std::string A;
				  std::string B;
				  if (!C.Convert(&Node, "A", In[0], 3, false, A) || !C.Convert(&Node, "B", In[1], 3, false, B))
				  {
					  return false;
				  }
				  Out.push_back(C.Local(Node.Type, 3, std::format("cross({}, {})", A, B)));
				  return true;
			  } },
			{ "Append", { Pin("A"), Pin("B") }, 1,
			  [](FCompiler& C, const FMaterialGraphNode& Node, const std::vector<FValue>& In, std::vector<FValue>& Out) {
				  const uint32 Width = In[0].Width + In[1].Width;
				  if (Width > 4)
				  {
					  C.Error(&Node, std::format("Append 결과가 float4보다 큽니다 (float{} + float{})", In[0].Width, In[1].Width));
					  return false;
				  }
				  Out.push_back(C.Local(Node.Type, Width, std::format("{}({}, {})", FloatType(Width), In[0].Code, In[1].Code)));
				  return true;
			  } },
			{ "ComponentMask", { Pin("A") }, 1,
			  [](FCompiler& C, const FMaterialGraphNode& Node, const std::vector<FValue>& In, std::vector<FValue>& Out) {
				  // 채널 문자 rgba/xyzw → xyzw (중복·순서 바꾸기 허용, 입력 너비 밖 채널은 오류)
				  std::string Swizzle;
				  for (const char Channel : Node.Option)
				  {
					  const size_t Found = std::string_view("xyzwrgba").find(Channel);
					  if (Found == std::string_view::npos || (Found % 4) >= In[0].Width)
					  {
						  C.Error(&Node, std::format("Channels {}: float{}에 없는 채널 '{}'", Node.Option, In[0].Width, Channel));
						  return false;
					  }
					  Swizzle.push_back("xyzw"[Found % 4]);
				  }
				  if (Swizzle.empty() || Swizzle.size() > 4)
				  {
					  C.Error(&Node, "Channels는 1~4글자여야 합니다 (예: \"xy\")");
					  return false;
				  }
				  // 입력이 float1이면 HLSL 스칼라 스위즐(.x)도 유효
				  Out.push_back({ std::format("({}).{}", In[0].Code, Swizzle), static_cast<uint32>(Swizzle.size()) });
				  return true;
			  }, MaterialNodeSetting_Channels },
			{ "Split", { Pin("A") }, 4,
			  [](FCompiler&, const FMaterialGraphNode&, const std::vector<FValue>& In, std::vector<FValue>& Out) {
				  for (uint32 Index = 0; Index < 4; ++Index)
				  {
					  // 입력 너비 밖 출력은 너비 0 = 참조하면 "출력 없음" 오류
					  Out.push_back(Index < In[0].Width ? FValue{ std::format("({}).{}", In[0].Code, "xyzw"[Index]), 1 } : FValue{});
				  }
				  return true;
			  } },
		});
		Section(Table, "머티리얼 함수", {
			{ "Fresnel", { PinDefault("Exponent", 5.0f), PinDefault("BaseReflectFraction", 0.04f), { "Normal", EPinDefault::WorldNormal } }, 1,
			  [](FCompiler& C, const FMaterialGraphNode& Node, const std::vector<FValue>& In, std::vector<FValue>& Out) {
				  std::string Exponent;
				  std::string Base;
				  std::string Normal;
				  if (!C.Convert(&Node, "Exponent", In[0], 1, false, Exponent) || !C.Convert(&Node, "BaseReflectFraction", In[1], 1, false, Base) ||
				      !C.Convert(&Node, "Normal", In[2], 3, false, Normal))
				  {
					  return false;
				  }
				  Out.push_back(C.Local(Node.Type, 1, std::format("MaterialFresnel({}, In.CameraVector, {}, {})", Normal, Exponent, Base)));
				  return true;
			  } },
			{ "Panner", { { "UV", EPinDefault::TexCoord }, { "Time", EPinDefault::Time }, Pin("Speed") }, 1,
			  [](FCompiler& C, const FMaterialGraphNode& Node, const std::vector<FValue>& In, std::vector<FValue>& Out) {
				  std::string Uv;
				  std::string Time;
				  std::string Speed;
				  if (!C.Convert(&Node, "UV", In[0], 2, true, Uv) || !C.Convert(&Node, "Time", In[1], 1, false, Time) ||
				      !C.Convert(&Node, "Speed", In[2], 2, false, Speed))
				  {
					  return false;
				  }
				  Out.push_back(C.Local(Node.Type, 2, std::format("{} + {} * {}", Uv, Speed, Time)));
				  return true;
			  } },
			{ "BlendNormals", { Pin("A"), Pin("B") }, 1,
			  [](FCompiler& C, const FMaterialGraphNode& Node, const std::vector<FValue>& In, std::vector<FValue>& Out) {
				  std::string A;
				  std::string B;
				  if (!C.Convert(&Node, "A", In[0], 3, true, A) || !C.Convert(&Node, "B", In[1], 3, true, B))
				  {
					  return false;
				  }
				  Out.push_back(C.Local(Node.Type, 3, std::format("MaterialBlendNormalsRnm({}, {})", A, B)));
				  return true;
			  } },
			{ "Compare", { Pin("A"), Pin("B"), Pin("True"), Pin("False") }, 1,
			  [](FCompiler& C, const FMaterialGraphNode& Node, const std::vector<FValue>& In, std::vector<FValue>& Out) {
				  static const std::pair<const char*, const char*> Ops[] = { { "Greater", ">" }, { "GreaterEqual", ">=" }, { "Less", "<" },
					                                                         { "LessEqual", "<=" }, { "Equal", "==" },      { "NotEqual", "!=" } };
				  const char* Operator = nullptr;
				  for (const auto& [Name, Symbol] : Ops)
				  {
					  Operator = Node.Option == Name ? Symbol : Operator;
				  }
				  if (Operator == nullptr)
				  {
					  C.Error(&Node, std::format("알 수 없는 Op {} (Greater|GreaterEqual|Less|LessEqual|Equal|NotEqual)", Node.Option));
					  return false;
				  }
				  uint32      Width = 0;
				  std::string A;
				  std::string B;
				  std::string True;
				  std::string False;
				  if (!C.Convert(&Node, "A", In[0], 1, false, A) || !C.Convert(&Node, "B", In[1], 1, false, B) || !C.CommonWidth(Node, In[2], In[3], Width) ||
				      !C.Convert(&Node, "True", In[2], Width, false, True) || !C.Convert(&Node, "False", In[3], Width, false, False))
				  {
					  return false;
				  }
				  Out.push_back(C.Local(Node.Type, Width, std::format("({} {} {}) ? {} : {}", A, Operator, B, True, False)));
				  return true;
			  }, MaterialNodeSetting_CompareOp },
		});
		return Table;
	}

	const std::vector<FNodeDesc>& GetNodeTable()
	{
		static const std::vector<FNodeDesc> Table = BuildNodeTable();
		return Table;
	}
} // namespace

FMaterialGraphCompileResult FMaterialGraphCompiler::Compile(const FMaterialGraph& Graph, const std::vector<FMaterialParameter>& Parameters)
{
	FCompiler Compiler(Graph, Parameters);
	return Compiler.Run();
}

FMaterialGraphAnalysis FMaterialGraphCompiler::Analyze(const FMaterialGraph& Graph, const std::vector<FMaterialParameter>& Parameters)
{
	FCompiler Compiler(Graph, Parameters);
	return Compiler.Analyze();
}

const std::vector<FMaterialGraphNodeInfo>& FMaterialGraphCompiler::GetNodeInfos()
{
	static const std::vector<FMaterialGraphNodeInfo> Infos = [] {
		std::vector<FMaterialGraphNodeInfo> Result;
		for (const FNodeDesc& Desc : GetNodeTable())
		{
			FMaterialGraphNodeInfo Info;
			Info.Type          = Desc.Type;
			Info.Category      = Desc.Category;
			Info.Settings      = Desc.Settings;
			Info.ParameterType = Desc.ParameterType;
			for (const FPinDesc& Pin : Desc.Pins)
			{
				Info.Inputs.push_back({ Pin.Name, Pin.Default, Pin.Constant, Pin.ConstantWidth });
			}
			// 출력 이름: 6개 = 색 출력 규약(PushColorOutputs), 4개 = 성분(Split), 그 밖은 번호 (1개면 이름 없음)
			static const char* const ColorNames[]     = { "RGBA", "RGB", "R", "G", "B", "A" };
			static const char* const ComponentNames[] = { "X", "Y", "Z", "W" };
			for (uint32 Output = 0; Output < Desc.OutputCount; ++Output)
			{
				Info.Outputs.push_back(Desc.OutputCount == 1   ? std::string()
				                       : Desc.OutputCount == 6 ? std::string(ColorNames[Output])
				                       : Desc.OutputCount == 4 ? std::string(ComponentNames[Output])
				                                               : std::to_string(Output));
			}
			Result.push_back(std::move(Info));
		}
		return Result;
	}();
	return Infos;
}

const FMaterialGraphNodeInfo* FMaterialGraphCompiler::FindNodeInfo(std::string_view Type)
{
	for (const FMaterialGraphNodeInfo& Info : GetNodeInfos())
	{
		if (Info.Type == Type)
		{
			return &Info;
		}
	}
	return nullptr;
}

std::vector<std::string> FMaterialGraphCompiler::GetNodeTypes()
{
	std::vector<std::string> Types;
	for (const FNodeDesc& Desc : GetNodeTable())
	{
		Types.emplace_back(Desc.Type);
	}
	return Types;
}

uint64 FMaterialGraphCompiler::HashText(std::string_view Text)
{
	uint64 Hash = 14695981039346656037ull; // FNV-1a 64
	for (const char Char : Text)
	{
		Hash ^= static_cast<uint8>(Char);
		Hash *= 1099511628211ull;
	}
	return Hash == 0 ? 1 : Hash;
}
