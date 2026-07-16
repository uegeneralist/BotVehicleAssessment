// VehicleBotDriver.cpp
#include "VehicleBotDriver.h"
#include "ChaosWheeledVehicleMovementComponent.h"
#include "NavigationSystem.h"
#include "GameFramework/Actor.h"
 
UVehicleBotDriver::UVehicleBotDriver()
{
	PrimaryComponentTick.bCanEverTick = true;
}
 
void UVehicleBotDriver::BeginPlay()
{
	Super::BeginPlay();
 
	if (AActor* Owner = GetOwner())
	{
		Movement = Owner->FindComponentByClass<UChaosWheeledVehicleMovementComponent>();
	}
 
	if (!Movement)
	{
		UE_LOG(LogTemp, Error, TEXT("VehicleBotDriver: no UChaosWheeledVehicleMovementComponent found on owner %s"),
			GetOwner() ? *GetOwner()->GetName() : TEXT("NONE"));
	}
 
	if (PatrolPoints.Num() < 4)
	{
		UE_LOG(LogTemp, Warning, TEXT("VehicleBotDriver: PatrolPoints has %d entries — the brief asks for 4+."),
			PatrolPoints.Num());
	}
}
 
void UVehicleBotDriver::TickComponent(float DeltaTime, ELevelTick TickType,
                                       FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
 
	AActor* Owner = GetOwner();
 
	// Server-authoritative: clients must never make their own driving decisions.
	if (!Owner || !Owner->HasAuthority() || !Movement || PatrolPoints.Num() == 0)
	{
		return;
	}
 
	TimeSinceLastPathRequest += DeltaTime;
 
	if (!CurrentPath.IsValid() || !CurrentPath->IsValid())
	{
		RequestPathToCurrentTarget();
	}
 
	FVector Lookahead;
	if (FindLookaheadPoint(Lookahead))
	{
		CurrentState = EBotDriveState::Patrolling;
		ApplyPurePursuit(Lookahead);
	}
	else
	{
		CurrentState = EBotDriveState::NoPath;
		Movement->SetThrottleInput(0.f);
		Movement->SetBrakeInput(1.f);
		Movement->SetSteeringInput(0.f);
	}
 
	AdvancePatrolIndexIfArrived();
}
 
void UVehicleBotDriver::RequestPathToCurrentTarget()
{
	if (TimeSinceLastPathRequest < RepathIntervalSeconds)
	{
		return;
	}
	TimeSinceLastPathRequest = 0.f;
 
	AActor* Owner = GetOwner();
	AActor* Target = PatrolPoints.IsValidIndex(CurrentTargetIndex) ? PatrolPoints[CurrentTargetIndex] : nullptr;
	if (!Owner || !Target)
	{
		return;
	}
 
	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
	if (!NavSys)
	{
		return;
	}
 
	const ANavigationData* NavData = NavSys->GetDefaultNavDataInstance();
	if (!NavData)
	{
		return;
	}
 
	FPathFindingQuery Query(Owner, *NavData, Owner->GetActorLocation(), Target->GetActorLocation());
	FPathFindingResult Result = NavSys->FindPathSync(Query);
 
	CurrentPath = Result.IsSuccessful() ? Result.Path : nullptr;
}
 
bool UVehicleBotDriver::FindLookaheadPoint(FVector& OutPoint) const
{
	if (!CurrentPath.IsValid())
	{
		return false;
	}
 
	const TArray<FNavPathPoint>& Points = CurrentPath->GetPathPoints();
	if (Points.Num() < 2)
	{
		return false;
	}
 
	const FVector VehicleLoc = GetOwner()->GetActorLocation();
 
	for (const FNavPathPoint& Point : Points)
	{
		if (FVector::Dist(VehicleLoc, Point.Location) >= LookaheadDistance)
		{
			OutPoint = Point.Location;
			return true;
		}
	}
 
	OutPoint = Points.Last().Location;
	return true;
}
 
void UVehicleBotDriver::ApplyPurePursuit(const FVector& LookaheadPoint)
{
	AActor* Owner = GetOwner();
	const FVector ToTarget = (LookaheadPoint - Owner->GetActorLocation()).GetSafeNormal();
	const FVector Forward = Owner->GetActorForwardVector();
 
	// Signed angle between forward vector and target vector, ground-plane only.
	const float ClampedDot = FMath::Clamp(FVector::DotProduct(Forward, ToTarget), -1.f, 1.f);
	float AngleDeg = FMath::RadiansToDegrees(FMath::Acos(ClampedDot));
 
	if (FVector::CrossProduct(Forward, ToTarget).Z < 0.f)
	{
		AngleDeg = -AngleDeg;
	}
 
	const float Steering = FMath::Clamp(AngleDeg / FullLockAngleDeg, -1.f, 1.f);
 
	const float TurnSeverity = FMath::Clamp(FMath::Abs(AngleDeg) / FullLockAngleDeg, 0.f, 1.f);
	const float Throttle = FMath::Lerp(MaxThrottle, MinCorneringThrottle, TurnSeverity);
 
	Movement->SetSteeringInput(Steering);
	Movement->SetThrottleInput(Throttle);
	Movement->SetBrakeInput(0.f);
}
 
void UVehicleBotDriver::AdvancePatrolIndexIfArrived()
{
	AActor* Owner = GetOwner();
	AActor* Target = PatrolPoints.IsValidIndex(CurrentTargetIndex) ? PatrolPoints[CurrentTargetIndex] : nullptr;
	if (!Owner || !Target)
	{
		return;
	}
 
	if (FVector::Dist(Owner->GetActorLocation(), Target->GetActorLocation()) <= ArrivalRadius)
	{
		CurrentTargetIndex = (CurrentTargetIndex + 1) % PatrolPoints.Num(); // loop, not one-shot
		CurrentPath.Reset();
		TimeSinceLastPathRequest = RepathIntervalSeconds; // allow an immediate repath next tick
	}
}
 