// 능력 정의 데이터: 계산 식(AbilityMath)과 데이터 테이블 → FAbilitySet 변환.
//
// 데이터 테이블 필드 (Phase 46 .estruct — 필드 이름으로 읽는다. 없는 필드는 아래 기본값, 타입이 다르면 경고 후 기본값)
//   속성 표   (행 이름 = 속성 이름): Base Float, Min Float(-무한), Max Float(+무한), MaxAttribute String, Meta Bool
//   효과 표   (행 이름 = 효과 이름): DurationPolicy Enum(Instant/HasDuration/Infinite), Duration Float, Period Float,
//             ExecutePeriodicOnApply Bool, Modifiers Array(RowRef → 수정자 표 | String 인라인 "속성 연산 크기"),
//             Stacking Enum(None/ByTarget/BySource), StackLimit Int(1), RefreshDurationOnStack Bool(true),
//             StackExpiration Enum(ClearAll/RemoveOneAndRefresh), AssetTags/GrantedTags/ApplicationRequiredTags/ApplicationBlockedTags/
//             RemoveEffectsWithTags/GrantedImmunityTags (String 배열 또는 ";" 구분 String)
//   수정자 표 (효과 Modifiers의 RowRef 대상): Attribute String, Op Enum(Add/Multiply/Override), MagnitudeType Enum(Constant/Curve/
//             AttributeBased/SetByCaller), Value Float, Curve Array(Vector2: X 레벨, Y 값), SourceAttribute String, FromSource Bool(true),
//             PostAdd Float, SetByCallerName String
//   능력 표   (행 이름 = 능력 이름): Script Asset(.lua), Native String, CostEffect/CooldownEffect String(또는 효과 표 RowRef),
//             ActivationEffects Array, InputAction String, AbilityTags/ActivationRequiredTags/ActivationBlockedTags/ActivationOwnedTags/
//             CancelAbilitiesWithTags/BlockAbilitiesWithTags (태그 목록), Predicted Bool(true), ManualCommit Bool(false)
// 이 형식을 고른 이유: Phase 46 표는 중첩 구조체가 없지만 RowRef 배열로 효과 → 수정자 목록을 표현할 수 있고, 편집기·CSV·핫 리로드·참조 검증을
// 그대로 쓴다. 곡선은 Vector2 배열. 전용 .eeffect 형식은 만들지 않았다.
#include "Scene/Ability/AbilityTypes.h"

#include "Scene/Ability/GameplayTags.h"
#include "Scene/DataTable.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

E_DEFINE_LOG_CATEGORY(LogAbility, Log)

namespace
{
	template <typename TEnum, size_t N>
	bool ParseByTable(std::string_view Text, const char* const (&Names)[N], TEnum& Out)
	{
		for (size_t Index = 0; Index < N; ++Index)
		{
			if (Text == Names[Index])
			{
				Out = static_cast<TEnum>(Index);
				return true;
			}
		}
		return false;
	}

	constexpr const char* OpNames[]         = { "Add", "Multiply", "Override" };
	constexpr const char* MagnitudeNames[]  = { "Constant", "Curve", "AttributeBased", "SetByCaller" };
	constexpr const char* DurationNames[]   = { "Instant", "HasDuration", "Infinite" };
	constexpr const char* StackingNames[]   = { "None", "ByTarget", "BySource" };
	constexpr const char* ExpirationNames[] = { "ClearAll", "RemoveOneAndRefresh" };

	// 표 한 행 읽기 도우미 (필드가 없으면 기본값, 타입이 다르면 경고)
	struct FRowReader
	{
		const FDataTable&         Table;
		const std::string&        Row;
		std::vector<std::string>* Warnings = nullptr;
		std::string               Context; // "효과 'Burn'"

		const FDataField* Field(std::string_view Name) const { return Table.Struct ? Table.Struct->GetField(Name) : nullptr; }

		void Warn(std::string Message) const
		{
			if (Warnings != nullptr)
			{
				Warnings->push_back(std::format("{}: {}", Context, std::move(Message)));
			}
		}

		float Float(std::string_view Name, float Default) const
		{
			const FDataField* Def = Field(Name);
			if (Def == nullptr)
			{
				return Default;
			}
			if (Def->Type != EDataFieldType::Float && Def->Type != EDataFieldType::Int)
			{
				Warn(std::format("필드 {}는 Float여야 합니다", Name));
				return Default;
			}
			return Table.GetFloat(Row, Name, Default);
		}
		int32 Int(std::string_view Name, int32 Default) const
		{
			const FDataField* Def = Field(Name);
			if (Def == nullptr)
			{
				return Default;
			}
			if (Def->Type == EDataFieldType::Float)
			{
				return static_cast<int32>(std::lround(Table.GetFloat(Row, Name, static_cast<float>(Default))));
			}
			return Def->Type == EDataFieldType::Int ? Table.GetInt(Row, Name, Default) : Default;
		}
		bool Bool(std::string_view Name, bool Default) const
		{
			const FDataField* Def = Field(Name);
			return Def != nullptr && Def->Type == EDataFieldType::Bool ? Table.GetBool(Row, Name, Default) : Default;
		}
		std::string String(std::string_view Name) const
		{
			const FDataField* Def = Field(Name);
			return Def != nullptr && IsStringDataType(Def->Type) ? Table.GetString(Row, Name) : std::string();
		}
		// String 배열 또는 ";" 구분 문자열
		std::vector<std::string> Strings(std::string_view Name) const
		{
			std::vector<std::string> Result;
			const FDataField*        Def   = Field(Name);
			const FDataValue*        Value = Def != nullptr ? Table.FindValue(Row, Name) : nullptr;
			if (Value == nullptr)
			{
				return Result;
			}
			if (Def->Type == EDataFieldType::Array)
			{
				for (const FDataValue& Element : Value->AsArray())
				{
					if (!Element.AsString().empty())
					{
						Result.push_back(Element.AsString());
					}
				}
			}
			else if (IsStringDataType(Def->Type))
			{
				Result = FGameplayTags::ParseList(Value->AsString());
			}
			return Result;
		}
		std::vector<std::string> Tags(std::string_view Name) const
		{
			std::vector<std::string> Result;
			for (std::string& Tag : Strings(Name))
			{
				if (!FGameplayTags::IsValid(Tag))
				{
					Warn(std::format("{}: 잘못된 태그 이름 '{}'", Name, Tag));
					continue;
				}
				FGameplayTagRegistry::Get().Validate(Tag, Context);
				Result.push_back(std::move(Tag));
			}
			return Result;
		}
		template <typename TEnum, size_t N>
		TEnum Enum(std::string_view Name, const char* const (&Names)[N], TEnum Default) const
		{
			const std::string Text = String(Name);
			if (Text.empty())
			{
				return Default;
			}
			TEnum Result = Default;
			if (!ParseByTable(Text, Names, Result))
			{
				Warn(std::format("{}: 알 수 없는 값 '{}'", Name, Text));
			}
			return Result;
		}
	};

	FModifierDef ReadModifierRow(const FDataTable& Table, const std::string& Row, std::vector<std::string>* Warnings)
	{
		const FRowReader Reader{ Table, Row, Warnings, std::format("수정자 '{}'", Row) };
		FModifierDef     Modifier;
		Modifier.Attribute           = Reader.String("Attribute");
		Modifier.Op                  = Reader.Enum("Op", OpNames, EModifierOp::Add);
		Modifier.Magnitude.Type      = Reader.Enum("MagnitudeType", MagnitudeNames, EMagnitudeType::Constant);
		Modifier.Magnitude.Value     = Reader.Float("Value", 0.0f);
		Modifier.Magnitude.Attribute = Reader.String("SourceAttribute");
		Modifier.Magnitude.bFromSource     = Reader.Bool("FromSource", true);
		Modifier.Magnitude.PostAdd         = Reader.Float("PostAdd", 0.0f);
		Modifier.Magnitude.SetByCallerName = Reader.String("SetByCallerName");
		if (const FDataField* CurveField = Reader.Field("Curve"); CurveField != nullptr && CurveField->Type == EDataFieldType::Array)
		{
			if (const FDataValue* Curve = Table.FindValue(Row, "Curve"))
			{
				for (const FDataValue& Point : Curve->AsArray())
				{
					Modifier.Magnitude.Curve.push_back(Point.AsVector2());
				}
				std::sort(Modifier.Magnitude.Curve.begin(), Modifier.Magnitude.Curve.end(), [](const FVector2& A, const FVector2& B) { return A.X < B.X; });
			}
		}
		if (Modifier.Attribute.empty())
		{
			Reader.Warn("Attribute가 비어 있습니다");
		}
		return Modifier;
	}
} // namespace

// ---------------------------------------------------------------- FAbilitySet

const FAttributeDef* FAbilitySet::FindAttribute(std::string_view Name) const
{
	const auto Found = std::find_if(Attributes.begin(), Attributes.end(), [&](const FAttributeDef& Def) { return Def.Name == Name; });
	return Found != Attributes.end() ? &*Found : nullptr;
}

const FGameplayEffectDef* FAbilitySet::FindEffect(std::string_view Name) const
{
	const auto Found = Effects.find(std::string(Name));
	return Found != Effects.end() ? &Found->second : nullptr;
}

const FAbilityDef* FAbilitySet::FindAbility(std::string_view Name) const
{
	const auto Found = Abilities.find(std::string(Name));
	return Found != Abilities.end() ? &Found->second : nullptr;
}

void FAbilitySet::AddAttribute(FAttributeDef Def)
{
	if (Def.Name == AbilityAttributes::IncomingDamage || Def.Name == AbilityAttributes::IncomingHeal)
	{
		Def.bMeta = true;
	}
	if (Def.bMeta)
	{
		Def.Base = 0.0f;
	}
	const auto Found = std::find_if(Attributes.begin(), Attributes.end(), [&](const FAttributeDef& Item) { return Item.Name == Def.Name; });
	if (Found != Attributes.end())
	{
		*Found = std::move(Def);
		return;
	}
	Attributes.push_back(std::move(Def));
}

void FAbilitySet::AddEffect(FGameplayEffectDef Def)
{
	Def.StackLimit = std::max(1, Def.StackLimit);
	const std::string Name = Def.Name;
	Effects[Name]          = std::move(Def);
}

void FAbilitySet::AddAbility(FAbilityDef Def)
{
	if (!Abilities.contains(Def.Name))
	{
		AbilityOrder.push_back(Def.Name);
	}
	const std::string Name = Def.Name;
	Abilities[Name]        = std::move(Def);
}

std::shared_ptr<FAbilitySet> FAbilitySet::FromTables(const FDataTable* AttributeTable, const FDataTable* EffectTable, const FDataTable* AbilityTable,
                                                     const FTableLoader& LoadTable, std::vector<std::string>* OutWarnings)
{
	auto Set = std::make_shared<FAbilitySet>();

	if (AttributeTable != nullptr)
	{
		for (const FDataRow& Row : AttributeTable->GetRows())
		{
			const FRowReader Reader{ *AttributeTable, Row.Name, OutWarnings, std::format("속성 '{}'", Row.Name) };
			FAttributeDef    Def;
			Def.Name         = Row.Name;
			Def.Base         = Reader.Float("Base", 0.0f);
			Def.Min          = Reader.Float("Min", -1.0e30f);
			Def.Max          = Reader.Float("Max", 1.0e30f);
			Def.MaxAttribute = Reader.String("MaxAttribute");
			Def.bMeta        = Reader.Bool("Meta", false);
			Set->AddAttribute(std::move(Def));
		}
	}

	if (EffectTable != nullptr)
	{
		for (const FDataRow& Row : EffectTable->GetRows())
		{
			const FRowReader   Reader{ *EffectTable, Row.Name, OutWarnings, std::format("효과 '{}'", Row.Name) };
			FGameplayEffectDef Def;
			Def.Name                    = Row.Name;
			Def.DurationPolicy          = Reader.Enum("DurationPolicy", DurationNames, EEffectDurationPolicy::Instant);
			Def.Duration                = std::max(0.0f, Reader.Float("Duration", 0.0f));
			Def.Period                  = std::max(0.0f, Reader.Float("Period", 0.0f));
			Def.bExecutePeriodicOnApply = Reader.Bool("ExecutePeriodicOnApply", false);
			Def.Stacking                = Reader.Enum("Stacking", StackingNames, EEffectStacking::None);
			Def.StackLimit              = std::max(1, Reader.Int("StackLimit", 1));
			Def.bRefreshDurationOnStack = Reader.Bool("RefreshDurationOnStack", true);
			Def.StackExpiration         = Reader.Enum("StackExpiration", ExpirationNames, EStackExpiration::ClearAll);
			Def.AssetTags               = Reader.Tags("AssetTags");
			Def.GrantedTags             = Reader.Tags("GrantedTags");
			Def.ApplicationRequiredTags = Reader.Tags("ApplicationRequiredTags");
			Def.ApplicationBlockedTags  = Reader.Tags("ApplicationBlockedTags");
			Def.RemoveEffectsWithTags   = Reader.Tags("RemoveEffectsWithTags");
			Def.GrantedImmunityTags     = Reader.Tags("GrantedImmunityTags");

			if (const FDataField* ModifierField = Reader.Field("Modifiers"); ModifierField != nullptr && ModifierField->Type == EDataFieldType::Array)
			{
				const FDataValue* Value = EffectTable->FindValue(Row.Name, "Modifiers");
				if (ModifierField->ElementType == EDataFieldType::RowRef)
				{
					const std::shared_ptr<const FDataTable> ModifierTable = LoadTable && !ModifierField->Table.empty() ? LoadTable(ModifierField->Table) : nullptr;
					if (ModifierTable == nullptr)
					{
						Reader.Warn(std::format("수정자 표 '{}'를 읽지 못했습니다", ModifierField->Table));
					}
					for (const FDataValue& Element : Value != nullptr && ModifierTable != nullptr ? Value->AsArray() : FDataValue::FArray())
					{
						if (Element.AsString().empty())
						{
							continue;
						}
						if (!ModifierTable->HasRow(Element.AsString()))
						{
							Reader.Warn(std::format("수정자 표에 없는 행 '{}'", Element.AsString()));
							continue;
						}
						Def.Modifiers.push_back(ReadModifierRow(*ModifierTable, Element.AsString(), OutWarnings));
					}
				}
				else
				{
					for (const FDataValue& Element : Value != nullptr ? Value->AsArray() : FDataValue::FArray())
					{
						FModifierDef Modifier;
						if (Element.AsString().empty())
						{
							continue;
						}
						if (AbilityMath::ParseInlineModifier(Element.AsString(), Modifier))
						{
							Def.Modifiers.push_back(std::move(Modifier));
						}
						else
						{
							Reader.Warn(std::format("수정자 글자 '{}'를 읽지 못했습니다 (\"속성 연산 크기\")", Element.AsString()));
						}
					}
				}
			}
			if (Def.DurationPolicy == EEffectDurationPolicy::HasDuration && Def.Duration <= 0.0f)
			{
				Reader.Warn("HasDuration인데 Duration이 0입니다");
			}
			Set->AddEffect(std::move(Def));
		}
	}

	if (AbilityTable != nullptr)
	{
		for (const FDataRow& Row : AbilityTable->GetRows())
		{
			const FRowReader Reader{ *AbilityTable, Row.Name, OutWarnings, std::format("능력 '{}'", Row.Name) };
			FAbilityDef      Def;
			Def.Name                    = Row.Name;
			Def.Script                  = Reader.String("Script");
			Def.Native                  = Reader.String("Native");
			Def.CostEffect              = Reader.String("CostEffect");
			Def.CooldownEffect          = Reader.String("CooldownEffect");
			Def.ActivationEffects       = Reader.Strings("ActivationEffects");
			Def.InputAction             = Reader.String("InputAction");
			Def.AbilityTags             = Reader.Tags("AbilityTags");
			Def.ActivationRequiredTags  = Reader.Tags("ActivationRequiredTags");
			Def.ActivationBlockedTags   = Reader.Tags("ActivationBlockedTags");
			Def.ActivationOwnedTags     = Reader.Tags("ActivationOwnedTags");
			Def.CancelAbilitiesWithTags = Reader.Tags("CancelAbilitiesWithTags");
			Def.BlockAbilitiesWithTags  = Reader.Tags("BlockAbilitiesWithTags");
			Def.bPredicted              = Reader.Bool("Predicted", true);
			Def.bManualCommit           = Reader.Bool("ManualCommit", false);
			Set->AddAbility(std::move(Def));
		}
	}

	// 참조 검사 (경고만 — 데이터는 그대로 쓴다)
	if (OutWarnings != nullptr)
	{
		for (const auto& [Name, Ability] : Set->Abilities)
		{
			for (const std::string* Effect : { &Ability.CostEffect, &Ability.CooldownEffect })
			{
				if (!Effect->empty() && Set->FindEffect(*Effect) == nullptr)
				{
					OutWarnings->push_back(std::format("능력 '{}': 없는 효과 '{}'", Name, *Effect));
				}
			}
			for (const std::string& Effect : Ability.ActivationEffects)
			{
				if (Set->FindEffect(Effect) == nullptr)
				{
					OutWarnings->push_back(std::format("능력 '{}': 없는 효과 '{}'", Name, Effect));
				}
			}
		}
		if (!Set->Attributes.empty())
		{
			for (const auto& [Name, Effect] : Set->Effects)
			{
				for (const FModifierDef& Modifier : Effect.Modifiers)
				{
					if (!Modifier.Attribute.empty() && Set->FindAttribute(Modifier.Attribute) == nullptr)
					{
						OutWarnings->push_back(std::format("효과 '{}': 속성 표에 없는 속성 '{}'", Name, Modifier.Attribute));
					}
				}
			}
		}
	}
	return Set;
}

// ---------------------------------------------------------------- AbilityMath

float AbilityMath::Aggregate(float Base, const std::vector<FAppliedModifier>& Modifiers)
{
	const FAppliedModifier* Override = nullptr;
	float                   Add      = 0.0f;
	float                   Multiply = 1.0f;
	for (const FAppliedModifier& Modifier : Modifiers)
	{
		const int32 Stacks = std::max(1, Modifier.Stacks);
		switch (Modifier.Op)
		{
		case EModifierOp::Add:      Add += Modifier.Value * static_cast<float>(Stacks); break;
		case EModifierOp::Multiply: Multiply *= std::pow(Modifier.Value, static_cast<float>(Stacks)); break;
		case EModifierOp::Override:
			if (Override == nullptr || Modifier.Order >= Override->Order)
			{
				Override = &Modifier;
			}
			break;
		}
	}
	return Override != nullptr ? Override->Value : (Base + Add) * Multiply;
}

float AbilityMath::EvaluateCurve(const std::vector<FVector2>& Points, float X)
{
	if (Points.empty())
	{
		return 0.0f;
	}
	if (X <= Points.front().X)
	{
		return Points.front().Y;
	}
	for (size_t Index = 1; Index < Points.size(); ++Index)
	{
		const FVector2& A = Points[Index - 1];
		const FVector2& B = Points[Index];
		if (X <= B.X)
		{
			const float Span = B.X - A.X;
			return Span > 1.0e-6f ? A.Y + (B.Y - A.Y) * (X - A.X) / Span : B.Y;
		}
	}
	return Points.back().Y;
}

float AbilityMath::EvaluateMagnitude(const FMagnitudeDef& Magnitude, float Level, const FAttributeReader& Source, const FAttributeReader& Target,
                                     const std::map<std::string, float>& SetByCaller, bool* bOutMissing)
{
	switch (Magnitude.Type)
	{
	case EMagnitudeType::Curve: return EvaluateCurve(Magnitude.Curve, Level);
	case EMagnitudeType::AttributeBased:
	{
		float                    Value  = 0.0f;
		const FAttributeReader&  Reader = Magnitude.bFromSource ? Source : Target;
		if (!Reader || !Reader(Magnitude.Attribute, Value))
		{
			if (bOutMissing != nullptr)
			{
				*bOutMissing = true;
			}
			Value = 0.0f;
		}
		return Magnitude.Value * Value + Magnitude.PostAdd;
	}
	case EMagnitudeType::SetByCaller:
	{
		const auto Found = SetByCaller.find(Magnitude.SetByCallerName);
		if (Found == SetByCaller.end())
		{
			if (bOutMissing != nullptr)
			{
				*bOutMissing = true;
			}
			return 0.0f;
		}
		return Found->second;
	}
	default: return Magnitude.Value;
	}
}

const char* AbilityMath::ToString(EModifierOp Op) { return OpNames[static_cast<size_t>(Op)]; }
const char* AbilityMath::ToString(EMagnitudeType Type) { return MagnitudeNames[static_cast<size_t>(Type)]; }
const char* AbilityMath::ToString(EEffectDurationPolicy Policy) { return DurationNames[static_cast<size_t>(Policy)]; }
const char* AbilityMath::ToString(EEffectStacking Stacking) { return StackingNames[static_cast<size_t>(Stacking)]; }
bool        AbilityMath::Parse(std::string_view Text, EModifierOp& Out) { return ParseByTable(Text, OpNames, Out); }
bool        AbilityMath::Parse(std::string_view Text, EMagnitudeType& Out) { return ParseByTable(Text, MagnitudeNames, Out); }
bool        AbilityMath::Parse(std::string_view Text, EEffectDurationPolicy& Out) { return ParseByTable(Text, DurationNames, Out); }
bool        AbilityMath::Parse(std::string_view Text, EEffectStacking& Out) { return ParseByTable(Text, StackingNames, Out); }
bool        AbilityMath::Parse(std::string_view Text, EStackExpiration& Out) { return ParseByTable(Text, ExpirationNames, Out); }

bool AbilityMath::ParseInlineModifier(std::string_view Text, FModifierDef& Out)
{
	std::vector<std::string_view> Parts;
	size_t                        Start = 0;
	for (size_t Index = 0; Index <= Text.size(); ++Index)
	{
		if (Index == Text.size() || Text[Index] == ' ' || Text[Index] == '\t')
		{
			if (Index > Start)
			{
				Parts.push_back(Text.substr(Start, Index - Start));
			}
			Start = Index + 1;
		}
	}
	if (Parts.size() != 3 || !Parse(Parts[1], Out.Op))
	{
		return false;
	}
	Out.Attribute = std::string(Parts[0]);
	constexpr std::string_view SetByCallerPrefix = "SetByCaller:";
	if (Parts[2].starts_with(SetByCallerPrefix))
	{
		Out.Magnitude.Type            = EMagnitudeType::SetByCaller;
		Out.Magnitude.SetByCallerName = std::string(Parts[2].substr(SetByCallerPrefix.size()));
		return !Out.Magnitude.SetByCallerName.empty();
	}
	float      Value  = 0.0f;
	const auto Result = std::from_chars(Parts[2].data(), Parts[2].data() + Parts[2].size(), Value);
	if (Result.ec != std::errc() || Result.ptr != Parts[2].data() + Parts[2].size())
	{
		return false;
	}
	Out.Magnitude.Type  = EMagnitudeType::Constant;
	Out.Magnitude.Value = Value;
	return FGameplayTags::IsValid(Out.Attribute); // 속성 이름도 점/공백 규칙이 같다
}
