// Copyright Kinetix. All Rights Reserved.

#include "KinanimWrapper/KinanimParser.h"

#include "HttpModule.h"
#include "IKinanimInterface.h"
#include "InterpoCompression.h"
#include "KinanimTypes.h"
#include "KinanimData.h"
#include "KinanimWrapper.h"

#include "KinanimBoneCompressionCodec.h"
#include "KinanimBonesDataAsset.h"
#include "KinanimCurveCompressionCodec.h"
#include "Interfaces/IHttpResponse.h"

#include "Kismet/KismetSystemLibrary.h"
#include <KinanimImporter.h>

DEFINE_LOG_CATEGORY(LogKinanimParser);

#pragma region KinanimDownloader

bool UKinanimDownloader::DownloadRemainingFrames()
{
	void* result = KinanimWrapper::KinanimImporter_GetResult(Importer);
	if (result == nullptr)
	{
		UE_LOG(LogKinanimParser, Error, TEXT("DownloadRemainingFrames(): Unable to get result from Importer !"));
		return false;
	}

	int RemainingFrames =
		KinanimWrapper::KinanimContent_GetFrameCount(KinanimWrapper::KinanimData_Get_content(result));
	RemainingFrames -= KinanimWrapper::KinanimImporter_GetHighestImportedFrame(Importer) + 1;

	ChunkCount = FMath::CeilToInt(RemainingFrames / (float)InterpoCompression::DEFAULT_BATCH_SIZE);

	FrameCount = KinanimWrapper::KinanimContent_GetFrameCount(
		KinanimWrapper::KinanimData_Get_content(KinanimWrapper::KinanimImporter_GetResult(Importer)));

	CurrentChunk = 0;

	LoadBatchFrameKinanim();

	// MaxFrame = KinanimWrapper::InterpoCompression_GetMaxUncompressedFrame(
	// 	KinanimWrapper::KinanimImporter_Get_compression(Importer));
	// if (MaxFrame == -1)
	// 	MaxFrame = KinanimWrapper::KinanimImporter_GetHighestImportedFrame(Importer);
	//
	// FrameCount = KinanimWrapper::KinanimContent_GetFrameCount(
	// 	KinanimWrapper::KinanimData_Get_content(result));
	//
	// if (MaxFrame >= FrameCount)
	// {
	// 	MaxFrame = FrameCount - 1;
	// }

	return true;
	// Should write on disk but we will not do that
}

void UKinanimDownloader::LoadBatchFrameKinanim()
{
	void* Result = KinanimWrapper::KinanimImporter_GetResult(Importer);

	MinFrameDownloaded = KinanimWrapper::KinanimImporter_GetHighestImportedFrame(Importer) + 1;
	MaxFrameDownloaded = MinFrameDownloaded + InterpoCompression::DEFAULT_BATCH_SIZE - 1;

	void* Header = KinanimWrapper::KinanimData_Get_header(Result);
	void* Content = KinanimWrapper::KinanimData_Get_content(Result);

	uint16 TotalFrameCount = KinanimWrapper::KinanimHeader_GetFrameCount(Header);
	if (MaxFrameDownloaded >= TotalFrameCount)
		MaxFrameDownloaded = TotalFrameCount - 1;

	int64 ByteMin = KinanimWrapper::KinanimHeader_Get_binarySize(
		KinanimWrapper::KinanimData_Get_header(Result)) - 1;

	UE_LOG(LogKinanimParser, Log, TEXT("LoadBatchFrameKinanim(): Downloaded:[%i - %i]"),
	       MinFrameDownloaded, MaxFrameDownloaded);

	int64 ByteMax = ByteMin;

	for (int32 i = 0; i <= MaxFrameDownloaded; ++i)
	{
		if (i < MinFrameDownloaded)
		{
			ByteMin += KinanimWrapper::KinanimHeader_Get_frameSizes(Header, i);
			ByteMax = ByteMin;
		}
		else
		{
			ByteMax += KinanimWrapper::KinanimHeader_Get_frameSizes(Header, i);
		}
	}

	ByteMax -= 1;

	TSharedRef<IHttpRequest> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(Url);
	HttpRequest->SetHeader(TEXT("User-Agent"), SDKUSERAGENT);
	HttpRequest->AppendToHeader("Content-Type", TEXT("application/octet-stream"));
	HttpRequest->SetVerb("GET");
	HttpRequest->AppendToHeader(
		TEXT("Range"),
		FString::Printf(TEXT("bytes=%lld-%lld"), ByteMin, ByteMax));

	HttpRequest->OnProcessRequestComplete().BindUObject(this, &UKinanimDownloader::OnRequestComplete);

	if (!HttpRequest->ProcessRequest())
	{
		UE_LOG(LogKinanimParser, Error, TEXT("LoadBatchFrameKinanim(): Unable to process request !"));
		// return false;
	}

	// return true;
}

void UKinanimDownloader::OnRequestComplete(TSharedPtr<IHttpRequest> HttpRequest, TSharedPtr<IHttpResponse> HttpResponse,
                                           bool bSuccess)
{
	if (!HttpResponse.IsValid()
		|| !EHttpResponseCodes::IsOk(HttpResponse->GetResponseCode()))
		return;

	FString StringResponse = HttpResponse->GetContentAsString();
	if (!StringResponse.IsEmpty())
	{
		UE_LOG(LogKinanimParser, Log, TEXT("OnRequestComplete(): received %s bytes from server"),
		       *StringResponse);
	}

	const TArray<uint8> JsonContent = HttpResponse->GetContent();
	UE_LOG(LogKinanimParser, Log, TEXT("OnRequestComplete(): received %d bytes from server"),
	       JsonContent.Num());

	const char* Datas = reinterpret_cast<const char*>(JsonContent.GetData());
	void* BinaryStream = Kinanim::CreateBinaryStreamFromArray(Datas, JsonContent.Num());
	if (BinaryStream == nullptr)
	{
		UE_LOG(LogKinanimParser, Error, TEXT("LoadBatchFrameKinanim(): Unable to create stream !"));
		return;
	}

	if (Importer == nullptr)
	{
		UE_LOG(LogKinanimParser, Error, TEXT("LoadBatchFrameKinanim(): Failed to create importer !"));
		return;
	}

	MinFrameUncompressed = KinanimWrapper::InterpoCompression_GetMaxUncompressedFrame(
		KinanimWrapper::KinanimImporter_Get_compression(Importer));

	KinanimWrapper::KinanimImporter_ReadFrames(Importer, BinaryStream);

	MaxFrameUncompressed = KinanimWrapper::InterpoCompression_GetMaxUncompressedFrame(
		KinanimWrapper::KinanimImporter_Get_compression(Importer));

	FrameCount = KinanimWrapper::KinanimContent_GetFrameCount(
		KinanimWrapper::KinanimData_Get_content(KinanimWrapper::KinanimImporter_GetResult(Importer)));

	if (MaxFrameUncompressed >= FrameCount)
	{
		MaxFrameUncompressed = FrameCount - 1;
	}

	CurrentChunk += 1;

	const TArray<FTransform> BonesPoses = AnimSequence->GetSkeleton()->GetReferenceSkeleton().GetRefBonePose();
	FKinanimData* data = (FKinanimData*)KinanimWrapper::KinanimImporter_GetResult(Importer);
	if (data == nullptr)
	{
		UE_LOG(LogKinanimParser, Error, TEXT("ERROR! Failed to open kinanim stream !"));
		return;
	}
	
	//Iterate on blendshapes
	if (bBlendshapesEnabled)
	{
		for (uint8 i = 0; i < static_cast<uint8>(EKinanimBlendshape::KB_Count); ++i)
		{
			FName MorphTargetName = FName(UKinanimParser::KinanimEnumToMorphTarget(static_cast<EKinanimBlendshape>(i)));
			TArray<TPair<float, float>> Curves;
			Curves.SetNumZeroed(FrameCount);

#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 3
#if WITH_EDITOR
		FAnimationCurveIdentifier CurveId(MorphTargetName, ERawCurveTrackTypes::RCT_Float);
		AnimSequence->GetController().AddCurve(CurveId);
		FRichCurve RichCurve;
#else
		FRawCurveTracks& CurveTracks = const_cast<FRawCurveTracks&>(AnimSequence->GetCurveData());
		int32 NewCurveIndex = CurveTracks.FloatCurves.Add(FFloatCurve(MorphTargetName, 0));
		FFloatCurve* NewCurve = &CurveTracks.FloatCurves[NewCurveIndex];
		FRichCurve& RichCurve = NewCurve->FloatCurve;
#endif
#else
			FSmartName SmartName;
			if (!AnimSequence->GetSkeleton()->GetSmartNameByName(USkeleton::AnimCurveMappingName, MorphTargetName,
			                                                     SmartName))
			{
				SmartName.DisplayName = MorphTargetName;
				AnimSequence->GetSkeleton()->VerifySmartName(USkeleton::AnimCurveMappingName, SmartName);
			}

#if ENGINE_MAJOR_VERSION > 4
#if WITH_EDITOR
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 2
			FAnimationCurveIdentifier CurveId(SmartName, ERawCurveTrackTypes::RCT_Float);
			AnimSequence->GetController().AddCurve(CurveId);
			FRichCurve RichCurve;
#else
		FAnimationCurveData& RawCurveData = const_cast<FAnimationCurveData&>(AnimSequence->GetDataModel()->GetCurveData());
		int32 NewCurveIndex = RawCurveData.FloatCurves.Add(FFloatCurve(SmartName, 0));
		FFloatCurve* NewCurve = &RawCurveData.FloatCurves[NewCurveIndex];
		FRichCurve& RichCurve = NewCurve->FloatCurve;
#endif
#else
		FRawCurveTracks& CurveTracks = const_cast<FRawCurveTracks&>(AnimSequence->GetCurveData());
		int32 NewCurveIndex = CurveTracks.FloatCurves.Add(FFloatCurve(SmartName, 0));
		FFloatCurve* NewCurve = &CurveTracks.FloatCurves[NewCurveIndex];
		FRichCurve& RichCurve = NewCurve->FloatCurve;
#endif
#else
		AnimSequence->RawCurveData.AddCurveData(SmartName);
		FFloatCurve* NewCurve = (FFloatCurve*)AnimSequence->RawCurveData.GetCurveData(SmartName.UID, ERawCurveTrackTypes::RCT_Float);
		FRichCurve& RichCurve = NewCurve->FloatCurve;
#endif
#endif

			for (int32 j = MinFrameUncompressed; j <= MaxFrameUncompressed; j++)
			{
				FFrameData frame = data->Content->frames[j];

				TPair<float, float> Curve = TPair<float, float>(j / data->Header->frameRate, frame.Blendshapes[i]);
				Curves.Add(Curve);

				FKeyHandle NewKeyHandle = RichCurve.AddKey(j / data->Header->frameRate, frame.Blendshapes[i], false);

				// UE_LOG(LogKinanimParser, Log, TEXT("Blendshape value: %f"), frame.Blendshapes[i]);

				ERichCurveInterpMode NewInterpMode = RCIM_Linear;
				ERichCurveTangentMode NewTangentMode = RCTM_Auto;
				ERichCurveTangentWeightMode NewTangentWeightMode = RCTWM_WeightedNone;

				float LeaveTangent = 0.f;
				float ArriveTangent = 0.f;
				float LeaveTangentWeight = 0.f;
				float ArriveTangentWeight = 0.f;

				RichCurve.SetKeyInterpMode(NewKeyHandle, NewInterpMode);
				RichCurve.SetKeyTangentMode(NewKeyHandle, NewTangentMode);
				RichCurve.SetKeyTangentWeightMode(NewKeyHandle, NewTangentWeightMode);
			}

			// MorphTargetCurves.Add(MorphTargetName, Curves);

			AnimSequence->GetSkeleton()->AccumulateCurveMetaData(MorphTargetName, false, true);

#if !WITH_EDITOR
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 3
		FAnimCompressedCurveIndexedName IndexedName;
		IndexedName.CurveName = MorphTargetName;
		AnimSequence->CompressedData.IndexedCurveNames.Add(IndexedName);
		const_cast<FCurveMetaData*>(
			AnimSequence->GetSkeleton()->GetCurveMetaData(MorphTargetName))->Type.bMorphtarget = true;
#else
		AnimSequence->CompressedData.CompressedCurveNames.Add(SmartName);
		const_cast<FCurveMetaData*>(AnimSequence->GetSkeleton()->GetCurveMetaData(SmartName.UID))->Type.bMorphtarget = true;
#endif
#else
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 2
			AnimSequence->GetController().SetCurveKeys(CurveId, RichCurve.GetConstRefOfKeys());
#endif
#endif
		}
	}

	//Iterate on bones
	for (uint8 i = 0; i < static_cast<uint8>(EKinanimTransform::KT_Count); i++)
	{
		FString TrackName;
		if (!IsValid(BoneMapping))
		{
			//Enum to sam Bone name
			TrackName = UKinanimParser::KinanimEnumToSamBone(static_cast<EKinanimTransform>(i));
		}
		else
		{
			TrackName = BoneMapping->GetBoneNameByIndex(static_cast<EKinanimTransform>(i));
		}

		FName BoneName = FName(TrackName);

		FRawAnimSequenceTrack Track = FRawAnimSequenceTrack();

		//Get and check bone index
		int32 BoneIndex = AnimSequence->GetSkeleton()->GetReferenceSkeleton().FindBoneIndex(BoneName);
		if (!AnimSequence->GetSkeleton()->GetReferenceSkeleton().IsValidIndex(BoneIndex))
		{
			UE_LOG(LogKinanimParser, Log, TEXT("Couldn't find bone '%s'"), *TrackName);
			continue;
		}

		//Get T-Pose bone
		FTransform BoneTransform = BonesPoses[BoneIndex];

		for (int32 j = MinFrameUncompressed; j <= MaxFrameUncompressed; j++)
		{
			FFrameData frame = data->Content->frames[j];
			FTransformData trData = frame.Transforms[i];
			
			FTransform tr;

			Frames[j] = frame;

			tr = UKinanimParser::ToUnrealTransform(trData);
			if (i == 0 && j == 100)
			{
				UE_LOG(LogKinanimParser, Log, TEXT("%s"), *tr.ToHumanReadableString());
			}

			//All the zombie code are tries to get the rotation and stuff working
			if (trData.bHasRotation)
			{
#if WITH_EDITOR
				Track.RotKeys.Add(FQuat4f(tr.GetRotation()));
#else
				CompressionCodec->Tracks[BoneIndex].RotKeys[j] = FQuat4f(tr.GetRotation());
#endif
			}
			else
			{
#if WITH_EDITOR
				Track.RotKeys.Add(FQuat4f(BoneTransform.GetRotation()));
#else
				CompressionCodec->Tracks[BoneIndex].RotKeys[j] = FQuat4f(BoneTransform.GetRotation());
#endif
			}

			if (trData.bHasPosition)
			{
#if WITH_EDITOR
				Track.PosKeys.Add(FVector3f(tr.GetLocation()));
#else
				CompressionCodec->Tracks[BoneIndex].PosKeys[j] = FVector3f(tr.GetLocation());
#endif
			}
			else
			{
#if WITH_EDITOR
				Track.PosKeys.Add(FVector3f(BoneTransform.GetLocation()));
#else
				CompressionCodec->Tracks[BoneIndex].PosKeys[j] = FVector3f(BoneTransform.GetLocation());
#endif
			}

			if (trData.bHasScale)
			{
#if WITH_EDITOR
				Track.ScaleKeys.Add(FVector3f(tr.GetScale3D()));
#else
				CompressionCodec->Tracks[BoneIndex].ScaleKeys[j] = FVector3f(tr.GetScale3D());
#endif
			}
			else
			{
#if WITH_EDITOR
				Track.ScaleKeys.Add(FVector3f(BoneTransform.GetScale3D()));
#else
				CompressionCodec->Tracks[BoneIndex].ScaleKeys[j] = FVector3f(BoneTransform.GetScale3D());
#endif
			}
		}

#if WITH_EDITOR
		if (!AnimSequence->GetController().UpdateBoneTrackKeys(BoneName,
		                                                       FInt32Range(
			                                                       FInt32Range::BoundsType::Inclusive(
				                                                       MinFrameUncompressed),
			                                                       FInt32Range::BoundsType::Inclusive(
				                                                       MaxFrameUncompressed)),
		                                                       Track.PosKeys,
		                                                       Track.RotKeys,
		                                                       Track.ScaleKeys, true))
		{
			UE_LOG(LogKinanimParser, Error,
			       TEXT("Couldn't update bone track '%s' from frame %i to %i with length of %i"),
			       *TrackName, MinFrameUncompressed, MaxFrameUncompressed, Track.PosKeys.Num());
			continue;
		}
		// AnimSequence->GetController().NotifyPopulated();
#else
		// CompressionCodec->Tracks[BoneIndex] = Track;
#endif
	}

#if WITH_EDITOR
	AnimSequence->GetController().CloseBracket(false);
#else

#endif

	if (CurrentChunk >= ChunkCount)
	{
		Importer->ComputeUncompressedFrameSize(0, FrameCount - 1);
		UncompressedHeader = Importer->GetUncompressedHeader();
		FinalContent = Importer->GetResult()->Content;

#if  !WITH_EDITOR
		// CompressionCodec->RemoveFromRoot();
		CompressionCodec = nullptr;
#endif

		AnimSequence->PostLoad();

		OnKinanimDownloadComplete.ExecuteIfBound(this);
		return;
	}

	if (CurrentChunk == FMath::Floor(ChunkCount / 2))
	{
		OnKinanimPlayAvailable.ExecuteIfBound(this);
	}

	LoadBatchFrameKinanim();
}

void* UKinanimDownloader::GetImporter() const
{
	return Importer;
}

void* UKinanimDownloader::GetUncompressedHeader() const
{
	return UncompressedHeader;
}

FFrameData* UKinanimDownloader::GetFrames() const
{
	return Importer->GetResult()->Content->frames;
}

int32 UKinanimDownloader::GetFrameCount() const
{
	return FrameCount;
}

void UKinanimDownloader::SetAnimationMetadataID(const FGuid& InID)
{
	AnimationMetadataID = InID;
}

const FGuid& UKinanimDownloader::GetAnimationMetadataID() const
{
	return AnimationMetadataID;
}

void UKinanimDownloader::SetBlendshapesEnabled(bool& bInBlendshapesEnabled)
{
	bBlendshapesEnabled = bInBlendshapesEnabled;
}

UAnimSequence* UKinanimDownloader::GetAnimationSequence() const
{
	return AnimSequence;
}

void UKinanimDownloader::SetupAnimSequence(USkeletalMesh* SkeletalMesh, const UKinanimBonesDataAsset* InBoneMapping)
{
	FKinanimData* data = (FKinanimData*)KinanimWrapper::KinanimImporter_GetResult(Importer);
	if (data == nullptr)
	{
		UKismetSystemLibrary::PrintString(SkeletalMesh,
		                                  FString::Printf(
			                                  TEXT("ERROR! Failed to open kinanim stream !")), true,
		                                  true, FLinearColor::Red);
		return;
	}

	//Init framecount / frameRate / duration
	FrameCount = data->Header->GetFrameCount();
	if (FrameCount <= 0)
	{
		UKismetSystemLibrary::PrintString(SkeletalMesh,
		                                  FString::Printf(TEXT("ERROR! kinanim file corrupted, 0 frames found !")),
		                                  true,
		                                  true, FLinearColor::Red);
		return;
	}

	Frames.SetNumZeroed(FrameCount);

	FFrameRate FrameRate(data->Header->frameRate, 1);
	float Duration = FrameCount / data->Header->frameRate;

	//Create sequence
	UAnimSequence* NewAnimSequence = NewObject<UAnimSequence>(GetTransientPackage(), NAME_None, RF_Public);

	//Init with mesh
	NewAnimSequence->SetSkeleton(SkeletalMesh->GetSkeleton());
	NewAnimSequence->SetPreviewMesh(SkeletalMesh);

	const TArray<FTransform> BonesPoses = NewAnimSequence->GetSkeleton()->GetReferenceSkeleton().GetRefBonePose();

	// Use reflection to find the property field related to the AnimSequence's duration
	FFloatProperty* FloatProperty = CastField<FFloatProperty>(
		UAnimSequence::StaticClass()->FindPropertyByName(TEXT("SequenceLength")));
	FloatProperty->SetPropertyValue_InContainer(NewAnimSequence, Duration);

	NewAnimSequence->bEnableRootMotion = false;
	NewAnimSequence->RootMotionRootLock = ERootMotionRootLock::RefPose;

	MaxFrameUncompressed = KinanimWrapper::InterpoCompression_GetMaxUncompressedFrame(
		KinanimWrapper::KinanimImporter_Get_compression(Importer));
	if (MaxFrameUncompressed == -1)
		MaxFrameUncompressed = KinanimWrapper::KinanimImporter_GetHighestImportedFrame(Importer);

#if WITH_EDITOR
	NewAnimSequence->GetController().OpenBracket(FText::FromString("kinanimRuntime"), false);
	NewAnimSequence->GetController().InitializeModel();
#else
	CompressionCodec = NewObject<UKinanimBoneCompressionCodec>(NewAnimSequence, TEXT("Kinanim"));
	CompressionCodec->Tracks.AddDefaulted(BonesPoses.Num());
	NewAnimSequence->CompressedData.CompressedTrackToSkeletonMapTable.AddDefaulted(BonesPoses.Num());

	for (int BoneIndex = 0; BoneIndex < BonesPoses.Num(); ++BoneIndex)
	{
		NewAnimSequence->CompressedData.CompressedTrackToSkeletonMapTable[BoneIndex] = BoneIndex;
		for (int FrameIndex = 0; FrameIndex < FrameCount; ++FrameIndex)
		{
			CompressionCodec->Tracks[BoneIndex].PosKeys.Add(FVector3f(BonesPoses[BoneIndex].GetLocation()));
			CompressionCodec->Tracks[BoneIndex].RotKeys.Add(FQuat4f(BonesPoses[BoneIndex].GetRotation()));
			CompressionCodec->Tracks[BoneIndex].ScaleKeys.Add(FVector3f(BonesPoses[BoneIndex].GetScale3D()));
		}
	}
	
	CompressionCodec->AddToRoot();
	NewAnimSequence->AddToRoot();
	
#endif

	//Iterate on blendshapes
	if (bBlendshapesEnabled)
	{
		TMap<FName, TArray<TPair<float, float>>> MorphTargetCurves;
		MorphTargetCurves.Reserve(static_cast<uint8>(EKinanimBlendshape::KB_Count));
		for (uint8 i = 0; i < static_cast<uint8>(EKinanimBlendshape::KB_Count); ++i)
		{
			FName MorphTargetName = FName(UKinanimParser::KinanimEnumToMorphTarget(static_cast<EKinanimBlendshape>(i)));
			TArray<TPair<float, float>> Curves;
			Curves.SetNumZeroed(FrameCount);

#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 3
#if WITH_EDITOR
		FAnimationCurveIdentifier CurveId(MorphTargetName, ERawCurveTrackTypes::RCT_Float);
		NewAnimSequence->GetController().AddCurve(CurveId);
		FRichCurve RichCurve;
#else
		FRawCurveTracks& CurveTracks = const_cast<FRawCurveTracks&>(NewAnimSequence->GetCurveData());
		int32 NewCurveIndex = CurveTracks.FloatCurves.Add(FFloatCurve(MorphTargetName, 0));
		FFloatCurve* NewCurve = &CurveTracks.FloatCurves[NewCurveIndex];
		FRichCurve& RichCurve = NewCurve->FloatCurve;
#endif
#else
			FSmartName SmartName;
			if (!NewAnimSequence->GetSkeleton()->GetSmartNameByName(USkeleton::AnimCurveMappingName, MorphTargetName,
			                                                        SmartName))
			{
				SmartName.DisplayName = MorphTargetName;
				NewAnimSequence->GetSkeleton()->VerifySmartName(USkeleton::AnimCurveMappingName, SmartName);
			}

#if ENGINE_MAJOR_VERSION > 4
#if WITH_EDITOR
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 2
			FAnimationCurveIdentifier CurveId(SmartName, ERawCurveTrackTypes::RCT_Float);
			NewAnimSequence->GetController().AddCurve(CurveId);
			FRichCurve RichCurve;
#else
		FAnimationCurveData& RawCurveData = const_cast<FAnimationCurveData&>(NewAnimSequence->GetDataModel()->GetCurveData());
		int32 NewCurveIndex = RawCurveData.FloatCurves.Add(FFloatCurve(SmartName, 0));
		FFloatCurve* NewCurve = &RawCurveData.FloatCurves[NewCurveIndex];
		FRichCurve& RichCurve = NewCurve->FloatCurve;
#endif
#else
		FRawCurveTracks& CurveTracks = const_cast<FRawCurveTracks&>(NewAnimSequence->GetCurveData());
		int32 NewCurveIndex = CurveTracks.FloatCurves.Add(FFloatCurve(SmartName, 0));
		FFloatCurve* NewCurve = &CurveTracks.FloatCurves[NewCurveIndex];
		FRichCurve& RichCurve = NewCurve->FloatCurve;
#endif
#else
		NewAnimSequence->RawCurveData.AddCurveData(SmartName);
		FFloatCurve* NewCurve = (FFloatCurve*)NewAnimSequence->RawCurveData.GetCurveData(SmartName.UID, ERawCurveTrackTypes::RCT_Float);
		FRichCurve& RichCurve = NewCurve->FloatCurve;
#endif
#endif

			for (int j = 0; j < FrameCount; ++j)
			{
				FFrameData frame = data->Content->frames[FMath::Clamp(j, 0, MaxFrameUncompressed)];

				TPair<float, float> Curve = TPair<float, float>(j / FrameRate.Numerator, frame.Blendshapes[i]);
				Curves.Add(Curve);

				FKeyHandle NewKeyHandle = RichCurve.AddKey(j / FrameRate.Numerator, frame.Blendshapes[i], false);

				// UE_LOG(LogKinanimParser, Log, TEXT("Blendshape value: %f"), frame.Blendshapes[i]);

				ERichCurveInterpMode NewInterpMode = RCIM_Linear;
				ERichCurveTangentMode NewTangentMode = RCTM_Auto;
				ERichCurveTangentWeightMode NewTangentWeightMode = RCTWM_WeightedNone;

				float LeaveTangent = 0.f;
				float ArriveTangent = 0.f;
				float LeaveTangentWeight = 0.f;
				float ArriveTangentWeight = 0.f;

				RichCurve.SetKeyInterpMode(NewKeyHandle, NewInterpMode);
				RichCurve.SetKeyTangentMode(NewKeyHandle, NewTangentMode);
				RichCurve.SetKeyTangentWeightMode(NewKeyHandle, NewTangentWeightMode);
			}

			MorphTargetCurves.Add(MorphTargetName, Curves);

			NewAnimSequence->GetSkeleton()->AccumulateCurveMetaData(MorphTargetName, false, true);

#if !WITH_EDITOR
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 3
		FAnimCompressedCurveIndexedName IndexedName;
		IndexedName.CurveName = MorphTargetName;
		NewAnimSequence->CompressedData.IndexedCurveNames.Add(IndexedName);
		const_cast<FCurveMetaData*>(
			NewAnimSequence->GetSkeleton()->GetCurveMetaData(MorphTargetName))->Type.bMorphtarget = true;
#else
		NewAnimSequence->CompressedData.CompressedCurveNames.Add(SmartName);
		const_cast<FCurveMetaData*>(NewAnimSequence->GetSkeleton()->GetCurveMetaData(SmartName.UID))->Type.bMorphtarget = true;
#endif
#else
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 2
			NewAnimSequence->GetController().SetCurveKeys(CurveId, RichCurve.GetConstRefOfKeys());
#endif
#endif
		}
	}

	//Iterate on bones
	for (uint8 i = 0; i < static_cast<uint8>(EKinanimTransform::KT_Count); i++)
	{
		FString TrackName;
		if (!IsValid(InBoneMapping))
		{
			//Enum to sam Bone name
			TrackName = UKinanimParser::KinanimEnumToSamBone(static_cast<EKinanimTransform>(i));
		}
		else
		{
			TrackName = InBoneMapping->GetBoneNameByIndex(static_cast<EKinanimTransform>(i));
		}

		FName BoneName = FName(TrackName);

		FRawAnimSequenceTrack Track = FRawAnimSequenceTrack();

		//Get and check bone index
		int32 BoneIndex = NewAnimSequence->GetSkeleton()->GetReferenceSkeleton().FindBoneIndex(BoneName);
		if (!NewAnimSequence->GetSkeleton()->GetReferenceSkeleton().IsValidIndex(BoneIndex))
		{
			// UKismetSystemLibrary::PrintString(SkeletalMesh,
			//                                   FString::Printf(TEXT("Couldn't find bone '%s' (id: %d)"),
			//                                                   *TrackName, static_cast<EKinanimTransform>(i)), true,
			//                                   true, FLinearColor::Yellow);
			continue;
		}

		//Get T-Pose bone
		FTransform BoneTransform = BonesPoses[BoneIndex];

		for (int32 j = 0; j < FrameCount; j++)
		{
			FFrameData frame = data->Content->frames[FMath::Clamp(j, 0, MaxFrameUncompressed)];
			FTransformData trData = frame.Transforms[i];
			
			Frames[j] = frame;

			FTransform tr;

			tr = UKinanimParser::ToUnrealTransform(trData);

			//All the zombie code are tries to get the rotation and stuff working
			if (trData.bHasRotation)
			{
				Track.RotKeys.Add(FQuat4f(tr.GetRotation()));
			}
			else
			{
				Track.RotKeys.Add(FQuat4f(BoneTransform.GetRotation()));
			}

			if (trData.bHasPosition)
			{
				Track.PosKeys.Add(FVector3f(tr.GetLocation()));
			}
			else
			{
				Track.PosKeys.Add(FVector3f(BoneTransform.GetLocation()));
			}

			if (trData.bHasScale)
			{
				Track.ScaleKeys.Add(FVector3f(tr.GetScale3D()));
			}
			else
			{
				Track.ScaleKeys.Add(FVector3f(BoneTransform.GetScale3D()));
			}
		}

#if WITH_EDITOR
		NewAnimSequence->GetController().AddBoneCurve(BoneName, false);
		NewAnimSequence->GetController().SetBoneTrackKeys(BoneName,
		                                                  Track.PosKeys, Track.RotKeys, Track.ScaleKeys,
		                                                  false);
#else
		CompressionCodec->Tracks[BoneIndex] = Track;
#endif
	}

#if WITH_EDITOR
	NewAnimSequence->GetController().SetFrameRate(FrameRate, false);
	NewAnimSequence->GetController().SetNumberOfFrames(FrameCount - 1);
	NewAnimSequence->GetController().NotifyPopulated();
	NewAnimSequence->GetController().CloseBracket(false);

#else
	NewAnimSequence->CompressedData.CompressedDataStructure = MakeUnique<FUECompressedAnimData>();
	NewAnimSequence->CompressedData.CompressedDataStructure->CompressedNumberOfKeys = FrameCount;
	NewAnimSequence->CompressedData.BoneCompressionCodec = CompressionCodec;
	UKinanimCurveCompressionCodec* AnimCurveCompressionCodec = NewObject<UKinanimCurveCompressionCodec>();
	AnimCurveCompressionCodec->AnimSequence = AnimSequence;
	NewAnimSequence->CompressedData.CurveCompressionCodec = AnimCurveCompressionCodec;
	// NewAnimSequence->PostLoad();
#endif

	BoneMapping = InBoneMapping;
	AnimSequence = NewAnimSequence;
	AnimSequence->AddToRoot();
}

#pragma endregion

FString UKinanimParser::KinanimEnumToSamBone(EKinanimTransform KinanimTransform)
{
	switch (KinanimTransform)
	{
	case EKinanimTransform::KT_Hips:
		return "Hips";
	case EKinanimTransform::KT_LeftUpperLeg:
		return "LeftThigh";
	case EKinanimTransform::KT_RightUpperLeg:
		return "RightThigh";
	case EKinanimTransform::KT_LeftLowerLeg:
		return "LeftShin";
	case EKinanimTransform::KT_RightLowerLeg:
		return "RightShin";
	case EKinanimTransform::KT_LeftFoot:
		return "LeftFoot";
	case EKinanimTransform::KT_RightFoot:
		return "RightFoot";
	case EKinanimTransform::KT_Spine:
		return "Spine1";
	case EKinanimTransform::KT_Chest:
		return "Spine2";
	case EKinanimTransform::KT_UpperChest:
		return "Spine4";
	case EKinanimTransform::KT_Neck:
		return "Neck";
	case EKinanimTransform::KT_Head:
		return "Head";
	case EKinanimTransform::KT_LeftShoulder:
		return "LeftShoulder";
	case EKinanimTransform::KT_RightShoulder:
		return "RightShoulder";
	case EKinanimTransform::KT_LeftUpperArm:
		return "LeftArm";
	case EKinanimTransform::KT_RightUpperArm:
		return "RightArm";
	case EKinanimTransform::KT_LeftLowerArm:
		return "LeftForeArm";
	case EKinanimTransform::KT_RightLowerArm:
		return "RightForeArm";
	case EKinanimTransform::KT_LeftHand:
		return "LeftHand";
	case EKinanimTransform::KT_RightHand:
		return "RightHand";
	case EKinanimTransform::KT_LeftToes:
		return "LeftToe";
	case EKinanimTransform::KT_RightToes:
		return "RightToe";
	case EKinanimTransform::KT_LeftEye:
		return "LeftEye";
	case EKinanimTransform::KT_RightEye:
		return "RightEye";
	case EKinanimTransform::KT_Jaw:
		return "Jaw";
	case EKinanimTransform::KT_LeftThumbProximal:
		return "LeftFinger1Metacarpal";
	case EKinanimTransform::KT_LeftThumbIntermediate:
		return "LeftFinger1Proximal";
	case EKinanimTransform::KT_LeftThumbDistal:
		return "LeftFinger1Distal";
	case EKinanimTransform::KT_LeftIndexProximal:
		return "LeftFinger2Metacarpal";
	case EKinanimTransform::KT_LeftIndexIntermediate:
		return "LeftFinger2Proximal";
	case EKinanimTransform::KT_LeftIndexDistal:
		return "LeftFinger2Distal";
	case EKinanimTransform::KT_LeftMiddleProximal:
		return "LeftFinger3Metacarpal";
	case EKinanimTransform::KT_LeftMiddleIntermediate:
		return "LeftFinger3Proximal";
	case EKinanimTransform::KT_LeftMiddleDistal:
		return "LeftFinger3Distal";
	case EKinanimTransform::KT_LeftRingProximal:
		return "LeftFinger4Metacarpal";
	case EKinanimTransform::KT_LeftRingIntermediate:
		return "LeftFinger4Proximal";
	case EKinanimTransform::KT_LeftRingDistal:
		return "LeftFinger4Distal";
	case EKinanimTransform::KT_LeftLittleProximal:
		return "LeftFinger5Metacarpal";
	case EKinanimTransform::KT_LeftLittleIntermediate:
		return "LeftFinger5Proximal";
	case EKinanimTransform::KT_LeftLittleDistal:
		return "LeftFinger5Distal";
	case EKinanimTransform::KT_RightThumbProximal:
		return "RightFinger1Metacarpal";
	case EKinanimTransform::KT_RightThumbIntermediate:
		return "RightFinger1Proximal";
	case EKinanimTransform::KT_RightThumbDistal:
		return "RightFinger1Distal";
	case EKinanimTransform::KT_RightIndexProximal:
		return "RightFinger2Metacarpal";
	case EKinanimTransform::KT_RightIndexIntermediate:
		return "RightFinger2Proximal";
	case EKinanimTransform::KT_RightIndexDistal:
		return "RightFinger2Distal";
	case EKinanimTransform::KT_RightMiddleProximal:
		return "RightFinger3Metacarpal";
	case EKinanimTransform::KT_RightMiddleIntermediate:
		return "RightFinger3Proximal";
	case EKinanimTransform::KT_RightMiddleDistal:
		return "RightFinger3Distal";
	case EKinanimTransform::KT_RightRingProximal:
		return "RightFinger4Metacarpal";
	case EKinanimTransform::KT_RightRingIntermediate:
		return "RightFinger4Proximal";
	case EKinanimTransform::KT_RightRingDistal:
		return "RightFinger4Distal";
	case EKinanimTransform::KT_RightLittleProximal:
		return "RightFinger5Metacarpal";
	case EKinanimTransform::KT_RightLittleIntermediate:
		return "RightFinger5Proximal";
	case EKinanimTransform::KT_RightLittleDistal:
		return "RightFinger5Distal";
	default:
		return "";
	}
}

FString UKinanimParser::KinanimEnumToMorphTarget(EKinanimBlendshape KinanimBlendshape)
{
	switch (KinanimBlendshape)
	{
	case EKinanimBlendshape::KB_BrowInnerUp:
		return "BrowInnerUp";
	case EKinanimBlendshape::KB_BrowDownLeft:
		return "BrowDownLeft";
	case EKinanimBlendshape::KB_BrowDownRight:
		return "BrowDownRight";
	case EKinanimBlendshape::KB_BrowOuterUpLeft:
		return "BrowOuterUpLeft";
	case EKinanimBlendshape::KB_BrowOuterUpRight:
		return "BrowOuterUpRight";
	case EKinanimBlendshape::KB_EyeLookUpLeft:
		return "EyeLookUpLeft";
	case EKinanimBlendshape::KB_EyeLookUpRight:
		return "EyeLookUpRight";
	case EKinanimBlendshape::KB_EyeLookDownLeft:
		return "EyeLookDownLeft";
	case EKinanimBlendshape::KB_EyeLookDownRight:
		return "EyeLookDownRight";
	case EKinanimBlendshape::KB_EyeLookInLeft:
		return "EyeLookInLeft";
	case EKinanimBlendshape::KB_EyeLookInRight:
		return "EyeLookInRight";
	case EKinanimBlendshape::KB_EyeLookOutLeft:
		return "EyeLookOutLeft";
	case EKinanimBlendshape::KB_EyeLookOutRight:
		return "EyeLookOutRight";
	case EKinanimBlendshape::KB_EyeBlinkLeft:
		return "EyeBlinkLeft";
	case EKinanimBlendshape::KB_EyeBlinkRight:
		return "EyeBlinkRight";
	case EKinanimBlendshape::KB_EyeSquintRight:
		return "EyeSquintRight";
	case EKinanimBlendshape::KB_EyeSquintLeft:
		return "EyeSquintLeft";
	case EKinanimBlendshape::KB_EyeWideLeft:
		return "EyeWideLeft";
	case EKinanimBlendshape::KB_EyeWideRight:
		return "EyeWideRight";
	case EKinanimBlendshape::KB_CheekPuff:
		return "CheekPuff";
	case EKinanimBlendshape::KB_CheekSquintLeft:
		return "CheekSquintLeft";
	case EKinanimBlendshape::KB_CheekSquintRight:
		return "CheekSquintRight";
	case EKinanimBlendshape::KB_NoseSneerLeft:
		return "NoseSneerLeft";
	case EKinanimBlendshape::KB_NoseSneerRight:
		return "NoseSneerRight";
	case EKinanimBlendshape::KB_JawOpen:
		return "JawOpen";
	case EKinanimBlendshape::KB_JawForward:
		return "JawForward";
	case EKinanimBlendshape::KB_JawLeft:
		return "JawLeft";
	case EKinanimBlendshape::KB_JawRight:
		return "JawRight";
	case EKinanimBlendshape::KB_MouthFunnel:
		return "MouthFunnel";
	case EKinanimBlendshape::KB_MouthPucker:
		return "MouthPucker";
	case EKinanimBlendshape::KB_MouthLeft:
		return "MouthLeft";
	case EKinanimBlendshape::KB_MouthRight:
		return "MouthRight";
	case EKinanimBlendshape::KB_MouthRollUpper:
		return "MouthRollUpper";
	case EKinanimBlendshape::KB_MouthRollLower:
		return "MouthRollLower";
	case EKinanimBlendshape::KB_MouthShrugUpper:
		return "MouthShrugUpper";
	case EKinanimBlendshape::KB_MouthShrugLower:
		return "MouthShrugLower";
	case EKinanimBlendshape::KB_MouthOpen:
		return "MouthOpen";
	case EKinanimBlendshape::KB_MouthClose:
		return "MouthClose";
	case EKinanimBlendshape::KB_MouthSmileLeft:
		return "MouthSmileLeft";
	case EKinanimBlendshape::KB_MouthSmileRight:
		return "MouthSmileRight";
	case EKinanimBlendshape::KB_MouthFrownLeft:
		return "MouthFrownLeft";
	case EKinanimBlendshape::KB_MouthFrownRight:
		return "MouthFrownRight";
	case EKinanimBlendshape::KB_MouthDimpleLeft:
		return "MouthDimpleLeft";
	case EKinanimBlendshape::KB_MouthDimpleRight:
		return "MouthDimpleRight";
	case EKinanimBlendshape::KB_MouthUpperUpLeft:
		return "MouthUpperUpLeft";
	case EKinanimBlendshape::KB_MouthUpperUpRight:
		return "MouthUpperUpRight";
	case EKinanimBlendshape::KB_MouthLowerDownLeft:
		return "MouthLowerDownLeft";
	case EKinanimBlendshape::KB_MouthLowerDownRight:
		return "MouthLowerDownRight";
	case EKinanimBlendshape::KB_MouthPressLeft:
		return "MouthPressLeft";
	case EKinanimBlendshape::KB_MouthPressRight:
		return "MouthPressRight";
	case EKinanimBlendshape::KB_MouthStretchLeft:
		return "MouthStretchLeft";
	case EKinanimBlendshape::KB_MouthStretchRight:
		return "MouthStretchRight";
	case EKinanimBlendshape::KB_TongueOut:
		return "TongueOut";
	default:
		return "";
	}
}

FTransform UKinanimParser::ToUnrealTransform(const FTransformData& TrData)
{
	FTransform ToReturn(
		FQuat(
			/* To unity */
			//( TransformData.rotation.x),
			//(-TransformData.rotation.y),
			//(-TransformData.rotation.z),
			//( TransformData.rotation.w)

			-(TrData.Rotation.X),
			-(-TrData.Rotation.Y),
			(-TrData.Rotation.Z),
			(TrData.Rotation.W)
		),

		FVector(
			/* To unity */
			//(-TransformData.position.x),
			//( TransformData.position.y),
			//( TransformData.position.z)

			-(-TrData.Position.X) * 100,
			-(TrData.Position.Y) * 100,
			(TrData.Position.Z) * 100
		),

		TrData.bHasScale
			? FVector(
				/* To unity */
				//TransformData.scale.x,
				//TransformData.scale.y,
				//TransformData.scale.z

				TrData.Scale.X,
				TrData.Scale.Y,
				TrData.Scale.Z
			)
			: FVector(1, 1, 1)
	);

	return ToReturn;
}

UAnimSequence* UKinanimParser::LoadSkeletalAnimationFromStream(USkeletalMesh* SkeletalMesh, void* Stream,
                                                               const UKinanimBonesDataAsset* InBoneMapping, bool bEnableBlandshapes)
{
	//Default scale and matrice
	const float SceneScale = 100;
	FTransform BaseTransform;
	BaseTransform.SetRotation(FRotator(0.f, 180.f, 0.f).Quaternion());
	BaseTransform.SetScale3D(FVector(-1.f, 1.f, 1.f));
	FMatrix SceneBasis = BaseTransform.ToMatrixWithScale();

	//Import kinanim file
	KinanimImporter* Importer = new KinanimImporter(new InterpoCompression());
	if (Importer == nullptr)
	{
		UKismetSystemLibrary::PrintString(SkeletalMesh,
		                                  FString::Printf(TEXT("ERROR! Failed to open kinanim stream !")), true,
		                                  true, FLinearColor::Red);
		return nullptr;
	}

	KinanimWrapper::KinanimImporter_ReadFile(Importer, Stream);

	FKinanimData* data = (FKinanimData*)KinanimWrapper::KinanimImporter_GetResult(Importer);
	if (data == nullptr)
	{
		UKismetSystemLibrary::PrintString(SkeletalMesh,
		                                  FString::Printf(TEXT("ERROR! Failed to open kinanim stream !")), true,
		                                  true, FLinearColor::Red);
		return nullptr;
	}

	KinanimWrapper::KinanimImporter_ReleaseResult(Importer);
	KinanimWrapper::Delete_KinanimImporter(Importer);
	Importer = nullptr;

	//Init framecount / frameRate / duration
	int32 NumFrames = data->Header->GetFrameCount();
	if (NumFrames <= 0)
	{
		UKismetSystemLibrary::PrintString(SkeletalMesh,
		                                  FString::Printf(TEXT("ERROR! kinanim file corrupted, 0 frames found !")),
		                                  true,
		                                  true, FLinearColor::Red);
		return nullptr;
	}
	FFrameRate FrameRate(data->Header->frameRate, 1);
	float Duration = NumFrames / data->Header->frameRate;

	//Create sequence
	UAnimSequence* AnimSequence = NewObject<UAnimSequence>(GetTransientPackage(), NAME_None, RF_Public);

	//Init with mesh
	AnimSequence->SetSkeleton(SkeletalMesh->GetSkeleton());
	AnimSequence->SetPreviewMesh(SkeletalMesh);

	const TArray<FTransform> BonesPoses = AnimSequence->GetSkeleton()->GetReferenceSkeleton().GetRefBonePose();

	// Use reflection to find the property field related to the AnimSequence's duration
	FFloatProperty* FloatProperty = CastField<FFloatProperty>(
		UAnimSequence::StaticClass()->FindPropertyByName(TEXT("SequenceLength")));
	FloatProperty->SetPropertyValue_InContainer(AnimSequence, Duration);

	AnimSequence->bEnableRootMotion = false;
	AnimSequence->RootMotionRootLock = ERootMotionRootLock::RefPose;


#if WITH_EDITOR
	AnimSequence->GetController().OpenBracket(FText::FromString("kinanimRuntime"), false);
	AnimSequence->GetController().InitializeModel();
#else
	UKinanimBoneCompressionCodec* CompressionCodec = NewObject<UKinanimBoneCompressionCodec>();
	CompressionCodec->Tracks.AddDefaulted(BonesPoses.Num());
	AnimSequence->CompressedData.CompressedTrackToSkeletonMapTable.AddDefaulted(BonesPoses.Num());
	for (int BoneIndex = 0; BoneIndex < BonesPoses.Num(); ++BoneIndex)
	{
		AnimSequence->CompressedData.CompressedTrackToSkeletonMapTable[BoneIndex] = BoneIndex;
		for (int FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
		{
			CompressionCodec->Tracks[BoneIndex].PosKeys.Add(FVector3f(BonesPoses[BoneIndex].GetLocation()));
			CompressionCodec->Tracks[BoneIndex].RotKeys.Add(FQuat4f(BonesPoses[BoneIndex].GetRotation()));
			CompressionCodec->Tracks[BoneIndex].ScaleKeys.Add(FVector3f(BonesPoses[BoneIndex].GetScale3D()));
		}
	}
	
	AnimSequence->AddToRoot();
#endif
	
	//Iterate on bones
	for (uint8 i = 0; i < static_cast<uint8>(EKinanimTransform::KT_Count); i++)
	{
		FString TrackName;
		if (!IsValid(InBoneMapping))
		{
			//Enum to sam Bone name
			TrackName = KinanimEnumToSamBone(static_cast<EKinanimTransform>(i));
		}
		else
		{
			TrackName = InBoneMapping->GetBoneNameByIndex(static_cast<EKinanimTransform>(i));
		}

		FName BoneName = FName(TrackName);

		FRawAnimSequenceTrack Track = FRawAnimSequenceTrack();

		//Get and check bone index
		int32 BoneIndex = AnimSequence->GetSkeleton()->GetReferenceSkeleton().FindBoneIndex(BoneName);
		if (!AnimSequence->GetSkeleton()->GetReferenceSkeleton().IsValidIndex(BoneIndex))
		{
			// UKismetSystemLibrary::PrintString(SkeletalMesh,
			//                                   FString::Printf(TEXT("Couldn't find bone '%s' (id: %d)"),
			//                                                   *TrackName, static_cast<EKinanimTransform>(i)), true,
			//                                   true, FLinearColor::Yellow);
			continue;
		}

		//Get T-Pose bone
		FTransform BoneTransform = BonesPoses[BoneIndex];

		for (int32 j = 0; j < NumFrames; j++)
		{
			FFrameData frame = data->Content->frames[j];
			FTransformData trData = frame.Transforms[i];

			FTransform tr;

			tr = ToUnrealTransform(trData);

			//All the zombie code are tries to get the rotation and stuff working
			if (trData.bHasRotation)
			{
				Track.RotKeys.Add(FQuat4f(tr.GetRotation()));
			}
			else
			{
				Track.RotKeys.Add(FQuat4f(BoneTransform.GetRotation()));
			}

			if (trData.bHasPosition)
			{
				Track.PosKeys.Add(FVector3f(tr.GetLocation()));
			}
			else
			{
				Track.PosKeys.Add(FVector3f(BoneTransform.GetLocation()));
			}

			if (trData.bHasScale)
			{
				Track.ScaleKeys.Add(FVector3f(tr.GetScale3D()));
			}
			else
			{
				Track.ScaleKeys.Add(FVector3f(BoneTransform.GetScale3D()));
			}
		}

#if WITH_EDITOR
		AnimSequence->GetController().AddBoneCurve(BoneName, false);
		AnimSequence->GetController().SetBoneTrackKeys(BoneName, Track.PosKeys, Track.RotKeys, Track.ScaleKeys, false);
#else
		CompressionCodec->Tracks[BoneIndex] = Track;
#endif
	}

	//Iterate on blendshapes
	if (bEnableBlandshapes)
	{
		TMap<FName, TArray<TPair<float, float>>> MorphTargetCurves;
		MorphTargetCurves.Reserve(static_cast<uint8>(EKinanimBlendshape::KB_Count));
		for (uint8 i = 0; i < static_cast<uint8>(EKinanimBlendshape::KB_Count); ++i)
		{
			FName MorphTargetName = FName(KinanimEnumToMorphTarget(static_cast<EKinanimBlendshape>(i)));
			TArray<TPair<float, float>> Curves;
			Curves.SetNumZeroed(NumFrames);

#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 3
#if WITH_EDITOR
		FAnimationCurveIdentifier CurveId(MorphTargetName, ERawCurveTrackTypes::RCT_Float);
		AnimSequence->GetController().AddCurve(CurveId);
		FRichCurve RichCurve;
#else
		FRawCurveTracks& CurveTracks = const_cast<FRawCurveTracks&>(AnimSequence->GetCurveData());
		int32 NewCurveIndex = CurveTracks.FloatCurves.Add(FFloatCurve(MorphTargetName, 0));
		FFloatCurve* NewCurve = &CurveTracks.FloatCurves[NewCurveIndex];
		FRichCurve& RichCurve = NewCurve->FloatCurve;
#endif
#else
			FSmartName SmartName;
			if (!AnimSequence->GetSkeleton()->GetSmartNameByName(USkeleton::AnimCurveMappingName, MorphTargetName,
			                                                     SmartName))
			{
				SmartName.DisplayName = MorphTargetName;
				AnimSequence->GetSkeleton()->VerifySmartName(USkeleton::AnimCurveMappingName, SmartName);
			}

#if ENGINE_MAJOR_VERSION > 4
#if WITH_EDITOR
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 2
			FAnimationCurveIdentifier CurveId(SmartName, ERawCurveTrackTypes::RCT_Float);
			AnimSequence->GetController().AddCurve(CurveId);
			FRichCurve RichCurve;
#else
		FAnimationCurveData& RawCurveData = const_cast<FAnimationCurveData&>(AnimSequence->GetDataModel()->GetCurveData());
		int32 NewCurveIndex = RawCurveData.FloatCurves.Add(FFloatCurve(SmartName, 0));
		FFloatCurve* NewCurve = &RawCurveData.FloatCurves[NewCurveIndex];
		FRichCurve& RichCurve = NewCurve->FloatCurve;
#endif
#else
		FRawCurveTracks& CurveTracks = const_cast<FRawCurveTracks&>(AnimSequence->GetCurveData());
		int32 NewCurveIndex = CurveTracks.FloatCurves.Add(FFloatCurve(SmartName, 0));
		FFloatCurve* NewCurve = &CurveTracks.FloatCurves[NewCurveIndex];
		FRichCurve& RichCurve = NewCurve->FloatCurve;
#endif
#else
		AnimSequence->RawCurveData.AddCurveData(SmartName);
		FFloatCurve* NewCurve = (FFloatCurve*)AnimSequence->RawCurveData.GetCurveData(SmartName.UID, ERawCurveTrackTypes::RCT_Float);
		FRichCurve& RichCurve = NewCurve->FloatCurve;
#endif
#endif

			for (int j = 0; j < NumFrames; ++j)
			{
				FFrameData frame = data->Content->frames[j];

				TPair<float, float> Curve = TPair<float, float>(j / FrameRate.Numerator, frame.Blendshapes[i]);
				Curves.Add(Curve);

				FKeyHandle NewKeyHandle = RichCurve.AddKey(j / FrameRate.Numerator, frame.Blendshapes[i], false);

				// UE_LOG(LogKinanimParser, Log, TEXT("Blendshape value: %f"), frame.Blendshapes[i]);

				ERichCurveInterpMode NewInterpMode = RCIM_Linear;
				ERichCurveTangentMode NewTangentMode = RCTM_Auto;
				ERichCurveTangentWeightMode NewTangentWeightMode = RCTWM_WeightedNone;

				float LeaveTangent = 0.f;
				float ArriveTangent = 0.f;
				float LeaveTangentWeight = 0.f;
				float ArriveTangentWeight = 0.f;

				RichCurve.SetKeyInterpMode(NewKeyHandle, NewInterpMode);
				RichCurve.SetKeyTangentMode(NewKeyHandle, NewTangentMode);
				RichCurve.SetKeyTangentWeightMode(NewKeyHandle, NewTangentWeightMode);
			}

			MorphTargetCurves.Add(MorphTargetName, Curves);

			AnimSequence->GetSkeleton()->AccumulateCurveMetaData(MorphTargetName, false, true);

#if !WITH_EDITOR
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 3
		FAnimCompressedCurveIndexedName IndexedName;
		IndexedName.CurveName = MorphTargetName;
		AnimSequence->CompressedData.IndexedCurveNames.Add(IndexedName);
		const_cast<FCurveMetaData*>(AnimSequence->GetSkeleton()->GetCurveMetaData(MorphTargetName))->Type.bMorphtarget = true;
#else
		AnimSequence->CompressedData.CompressedCurveNames.Add(SmartName);
		const_cast<FCurveMetaData*>(AnimSequence->GetSkeleton()->GetCurveMetaData(SmartName.UID))->Type.bMorphtarget = true;
#endif
#else
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 2
			AnimSequence->GetController().SetCurveKeys(CurveId, RichCurve.GetConstRefOfKeys());
#endif
#endif
		}
	}
	
#if WITH_EDITOR
	AnimSequence->GetController().SetFrameRate(FrameRate, false);
	AnimSequence->GetController().SetNumberOfFrames(NumFrames);
	AnimSequence->GetController().NotifyPopulated();
	AnimSequence->GetController().CloseBracket(false);
#else
	AnimSequence->CompressedData.CompressedDataStructure = MakeUnique<FUECompressedAnimData>();
	AnimSequence->CompressedData.CompressedDataStructure->CompressedNumberOfKeys = NumFrames;
	AnimSequence->CompressedData.BoneCompressionCodec = CompressionCodec;
	UKinanimCurveCompressionCodec* AnimCurveCompressionCodec = NewObject<UKinanimCurveCompressionCodec>();
	AnimCurveCompressionCodec->AnimSequence = AnimSequence;
	AnimSequence->CompressedData.CurveCompressionCodec = AnimCurveCompressionCodec;
	AnimSequence->PostLoad();
#endif

	return AnimSequence;
}

UAnimSequence* UKinanimParser::LoadSkeletalAnimationFromImporter(USkeletalMesh* SkeletalMesh, void* Importer,
                                                                 const UKinanimBonesDataAsset* InBoneMapping)
{
	FKinanimData* data = (FKinanimData*)KinanimWrapper::KinanimImporter_GetResult(Importer);
	if (data == nullptr)
	{
		UKismetSystemLibrary::PrintString(SkeletalMesh,
		                                  FString::Printf(
			                                  TEXT("ERROR! Failed to open kinanim stream !")), true,
		                                  true, FLinearColor::Red);
		return nullptr;
	}

	KinanimWrapper::KinanimImporter_ReleaseResult(Importer);
	KinanimWrapper::Delete_KinanimImporter(Importer);
	Importer = nullptr;

	//Init framecount / frameRate / duration
	int32 NumFrames = data->Header->GetFrameCount();
	if (NumFrames <= 0)
	{
		UKismetSystemLibrary::PrintString(SkeletalMesh,
		                                  FString::Printf(TEXT("ERROR! kinanim file corrupted, 0 frames found !")),
		                                  true,
		                                  true, FLinearColor::Red);
		return nullptr;
	}
	FFrameRate FrameRate(data->Header->frameRate, 1);
	float Duration = NumFrames / data->Header->frameRate;

	//Create sequence
	UAnimSequence* AnimSequence = NewObject<UAnimSequence>(GetTransientPackage(), NAME_None, RF_Public);

	//Init with mesh
	AnimSequence->SetSkeleton(SkeletalMesh->GetSkeleton());
	AnimSequence->SetPreviewMesh(SkeletalMesh);

	const TArray<FTransform> BonesPoses = AnimSequence->GetSkeleton()->GetReferenceSkeleton().GetRefBonePose();

	// Use reflection to find the property field related to the AnimSequence's duration
	FFloatProperty* FloatProperty = CastField<FFloatProperty>(
		UAnimSequence::StaticClass()->FindPropertyByName(TEXT("SequenceLength")));
	FloatProperty->SetPropertyValue_InContainer(AnimSequence, Duration);

	AnimSequence->bEnableRootMotion = false;
	AnimSequence->RootMotionRootLock = ERootMotionRootLock::RefPose;

#if WITH_EDITOR
	AnimSequence->GetController().OpenBracket(FText::FromString("kinanimRuntime"), false);
	AnimSequence->GetController().InitializeModel();
#else
	UKinanimBoneCompressionCodec* CompressionCodec = NewObject<UKinanimBoneCompressionCodec>();
	CompressionCodec->Tracks.AddDefaulted(BonesPoses.Num());
	AnimSequence->CompressedData.CompressedTrackToSkeletonMapTable.AddDefaulted(BonesPoses.Num());
	for (int BoneIndex = 0; BoneIndex < BonesPoses.Num(); ++BoneIndex)
	{
		AnimSequence->CompressedData.CompressedTrackToSkeletonMapTable[BoneIndex] = BoneIndex;
		for (int FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
		{
			CompressionCodec->Tracks[BoneIndex].PosKeys.Add(FVector3f(BonesPoses[BoneIndex].GetLocation()));
			CompressionCodec->Tracks[BoneIndex].RotKeys.Add(FQuat4f(BonesPoses[BoneIndex].GetRotation()));
			CompressionCodec->Tracks[BoneIndex].ScaleKeys.Add(FVector3f(BonesPoses[BoneIndex].GetScale3D()));
		}
	}
	
	AnimSequence->AddToRoot();
#endif

	//Iterate on bones
	for (uint8 i = 0; i < static_cast<uint8>(EKinanimTransform::KT_Count); i++)
	{
		FString TrackName;
		if (!IsValid(InBoneMapping))
		{
			//Enum to sam Bone name
			TrackName = KinanimEnumToSamBone(static_cast<EKinanimTransform>(i));
		}
		else
		{
			TrackName = InBoneMapping->GetBoneNameByIndex(static_cast<EKinanimTransform>(i));
		}

		FName BoneName = FName(TrackName);

		FRawAnimSequenceTrack Track = FRawAnimSequenceTrack();

		//Get and check bone index
		int32 BoneIndex = AnimSequence->GetSkeleton()->GetReferenceSkeleton().FindBoneIndex(BoneName);
		if (!AnimSequence->GetSkeleton()->GetReferenceSkeleton().IsValidIndex(BoneIndex))
		{
			// UKismetSystemLibrary::PrintString(SkeletalMesh,
			//                                   FString::Printf(TEXT("Couldn't find bone '%s' (id: %d)"),
			//                                                   *TrackName, static_cast<EKinanimTransform>(i)), true,
			//                                   true, FLinearColor::Yellow);
			continue;
		}

		//Get T-Pose bone
		FTransform BoneTransform = BonesPoses[BoneIndex];

		for (int32 j = 0; j < NumFrames; j++)
		{
			FFrameData frame = data->Content->frames[j];
			FTransformData trData = frame.Transforms[i];

			FTransform tr;

			tr = ToUnrealTransform(trData);

			//All the zombie code are tries to get the rotation and stuff working
			if (trData.bHasRotation)
			{
				Track.RotKeys.Add(FQuat4f(tr.GetRotation()));
			}
			else
			{
				Track.RotKeys.Add(FQuat4f(BoneTransform.GetRotation()));
			}

			if (trData.bHasPosition)
			{
				Track.PosKeys.Add(FVector3f(tr.GetLocation()));
			}
			else
			{
				Track.PosKeys.Add(FVector3f(BoneTransform.GetLocation()));
			}

			if (trData.bHasScale)
			{
				Track.ScaleKeys.Add(FVector3f(tr.GetScale3D()));
			}
			else
			{
				Track.ScaleKeys.Add(FVector3f(BoneTransform.GetScale3D()));
			}
		}

#if WITH_EDITOR
		AnimSequence->GetController().AddBoneCurve(BoneName, false);
		AnimSequence->GetController().SetBoneTrackKeys(BoneName, Track.PosKeys, Track.RotKeys, Track.ScaleKeys, false);
#else
		CompressionCodec->Tracks[BoneIndex] = Track;
#endif
	}

	//Iterate on blendshapes
	TMap<FName, TArray<TPair<float, float>>> MorphTargetCurves;
	MorphTargetCurves.Reserve(static_cast<uint8>(EKinanimBlendshape::KB_Count));
	for (uint8 i = 0; i < static_cast<uint8>(EKinanimBlendshape::KB_Count); ++i)
	{
		FName MorphTargetName = FName(KinanimEnumToMorphTarget(static_cast<EKinanimBlendshape>(i)));
		TArray<TPair<float, float>> Curves;
		Curves.SetNumZeroed(NumFrames);

#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 3
#if WITH_EDITOR
		FAnimationCurveIdentifier CurveId(MorphTargetName, ERawCurveTrackTypes::RCT_Float);
		AnimSequence->GetController().AddCurve(CurveId);
		FRichCurve RichCurve;
#else
		FRawCurveTracks& CurveTracks = const_cast<FRawCurveTracks&>(AnimSequence->GetCurveData());
		int32 NewCurveIndex = CurveTracks.FloatCurves.Add(FFloatCurve(MorphTargetName, 0));
		FFloatCurve* NewCurve = &CurveTracks.FloatCurves[NewCurveIndex];
		FRichCurve& RichCurve = NewCurve->FloatCurve;
#endif
#else
		FSmartName SmartName;
		if (!AnimSequence->GetSkeleton()->GetSmartNameByName(USkeleton::AnimCurveMappingName, MorphTargetName,
		                                                     SmartName))
		{
			SmartName.DisplayName = MorphTargetName;
			AnimSequence->GetSkeleton()->VerifySmartName(USkeleton::AnimCurveMappingName, SmartName);
		}

#if ENGINE_MAJOR_VERSION > 4
#if WITH_EDITOR
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 2
		FAnimationCurveIdentifier CurveId(SmartName, ERawCurveTrackTypes::RCT_Float);
		AnimSequence->GetController().AddCurve(CurveId);
		FRichCurve RichCurve;
#else
		FAnimationCurveData& RawCurveData = const_cast<FAnimationCurveData&>(AnimSequence->GetDataModel()->GetCurveData());
		int32 NewCurveIndex = RawCurveData.FloatCurves.Add(FFloatCurve(SmartName, 0));
		FFloatCurve* NewCurve = &RawCurveData.FloatCurves[NewCurveIndex];
		FRichCurve& RichCurve = NewCurve->FloatCurve;
#endif
#else
		FRawCurveTracks& CurveTracks = const_cast<FRawCurveTracks&>(AnimSequence->GetCurveData());
		int32 NewCurveIndex = CurveTracks.FloatCurves.Add(FFloatCurve(SmartName, 0));
		FFloatCurve* NewCurve = &CurveTracks.FloatCurves[NewCurveIndex];
		FRichCurve& RichCurve = NewCurve->FloatCurve;
#endif
#else
		AnimSequence->RawCurveData.AddCurveData(SmartName);
		FFloatCurve* NewCurve = (FFloatCurve*)AnimSequence->RawCurveData.GetCurveData(SmartName.UID, ERawCurveTrackTypes::RCT_Float);
		FRichCurve& RichCurve = NewCurve->FloatCurve;
#endif
#endif

		for (int j = 0; j < NumFrames; ++j)
		{
			FFrameData frame = data->Content->frames[j];

			TPair<float, float> Curve = TPair<float, float>(j / FrameRate.Numerator, frame.Blendshapes[i]);
			Curves.Add(Curve);
		}

		MorphTargetCurves.Add(MorphTargetName, Curves);
	}

#if WITH_EDITOR
	AnimSequence->GetController().SetFrameRate(FrameRate, false);
	AnimSequence->GetController().SetNumberOfFrames(NumFrames);
	AnimSequence->GetController().NotifyPopulated();
	AnimSequence->GetController().CloseBracket(false);
#else
	AnimSequence->CompressedData.CompressedDataStructure = MakeUnique<FUECompressedAnimData>();
	AnimSequence->CompressedData.CompressedDataStructure->CompressedNumberOfKeys = NumFrames;
	AnimSequence->CompressedData.BoneCompressionCodec = CompressionCodec;
	UKinanimCurveCompressionCodec* AnimCurveCompressionCodec = NewObject<UKinanimCurveCompressionCodec>();
	AnimCurveCompressionCodec->AnimSequence = AnimSequence;
	AnimSequence->CompressedData.CurveCompressionCodec = AnimCurveCompressionCodec;
	AnimSequence->PostLoad();
#endif

	return AnimSequence;
}

bool UKinanimParser::LoadStartDataFromStream(UObject* WorldContext, void* stream, void** OutImporter)
{
	*OutImporter = KinanimWrapper::Ctor_KinanimImporter(KinanimWrapper::Ctor_InterpoCompression());
	if (*OutImporter == nullptr)
	{
		UKismetSystemLibrary::PrintString(WorldContext,
		                                  FString::Printf(TEXT("ERROR! Failed to open kinanim stream !")), true,
		                                  true, FLinearColor::Red);
		return false;
	}

	KinanimWrapper::KinanimImporter_ReadHeader(*OutImporter, stream);
	KinanimWrapper::KinanimImporter_ReadFrames(*OutImporter, stream);

	void* Result = KinanimWrapper::KinanimImporter_GetResult(*OutImporter);

	void* Header = KinanimWrapper::KinanimData_Get_header(Result);
	// void* Header = KinanimWrapper::KinanimImporter_GetUncompressedHeader(Importer);

	if (Header == nullptr)
	{
		UKismetSystemLibrary::PrintString(WorldContext,
		                                  FString::Printf(TEXT("ERROR! Failed to retreive kinanim header !")), true,
		                                  true, FLinearColor::Red);
		return false;
	}

	return true;
}

bool UKinanimParser::DownloadRemainingFrames(UObject* WorldContext, void* stream, const FString& Url, void** Importer)
{
	void* result = KinanimWrapper::KinanimImporter_GetResult(*Importer);

	int RemainingFrames =
		KinanimWrapper::KinanimContent_GetFrameCount(KinanimWrapper::KinanimData_Get_content(result));
	RemainingFrames -= KinanimWrapper::KinanimImporter_GetHighestImportedFrame(*Importer) + 1;

	int ChunkCount = FMath::CeilToInt(RemainingFrames / (float)InterpoCompression::DEFAULT_BATCH_SIZE);

	int MaxFrame = 0;
	void* Compression = KinanimWrapper::KinanimImporter_Get_compression(*Importer);
	if (Compression == nullptr)
	{
		MaxFrame = KinanimWrapper::KinanimImporter_GetHighestImportedFrame(*Importer);
	}
	else
	{
		MaxFrame = KinanimWrapper::InterpoCompression_GetMaxUncompressedFrame(Compression);
	}

	int FrameCount = KinanimWrapper::KinanimContent_GetFrameCount(
		KinanimWrapper::KinanimData_Get_content(KinanimWrapper::KinanimImporter_GetResult(*Importer)));

	int MinFrame = 0;
	for (int chunk = 0; chunk < ChunkCount; ++chunk)
	{
		MinFrame = KinanimWrapper::InterpoCompression_GetMaxUncompressedFrame(
			KinanimWrapper::KinanimImporter_Get_compression(*Importer));
		if (MinFrame == -1)
			MinFrame = KinanimWrapper::KinanimImporter_GetHighestImportedFrame(*Importer) + 1;

		// LoadBatchFrameKinanim(stream, Url, InterpoCompression::DEFAULT_BATCH_SIZE, *Importer);

		MaxFrame = KinanimWrapper::InterpoCompression_GetMaxUncompressedFrame(
			KinanimWrapper::KinanimImporter_Get_compression(*Importer));
		if (MaxFrame == -1)
			MaxFrame = KinanimWrapper::KinanimImporter_GetHighestImportedFrame(*Importer);

		FrameCount = KinanimWrapper::KinanimContent_GetFrameCount(
			KinanimWrapper::KinanimData_Get_content(result));

		if (MaxFrame >= FrameCount)
		{
			MaxFrame = FrameCount - 1;
		}

		// Should write on disk but we will not do that
	}
	return true;
}

bool UKinanimParser::GetByteArrayFromStream(void* InImporter, TArray<uint8>& OutResult)
{
	TArray<uint8> Result;

	void* UncompressedHeader =
		KinanimWrapper::KinanimImporter_GetUncompressedHeader(InImporter);

	if (UncompressedHeader == nullptr)
	{
		UE_LOG(LogKinanimParser,
		       Error,
		       TEXT("GetByteArrayFromStream: Unable to get uncompressed header from stream"));
		return false;
	}

	int UncompressedHeaderSize = KinanimWrapper::KinanimHeader_Get_binarySize(UncompressedHeader);
	if (UncompressedHeaderSize == 0)
	{
		UE_LOG(LogKinanimParser,
		       Error,
		       TEXT("GetByteArrayFromStream: Unable to get uncompressed header from stream"));
		return false;
	}


	// KinanimWrapper::KinanimExporter_WriteHeader();
	// KinanimWrapper::KinanimExporter_Content_WriteFrames();

	return true;
}

// bool UKinanimParser::LoadBatchFrameKinanim(void* stream,
//                                            const FString& Url, const int FrameCount, void* Importer)
// {
// 	bool bWaitForRequest = true;
//
// 	void* Result = KinanimWrapper::KinanimImporter_GetResult(Importer);
//
// 	uint32 MinFrame = KinanimWrapper::KinanimImporter_GetHighestImportedFrame(Importer) + 1;
// 	uint32 MaxFrame = MinFrame + FrameCount - 1;
//
// 	void* Header = KinanimWrapper::KinanimData_Get_header(Result);
// 	void* Content = KinanimWrapper::KinanimData_Get_content(Result);
// 	uint16 TotalFrameCount = KinanimWrapper::KinanimHeader_GetFrameCount(Header);
// 	if (MaxFrame >= TotalFrameCount)
// 		MaxFrame = TotalFrameCount - 1;
//
// 	int64 ByteMin = KinanimWrapper::KinanimHeader_Get_binarySize(
// 		KinanimWrapper::KinanimData_Get_header(Result)) - 1;
// 	int64 ByteMax = ByteMin;
//
// 	for (uint32 i = 0; i < MaxFrame; ++i)
// 	{
// 		if (i < MinFrame)
// 		{
// 			ByteMin += KinanimWrapper::KinanimHeader_Get_frameSizes(Header, i);
// 			ByteMax = ByteMin;
// 		}
// 		else
// 		{
// 			ByteMax += KinanimWrapper::KinanimHeader_Get_frameSizes(Header, i);
// 		}
// 	}
//
// 	ByteMax -= 1;
//
// 	TSharedRef<IHttpRequest> HttpRequest = FHttpModule::Get().CreateRequest();
// 	HttpRequest->SetURL(Url);
// 	HttpRequest->SetHeader(TEXT("User-Agent"), "X-UnrealEngine-Agent");
// 	HttpRequest->SetHeader("Content-Type", TEXT("application/octet-stream"));
// 	HttpRequest->SetVerb("GET");
// 	HttpRequest->AppendToHeader(
// 		TEXT("Range"),
// 		FString::Printf(TEXT("bytes=%lld-%lld"), ByteMin, ByteMax));
//
// 	HttpRequest->OnProcessRequestComplete().BindLambda(
// 		[&Importer, &stream, &bWaitForRequest](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded)
// 		{
// 			if (!Response.IsValid()
// 				|| !EHttpResponseCodes::IsOk(Response->GetResponseCode()))
// 				return;
//
// 			const TArray<uint8> JsonContent = Response->GetContent();
// 			UE_LOG(LogKinanimParser, Log, TEXT("LoadBatchFrameKinanim(): received %d bytes from server"),
// 			       JsonContent.Num());
//
// 			const char* Datas = reinterpret_cast<const char*>(JsonContent.GetData());
// 			void* BinaryStream = Kinanim::CreateBinaryStreamFromArray(Datas, JsonContent.Num());
// 			if (BinaryStream == nullptr)
// 			{
// 				UE_LOG(LogKinanimParser, Error, TEXT("LoadBatchFrameKinanim(): Unable to create stream !"));
// 				return;
// 			}
//
// 			if (Importer == nullptr)
// 			{
// 				UE_LOG(LogKinanimParser, Error, TEXT("LoadBatchFrameKinanim(): Failed to create importer !"));
// 				return;
// 			}
//
// 			KinanimWrapper::KinanimImporter_ReadFrames(Importer, stream);
//
// 			bWaitForRequest = false;
// 		});
//
// 	if (!HttpRequest->ProcessRequest())
// 	{
// 		UE_LOG(LogKinanimParser, Error, TEXT("LoadBatchFrameKinanim(): Unable to process request !"));
// 		return false;
// 	}
//
// 	TSharedPtr<IHttpResponse> Response = HttpRequest->GetResponse();
// 	do
// 	{
// 		FPlatformProcess::Sleep(0.1f);
// 		Response = HttpRequest->GetResponse();
// 	}
// 	while (Response == nullptr);
//
// 	int32 Code = 0;
// 	while (Code == 0)
// 	{
// 		HttpRequest.Get().Tick(0.1f);
// 		
// 		EHttpRequestStatus::Type status = HttpRequest.Get().GetStatus();
// 		switch (status)
// 		{
// 		case EHttpRequestStatus::NotStarted:
// 			break;
// 		case EHttpRequestStatus::Processing:
// 			FPlatformProcess::Sleep(0.1f);
// 			break;
// 		case EHttpRequestStatus::Failed:
// 			break;
// 		case EHttpRequestStatus::Failed_ConnectionError:
// 			break;
// 		case EHttpRequestStatus::Succeeded:
// 			Code = Response->GetResponseCode();
// 			break;
// 		default: ;
// 		}
// 	}
//
// 	return true;
// }
