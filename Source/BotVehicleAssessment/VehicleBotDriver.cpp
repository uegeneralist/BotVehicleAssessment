// VehicleBotDriver.cpp
#include "VehicleBotDriver.h"
#include "ChaosWheeledVehicleMovementComponent.h"
#include "NavigationSystem.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"

UVehicleBotDriver::UVehicleBotDriver()
{
	// This component needs to run its own logic every frame (patrol steering,
	// stuck checks, recovery), so ticking must be enabled.
	PrimaryComponentTick.bCanEverTick = true;
}

void UVehicleBotDriver::BeginPlay()
{
	Super::BeginPlay();

	// Find the vehicle's movement component on our owning Pawn — we drive
	// through this directly (SetSteeringInput / SetThrottleInput / etc.)
	// rather than through any AI MoveTo task.
	if (AActor* Owner = GetOwner())
	{
		Movement = Owner->FindComponentByClass<UChaosWheeledVehicleMovementComponent>();
	}

	if (!Movement)
	{
		UE_LOG(LogTemp, Error, TEXT("VehicleBotDriver: no UChaosWheeledVehicleMovementComponent found on owner %s"),
			GetOwner() ? *GetOwner()->GetName() : TEXT("NONE"));
	}

	// The GameMode auto-spawns a player-controlled instance of this same
	// Pawn class at PlayerStart for the human to drive — that instance is
	// expected to have an empty PatrolPoints array, since it was never meant
	// to patrol. Only warn about missing patrol points on AI-controlled /
	// unpossessed instances (the actual bot you place and configure by hand).
	const bool bIsPlayerDriven = GetOwner() && GetOwner()->IsA<APawn>() &&
		Cast<APawn>(GetOwner())->IsPlayerControlled();

	if (!bIsPlayerDriven && PatrolPoints.Num() < 4)
	{
		UE_LOG(LogTemp, Warning, TEXT("VehicleBotDriver: PatrolPoints has %d entries — the brief asks for 4+."),
			PatrolPoints.Num());
	}

	// Seed the stuck-detection baseline at our starting position so the
	// first stuck check (StuckCheckIntervalSeconds later) has something
	// sensible to compare against instead of comparing to (0,0,0).
	if (AActor* Owner = GetOwner())
	{
		LastStuckCheckLocation = Owner->GetActorLocation();
	}
}

void UVehicleBotDriver::TickComponent(float DeltaTime, ELevelTick TickType,
                                       FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	AActor* Owner = GetOwner();

	// Server-authoritative: clients must never make their own driving
	// decisions. On a client this component simply does nothing, and the
	// vehicle's replicated physics state (position/rotation/wheel state)
	// is what clients actually see — handled automatically by
	// UChaosWheeledVehicleMovementComponent's own replication.
	if (!Owner || !Owner->HasAuthority() || !Movement || PatrolPoints.Num() == 0)
	{
		return;
	}

	// --- TEMP DEBUG: on-screen HUD showing this bot's state. Safe to leave
	// in during development; strip out (or wrap in #if !UE_BUILD_SHIPPING)
	// before final submission. Uses GetUniqueID() as the message key so
	// multiple bot instances each get their own line instead of overwriting
	// each other.
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(static_cast<int32>(GetOwner()->GetUniqueID()), 0.f, FColor::Yellow,
			FString::Printf(TEXT("[%s] State=%s  RecoveryAttempts=%d  Strikes=%d"),
				*GetOwner()->GetName(),
				CurrentState == EBotDriveState::Patrolling ? TEXT("Patrolling") :
				CurrentState == EBotDriveState::Recovering ? TEXT("Recovering") : TEXT("NoPath"),
				RecoveryAttemptCount, ConsecutiveStuckStrikes));
	}
	// --- END TEMP DEBUG ---

	// The state machine: while Recovering, hand off entirely to the
	// recovery maneuver and skip normal patrol logic. Once recovery
	// finishes (success or give-up-and-retry), control returns here next
	// tick with CurrentState back to Patrolling or NoPath.
	if (CurrentState == EBotDriveState::Recovering)
	{
		TickRecovery(DeltaTime);
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

		// Only check for "stuck" while we're actually trying to drive
		// somewhere — a car with a valid path but choosing not to move
		// isn't a scenario here, since ApplyPurePursuit always applies
		// some throttle when patrolling.
		UpdateStuckDetection(DeltaTime);
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
	// Don't re-path every tick (that was the Part A bug) — only every
	// RepathIntervalSeconds, or immediately after AdvancePatrolIndexIfArrived()
	// resets the timer for a fresh target.
	if (TimeSinceLastPathRequest < RepathIntervalSeconds)
	{
		return;
	}
	TimeSinceLastPathRequest = 0.f;

	AActor* Owner = GetOwner();
	AActor* Target = PatrolPoints.IsValidIndex(CurrentTargetIndex) ? PatrolPoints[CurrentTargetIndex] : nullptr;
	if (!Owner || !Target)
	{
		// --- TEMP DEBUG ---
		UE_LOG(LogTemp, Error, TEXT("VehicleBotDriver: no Owner or no Target (index %d, PatrolPoints.Num()=%d)"),
			CurrentTargetIndex, PatrolPoints.Num());
		// --- END TEMP DEBUG ---
		return;
	}

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
	if (!NavSys)
	{
		// --- TEMP DEBUG ---
		UE_LOG(LogTemp, Error, TEXT("VehicleBotDriver: no UNavigationSystemV1 in this world at all."));
		// --- END TEMP DEBUG ---
		return;
	}

	const ANavigationData* NavData = NavSys->GetDefaultNavDataInstance();
	if (!NavData)
	{
		// --- TEMP DEBUG ---
		UE_LOG(LogTemp, Error, TEXT("VehicleBotDriver: NavigationSystem exists but has no default NavData — is there a Nav Mesh Bounds Volume in the level, and has it been built?"));
		// --- END TEMP DEBUG ---
		return;
	}

	FPathFindingQuery Query(Owner, *NavData, Owner->GetActorLocation(), Target->GetActorLocation());
	FPathFindingResult Result = NavSys->FindPathSync(Query);

	// --- TEMP DEBUG ---
	UE_LOG(LogTemp, Warning, TEXT("VehicleBotDriver: path request to target %d — Successful=%s PartialPath=%s NumPoints=%d"),
		CurrentTargetIndex,
		Result.IsSuccessful() ? TEXT("true") : TEXT("false"),
		(Result.Path.IsValid() && Result.Path->IsPartial()) ? TEXT("true") : TEXT("false"),
		Result.Path.IsValid() ? Result.Path->GetPathPoints().Num() : 0);
	// --- END TEMP DEBUG ---

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

	// Walk the path forward and pick the first point at least
	// LookaheadDistance away — this is the "pure pursuit" part: we always
	// aim at a point a fixed distance ahead on the path, rather than the
	// path's raw waypoints, which gives smoother cornering.
	for (const FNavPathPoint& Point : Points)
	{
		if (FVector::Dist(VehicleLoc, Point.Location) >= LookaheadDistance)
		{
			OutPoint = Point.Location;
			return true;
		}
	}

	// Nothing far enough away — we're near the end of the path, aim at the
	// final point instead of failing outright.
	OutPoint = Points.Last().Location;
	return true;
}

void UVehicleBotDriver::ApplyPurePursuit(const FVector& LookaheadPoint)
{
	AActor* Owner = GetOwner();
	const FVector ToTarget = (LookaheadPoint - Owner->GetActorLocation()).GetSafeNormal();
	const FVector Forward = Owner->GetActorForwardVector();

	// Signed angle between forward vector and target vector (ground-plane
	// only — we don't care about pitch/roll for steering).
	const float ClampedDot = FMath::Clamp(FVector::DotProduct(Forward, ToTarget), -1.f, 1.f);
	float AngleDeg = FMath::RadiansToDegrees(FMath::Acos(ClampedDot));

	// Cross product's Z tells us which side the target is on (left/right),
	// which gives the angle its sign.
	if (FVector::CrossProduct(Forward, ToTarget).Z < 0.f)
	{
		AngleDeg = -AngleDeg;
	}

	const float Steering = FMath::Clamp(AngleDeg / FullLockAngleDeg, -1.f, 1.f);

	// Remember this for recovery later: if we get stuck right after
	// steering hard one way, countersteering the other way is a reasonable
	// first guess at "steer away from whatever we hit".
	if (!FMath::IsNearlyZero(Steering))
	{
		LastPatrolSteeringSign = FMath::Sign(Steering);
	}

	// Slow down proportionally to how sharp the turn is — this replaces
	// the Part A defect where throttle was a constant 0.9f regardless of
	// steering angle, which caused understeer/overshoot on tight corners.
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

// ---------------------------------------------------------------------
// Recover
// ---------------------------------------------------------------------

void UVehicleBotDriver::UpdateStuckDetection(float DeltaTime)
{
	TimeSinceLastStuckCheck += DeltaTime;

	if (TimeSinceLastStuckCheck < StuckCheckIntervalSeconds)
	{
		return; // not time to check yet
	}

	AActor* Owner = GetOwner();
	const FVector CurrentLocation = Owner->GetActorLocation();
	const float DistanceMoved = FVector::Dist(CurrentLocation, LastStuckCheckLocation);

	if (DistanceMoved < StuckMinDistanceCm)
	{
		// Didn't move far enough in the last StuckCheckIntervalSeconds
		// despite ApplyPurePursuit() applying throttle — that's one strike.
		// Using a window (not instantaneous speed) avoids false-positiving
		// on a car that's briefly slow through a tight corner, and avoids
		// false-negativing on a car whose wheels are spinning fast against
		// a wall (which the naive speed-based check from Part A would miss,
		// since wheel speed isn't the same as actual displacement).
		++ConsecutiveStuckStrikes;

		UE_LOG(LogTemp, Verbose, TEXT("VehicleBotDriver: stuck strike %d/%d (moved %.0fcm in %.1fs)"),
			ConsecutiveStuckStrikes, StuckStrikesToTriggerRecovery, DistanceMoved, TimeSinceLastStuckCheck);
	}
	else
	{
		// Made real progress — reset the strike counter. A single good
		// window of movement clears any prior strikes; we only care about
		// consecutive failure.
		ConsecutiveStuckStrikes = 0;
	}

	LastStuckCheckLocation = CurrentLocation;
	TimeSinceLastStuckCheck = 0.f;

	if (ConsecutiveStuckStrikes >= StuckStrikesToTriggerRecovery)
	{
		BeginRecovery();
	}
}

void UVehicleBotDriver::BeginRecovery()
{
	CurrentState = EBotDriveState::Recovering;
	RecoveryElapsedSeconds = 0.f;
	ConsecutiveStuckStrikes = 0;

	UE_LOG(LogTemp, Log, TEXT("VehicleBotDriver: stuck — beginning recovery attempt %d/%d"),
		RecoveryAttemptCount + 1, MaxPhysicalRecoveryAttempts);
}

void UVehicleBotDriver::TickRecovery(float DeltaTime)
{
	RecoveryElapsedSeconds += DeltaTime;

	const float TotalManeuverSeconds = ReverseStraightSeconds + ReverseTurnSeconds;

	if (RecoveryElapsedSeconds <= ReverseStraightSeconds)
	{
		// Phase 1: reverse with wheels straight. Purpose is purely to put
		// distance between us and whatever we hit before we start turning —
		// turning immediately on contact tends to just grind along the
		// obstacle rather than actually pulling away from it.
		Movement->SetThrottleInput(ReverseThrottle);
		Movement->SetSteeringInput(0.f);
		Movement->SetBrakeInput(0.f);
		return;
	}

	if (RecoveryElapsedSeconds <= TotalManeuverSeconds)
	{
		// Phase 2: keep reversing, but now steer hard away from the
		// direction we were turning right before we got stuck. This is the
		// phase that actually changes the vehicle's heading — without it,
		// pure pursuit would just point the car straight back at the same
		// blocked lookahead point the instant control returns, which is
		// exactly the "backs up, immediately drives into it again" symptom.
		Movement->SetThrottleInput(ReverseThrottle);
		Movement->SetSteeringInput(-LastPatrolSteeringSign * ReverseSteeringMagnitude);
		Movement->SetBrakeInput(0.f);
		return;
	}

	// Maneuver finished — stop and let physics settle for a moment before
	// judging whether it worked (an instant re-check would trigger while
	// the car is still slowing down from reversing, which isn't a fair test).
	Movement->SetThrottleInput(0.f);
	Movement->SetBrakeInput(1.f);
	Movement->SetSteeringInput(0.f);

	PostRecoveryGraceRemaining += DeltaTime;
	if (PostRecoveryGraceRemaining < PostRecoveryGraceSeconds)
	{
		return;
	}
	PostRecoveryGraceRemaining = 0.f;

	// Re-seed the stuck-detection baseline so the next check measures
	// progress *after* this maneuver, not across it.
	LastStuckCheckLocation = GetOwner()->GetActorLocation();
	TimeSinceLastStuckCheck = 0.f;

	++RecoveryAttemptCount;

	if (RecoveryAttemptCount >= MaxPhysicalRecoveryAttempts)
	{
		// Physical recovery has failed enough times in a row — only now do
		// we consider teleporting, and only if nobody can see it happen.
		if (!AnyPlayerCanSeeVehicle())
		{
			TeleportToRecoverySpot();
		}
		else
		{
			// A player is watching — do not teleport. Just try physical
			// recovery again; this can loop, which is an intentional,
			// documented tradeoff (see README defects list) rather than an
			// oversight: we prioritize "never pop in front of a player"
			// over "always eventually get unstuck".
			UE_LOG(LogTemp, Warning, TEXT("VehicleBotDriver: recovery attempts exhausted but a player can see the vehicle — retrying physical recovery instead of teleporting."));
			RecoveryAttemptCount = 0;
			BeginRecovery();
			return;
		}
	}

	// Hand control back to normal patrol logic next tick. If we're still
	// actually stuck, UpdateStuckDetection will notice again within
	// StuckCheckIntervalSeconds * StuckStrikesToTriggerRecovery and we'll
	// re-enter BeginRecovery(), incrementing RecoveryAttemptCount further.
	CurrentState = EBotDriveState::Patrolling;
}

bool UVehicleBotDriver::AnyPlayerCanSeeVehicle() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return true; // fail safe: assume visible, never teleport blind
	}

	AActor* Owner = GetOwner();

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		if (PC && PC->LineOfSightTo(Owner))
		{
			return true;
		}
	}

	return false;
}

void UVehicleBotDriver::TeleportToRecoverySpot()
{
	AActor* Owner = GetOwner();
	AActor* Target = PatrolPoints.IsValidIndex(CurrentTargetIndex) ? PatrolPoints[CurrentTargetIndex] : nullptr;

	// Simplest reasonable recovery spot: back on the nav mesh near our
	// current patrol target. A more thorough version would search nearby
	// nav mesh points for one with a clear path back to the target — noted
	// as a possible improvement in the README.
	const FVector Fallback = Target ? Target->GetActorLocation() : Owner->GetActorLocation();

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
	FNavLocation ProjectedLocation;
	if (NavSys && NavSys->ProjectPointToNavigation(Fallback, ProjectedLocation))
	{
		Owner->SetActorLocation(ProjectedLocation.Location, false, nullptr, ETeleportType::TeleportPhysics);
	}
	else
	{
		Owner->SetActorLocation(Fallback, false, nullptr, ETeleportType::TeleportPhysics);
	}

	UE_LOG(LogTemp, Warning, TEXT("VehicleBotDriver: teleported to recovery spot after %d failed physical attempts (no player was watching)."),
		RecoveryAttemptCount);

	RecoveryAttemptCount = 0;
	ConsecutiveStuckStrikes = 0;
	CurrentPath.Reset();          // force a fresh path from the new location
	TimeSinceLastPathRequest = RepathIntervalSeconds;
	CurrentState = EBotDriveState::Patrolling;
}