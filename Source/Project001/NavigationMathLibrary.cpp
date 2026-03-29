#include "NavigationMathLibrary.h"

FString UNavigationMathLibrary::GetRelativeDirectionText(
    const FVector &Forward, const FVector &Right,
    const FVector &DirectionToTarget) {
  float ForwardDot = FVector::DotProduct(Forward, DirectionToTarget);
  float RightDot = FVector::DotProduct(Right, DirectionToTarget);
  float AngleDegrees =
      FMath::RadiansToDegrees(FMath::Atan2(RightDot, ForwardDot));

  if (AngleDegrees >= -22.5f && AngleDegrees < 22.5f) {
    return UTF8_TO_TCHAR(u8"向前");
  } else if (AngleDegrees >= 22.5f && AngleDegrees < 67.5f) {
    return UTF8_TO_TCHAR(u8"向右前方");
  } else if (AngleDegrees >= 67.5f && AngleDegrees < 112.5f) {
    return UTF8_TO_TCHAR(u8"向右转");
  } else if (AngleDegrees >= 112.5f && AngleDegrees < 157.5f) {
    return UTF8_TO_TCHAR(u8"向右后方");
  } else if (AngleDegrees >= 157.5f || AngleDegrees < -157.5f) {
    return UTF8_TO_TCHAR(u8"向后转");
  } else if (AngleDegrees >= -157.5f && AngleDegrees < -112.5f) {
    return UTF8_TO_TCHAR(u8"向左后方");
  } else if (AngleDegrees >= -112.5f && AngleDegrees < -67.5f) {
    return UTF8_TO_TCHAR(u8"向左转");
  } else {
    return UTF8_TO_TCHAR(u8"向左前方");
  }
}

FString UNavigationMathLibrary::GetFullPathDescription(
    const TArray<FVector> &PathPoints, const FVector &CurrentForward,
    const FVector &CurrentRight, float DistanceScale) {
  if (PathPoints.Num() < 2)
    return TEXT("Arrived");

  FString FullDescription = TEXT("");
  FVector LastForward = CurrentForward;
  FVector LastRight = CurrentRight;
  FVector LastLoc = PathPoints[0];

  int32 MaxSegments = FMath::Min(PathPoints.Num(), 4);

  for (int32 i = 1; i < MaxSegments; i++) {
    FVector SegmentDir = (PathPoints[i] - LastLoc).GetSafeNormal();
    float SegmentDist =
        FVector::Dist(LastLoc, PathPoints[i]) / 100.0f / DistanceScale;

    FString DirText =
        GetRelativeDirectionText(LastForward, LastRight, SegmentDir);
    FullDescription +=
        FString::Printf(TEXT("[%s %.1fm] "), *DirText, SegmentDist);

    if (i < MaxSegments - 1)
      FullDescription += TEXT("-> ");

    LastForward = SegmentDir;
    LastRight = FVector::CrossProduct(FVector::UpVector, LastForward);
    LastLoc = PathPoints[i];
  }

  if (PathPoints.Num() > 4)
    FullDescription += TEXT("...");
  return FullDescription;
}
