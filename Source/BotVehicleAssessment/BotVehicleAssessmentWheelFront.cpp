// Copyright Epic Games, Inc. All Rights Reserved.

#include "BotVehicleAssessmentWheelFront.h"
#include "UObject/ConstructorHelpers.h"

UBotVehicleAssessmentWheelFront::UBotVehicleAssessmentWheelFront()
{
	AxleType = EAxleType::Front;
	bAffectedBySteering = true;
	MaxSteerAngle = 40.f;
}