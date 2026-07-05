#include "NavigationMathLibrary.h"

namespace {
float GetSignedAngleDegrees(const FVector &Forward, const FVector &Right,
                            const FVector &DirectionToTarget) {
  const float ForwardDot = FVector::DotProduct(Forward, DirectionToTarget);
  const float RightDot = FVector::DotProduct(Right, DirectionToTarget);
  return FMath::RadiansToDegrees(FMath::Atan2(RightDot, ForwardDot));
}

FString GetTurnInstruction(float SignedAngleDegrees) {
  if (SignedAngleDegrees >= -25.0f && SignedAngleDegrees < 25.0f) {
    return UTF8_TO_TCHAR(u8"直走");
  }
  if (SignedAngleDegrees >= 155.0f || SignedAngleDegrees < -155.0f) {
    return UTF8_TO_TCHAR(u8"向后转");
  }
  if (SignedAngleDegrees > 0.0f) {
    return UTF8_TO_TCHAR(u8"向右转");
  }
  return UTF8_TO_TCHAR(u8"向左转");
}

FString GetDirectionPhrase(float SignedAngleDegrees) {
  if (SignedAngleDegrees >= -22.5f && SignedAngleDegrees < 22.5f) {
    return UTF8_TO_TCHAR(u8"前方");
  }
  if (SignedAngleDegrees >= 22.5f && SignedAngleDegrees < 67.5f) {
    return UTF8_TO_TCHAR(u8"右前方");
  }
  if (SignedAngleDegrees >= 67.5f && SignedAngleDegrees < 112.5f) {
    return UTF8_TO_TCHAR(u8"右侧");
  }
  if (SignedAngleDegrees >= 112.5f && SignedAngleDegrees < 157.5f) {
    return UTF8_TO_TCHAR(u8"右后方");
  }
  if (SignedAngleDegrees >= 157.5f || SignedAngleDegrees < -157.5f) {
    return UTF8_TO_TCHAR(u8"后方");
  }
  if (SignedAngleDegrees >= -157.5f && SignedAngleDegrees < -112.5f) {
    return UTF8_TO_TCHAR(u8"左后方");
  }
  if (SignedAngleDegrees >= -112.5f && SignedAngleDegrees < -67.5f) {
    return UTF8_TO_TCHAR(u8"左侧");
  }
  return UTF8_TO_TCHAR(u8"左前方");
}

int32 GetStepCount(float DistanceMeters) {
  return FMath::Max(1, FMath::RoundToInt(DistanceMeters * 3.0f));
}

float GetPathDistanceMeters(const TArray<FVector> &PathPoints,
                            float DistanceScale) {
  float TotalDistance = 0.0f;
  for (int32 i = 0; i < PathPoints.Num() - 1; ++i) {
    TotalDistance += FVector::Dist(PathPoints[i], PathPoints[i + 1]);
  }
  return TotalDistance / 100.0f / DistanceScale;
}
} // namespace

FString UNavigationMathLibrary::GetRelativeDirectionText(
    const FVector &Forward, const FVector &Right,
    const FVector &DirectionToTarget) {
  const float AngleDegrees =
      GetSignedAngleDegrees(Forward, Right, DirectionToTarget);

  if (AngleDegrees >= -15.0f && AngleDegrees < 15.0f) {
    return UTF8_TO_TCHAR(u8"直走");
  }

  float Normalized = AngleDegrees;
  if (Normalized < 0.0f) {
    Normalized += 360.0f;
  }
  int32 ClockHour = FMath::RoundToInt(Normalized / 30.0f) % 12;
  if (ClockHour == 0) {
    ClockHour = 12;
  }

  return FString::Printf(TEXT("%d%s"), ClockHour,
                         UTF8_TO_TCHAR(u8" 点钟方向"));
}

FString UNavigationMathLibrary::GetDestinationDisplayName(FName DestinationTag) {
  const FString Tag = DestinationTag.ToString();
  if (Tag.Equals(TEXT("Door"), ESearchCase::IgnoreCase)) {
    return UTF8_TO_TCHAR(u8"门口");
  }
  if (Tag.Equals(TEXT("Sofa"), ESearchCase::IgnoreCase)) {
    return UTF8_TO_TCHAR(u8"沙发");
  }
  if (Tag.Equals(TEXT("WorkStation"), ESearchCase::IgnoreCase)) {
    return UTF8_TO_TCHAR(u8"工位");
  }
  if (Tag.Equals(TEXT("ConferenceTable"), ESearchCase::IgnoreCase)) {
    return UTF8_TO_TCHAR(u8"会议桌");
  }
  if (Tag.Equals(TEXT("AirConditioner"), ESearchCase::IgnoreCase)) {
    return UTF8_TO_TCHAR(u8"空调附近");
  }
  return Tag;
}

FString UNavigationMathLibrary::GetFullPathDescription(
    const TArray<FVector> &PathPoints, const FVector &CurrentForward,
    const FVector &CurrentRight, float DistanceScale) {
  if (PathPoints.Num() < 2) {
    return TEXT("Arrived");
  }

  FString FullDescription;
  FVector LastForward = CurrentForward;
  FVector LastRight = CurrentRight;
  FVector LastLoc = PathPoints[0];
  const int32 MaxSegments = FMath::Min(PathPoints.Num(), 4);

  for (int32 i = 1; i < MaxSegments; ++i) {
    const FVector SegmentDir = (PathPoints[i] - LastLoc).GetSafeNormal();
    const float SegmentDist =
        FVector::Dist(LastLoc, PathPoints[i]) / 100.0f / DistanceScale;
    const int32 SegmentSteps = GetStepCount(SegmentDist);

    FullDescription += FString::Printf(
        TEXT("[%s %.1f米] "),
        *GetRelativeDirectionText(LastForward, LastRight, SegmentDir),
        SegmentDist);
    if (i < MaxSegments - 1) {
      FullDescription += TEXT("-> ");
    }

    LastForward = SegmentDir;
    LastRight = FVector::CrossProduct(FVector::UpVector, LastForward);
    LastLoc = PathPoints[i];
  }

  if (PathPoints.Num() > 4) {
    FullDescription += TEXT("...");
  }
  return FullDescription;
}

FString UNavigationMathLibrary::BuildRouteOverview(
    const TArray<FVector> &PathPoints, const FVector &CurrentForward,
    const FVector &CurrentRight, float DistanceScale,
    const FString &DestinationName) {
  if (PathPoints.Num() < 2) {
    return DestinationName + UTF8_TO_TCHAR(u8"到了");
  }

  const FVector ToDestination =
      (PathPoints.Last() - PathPoints[0]).GetSafeNormal();
  const float DirectDistanceMeters =
      FVector::Dist(PathPoints[0], PathPoints.Last()) / 100.0f /
      DistanceScale;
  const float DirectAngle =
      GetSignedAngleDegrees(CurrentForward, CurrentRight, ToDestination);

  FString Overview = DestinationName + UTF8_TO_TCHAR(u8"在您") +
                     GetDirectionPhrase(DirectAngle) +
                     FString::Printf(TEXT("%s%.1f%s"),
                                     UTF8_TO_TCHAR(u8"大约"),
                                     DirectDistanceMeters,
                                     UTF8_TO_TCHAR(u8"米。"));

  TArray<FString> Steps;
  FVector LastForward = CurrentForward;
  FVector LastRight = CurrentRight;
  FVector LastPoint = PathPoints[0];
  float AccumulatedStraightMeters = 0.0f;
  const int32 MaxOverviewItems = 5;

  auto FlushStraightStep = [&]() {
    if (AccumulatedStraightMeters <= KINDA_SMALL_NUMBER ||
        Steps.Num() >= MaxOverviewItems) {
      AccumulatedStraightMeters = 0.0f;
      return;
    }

    const int32 StraightSteps = GetStepCount(AccumulatedStraightMeters);
    Steps.Add(FString::Printf(TEXT("%s%.1f%s"), UTF8_TO_TCHAR(u8"直走"),
                              AccumulatedStraightMeters, UTF8_TO_TCHAR(u8"米")));
    AccumulatedStraightMeters = 0.0f;
  };

  for (int32 i = 1; i < PathPoints.Num() && Steps.Num() < MaxOverviewItems;
       ++i) {
    const FVector SegmentVector = PathPoints[i] - LastPoint;
    const float SegmentDistance =
        SegmentVector.Size() / 100.0f / DistanceScale;
    const FVector SegmentDir = SegmentVector.GetSafeNormal();
    if (SegmentDir.IsNearlyZero()) {
      LastPoint = PathPoints[i];
      continue;
    }

    const float SegmentAngle =
        GetSignedAngleDegrees(LastForward, LastRight, SegmentDir);
    const bool bRequiresTurn = FMath::Abs(SegmentAngle) >= 25.0f;

    if (bRequiresTurn) {
      FlushStraightStep();
      if (Steps.Num() < MaxOverviewItems) {
        Steps.Add(GetTurnInstruction(SegmentAngle));
      }
      LastForward = SegmentDir;
      LastRight = FVector::CrossProduct(FVector::UpVector, LastForward);
    }

    AccumulatedStraightMeters += SegmentDistance;
    LastPoint = PathPoints[i];
  }
  FlushStraightStep();

  if (Steps.Num() == 0) {
    return Overview;
  }

  return Overview + UTF8_TO_TCHAR(u8"您需要") +
         FString::Join(Steps, UTF8_TO_TCHAR(u8"，"));
}