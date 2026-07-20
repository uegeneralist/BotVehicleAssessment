# Vehicle Bot — Take-Home Build (Part B)

## Build / run
- Engine: **UE 5.8**, Vehicle template (SportsCar), C++.
- Open `BotVehicleAssessment.uproject`. If prompted to rebuild modules, allow it (or build from
  Visual Studio: Build → Rebuild Solution, Development Editor / Win64).
- Open the map with the bot placed (patrol points + test obstacle in the level).
- Hit **Play**. The player-driven car has been removed (`DefaultPawnClass = None` on the
  GameMode) so only the AI bot exists in the world — this is intentional, to keep the test
  focused on the bot rather than a second driveable vehicle.
- To see the diagnostic: press **`** (tilde) to open the console, type:
  ```
  yourbot.Diag 1
  ```
  This prints one line per second per bot to the Output Log: speed, current state, the reason
  it isn't progressing (when it isn't), and running totals (laps / stuck events / recovery
  cycles / teleports). `yourbot.Diag 0` turns it off.

## What it does
An AI vehicle bot (`UVehicleBotDriver`, an `ActorComponent` on the vehicle Pawn) patrols a loop
of 4+ placed Target Point actors using its own C++ pure-pursuit steering — no `MoveTo` /
Behavior Tree tasks drive it. It detects when it's stuck (no real displacement over a time
window, not just instantaneous speed) and recovers physically: reverse straight, then reverse
while turning hard away from the obstacle, repeated up to a capped number of attempts. Only
after repeated physical failures **and** only if no player currently has line-of-sight to it,
does it fall back to a teleport — and even then it searches along a fresh path for a point
that's both far enough away and collision-verified clear, rather than just jumping to the
patrol target's raw location. All of this is server-authoritative (`HasAuthority()` gated);
the Chaos vehicle movement component replicates the resulting physics state to clients on its
own.

## Measurement (5-minute PIE run)

| Metric | Value |
|---|---|
| Laps completed | 1 |
| Stuck events | 0 |
| Recovery cycles completed | 0 |
| Teleport fallbacks | 0 |
| Speed sample (Patrolling) | ~412 cm/s |

**How produced**: `yourbot.Diag 1` enabled at the start of a single uninterrupted 5-minute PIE
session; the numbers above are read directly from the last `Totals:` line the bot printed to
the Output Log at the end of that session (the diagnostic line accumulates running totals, so
the final line is the complete tally for the run).

**Important caveat, stated honestly rather than glossed over**: this particular run's patrol
path was clear of obstacles, so it demonstrates sustained patrol stability over 5 minutes but
doesn't exercise recovery or teleport in the same run. The stuck-detection → physical recovery
→ escalation-to-teleport pipeline **was verified separately and repeatedly** by manually
wedging the vehicle against a static wall obstacle mid-session and observing, via an on-screen
debug HUD (`State` / `RecoveryAttempts` / `Strikes`), the state machine correctly cycling
`Patrolling → Recovering → Patrolling`, escalating to a teleport only when the recovery attempt
cap was hit and no player had line-of-sight. Given more time, the next thing I'd do is re-run
the 5-minute measurement with the obstacle left in the patrol path, so a single run produces
nonzero numbers across all four metrics at once (see "next 2 days" below).

## What's still wrong or unfinished (honest defects list)

- **Combined measurement run not done**: the 5-minute numbers above come from an obstacle-free
  path; recovery/teleport behavior was verified manually but not in the same automated run
  (see caveat above).
- **Countersteer direction during recovery is a heuristic, not derived from vehicle physics**:
  `TickRecovery` countersteers opposite to the last steering sign applied while patrolling.
  Reversing can swing a vehicle's heading in a less intuitive way than driving forward, and this
  wasn't rigorously verified across all approach angles — it worked in the test cases I tried,
  but I wouldn't claim it's correct for every collision geometry.
- **"Tune" pass done, but not exhaustively audited**: all constants I wrote are `UPROPERTY`s
  (steering, stuck-detection thresholds, recovery timing, teleport clearance) — I did not do a
  final line-by-line grep pass to guarantee zero remaining literals anywhere.
- **Multiplayer sanity (2 players, listen server + client) was not tested this session** — the
  code is written to be server-authoritative (`HasAuthority()` gate at the top of
  `TickComponent`), and the vehicle's own movement replication is inherited from the Chaos
  Vehicle template, but I did not do a live 2-player PIE test to visually confirm client-side
  behavior before time ran out.
- **Stuck detection uses a fixed window/distance threshold** (not adaptive to vehicle speed or
  terrain), so very slow, deliberate cornering near the threshold could in principle still
  false-positive; I tuned it empirically against my test scenario, not analytically.
- **Path invalidation isn't handled**: if a dynamic obstacle appears mid-path after a route has
  already been computed, the bot won't re-path until it either arrives at/re-triggers a target
  or gets physically stuck against the new obstacle — there's no active re-plan on path
  invalidation.
- **Recovery's "steer away" decision doesn't raycast to confirm which side is actually clear** —
  it's a simple heuristic (opposite of last steering sign), not a check of which direction has
  physical room.
- Debug/temporary logging (`UE_LOG` calls marked `--- TEMP DEBUG ---`, and the on-screen HUD via
  `AddOnScreenDebugMessage`) is still present in the shipped code — left in deliberately so
  behavior stays inspectable in the live follow-up, but would be stripped or gated behind a
  cvar for a real production build.

## One thing I'd do next with 2 more days
Re-run and record a single combined 5-minute measurement with the test obstacle left in the
patrol path, so laps/stuck/recovery/teleport all get exercised and counted in one automated
run — and use that as the basis to properly tune `StuckMinDistanceCm` / `StuckStrikesToTriggerRecovery`
analytically (based on the vehicle's actual turning radius and top speed) rather than by feel.
I'd also do the live 2-player listen-server test and raycast-based "which side is clear"
improvement to the recovery countersteer.

## Multiplayer
`UVehicleBotDriver`'s decision-making (pure pursuit, stuck detection, recovery, teleport) is
gated by `HasAuthority()` at the top of `TickComponent`, so it only ever runs on the server;
clients never make independent driving decisions. The resulting transform/physics state is
replicated to clients automatically by the Chaos vehicle movement component the Vehicle
template provides. The `yourbot.Diag` diagnostic only prints on whichever machine's Output Log
you're viewing, so it should be checked on the server/listen-host to see the authoritative
reasoning. As noted above, I did not get to run a live 2-player test this session to visually
confirm this end-to-end.

## Rules acknowledgment
- All code is my own, written with the assistance of an AI assistant (see disclosure below) —
  no wholesale copied tutorial code. I can walk through and explain any line in the live
  follow-up.
- No time was spent on art, sound, UI, or menus — this is a blockout map with a Vehicle
  template mesh, matching the brief's "not an art test" note.

## AI assistant usage disclosure
I used **Claude (Anthropic)** throughout this build — for architectural guidance (patrol/pure
pursuit design, the stuck-detection/recovery state machine, the diagnostic console command),
generating the initial C++ implementation, and for debugging help during development (working
through a navmesh setup issue, a `TSoftObjectPtr` fix for an actor-reference corruption bug
during recompiles, and a build-environment/SDK setup issue early on). I reviewed, tested, and
iterated on all of the resulting code myself in the editor and can explain any line — the
design decisions (two-phase recovery, soft-pointer patrol references, the diagnostic log
format) and their tradeoffs are documented in comments throughout `VehicleBotDriver.h/.cpp`.

## Timeline
- Engine/toolchain setup (UE 5.8, Visual Studio, SDK troubleshooting), project creation from
  the Vehicle template, and initial GitHub repo setup: done first, before any bot code.
- Remaining time (~6 hours total from project creation to this submission) went into: Drive
  (pure pursuit patrol), Recover (stuck detection + physical recovery + gated teleport), and
  Measure (the `yourbot.Diag` console command and a 5-minute measurement run) — in that order,
  matching the brief's requirement order. Tune and full multiplayer verification are partially
  done / not fully verified, as noted in the defects list above.
