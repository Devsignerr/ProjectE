#include "Scene/CollisionEvents.h"

const char* GetCollisionEventName(ECollisionEventType Type)
{
	switch (Type)
	{
	case ECollisionEventType::CollisionBegin: return "CollisionBegin";
	case ECollisionEventType::CollisionEnd:   return "CollisionEnd";
	case ECollisionEventType::TriggerEnter:   return "TriggerEnter";
	case ECollisionEventType::TriggerExit:    return "TriggerExit";
	case ECollisionEventType::JointBreak:     return "JointBreak";
	default:                                  return "Unknown";
	}
}
