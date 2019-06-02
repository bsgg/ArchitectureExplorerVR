
// Fill out your copyright notice in the Description page of Project Settings.


#include "VRCharacter.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "TimerManager.h"
#include "Components/CapsuleComponent.h"
#include "NavigationSystem.h"
#include "Components/PostProcessComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Curves/CurveFloat.h"


#include "Kismet/GameplayStatics.h"
#include "Components/SplineComponent.h"
#include "Components/SplineMeshComponent.h"
#include "XRMotionControllerBase.h"
#include "HandController.h"


// Sets default values
AVRCharacter::AVRCharacter()
{
	VRRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));     
	VRRoot->SetupAttachment(GetRootComponent());

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(VRRoot); 

	 
	DestinationMarker = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("DestinationMarker")); 
	DestinationMarker->SetupAttachment(GetRootComponent());
 	
	TeleportPath = CreateDefaultSubobject<USplineComponent>(TEXT("TeleportPath"));
	TeleportPath->SetupAttachment(VRRoot);

	PostProcessComponent = CreateDefaultSubobject<UPostProcessComponent>(TEXT("PostProcessComponent"));
	PostProcessComponent->SetupAttachment(GetRootComponent());

	PrimaryActorTick.bCanEverTick = true;

}

void AVRCharacter::BeginPlay()
{
	Super::BeginPlay();

	DestinationMarker->SetVisibility(false);

	if (BlinkerMaterialBase != nullptr)
	{
		BlinkerMaterialInstance = UMaterialInstanceDynamic::Create(BlinkerMaterialBase, this);
		PostProcessComponent->AddOrUpdateBlendable(BlinkerMaterialInstance);
	}

	LeftController = GetWorld()->SpawnActor<AHandController>(HandControllerClass);
	RightController = GetWorld()->SpawnActor<AHandController>(HandControllerClass);

	if (LeftController != nullptr)
	{
		LeftController->AttachToComponent(VRRoot, FAttachmentTransformRules::KeepRelativeTransform);
		LeftController->SetHand(FXRMotionControllerBase::LeftHandSourceId);
		LeftController->PairController(RightController);
	}
	
	if (RightController != nullptr)
	{
		RightController->AttachToComponent(VRRoot, FAttachmentTransformRules::KeepRelativeTransform);
		RightController->SetHand(FXRMotionControllerBase::RightHandSourceId);
		RightController->PairController(LeftController);
	}
}

void AVRCharacter::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Offset from last frame and this frame
	FVector NeCameraOffset = Camera->GetComponentLocation() - GetActorLocation();
	NeCameraOffset.Z = 0;

	AddActorWorldOffset(NeCameraOffset);
	VRRoot->AddWorldOffset(-NeCameraOffset);

	UpdateDestinationMarker();

	UpdateBlinkers();
}


void AVRCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	PlayerInputComponent->BindAction(TEXT("Teleport"), EInputEvent::IE_Released, this, &AVRCharacter::BeginTeleport);	

	PlayerInputComponent->BindAction(TEXT("GripLeft"), EInputEvent::IE_Pressed, this, &AVRCharacter::GripLeft);
	PlayerInputComponent->BindAction(TEXT("GripRight"), EInputEvent::IE_Pressed, this, &AVRCharacter::GripRight);

	PlayerInputComponent->BindAction(TEXT("GripLeft"), EInputEvent::IE_Released, this, &AVRCharacter::GripReleaseLeft);
	PlayerInputComponent->BindAction(TEXT("GripRight"), EInputEvent::IE_Released, this, &AVRCharacter::GripReleaseRight);
	

	PlayerInputComponent->BindAxis(TEXT("Forward"), this, &AVRCharacter::MoveForward);
	PlayerInputComponent->BindAxis(TEXT("Right"), this, &AVRCharacter::MoveRight);
}


void AVRCharacter::MoveForward(float throttle)
{
	AddMovementInput(throttle * Camera->GetForwardVector());
}

void AVRCharacter::MoveRight(float throttle)
{
	AddMovementInput(throttle * Camera->GetRightVector());
}

void AVRCharacter::BeginTeleport()
{
	StartFade(0.0f, 1.0f);

	FTimerHandle Handle;
	GetWorldTimerManager().SetTimer(Handle, this, &AVRCharacter::FinishTeleport, TeleportFadeTime);
	
}

void AVRCharacter::FinishTeleport()
{
	FVector Destination = DestinationMarker->GetComponentLocation();
	Destination += GetCapsuleComponent()->GetScaledCapsuleHalfHeight() * GetActorUpVector();
	SetActorLocation(Destination);

	StartFade(1.0f, 0.0f);

}

void AVRCharacter::StartFade(float FromAlpha, float ToAlpha)
{
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (PC != nullptr)
	{
		PC->PlayerCameraManager->StartCameraFade(FromAlpha, ToAlpha, TeleportFadeTime, FLinearColor::Black);
	}
}

bool AVRCharacter::FindTeleportDestination(TArray<FVector> &OutPath, FVector &OutLocation)
{

	FVector Start = RightController->GetActorLocation();
	FVector Look = RightController->GetActorForwardVector();

	FPredictProjectilePathParams Params(
		TeleportProjectileRadius,
		Start,
		Look * TeleportProjectileSpeed,
		TeleportSimulationTime,
		ECollisionChannel::ECC_Visibility,
		this
	);

	//Params.DrawDebugType = EDrawDebugTrace::ForOneFrame;
	Params.bTraceComplex = true;

	FPredictProjectilePathResult Result;
	bool bHit = UGameplayStatics::PredictProjectilePath(this, Params, Result);

	if (!bHit) return false;

	UNavigationSystemV1* NavSystem = Cast<UNavigationSystemV1>(GetWorld()->GetNavigationSystem());

	for (FPredictProjectilePathPointData PointData : Result.PathData)
	{
		OutPath.Add(PointData.Location);
	}

	FNavLocation NavLocation;
	bool bOnNavMesh = NavSystem->ProjectPointToNavigation(Result.HitResult.Location, NavLocation, TeleportProjectionExtent);
	if (!bOnNavMesh) return false;

	OutLocation = NavLocation.Location;

	return true;

}

void AVRCharacter::UpdateDestinationMarker()
{
	TArray<FVector> Path;

	FVector Location;
	bool bHasDestination = FindTeleportDestination(Path, Location);

	if (bHasDestination)
	{
		DestinationMarker->SetVisibility(true);
		DestinationMarker->SetWorldLocation(Location);

		DrawTeleportPath(Path);
	}
	else
	{
		DestinationMarker->SetVisibility(false);

		TArray<FVector> EmptyPath;
		DrawTeleportPath(EmptyPath);
	}
}


void AVRCharacter::UpdateBlinkers()
{
	if (RadiusVsVelocity == nullptr) return;

	float Speed = GetVelocity().Size();
	float Radius = RadiusVsVelocity->GetFloatValue(Speed);

	if (BlinkerMaterialInstance == nullptr) return;

	BlinkerMaterialInstance->SetScalarParameterValue(TEXT("Radius"), Radius);

	FVector2D Centre = GetBlinkerCentre();

	BlinkerMaterialInstance->SetVectorParameterValue(TEXT("Centre"), FLinearColor(Centre.X, Centre.Y, 0.0f));	
}

void AVRCharacter::DrawTeleportPath(const TArray<FVector> &Path)
{
	UpdateSpline(Path);

	for (USplineMeshComponent * SplineMesh : TeleportPathMeshPool)
	{
		SplineMesh->SetVisibility(false);
	}

	int32 SegmentNumber = Path.Num() - 1;
	for (int32 i = 0; i < SegmentNumber; ++i)
	{
		if (TeleportPathMeshPool.Num() <= i)
		{
			USplineMeshComponent * SplineMesh = NewObject<USplineMeshComponent>(this);
			SplineMesh->SetMobility(EComponentMobility::Movable);
			SplineMesh->AttachToComponent(VRRoot, FAttachmentTransformRules::KeepRelativeTransform);
			SplineMesh->SetStaticMesh(TeleportArchMesh);
			SplineMesh->SetMaterial(0, TeleportArchMaterial);
			SplineMesh->RegisterComponent();

			TeleportPathMeshPool.Add(SplineMesh);
		}

		USplineMeshComponent * SplineMesh = TeleportPathMeshPool[i];
		SplineMesh->SetVisibility(true);

		FVector StartPos, StartTanget, EndPos, EndTangent;
		TeleportPath->GetLocalLocationAndTangentAtSplinePoint(i, StartPos, StartTanget);
		TeleportPath->GetLocalLocationAndTangentAtSplinePoint(i +1, EndPos, EndTangent);
		SplineMesh->SetStartAndEnd(StartPos, StartTanget, EndPos, EndTangent);

		//SplineMesh->SetWorldLocation(Path[i]);
	}
}

void AVRCharacter::UpdateSpline(const TArray<FVector> &Path)
{
	TeleportPath->ClearSplinePoints(false);

	for (int32 i = 0; i < Path.Num(); ++i)
	{
		FVector LocalPosition = TeleportPath->GetComponentTransform().InverseTransformPosition(Path[i]);
		FSplinePoint Point(i, LocalPosition, ESplinePointType::Curve);
		TeleportPath->AddPoint(Point, false);
	}

	TeleportPath->UpdateSpline();
}




FVector2D AVRCharacter::GetBlinkerCentre()
{
	FVector MovementDirection = GetVelocity().GetSafeNormal();

	if (MovementDirection.IsNearlyZero())
	{
		return FVector2D(0.5, 0.5);
	}

	FVector WorldStationaryLocation;

	if (FVector::DotProduct(Camera->GetForwardVector(), MovementDirection) > 0)
	{
		WorldStationaryLocation = Camera->GetComponentLocation() + MovementDirection * 1000;
	}
	else
	{
		WorldStationaryLocation = Camera->GetComponentLocation() - MovementDirection * 1000;
	}

	APlayerController* PC = Cast<APlayerController>(GetController());
	if (PC != nullptr)
	{
		return FVector2D(0.5, 0.5);
	}

	FVector2D ScreenStationaryLocation;
	PC->ProjectWorldLocationToScreen(WorldStationaryLocation, ScreenStationaryLocation);

	int32 SizeX, SizeY;
	PC->GetViewportSize(SizeX, SizeY);
	ScreenStationaryLocation.X /= SizeX;
	ScreenStationaryLocation.Y /= SizeY;


	return ScreenStationaryLocation;
}



