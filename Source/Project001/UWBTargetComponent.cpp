#include "UWBTargetComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/Engine.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"

UUWBTargetComponent::UUWBTargetComponent() {
  PrimaryComponentTick.bCanEverTick = true;
}

void UUWBTargetComponent::TickComponent(
    float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction *ThisTickFunction) {
  Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

  APawn *OwnerPawn = Cast<APawn>(GetOwner());
  if (!OwnerPawn)
    return;

  // --- Force built-in Virtual Joystick to drive the character (Fixes UE5
  // Enhanced Input conflict) ---
  if (APlayerController *PC =
          Cast<APlayerController>(OwnerPawn->GetController())) {
    float JoyY = PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftY);
    float JoyX = PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftX);
    if (FMath::Abs(JoyX) > 0.05f || FMath::Abs(JoyY) > 0.05f) {
      if (PC->PlayerCameraManager) {
        // 利用相机的真实渲染朝向（俯视时：相机的"上"就是地面的"前"）
        FRotator CamRot = PC->PlayerCameraManager->GetCameraRotation();
        FVector CamUp = FRotationMatrix(CamRot).GetUnitAxis(EAxis::Z);
        FVector CamRight = FRotationMatrix(CamRot).GetUnitAxis(EAxis::Y);

        CamUp.Z = 0.0f;
        CamRight.Z = 0.0f;

        OwnerPawn->AddMovementInput(CamUp.GetSafeNormal(), JoyY);
        OwnerPawn->AddMovementInput(CamRight.GetSafeNormal(), JoyX);
      }
      bHasTarget = false; // yield automatic pilot to manual control
    }
  }

  if (!bHasTarget)
    return;

  FVector CurrentLoc = OwnerPawn->GetActorLocation();
  FVector Direction =
      FVector(TargetLocation.X, TargetLocation.Y, CurrentLoc.Z) - CurrentLoc;
  Direction.Z = 0.0f; // Ignore Z axis

  if (Direction.Size2D() > 10.0f) {
    Direction.Normalize();
    OwnerPawn->AddMovementInput(Direction, 1.0f);
  } else {
    bHasTarget = false;
  }

  // Apply rotation if set
  if (bHasRotation) {
    OwnerPawn->SetActorRotation(TargetRotation);
  }
}

void UUWBTargetComponent::SetUWBTarget(float InX, float InY) {
  AActor *Owner = GetOwner();
  if (!Owner)
    return;

  TargetLocation = FVector(InX, InY, Owner->GetActorLocation().Z);
  bHasTarget = true;

  if (GEngine) {
    GEngine->AddOnScreenDebugMessage(
        3001, 1.f, FColor::Yellow,
        FString::Printf(TEXT("[UWB] Moving to target X:%.0f Y:%.0f"), InX,
                        InY));
  }
}

void UUWBTargetComponent::SetUWBRotation(float Yaw) {
  TargetRotation = FRotator(0.0f, Yaw, 0.0f);
  bHasRotation = true;

  if (GEngine) {
    GEngine->AddOnScreenDebugMessage(
        3002, 1.f, FColor::Cyan,
        FString::Printf(TEXT("[UWB] Set rotation Yaw:%.1f"), Yaw));
  }
}
