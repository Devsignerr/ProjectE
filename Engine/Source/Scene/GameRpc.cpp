#include "Scene/GameRpc.h"

const char* GetRpcMethodPrefix(EGameRpcKind Kind)
{
	switch (Kind)
	{
	case EGameRpcKind::Client:    return "Client_";
	case EGameRpcKind::Multicast: return "Multicast_";
	default:                      return "Server_";
	}
}

FGameRpcValue FGameRpcValue::MakeBool(bool bValue)
{
	FGameRpcValue Value;
	Value.Type  = EType::Bool;
	Value.bBool = bValue;
	return Value;
}

FGameRpcValue FGameRpcValue::MakeNumber(double Number, bool bIsInteger)
{
	FGameRpcValue Value;
	Value.Type     = EType::Number;
	Value.Number   = Number;
	Value.bInteger = bIsInteger;
	return Value;
}

FGameRpcValue FGameRpcValue::MakeString(std::string Text)
{
	FGameRpcValue Value;
	Value.Type   = EType::String;
	Value.String = std::move(Text);
	return Value;
}

FGameRpcValue FGameRpcValue::MakeVector3(const FVector3& Vector)
{
	FGameRpcValue Value;
	Value.Type   = EType::Vector3;
	Value.Vector = Vector;
	return Value;
}

FGameRpcValue FGameRpcValue::MakeAsset(std::string Path, std::string Filter)
{
	FGameRpcValue Value;
	Value.Type        = EType::Asset;
	Value.String      = std::move(Path);
	Value.AssetFilter = std::move(Filter);
	return Value;
}

FGameRpcValue FGameRpcValue::MakeEntity(FEntity Entity)
{
	FGameRpcValue Value;
	Value.Type   = EType::Entity;
	Value.Entity = Entity;
	return Value;
}
