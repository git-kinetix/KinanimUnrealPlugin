// Copyright Kinetix. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimCurveCompressionCodec.h"
#include "KinanimCurveCompressionCodec.generated.h"

/**
 * 
 */
UCLASS()
class UKinanimCurveCompressionCodec : public UAnimCurveCompressionCodec
{
	GENERATED_BODY()

public:

	virtual void DecompressCurves(const FCompressedAnimSequence& AnimSeq, FBlendedCurve& Curves,
		float CurrentTime) const override;

#if ENGINE_MAJOR_VERSION >=5 && ENGINE_MINOR_VERSION >= 3
	virtual float DecompressCurve(const FCompressedAnimSequence& AnimSeq, FName CurveName, float CurrentTime) const override;
#else
	virtual float DecompressCurve(const FCompressedAnimSequence& AnimSeq, SmartName::UID_Type CurveUID, float CurrentTime) const override;
#endif

	UPROPERTY()
	UAnimSequence* AnimSequence;
};
