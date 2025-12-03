#include "UnrealGPTSettings.h"

UUnrealGPTSettings::UUnrealGPTSettings(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

FName UUnrealGPTSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}

