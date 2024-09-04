// Copyright Kinetix. All Rights Reserved.

#pragma once

#include <KinanimTypes.h>

#include "Interfaces/IHttpRequest.h"

#include "CoreMinimal.h"

#include <Kinanim/Private/KinanimImporter.h>
#include <Kinanim/Private/KinanimData.h>
#include <Kinanim/Private/KinanimData.h>

#include "KinanimParser.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogKinanimParser, Log, All);

DECLARE_DELEGATE_OneParam(FOnKinanimDownloadComplete, UKinanimDownloader*);

DECLARE_DELEGATE_OneParam(FOnKinanimPlayAvailable, UKinanimDownloader*);

class UKinanimBoneCompressionCodec;
class UKinanimBonesDataAsset;

#define SDKUSERAGENT TEXT("SDK-Unreal/0.6")


UCLASS()
class KINANIM_API UKinanimDownloader : public UObject
{
	GENERATED_BODY()

public:
	void SetUrl(const FString& InUrl) { Url = InUrl; }
	void SetImporter(void** InImporter) { Importer = (KinanimImporter*)(*InImporter); }

	bool DownloadRemainingFrames();

	void LoadBatchFrameKinanim();

	void OnRequestComplete(
		TSharedPtr<IHttpRequest> HttpRequest,
		TSharedPtr<IHttpResponse> HttpResponse, bool bSuccess);

	FOnKinanimPlayAvailable OnKinanimPlayAvailable;
	FOnKinanimDownloadComplete OnKinanimDownloadComplete;
	
	bool bBlendshapesEnabled;

	void* GetImporter() const;
	void* GetUncompressedHeader() const;
	void* GetContent() const;
	FFrameData* GetFrames() const;
	int32 GetFrameCount() const;
	
	void SetAnimationMetadataID(const FGuid& InID);
	const FGuid& GetAnimationMetadataID() const;
	
	void SetBlendshapesEnabled(bool& bInBlendshapesEnabled);

	UAnimSequence* GetAnimationSequence() const;

	void SetupAnimSequence(USkeletalMesh* SkeletalMesh, const UKinanimBonesDataAsset* InBoneMapping);

private:
	FString Url;
	KinanimImporter* Importer;

	// Used for the exporter
	FKinanimHeader* UncompressedHeader;
	FKinanimContent* FinalContent;

	int32 FrameCount;
	int32 ChunkCount;
	int32 CurrentChunk;
	int32 MaxFrameDownloaded;
	int32 MaxFrameUncompressed;
	int32 MinFrameDownloaded;
	int32 MinFrameUncompressed;

	FGuid AnimationMetadataID;

	UPROPERTY()
	UKinanimBoneCompressionCodec* CompressionCodec;


	/**
	 * 
	 */
	UPROPERTY()
	UAnimSequence* AnimSequence;

	UPROPERTY()
	const UKinanimBonesDataAsset* BoneMapping;

	TArray<FFrameData> Frames;
};

/**
 * 
 */
UCLASS(BlueprintType, Blueprintable)
class KINANIM_API UKinanimParser : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	static FString KinanimEnumToSamBone(EKinanimTransform KinanimTransform);
	static FString KinanimEnumToMorphTarget(EKinanimBlendshape KinanimBlendshape);

	static FTransform ToUnrealTransform(const FTransformData& TrData);

	static UAnimSequence* LoadSkeletalAnimationFromStream(
		USkeletalMesh* SkeletalMesh, void* Stream, const UKinanimBonesDataAsset* InBoneMapping, bool bEnableBlandshapes);

	static UAnimSequence* LoadSkeletalAnimationFromImporter(
		USkeletalMesh* SkeletalMesh, void* Importer, const UKinanimBonesDataAsset* InBoneMapping);

	static bool LoadStartDataFromStream(UObject* WorldContext, void* stream, void** OutImporter);

	static bool DownloadRemainingFrames(UObject* WorldContext, void* stream, const FString& Url, void** Importer);
	
	static bool GetByteArrayFromStream(void* InImporter, TArray<uint8>& OutResult);

	// static bool LoadBatchFrameKinanim(void* stream,
	//                                   const FString& Url, const int FrameCount, void* Importer);
};
