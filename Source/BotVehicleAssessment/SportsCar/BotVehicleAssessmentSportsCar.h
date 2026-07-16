// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "BotVehicleAssessmentPawn.h"
#include "BotVehicleAssessmentSportsCar.generated.h"

/**
 *  Sports car wheeled vehicle implementation
 */
UCLASS(abstract)
class ABotVehicleAssessmentSportsCar : public ABotVehicleAssessmentPawn
{
	GENERATED_BODY()
	
public:

	ABotVehicleAssessmentSportsCar();
};
