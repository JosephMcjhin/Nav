#include "NavigationMathLibrary.h"

FString UNavigationMathLibrary::GetRelativeDirectionText(
    const FVector &Forward, const FVector &Right,
    const FVector &DirectionToTarget) {
  float ForwardDot = FVector::DotProduct(Forward, DirectionToTarget);
  float RightDot = FVector::DotProduct(Right, DirectionToTarget);
  float AngleDegrees =
      FMath::RadiansToDegrees(FMath::Atan2(RightDot, ForwardDot));

  // Dead-zone: within ±15° is "forward", within ±165° flip is "backward"
  if (AngleDegrees >= -15.0f && AngleDegrees < 15.0f) {
    return UTF8_TO_TCHAR(u8"向前");
  } else if (AngleDegrees >= 165.0f || AngleDegrees < -165.0f) {
    return UTF8_TO_TCHAR(u8"向后转");
  } else if (AngleDegrees > 0.0f) {
    int32 Deg = FMath::RoundToInt(AngleDegrees);
    return FString::Printf(TEXT("%s%d%s"), UTF8_TO_TCHAR(u8"向右转 "), Deg,
                           UTF8_TO_TCHAR(u8" 度"));
  } else {
    int32 Deg = FMath::RoundToInt(-AngleDegrees);
    return FString::Printf(TEXT("%s%d%s"), UTF8_TO_TCHAR(u8"向左转 "), Deg,
                           UTF8_TO_TCHAR(u8" 度"));
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
