// Copyright Epic Games, Inc. All Rights Reserved.

#include "BotVehicleAssessmentWheelRear.h"
#include "UObject/ConstructorHelpers.h"

UBotVehicleAssessmentWheelRear::UBotVehicleAssessmentWheelRear()
{
	AxleType = EAxleType::Rear;
	bAffectedByHandbrake = true;
	bAffectedByEngine = true;
}