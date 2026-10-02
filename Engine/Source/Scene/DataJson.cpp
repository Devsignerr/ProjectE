#include "Scene/DataJson.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <format>
#include <limits>

namespace DataJson
{
	namespace
	{
		std::string Preview(const FJson& Json)
		{
			std::string Text = DumpCompact(Json);
			if (Text.size() > 40)
			{
				Text = Text.substr(0, 37) + "...";
			}
			return Text;
		}

		std::string Mismatch(const FDataField& Field, const FJson& Json)
		{
			return std::format("타입이 맞지 않습니다 ({} 기대, 값 {}) → 기본값", ToString(Field.Type), Preview(Json));
		}

		bool ReadNumber(const FJson& Json, float& Out)
		{
			if (!Json.is_number())
			{
				return false;
			}
			const double Value = Json.get<double>();
			if (!std::isfinite(Value))
			{
				return false;
			}
			Out = static_cast<float>(Value);
			return true;
		}

		// 성분 배열 [x, y, ...] (색은 3개면 알파 1)
		bool ReadComponents(const FJson& Json, size_t Count, bool bColor, float (&Out)[4])
		{
			Out[0] = Out[1] = Out[2] = 0.0f;
			Out[3]               = bColor ? 1.0f : 0.0f;
			if (bColor && Json.is_string())
			{
				FDataValue  Parsed;
				FDataField  ColorField;
				ColorField.Type = EDataFieldType::Color;
				if (!DataValueText::FromText(ColorField, Json.get_ref<const std::string&>(), Parsed, nullptr))
				{
					return false;
				}
				const FVector4 Color = Parsed.AsVector4();
				Out[0]               = Color.X;
				Out[1]               = Color.Y;
				Out[2]               = Color.Z;
				Out[3]               = Color.W;
				return true;
			}
			if (!Json.is_array() || !(Json.size() == Count || (bColor && Json.size() == 3)))
			{
				return false;
			}
			for (size_t Index = 0; Index < Json.size(); ++Index)
			{
				if (!ReadNumber(Json[Index], Out[Index]))
				{
					return false;
				}
			}
			return true;
		}
	} // namespace

	FJson Parse(const std::string& Text)
	{
		return FJson::parse(Text, nullptr, false, true);
	}

	std::string DumpCompact(const FJson& Value)
	{
		return Value.dump(-1, ' ', false, FJson::error_handler_t::replace);
	}

	std::string GetString(const FJson& Object, const char* Key)
	{
		if (!Object.is_object())
		{
			return std::string();
		}
		const auto Found = Object.find(Key);
		return Found != Object.end() && Found->is_string() ? Found->get<std::string>() : std::string();
	}

	std::string FormatDocument(const FJson& Root)
	{
		std::string Text = "{\n";
		size_t      Index = 0;
		for (auto It = Root.begin(); It != Root.end(); ++It, ++Index)
		{
			Text += "  " + DumpCompact(FJson(It.key())) + ": ";
			const FJson& Value = It.value();
			if (Value.is_array() && !Value.empty())
			{
				Text += "[\n";
				for (size_t Item = 0; Item < Value.size(); ++Item)
				{
					Text += "    " + DumpCompact(Value[Item]) + (Item + 1 < Value.size() ? ",\n" : "\n");
				}
				Text += "  ]";
			}
			else if (Value.is_object() && !Value.empty())
			{
				Text += "{\n";
				size_t Member = 0;
				for (auto Child = Value.begin(); Child != Value.end(); ++Child, ++Member)
				{
					Text += "    " + DumpCompact(FJson(Child.key())) + ": " + DumpCompact(Child.value()) + (Member + 1 < Value.size() ? ",\n" : "\n");
				}
				Text += "  }";
			}
			else
			{
				Text += DumpCompact(Value);
			}
			Text += Index + 1 < Root.size() ? ",\n" : "\n";
		}
		Text += "}\n";
		return Text;
	}

	std::string FormatFloat(float Value)
	{
		if (!std::isfinite(Value))
		{
			return "0";
		}
		char       Buffer[64];
		const auto Result = std::to_chars(Buffer, Buffer + sizeof(Buffer), Value);
		return std::string(Buffer, Result.ptr);
	}

	FJson FloatToJson(float Value)
	{
		// float → 가장 짧은 10진 표현 → double (0.1f가 0.10000000149011612로 저장되지 않게)
		const std::string Text = FormatFloat(Value);
		double            Number = 0.0;
		std::from_chars(Text.data(), Text.data() + Text.size(), Number);
		if (Number == std::floor(Number) && std::abs(Number) < 1.0e15)
		{
			return FJson(static_cast<int64>(Number)); // 정수 값은 "10.0" 대신 "10"
		}
		return FJson(Number);
	}

	FJson ValueToJson(const FDataField& Field, const FDataValue& Value)
	{
		switch (Field.Type)
		{
		case EDataFieldType::Bool: return FJson(Value.AsBool());
		case EDataFieldType::Int: return FJson(Value.AsInt());
		case EDataFieldType::Float: return FloatToJson(Value.AsFloat());
		case EDataFieldType::Vector2:
		{
			const FVector2 V = Value.AsVector2();
			return FJson::array({ FloatToJson(V.X), FloatToJson(V.Y) });
		}
		case EDataFieldType::Vector3:
		{
			const FVector3 V = Value.AsVector3();
			return FJson::array({ FloatToJson(V.X), FloatToJson(V.Y), FloatToJson(V.Z) });
		}
		case EDataFieldType::Vector4:
		case EDataFieldType::Color:
		{
			const FVector4 V = Value.AsVector4();
			return FJson::array({ FloatToJson(V.X), FloatToJson(V.Y), FloatToJson(V.Z), FloatToJson(V.W) });
		}
		case EDataFieldType::Array:
		{
			const FDataField Element = Field.MakeElementField();
			FJson            List    = FJson::array();
			for (const FDataValue& Item : Value.AsArray())
			{
				List.push_back(ValueToJson(Element, Item));
			}
			return List;
		}
		default: return FJson(Value.AsString());
		}
	}

	bool ValueFromJson(const FDataField& Field, const FJson& Json, FDataValue& Out, std::string& Problem)
	{
		Problem.clear();
		switch (Field.Type)
		{
		case EDataFieldType::Bool:
			if (Json.is_boolean())
			{
				Out = FDataValue::MakeBool(Json.get<bool>());
				return true;
			}
			break;
		case EDataFieldType::Int:
			if (Json.is_number_integer())
			{
				const int64 Value = Json.is_number_unsigned() && Json.get<uint64>() > static_cast<uint64>(std::numeric_limits<int64>::max())
				                        ? std::numeric_limits<int64>::max()
				                        : Json.get<int64>();
				if (Value < std::numeric_limits<int32>::min() || Value > std::numeric_limits<int32>::max())
				{
					Problem = std::format("int32 범위를 벗어났습니다 ({}) → 기본값", Preview(Json));
					return false;
				}
				Out = FDataValue::MakeInt(static_cast<int32>(Value));
				return true;
			}
			if (Json.is_number_float())
			{
				const double Value = Json.get<double>();
				if (std::isfinite(Value) && Value == std::floor(Value) && Value >= std::numeric_limits<int32>::min() && Value <= std::numeric_limits<int32>::max())
				{
					Out = FDataValue::MakeInt(static_cast<int32>(Value));
					return true;
				}
			}
			break;
		case EDataFieldType::Float:
		{
			float Value = 0.0f;
			if (ReadNumber(Json, Value))
			{
				Out = FDataValue::MakeFloat(Value);
				return true;
			}
			break;
		}
		case EDataFieldType::Vector2:
		case EDataFieldType::Vector3:
		case EDataFieldType::Vector4:
		case EDataFieldType::Color:
		{
			const size_t Count = Field.Type == EDataFieldType::Vector2 ? 2 : Field.Type == EDataFieldType::Vector3 ? 3 : 4;
			float        C[4];
			if (ReadComponents(Json, Count, Field.Type == EDataFieldType::Color, C))
			{
				Out = Field.Type == EDataFieldType::Vector2   ? FDataValue::MakeVector2(FVector2(C[0], C[1]))
				      : Field.Type == EDataFieldType::Vector3 ? FDataValue::MakeVector3(FVector3(C[0], C[1], C[2]))
				                                              : FDataValue::MakeVector4(FVector4(C[0], C[1], C[2], C[3]));
				return true;
			}
			break;
		}
		case EDataFieldType::Enum:
			if (Json.is_string())
			{
				FDataValue Value = FDataValue::MakeString(Json.get<std::string>());
				if (Field.Accepts(Value))
				{
					Out = std::move(Value);
					return true;
				}
				Problem = std::format("Enum 목록에 없는 값 {} → 기본값", Preview(Json));
				return false;
			}
			break;
		case EDataFieldType::Array:
			if (Json.is_array())
			{
				const FDataField   Element = Field.MakeElementField();
				FDataValue::FArray Elements;
				Elements.reserve(Json.size());
				for (size_t Index = 0; Index < Json.size(); ++Index)
				{
					FDataValue  Item;
					std::string ItemProblem;
					if (!ValueFromJson(Element, Json[Index], Item, ItemProblem))
					{
						Item = Element.Default;
						if (Problem.empty())
						{
							Problem = std::format("배열 요소 [{}]: {}", Index, ItemProblem);
						}
					}
					Elements.push_back(std::move(Item));
				}
				Out = FDataValue::MakeArray(std::move(Elements));
				return true;
			}
			break;
		default: // String/Text/Asset/RowRef
			if (Json.is_string())
			{
				Out = FDataValue::MakeString(Json.get<std::string>());
				return true;
			}
			break;
		}
		Problem = Mismatch(Field, Json);
		return false;
	}
} // namespace DataJson
