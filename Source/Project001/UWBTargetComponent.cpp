#include "UWBTargetComponent.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/Engine.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"

UUWBTargetComponent::UUWBTargetComponent() {
  PrimaryComponentTick.bCanEverTick = true;
}

void UUWBTargetComponent::BeginPlay() {
  Super::BeginPlay();

  if (ACharacter *OwnerCharacter = Cast<ACharacter>(GetOwner())) {
    if (UCharacterMovementComponent *MoveComp =
            OwnerCharacter->GetCharacterMovement()) {
      MoveComp->bOrientRotationToMovement = false;
      MoveComp->RotationRate = FRotator(0.0f, 540.0f, 0.0f);
      MoveComp->MaxWalkSpeed = DefaultMaxWalkSpeed;
      MoveComp->BrakingDecelerationWalking = 720.0f;
      MoveComp->GroundFriction = 6.0f;
      CachedMoveComp = MoveComp;
    }

    OwnerCharacter->SetActorScale3D(FVector(0.94f, 0.94f, 0.94f));
  }

  if (GetOwner()) {
    SmoothedTargetLocation = GetOwner()->GetActorLocation();
    SmoothedTargetRotation = GetOwner()->GetActorRotation();
  }
}

void UUWBTargetComponent::TickComponent(
    float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction *ThisTickFunction) {
  Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

  APawn *OwnerPawn = Cast<APawn>(GetOwner());
  if (!OwnerPawn) {
    return;
  }

  if (APlayerController *PC =
          Cast<APlayerController>(OwnerPawn->GetController())) {
    const float JoyY = PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftY);
    const float JoyX = PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftX);
    float InX = JoyX;
    float InY = JoyY;
    if (PC->IsInputKeyDown(EKeys::W)) InY += 1.0f;
    if (PC->IsInputKeyDown(EKeys::S)) InY -= 1.0f;
    if (PC->IsInputKeyDown(EKeys::D)) InX += 1.0f;
    if (PC->IsInputKeyDown(EKeys::A)) InX -= 1.0f;
    const float InMag = FMath::Sqrt(InX * InX + InY * InY);
    if (InMag > 1.0f) {
      InX /= InMag;
      InY /= InMag;
    }
    const bool bManualActive =
        FMath::Abs(InX) > 0.05f || FMath::Abs(InY) > 0.05f;

    if (bManualActive) {
      if (PC->PlayerCameraManager) {
        FRotator CamRot = PC->PlayerCameraManager->GetCameraRotation();
        FVector CamUp = FRotationMatrix(CamRot).GetUnitAxis(EAxis::Z);
        FVector CamRight = FRotationMatrix(CamRot).GetUnitAxis(EAxis::Y);

        CamUp.Z = 0.0f;
        CamRight.Z = 0.0f;

        OwnerPawn->AddMovementInput(CamUp.GetSafeNormal(), InY);
        OwnerPawn->AddMovementInput(CamRight.GetSafeNormal(), InX);
      }
      bHasTarget = false;

      if (CachedMoveComp) {
        CachedMoveComp->bOrientRotationToMovement = true;
      }
      bUsingManualInput = true;
    } else if (bUsingManualInput) {
      if (CachedMoveComp) {
        CachedMoveComp->bOrientRotationToMovement = false;
      }
      bUsingManualInput = false;
      SmoothedTargetRotation = OwnerPawn->GetActorRotation();
      TargetRotation = SmoothedTargetRotation;
    }
  }

  if (bHasTarget) {
    SmoothedTargetLocation = FMath::VInterpTo(
        SmoothedTargetLocation, TargetLocation, DeltaTime, TargetSmoothingSpeed);

    const FVector CurrentLoc = OwnerPawn->GetActorLocation();
    FVector Direction =
        FVector(SmoothedTargetLocation.X, SmoothedTargetLocation.Y, CurrentLoc.Z) -
        CurrentLoc;
    Direction.Z = 0.0f;

    const float DistanceCm = Direction.Size2D();
    const double NowSeconds =
        GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
    const bool bHasFreshTargetStream =
        (NowSeconds - LastTargetUpdateTime) <= TargetStreamHoldSeconds;

    if (DistanceCm > AutoMoveDeadZoneCm) {
      Direction.Normalize();
      const float InputScale =
          FMath::GetMappedRangeValueClamped(FVector2D(0.0f, 160.0f),
                                            FVector2D(0.12f, 1.0f), DistanceCm);
      OwnerPawn->AddMovementInput(Direction, InputScale);
    } else if (!bHasFreshTargetStream) {
      bHasTarget = false;
    }
  }

  if (bHasRotation && !bUsingManualInput) {
    SmoothedTargetRotation = FMath::RInterpTo(
        SmoothedTargetRotation, TargetRotation, DeltaTime,
        RotationTargetSmoothingSpeed);

    FRotator CurrentRotation = OwnerPawn->GetActorRotation();
    CurrentRotation.Yaw = FMath::FixedTurn(
        CurrentRotation.Yaw, SmoothedTargetRotation.Yaw,
        RotationInterpSpeedDegPerSec * DeltaTime);
    OwnerPawn->SetActorRotation(CurrentRotation);
  }
}

void UUWBTargetComponent::SetUWBTarget(float InX, float InY) {
  AActor *Owner = GetOwner();
  if (!Owner) {
    return;
  }

  const double CurrentSampleTime =
      GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
  const FVector IncomingTarget(InX, InY, Owner->GetActorLocation().Z);

  if (ACharacter *OwnerCharacter = Cast<ACharacter>(Owner)) {
    if (UCharacterMovementComponent *MoveComp =
            OwnerCharacter->GetCharacterMovement()) {
      const double DeltaSeconds = CurrentSampleTime - LastSpeedSampleTime;
      if (LastSpeedSampleTime > 0.0 && DeltaSeconds > KINDA_SMALL_NUMBER) {
        const float CatchUpDistanceCm =
            FVector::Dist2D(OwnerCharacter->GetActorLocation(), IncomingTarget);
        MoveComp->MaxWalkSpeed =
            static_cast<float>(CatchUpDistanceCm / DeltaSeconds);
      }
    }
  }

  if (bHasTarget) {
    TargetLocation = FMath::Lerp(TargetLocation, IncomingTarget, 0.35f);
  } else {
    TargetLocation = IncomingTarget;
    SmoothedTargetLocation = Owner->GetActorLocation();
  }

  LastTargetUpdateTime = CurrentSampleTime;
  LastSpeedSampleTime = CurrentSampleTime;
  bHasTarget = true;

  if (GEngine) {
    GEngine->AddOnScreenDebugMessage(
        3001, 1.f, FColor::Yellow,
        FString::Printf(TEXT("[UWB] Moving to target X:%.0f Y:%.0f"), InX,
                        InY));
  }
}

void UUWBTargetComponent::SetIMURotation(float Yaw) {
  TargetRotation = FRotator(0.0f, Yaw, 0.0f);
  if (!bHasRotation) {
    SmoothedTargetRotation = TargetRotation;
  }
  bHasRotation = true;

  if (GEngine) {
    GEngine->AddOnScreenDebugMessage(
        3002, 1.f, FColor::Cyan,
        FString::Printf(TEXT("[IMU] Set rotation Yaw:%.1f"), Yaw));
  }
}