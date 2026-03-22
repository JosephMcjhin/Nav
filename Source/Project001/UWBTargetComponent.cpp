#include "UWBTargetComponent.h"
#include "Engine/Engine.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/KismetMathLibrary.h"

UUWBTargetComponent::UUWBTargetComponent() {
  PrimaryComponentTick.bCanEverTick = true;
  PrimaryComponentTick.bStartWithTickEnabled = true;
}

void UUWBTargetComponent::BeginPlay() { Super::BeginPlay(); }

void UUWBTargetComponent::TickComponent(
    float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction *ThisTickFunction) {
  Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

  if (!bHasTarget || !bIsCalibrated)
    return;

  AActor *Owner = GetOwner();
  if (!Owner)
    return;

  FVector CurrentLoc = Owner->GetActorLocation();
  FVector TargetLoc(TargetX, TargetY, CurrentLoc.Z);
  FVector Delta = TargetLoc - CurrentLoc;

  float DistSq = Delta.SizeSquared2D();

  // Already close enough – nothing to do
  if (DistSq < 16.f) // within ~4 cm
    return;

  float Dist = FMath::Sqrt(DistSq);

  // Drive movement via CharacterMovementComponent so that CMC physics
  // (gravity, collision, ground-following) work correctly instead of
  // fighting against a raw SetActorLocation call.
  ACharacter *Char = Cast<ACharacter>(Owner);
  if (Char) {
    // Normalized XY direction toward target
    FVector Dir = Delta / Dist;
    Dir.Z = 0.f;
    Dir.Normalize();

    if (MoveSpeed <= 0.f) {
      // Instant/teleport mode — stay compatible with sensor snap
      Owner->SetActorLocation(FVector(TargetLoc.X, TargetLoc.Y, CurrentLoc.Z),
                              false, nullptr, ETeleportType::TeleportPhysics);
    } else {
      // Feed a scaled direction into CMC — this is identical to how
      // NavCommandComponent drives the character via AddMovementInput.
      // Scale: clamp so we don't overshoot when very close.
      float Scale = FMath::Min(1.f, Dist / (MoveSpeed * DeltaTime + 1.f));
      Char->AddMovementInput(Dir, Scale);
    }
  } else {
    // Non-character actor: fall back to direct location set
    FVector NewLoc;
    if (MoveSpeed <= 0.f) {
      NewLoc = TargetLoc;
    } else {
      float StepSize = MoveSpeed * DeltaTime;
      FVector Dir = Delta / Dist;
      NewLoc = (StepSize >= Dist) ? TargetLoc : CurrentLoc + Dir * StepSize;
      NewLoc.Z = CurrentLoc.Z;
    }
    Owner->SetActorLocation(NewLoc, false, nullptr,
                            ETeleportType::TeleportPhysics);
  }

  // Debug display — using a member timer to avoid static variable issues
  DebugTimer += DeltaTime;
  if (DebugTimer >= 0.5f) {
    DebugTimer = 0.f;
    if (GEngine) {
      GEngine->AddOnScreenDebugMessage(
          3001, 1.f, FColor::Yellow,
          FString::Printf(TEXT("[UWB] -> X:%.0f Y:%.0f  Dist:%.0f cm"), TargetX,
                          TargetY, Dist));
    }
  }
}

void UUWBTargetComponent::SetUWBTarget(float InX, float InY) {
  TargetX = InX;
  TargetY = InY;
  bHasTarget = true;
}

void UUWBTargetComponent::SetCalibrated(bool bInCalibrated) {
  if (bIsCalibrated != bInCalibrated) {
    bIsCalibrated = bInCalibrated;
    if (GEngine) {
      GEngine->AddOnScreenDebugMessage(
          3002, 5.f, bIsCalibrated ? FColor::Cyan : FColor::Orange,
          FString::Printf(TEXT("[UWB] Calibration State: %s"),
                          bIsCalibrated ? TEXT("READY") : TEXT("NOT READY")));
    }
  }
}
