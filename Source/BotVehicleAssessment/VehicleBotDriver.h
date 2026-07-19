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
	Recovering,   // stuck, actively trying to reverse/steer free
	NoPath
};

/**
 * Server-authoritative patrol brain for an AI vehicle bot.
 * Consumes a nav-mesh path with pure pursuit and drives the Chaos vehicle
 * movement component directly — no MoveTo / Behavior Tree tasks involved.
 * Also detects when it's stuck and recovers physically (reverse + steer),
 * only teleporting as a last resort when repeated physical attempts fail
 * AND no player can currently see the vehicle.
 */
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class BOTVEHICLEASSESSMENT_API UVehicleBotDriver : public UActorComponent
{
	GENERATED_BODY()

public:
	UVehicleBotDriver();

	// Drag 4+ empty Actors (or Target Points) from the level into this array.
	// TSoftObjectPtr (not raw AActor*) is deliberate: a hard AActor* here
	// gets corrupted every time this component's C++ class is recompiled
	// (Live Coding or a full rebuild), because reinstancing routes through
	// the class archetype/CDO, and Unreal disallows a hard reference from
	// a CDO to another level's actor ("Illegal TEXT reference to a private
	// object in external package"). A soft reference stores a path instead
	// of a hard pointer, so it survives recompiles — resolve it with
	// GetPatrolTarget() below rather than dereferencing the array directly.
	UPROPERTY(EditAnywhere, Category = "Bot|Patrol")
	TArray<TSoftObjectPtr<AActor>> PatrolPoints;

	// --- Steering tuning ---
	UPROPERTY(EditAnywhere, Category = "Bot|Steering")
	float LookaheadDistance = 800.f;

	UPROPERTY(EditAnywhere, Category = "Bot|Steering")
	float ArrivalRadius = 400.f;

	UPROPERTY(EditAnywhere, Category = "Bot|Steering")
	float MaxThrottle = 0.9f;

	UPROPERTY(EditAnywhere, Category = "Bot|Steering")
	float MinCorneringThrottle = 0.35f;

	UPROPERTY(EditAnywhere, Category = "Bot|Steering")
	float FullLockAngleDeg = 45.f;

	UPROPERTY(EditAnywhere, Category = "Bot|Steering")
	float RepathIntervalSeconds = 1.0f;

	// --- Stuck detection tuning ---
	// Every StuckCheckIntervalSeconds, we compare current position to the
	// position at the last check. If the vehicle moved less than
	// StuckMinDistanceCm, that's one "strike". StuckStrikesToTriggerRecovery
	// consecutive strikes = genuinely stuck (not just cornering slowly for
	// one instant).
	UPROPERTY(EditAnywhere, Category = "Bot|Stuck")
	float StuckCheckIntervalSeconds = 1.0f;

	UPROPERTY(EditAnywhere, Category = "Bot|Stuck")
	float StuckMinDistanceCm = 150.f;

	UPROPERTY(EditAnywhere, Category = "Bot|Stuck")
	int32 StuckStrikesToTriggerRecovery = 3;

	// --- Recovery tuning ---
	// Recovery is two phases, not one blend, because reversing straight
	// mostly just moves the car backward without changing which way it's
	// facing — it needs a second phase where it reverses WHILE turning
	// hard, so the nose actually swings away from the obstacle before we
	// hand control back to pure pursuit. Without this second phase, the
	// car backs up a little, then pure pursuit immediately points it back
	// at the same blocked lookahead point and it re-collides almost
	// instantly (the symptom of "keeps trying to turn right, can't").

	// Phase 1: reverse with wheels straight, to build real distance away
	// from whatever we hit before we start turning.
	UPROPERTY(EditAnywhere, Category = "Bot|Recovery")
	float ReverseStraightSeconds = 0.8f;

	// Phase 2: keep reversing but steer hard away from the obstacle, so
	// the vehicle's heading actually changes.
	UPROPERTY(EditAnywhere, Category = "Bot|Recovery")
	float ReverseTurnSeconds = 1.4f;

	UPROPERTY(EditAnywhere, Category = "Bot|Recovery")
	float ReverseThrottle = -0.6f;

	UPROPERTY(EditAnywhere, Category = "Bot|Recovery")
	float ReverseSteeringMagnitude = 1.0f;

	// After this many *consecutive* failed physical recovery attempts
	// (still stuck right after finishing a reverse maneuver), we allow a
	// teleport — but only if no player can currently see the vehicle.
	UPROPERTY(EditAnywhere, Category = "Bot|Recovery")
	int32 MaxPhysicalRecoveryAttempts = 3;

	// Grace period right after a recovery maneuver finishes before we start
	// stuck-checking again, so the car has a moment to actually accelerate
	// away before we judge whether it worked.
	UPROPERTY(EditAnywhere, Category = "Bot|Recovery")
	float PostRecoveryGraceSeconds = 1.0f;

	// When teleport-as-last-resort actually happens, we don't just jump to
	// wherever the patrol target is (it might be right past the obstacle we
	// got stuck on, landing us overlapping it). Instead we walk along a
	// fresh path toward the target and pick the first point that's at least
	// this far away AND verified clear of collision.
	UPROPERTY(EditAnywhere, Category = "Bot|Recovery")
	float TeleportClearanceDistance = 600.f;

	// Rough vehicle bounding box (half-extents, cm) used to test whether a
	// candidate teleport spot is actually clear before we use it. Tune this
	// to roughly match your vehicle's real dimensions.
	UPROPERTY(EditAnywhere, Category = "Bot|Recovery")
	FVector TeleportClearanceCheckExtent = FVector(150.f, 100.f, 75.f);

	// Read-only, useful to watch live in the Details panel while testing.
	UPROPERTY(BlueprintReadOnly, Category = "Bot|State")
	EBotDriveState CurrentState = EBotDriveState::NoPath;

	UPROPERTY(BlueprintReadOnly, Category = "Bot|State")
	int32 RecoveryAttemptCount = 0;

protected:
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                            FActorComponentTickFunction* ThisTickFunction) override;

private:
	// --- Drive (Phase 4) ---
	void RequestPathToCurrentTarget();
	bool FindLookaheadPoint(FVector& OutPoint) const;
	void ApplyPurePursuit(const FVector& LookaheadPoint);
	void AdvancePatrolIndexIfArrived();

	// Resolves PatrolPoints[Index] (a soft reference) into a real actor
	// pointer for this frame. Returns nullptr if the index is invalid or
	// the soft reference is unset/unresolvable.
	AActor* ResolvePatrolTarget(int32 Index) const;

	// --- Recover (this phase) ---
	// Called every tick while Patrolling. Updates the stuck-strike counter
	// and flips CurrentState to Recovering if the threshold is hit.
	void UpdateStuckDetection(float DeltaTime);

	// Called every tick while Recovering. Drives the reverse/steer maneuver
	// and decides when to hand control back to pure pursuit, or escalate to
	// a teleport.
	void TickRecovery(float DeltaTime);

	// Kicks off a fresh recovery maneuver: resets the timer, remembers which
	// way to countersteer (opposite of the steering we were applying right
	// before we got stuck).
	void BeginRecovery();

	// True if any connected player currently has line-of-sight to this
	// vehicle. Used to gate the teleport-as-last-resort fallback.
	bool AnyPlayerCanSeeVehicle() const;

	// Last-resort recovery: only called after MaxPhysicalRecoveryAttempts
	// consecutive failures AND AnyPlayerCanSeeVehicle() is false.
	void TeleportToRecoverySpot();

	// Walks a fresh path toward the current patrol target and returns the
	// first point that's both past TeleportClearanceDistance away and
	// verified clear of blocking collision. Returns false if no such point
	// is found (e.g. pathfinding itself fails).
	bool FindSafeTeleportSpot(FVector& OutPoint) const;

	UPROPERTY()
	TObjectPtr<UChaosWheeledVehicleMovementComponent> Movement;

	FNavPathSharedPtr CurrentPath;
	int32 CurrentTargetIndex = 0;
	float TimeSinceLastPathRequest = 0.f;

	// Stuck-detection bookkeeping
	FVector LastStuckCheckLocation = FVector::ZeroVector;
	float TimeSinceLastStuckCheck = 0.f;
	int32 ConsecutiveStuckStrikes = 0;

	// Remembers the sign of the last steering input applied while
	// Patrolling, so recovery can countersteer in the opposite direction
	// (a reasonable heuristic for "steer away from whatever we hit").
	float LastPatrolSteeringSign = 1.f;

	// Recovery maneuver bookkeeping
	float RecoveryElapsedSeconds = 0.f;
	float PostRecoveryGraceRemaining = 0.f;
};