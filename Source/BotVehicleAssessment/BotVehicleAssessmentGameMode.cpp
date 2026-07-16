// Copyright Epic Games, Inc. All Rights Reserved.

#include "BotVehicleAssessmentGameMode.h"
#include "BotVehicleAssessmentPlayerController.h"

ABotVehicleAssessmentGameMode::ABotVehicleAssessmentGameMode()
{
	PlayerControllerClass = ABotVehicleAssessmentPlayerController::StaticClass();
}
