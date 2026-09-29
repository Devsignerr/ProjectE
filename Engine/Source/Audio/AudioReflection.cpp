#include "Audio/AudioReflection.h"

#include "Audio/AudioComponents.h"
#include "Core/Reflection/TypeInfo.h"

void RegisterAudioTypes()
{
	static bool bRegistered = false;
	if (bRegistered)
	{
		return;
	}
	bRegistered = true;

	FTypeRegistry::Get().RegisterType<FAudioSourceComponent>("AudioSourceComponent", "오디오 소스")
		.Property(&FAudioSourceComponent::ClipAsset, "ClipAsset", "클립")
		.Property(&FAudioSourceComponent::Volume, "Volume", "볼륨").Range(0.0f, 4.0f, 0.01f)
		.Property(&FAudioSourceComponent::Pitch, "Pitch", "피치").Range(0.1f, 4.0f, 0.01f)
		.Property(&FAudioSourceComponent::bLoop, "Loop", "반복")
		.Property(&FAudioSourceComponent::bPlayOnStart, "PlayOnStart", "시작 시 재생")
		.Property(&FAudioSourceComponent::bSpatial, "Spatial", "3D 공간화")
		.Property(&FAudioSourceComponent::MinDistance, "MinDistance", "최소 거리 (cm)").Range(1.0f, 100000.0f, 1.0f)
		.Property(&FAudioSourceComponent::MaxDistance, "MaxDistance", "최대 거리 (cm)").Range(1.0f, 1000000.0f, 10.0f)
		.AsComponent();
}
