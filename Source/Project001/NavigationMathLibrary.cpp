#include "NavigationMathLibrary.h"

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

FString UNavigationMathLibrary::GetRelativeDirectionPhrase(
    const FVector &PlayerForward, const FVector &PlayerRight,
    const FVector &ToTarget) {
  // 计算目标相对玩家朝向的有符号偏角（度）。
  // +：偏右，-：偏左；用 Atan2(rightDot, forwardDot) 得到稳定的角度。
  const float SignedAngleDeg = FMath::RadiansToDegrees(FMath::Atan2(
      FVector::DotProduct(PlayerRight, ToTarget),
      FVector::DotProduct(PlayerForward, ToTarget)));

  // 8 方位划分（每 45° 一档，|angle| < 22.5° 视为正前方）
  if (FMath::Abs(SignedAngleDeg) < 22.5f) {
    return UTF8_TO_TCHAR(u8"正前方");
  }
  if (SignedAngleDeg >= 22.5f && SignedAngleDeg < 67.5f) {
    return UTF8_TO_TCHAR(u8"右前方");
  }
  if (SignedAngleDeg >= 67.5f && SignedAngleDeg < 112.5f) {
    return UTF8_TO_TCHAR(u8"右侧");
  }
  if (SignedAngleDeg >= 112.5f) {
    return UTF8_TO_TCHAR(u8"右后方");
  }
  if (SignedAngleDeg <= -22.5f && SignedAngleDeg > -67.5f) {
    return UTF8_TO_TCHAR(u8"左前方");
  }
  if (SignedAngleDeg <= -67.5f && SignedAngleDeg > -112.5f) {
    return UTF8_TO_TCHAR(u8"左侧");
  }
  if (SignedAngleDeg <= -112.5f) {
    return UTF8_TO_TCHAR(u8"左后方");
  }
  return UTF8_TO_TCHAR(u8"附近");
}
