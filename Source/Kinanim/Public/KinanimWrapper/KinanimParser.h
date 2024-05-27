// Copyright Kinetix. All Rights Reserved.

#pragma once

#include <KinanimTypes.h>

#include "CoreMinimal.h"

#include "KinanimParser.generated.h"

/**
 * 
 */
UCLASS(BlueprintType, Blueprintable)
class KINANIM_API UKinanimParser : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	static FString KinanimEnumToSamBone(EKinanimTransform KinanimTransform);

	static FTransform ToUnrealTransform(const FTransformData& TrData);

	static UAnimSequence* LoadSkeletalAnimationFromStream(
		USkeletalMesh* SkeletalMesh, void* Stream, const UKinanimBonesDataAsset* InBoneMapping);

	static bool LoadStartDataFromStream(UObject* WorldContext, void* stream);

	static bool DownloadRemainingFrames(UObject* WorldContext, void* stream);

	static bool LoadBatchFrameKinanim(void* stream, const FString& Url, const int FrameCount);
};
