// VehicleBotDriver.h
#pragma once
 
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "NavigationSystemTypes.h"
#include "VehicleBotDriver.generated.h"
 
class UChaosWheeledVehicleMovementComponent;
 
UENUM(BlueprintType)
enum class EBotDriveState : uint8
{
	Patrolling,
	Recovering,   // wired up in the next phase (stuck + recover)
	NoPath
};
 
/**
 * Server-authoritative patrol brain for an AI vehicle bot.
 * Consumes a nav-mesh path with pure pursuit and drives the Chaos vehicle
 * movement component directly — no MoveTo / Behavior Tree tasks involved.
 */
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class BOTVEHICLEASSESSMENT_API UVehicleBotDriver : public UActorComponent
{
	GENERATED_BODY()
 
public:
	UVehicleBotDriver();
 
	UPROPERTY(EditAnywhere, Category = "Bot|Patrol")
	TArray<AActor*> PatrolPoints;
 
	UPROPERTY(EditAnywhere, Category = "Bot|Steering")
	float LookaheadDistance = 800.f;
 
	UPROPERTY(EditAnywhere, Category = "Bot|Steering")
	float ArrivalRadius = 300.f;
 
	UPROPERTY(EditAnywhere, Category = "Bot|Steering")
	float MaxThrottle = 0.9f;
 
	UPROPERTY(EditAnywhere, Category = "Bot|Steering")
	float MinCorneringThrottle = 0.35f;
 
	UPROPERTY(EditAnywhere, Category = "Bot|Steering")
	float FullLockAngleDeg = 45.f;
 
	UPROPERTY(EditAnywhere, Category = "Bot|Steering")
	float RepathIntervalSeconds = 1.0f;
 
	UPROPERTY(BlueprintReadOnly, Category = "Bot|State")
	EBotDriveState CurrentState = EBotDriveState::NoPath;
 
protected:
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                            FActorComponentTickFunction* ThisTickFunction) override;
 
private:
	void RequestPathToCurrentTarget();
	bool FindLookaheadPoint(FVector& OutPoint) const;
	void ApplyPurePursuit(const FVector& LookaheadPoint);
	void AdvancePatrolIndexIfArrived();
 
	UPROPERTY()
	TObjectPtr<UChaosWheeledVehicleMovementComponent> Movement;
 
	FNavPathSharedPtr CurrentPath;
	int32 CurrentTargetIndex = 0;
	float TimeSinceLastPathRequest = 0.f;
};
 