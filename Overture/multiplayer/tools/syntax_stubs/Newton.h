// syntax-check stub
#pragma once
#include <cstddef>
#define NEWTON_API
#define dFloat float
typedef float dFloat32;
struct NewtonWorld; struct NewtonBody; struct NewtonContact; struct NewtonCollision; struct NewtonJoint; struct NewtonMaterial; struct NewtonUserJoint; struct NewtonMesh; struct NewtonSceneProxy;
typedef struct NewtonWorld NewtonWorld_t;
typedef void  (*NewtonApplyForceAndTorque)(const NewtonBody* body, dFloat timestep, int threadIndex);
typedef void  (*NewtonSetTransform)(const NewtonBody* body, const dFloat* matrix, int threadIndex);
typedef void  (*NewtonBodyDestructor)(const NewtonBody* body);
typedef void  (*NewtonBodyActivationState)(const NewtonBody* body, unsigned state);
typedef void  (*NewtonBodyLeaveWorld)(const NewtonBody* body, int threadIndex);
typedef int   (*NewtonOnAABBOverlap)(const NewtonMaterial* material, const NewtonBody* body0, const NewtonBody* body1, int threadIndex);
typedef void  (*NewtonContactsProcess)(const NewtonJoint* contact, dFloat timestep, int threadIndex);
typedef void  (*NewtonContactBegin)(const NewtonMaterial* material, const NewtonBody* body0, const NewtonBody* body1);
typedef int   (*NewtonContactProcess)(const NewtonMaterial* material, const NewtonContact* contact);
typedef void  (*NewtonContactEnd)(const NewtonMaterial* material);
typedef dFloat (*NewtonWorldRayFilterCallback)(const NewtonBody* body, const dFloat* hitNormal, int collisionID, void* userData, dFloat intersetParam);
typedef unsigned (*NewtonWorldRayPrefilterCallback)(const NewtonBody* body, const NewtonCollision* collision, void* userData);
typedef void  (*NewtonBodyIterator)(const NewtonBody* body, void* userData);
typedef void  (*NewtonCollisionIterator)(void* userData, int vertexCount, const dFloat* faceArray, int faceId);
typedef void  (*NewtonUserBilateralCallBack)(const NewtonJoint* userJoint, dFloat timestep, int threadIndex);
typedef void  (*NewtonUserBilateralGetInfoCallBack)(const NewtonJoint* userJoint, void* info);
typedef void  (*NewtonConstraintDestructor)(const NewtonJoint* me);
typedef unsigned (*NewtonBallCallBack)(const NewtonJoint* ball, dFloat timestep);
typedef unsigned (*NewtonHingeCallBack)(const NewtonJoint* hinge, void* desc);
typedef unsigned (*NewtonSliderCallBack)(const NewtonJoint* slider, void* desc);
typedef unsigned (*NewtonCorkscrewCallBack)(const NewtonJoint* corkscrew, void* desc);
typedef unsigned (*NewtonUniversalCallBack)(const NewtonJoint* universal, void* desc);
typedef void* (*NewtonAllocMemory)(int sizeInBytes);
typedef void (*NewtonFreeMemory)(void* ptr, int sizeInBytes);
typedef void (*NewtonSerialize)(void* serializeHandle, const void* buffer, size_t size);
typedef void (*NewtonDeserialize)(void* serializeHandle, void* buffer, size_t size);
typedef void (*NewtonCollisionTreeRayCastCallback)(const NewtonBody* body, const NewtonCollision* treeCollision, dFloat interception, dFloat* normal, int faceId, void* usedData);
typedef void (*NewtonTreeCollisionCallback)(const NewtonBody* bodyWithTreeCollision, const NewtonBody* body, int faceID, int vertexCount, const dFloat* vertex, int vertexStrideInBytes);
typedef void (*NewtonIslandUpdate)(const NewtonWorld* world, const void* islandHandle, int bodyCount);
typedef void (*NewtonDestroyBodyByExeciveForce)(const NewtonBody* body, const NewtonJoint* contact);
typedef void (*NewtonCollisionDestructor)(const NewtonWorld* world, const NewtonCollision* collision);
typedef int (*NewtonGetBuoyancyPlane)(const int collisionID, void* context, const dFloat* globalSpaceMatrix, dFloat* globalSpacePlane);
typedef struct NewtonHingeSliderUpdateDesc { dFloat m_accel; dFloat m_minFriction; dFloat m_maxFriction; dFloat m_timestep; } NewtonHingeSliderUpdateDesc;
typedef struct NewtonUserMeshCollisionCollideDesc { char pad[256]; } NewtonUserMeshCollisionCollideDesc;
typedef struct NewtonUserMeshCollisionRayHitDesc { char pad[256]; } NewtonUserMeshCollisionRayHitDesc;
typedef struct NewtonCollisionInfoRecord { char pad[512]; } NewtonCollisionInfoRecord;
typedef struct NewtonJointRecord { char pad[512]; } NewtonJointRecord;
// Everything else is declared as a variadic C function so any call site parses.
// (g++ accepts calling a variadic function with any argument list.)
#ifdef __cplusplus
extern "C" {
#endif
#define NSTUB(ret, name) ret name(...);
NSTUB(NewtonWorld*, NewtonCreate) NSTUB(void, NewtonDestroy) NSTUB(void, NewtonDestroyAllBodies) NSTUB(void, NewtonUpdate)
NSTUB(void, NewtonSetSolverModel) NSTUB(void, NewtonSetFrictionModel) NSTUB(void, NewtonSetPlatformArchitecture) NSTUB(void, NewtonSetMinimumFrameRate)
NSTUB(void, NewtonSetWorldSize) NSTUB(void, NewtonSetBodyLeaveWorldEvent) NSTUB(void, NewtonWorldFreezeBody) NSTUB(void, NewtonWorldUnfreezeBody)
NSTUB(void, NewtonWorldForEachBodyDo) NSTUB(void, NewtonWorldForEachBodyInAABBDo) NSTUB(void, NewtonWorldSetUserData) NSTUB(void*, NewtonWorldGetUserData)
NSTUB(int, NewtonWorldGetVersion) NSTUB(void, NewtonWorldRayCast) NSTUB(int, NewtonWorldCollide) NSTUB(void, NewtonWorldSetDestructorCallBack)
NSTUB(int, NewtonWorldGetBodyCount) NSTUB(int, NewtonMaterialGetDefaultGroupID) NSTUB(int, NewtonMaterialCreateGroupID) NSTUB(void, NewtonMaterialDestroyAllGroupID)
NSTUB(void, NewtonMaterialSetDefaultSoftness) NSTUB(void, NewtonMaterialSetDefaultElasticity) NSTUB(void, NewtonMaterialSetDefaultCollidable)
NSTUB(void, NewtonMaterialSetDefaultFriction) NSTUB(void, NewtonMaterialSetContinuousCollisionMode) NSTUB(void, NewtonMaterialSetCollisionCallback)
NSTUB(void, NewtonMaterialSetSurfaceThickness) NSTUB(void*, NewtonMaterialGetUserData) NSTUB(void, NewtonMaterialSetUserData)
NSTUB(void*, NewtonMaterialGetMaterialPairUserData) NSTUB(unsigned, NewtonMaterialGetContactFaceAttribute) NSTUB(unsigned, NewtonMaterialGetBodyCollisionID)
NSTUB(dFloat, NewtonMaterialGetContactNormalSpeed) NSTUB(void, NewtonMaterialGetContactForce) NSTUB(void, NewtonMaterialGetContactPositionAndNormal)
NSTUB(void, NewtonMaterialGetContactTangentDirections) NSTUB(dFloat, NewtonMaterialGetContactTangentSpeed) NSTUB(void, NewtonMaterialSetContactSoftness)
NSTUB(void, NewtonMaterialSetContactElasticity) NSTUB(void, NewtonMaterialSetContactFrictionState) NSTUB(void, NewtonMaterialSetContactStaticFrictionCoef)
NSTUB(void, NewtonMaterialSetContactKineticFrictionCoef) NSTUB(void, NewtonMaterialSetContactFrictionCoef) NSTUB(void, NewtonMaterialSetContactNormalAcceleration)
NSTUB(void, NewtonMaterialSetContactNormalDirection) NSTUB(void, NewtonMaterialSetContactTangentAcceleration) NSTUB(void, NewtonMaterialContactRotateTangentDirections)
NSTUB(void, NewtonMaterialDisableContact) NSTUB(void*, NewtonContactJointGetFirstContact) NSTUB(void*, NewtonContactJointGetNextContact) NSTUB(NewtonMaterial*, NewtonContactGetMaterial)
NSTUB(int, NewtonContactJointGetContactCount) NSTUB(void, NewtonContactJointRemoveContact)
NSTUB(NewtonCollision*, NewtonCreateNull) NSTUB(NewtonCollision*, NewtonCreateSphere) NSTUB(NewtonCollision*, NewtonCreateBox) NSTUB(NewtonCollision*, NewtonCreateCone)
NSTUB(NewtonCollision*, NewtonCreateCapsule) NSTUB(NewtonCollision*, NewtonCreateCylinder) NSTUB(NewtonCollision*, NewtonCreateChamferCylinder) NSTUB(NewtonCollision*, NewtonCreateConvexHull)
NSTUB(NewtonCollision*, NewtonCreateConvexHullModifier) NSTUB(void, NewtonConvexHullModifierGetMatrix) NSTUB(void, NewtonConvexHullModifierSetMatrix)
NSTUB(NewtonCollision*, NewtonCreateCompoundCollision) NSTUB(NewtonCollision*, NewtonCreateUserMeshCollision) NSTUB(NewtonCollision*, NewtonCreateSceneCollision)
NSTUB(void, NewtonConvexCollisionSetUserID) NSTUB(unsigned, NewtonConvexCollisionGetUserID) NSTUB(dFloat, NewtonConvexCollisionCalculateVolume) NSTUB(void, NewtonConvexCollisionCalculateInertialMatrix)
NSTUB(void, NewtonCollisionMakeUnique) NSTUB(void, NewtonReleaseCollision) NSTUB(void, NewtonCollisionSetUserData) NSTUB(void*, NewtonCollisionGetUserData)
NSTUB(void, NewtonCollisionGetInfo) NSTUB(void, NewtonCollisionSetAsTriggerVolume) NSTUB(int, NewtonCollisionIsTriggerVolume)
NSTUB(NewtonCollision*, NewtonCreateTreeCollision) NSTUB(void, NewtonTreeCollisionSetUserRayCastCallback) NSTUB(void, NewtonTreeCollisionBeginBuild) NSTUB(void, NewtonTreeCollisionAddFace)
NSTUB(void, NewtonTreeCollisionEndBuild) NSTUB(int, NewtonTreeCollisionGetFaceAtribute) NSTUB(void, NewtonTreeCollisionSetFaceAtribute) NSTUB(void, NewtonTreeCollisionSerialize)
NSTUB(NewtonCollision*, NewtonCreateTreeCollisionFromSerialization) NSTUB(void, NewtonStaticCollisionSetDebugCallback)
NSTUB(void, NewtonCollisionSerialize) NSTUB(NewtonCollision*, NewtonCreateCollisionFromSerialization)
NSTUB(void, NewtonCollisionCalculateAABB) NSTUB(void, NewtonCollisionForEachPolygonDo) NSTUB(int, NewtonCollisionPointDistance) NSTUB(int, NewtonCollisionClosestPoint) NSTUB(int, NewtonCollisionCollide)
NSTUB(int, NewtonCollisionCollideContinue) NSTUB(dFloat, NewtonCollisionRayCast) NSTUB(void, NewtonCollisionSupportVertex)
NSTUB(NewtonBody*, NewtonCreateBody) NSTUB(void, NewtonDestroyBody) NSTUB(void, NewtonBodyAddForce) NSTUB(void, NewtonBodyAddTorque) NSTUB(void, NewtonBodyAddImpulse) NSTUB(void, NewtonBodyCalculateInverseDynamicsForce)
NSTUB(void, NewtonBodySetMatrix) NSTUB(void, NewtonBodySetMatrixRecursive) NSTUB(void, NewtonBodySetMassMatrix) NSTUB(void, NewtonBodySetMaterialGroupID) NSTUB(void, NewtonBodySetContinuousCollisionMode)
NSTUB(void, NewtonBodySetJointRecursiveCollision) NSTUB(void, NewtonBodySetOmega) NSTUB(void, NewtonBodySetVelocity) NSTUB(void, NewtonBodySetForce) NSTUB(void, NewtonBodySetTorque)
NSTUB(void, NewtonBodySetCentreOfMass) NSTUB(void, NewtonBodySetLinearDamping) NSTUB(void, NewtonBodySetAngularDamping) NSTUB(void, NewtonBodySetUserData) NSTUB(void, NewtonBodySetCollision)
NSTUB(void, NewtonBodySetCollisionWithDrag) NSTUB(void, NewtonBodySetAutoFreeze) NSTUB(void, NewtonBodySetAutoSleep) NSTUB(int, NewtonBodyGetAutoSleep) NSTUB(int, NewtonBodyGetSleepState)
NSTUB(void, NewtonBodySetFreezeTreshold) NSTUB(void, NewtonBodyCoriolisForcesMode) NSTUB(void, NewtonBodySetDestructorCallback) NSTUB(void, NewtonBodySetTransformCallback) NSTUB(void, NewtonBodySetForceAndTorqueCallback)
NSTUB(void, NewtonBodySetAutoactiveCallback) NSTUB(void*, NewtonBodyGetUserData) NSTUB(NewtonWorld*, NewtonBodyGetWorld) NSTUB(NewtonCollision*, NewtonBodyGetCollision) NSTUB(int, NewtonBodyGetMaterialGroupID)
NSTUB(int, NewtonBodyGetContinuousCollisionMode) NSTUB(int, NewtonBodyGetJointRecursiveCollision) NSTUB(void, NewtonBodyGetMatrix) NSTUB(void, NewtonBodyGetRotation) NSTUB(void, NewtonBodyGetMassMatrix) NSTUB(void, NewtonBodyGetInvMass)
NSTUB(void, NewtonBodyGetOmega) NSTUB(void, NewtonBodyGetVelocity) NSTUB(void, NewtonBodyGetForce) NSTUB(void, NewtonBodyGetTorque) NSTUB(void, NewtonBodyGetForceAcc) NSTUB(void, NewtonBodyGetTorqueAcc) NSTUB(void, NewtonBodyGetCentreOfMass)
NSTUB(int, NewtonBodyGetSleepingState) NSTUB(int, NewtonBodyGetAutoFreeze) NSTUB(dFloat, NewtonBodyGetLinearDamping) NSTUB(void, NewtonBodyGetAngularDamping) NSTUB(void, NewtonBodyGetAABB) NSTUB(void, NewtonBodyGetFreezeTreshold)
NSTUB(NewtonJoint*, NewtonBodyGetFirstJoint) NSTUB(NewtonJoint*, NewtonBodyGetNextJoint) NSTUB(NewtonJoint*, NewtonBodyGetFirstContactJoint) NSTUB(NewtonJoint*, NewtonBodyGetNextContactJoint)
NSTUB(void, NewtonBodyAddBuoyancyForce) NSTUB(void, NewtonBodyForEachPolygonDo) NSTUB(void, NewtonAddBodyImpulse) NSTUB(void, NewtonBodyApplyImpulseArray)
NSTUB(NewtonBody*, NewtonJointGetBody0) NSTUB(NewtonBody*, NewtonJointGetBody1) NSTUB(void*, NewtonJointGetUserData) NSTUB(void, NewtonJointSetUserData) NSTUB(void, NewtonJointGetInfo)
NSTUB(int, NewtonJointGetCollisionState) NSTUB(void, NewtonJointSetCollisionState) NSTUB(dFloat, NewtonJointGetStiffness) NSTUB(void, NewtonJointSetStiffness) NSTUB(void, NewtonDestroyJoint) NSTUB(void, NewtonJointSetDestructor)
NSTUB(NewtonJoint*, NewtonConstraintCreateBall) NSTUB(void, NewtonBallSetUserCallback) NSTUB(void, NewtonBallGetJointAngle) NSTUB(void, NewtonBallGetJointOmega) NSTUB(void, NewtonBallGetJointForce) NSTUB(void, NewtonBallSetConeLimits)
NSTUB(NewtonJoint*, NewtonConstraintCreateHinge) NSTUB(void, NewtonHingeSetUserCallback) NSTUB(dFloat, NewtonHingeGetJointAngle) NSTUB(dFloat, NewtonHingeGetJointOmega) NSTUB(void, NewtonHingeGetJointForce) NSTUB(dFloat, NewtonHingeCalculateStopAlpha)
NSTUB(NewtonJoint*, NewtonConstraintCreateSlider) NSTUB(void, NewtonSliderSetUserCallback) NSTUB(dFloat, NewtonSliderGetJointPosit) NSTUB(dFloat, NewtonSliderGetJointVeloc) NSTUB(void, NewtonSliderGetJointForce) NSTUB(dFloat, NewtonSliderCalculateStopAccel)
NSTUB(NewtonJoint*, NewtonConstraintCreateCorkscrew) NSTUB(void, NewtonCorkscrewSetUserCallback) NSTUB(dFloat, NewtonCorkscrewGetJointPosit) NSTUB(dFloat, NewtonCorkscrewGetJointAngle) NSTUB(dFloat, NewtonCorkscrewGetJointVeloc) NSTUB(dFloat, NewtonCorkscrewGetJointOmega)
NSTUB(void, NewtonCorkscrewGetJointForce) NSTUB(dFloat, NewtonCorkscrewCalculateStopAlpha) NSTUB(dFloat, NewtonCorkscrewCalculateStopAccel)
NSTUB(NewtonJoint*, NewtonConstraintCreateUniversal) NSTUB(void, NewtonUniversalSetUserCallback) NSTUB(dFloat, NewtonUniversalGetJointAngle0) NSTUB(dFloat, NewtonUniversalGetJointAngle1) NSTUB(dFloat, NewtonUniversalGetJointOmega0) NSTUB(dFloat, NewtonUniversalGetJointOmega1)
NSTUB(void, NewtonUniversalGetJointForce) NSTUB(dFloat, NewtonUniversalCalculateStopAlpha0) NSTUB(dFloat, NewtonUniversalCalculateStopAlpha1)
NSTUB(NewtonJoint*, NewtonConstraintCreateUpVector) NSTUB(void, NewtonUpVectorGetPin) NSTUB(void, NewtonUpVectorSetPin)
NSTUB(NewtonJoint*, NewtonConstraintCreateUserJoint) NSTUB(void, NewtonUserJointSetFeedbackCollectorCallback) NSTUB(void, NewtonUserJointAddLinearRow) NSTUB(void, NewtonUserJointAddAngularRow) NSTUB(void, NewtonUserJointAddGeneralRow)
NSTUB(void, NewtonUserJointSetRowMinimumFriction) NSTUB(void, NewtonUserJointSetRowMaximumFriction) NSTUB(void, NewtonUserJointSetRowAcceleration) NSTUB(void, NewtonUserJointSetRowSpringDamperAcceleration) NSTUB(void, NewtonUserJointSetRowStiffness) NSTUB(dFloat, NewtonUserJointGetRowForce)
NSTUB(NewtonMesh*, NewtonMeshCreate) NSTUB(void, NewtonMeshDestroy) NSTUB(void, NewtonSetMemorySystem) NSTUB(int, NewtonGetMemoryUsed) NSTUB(void, NewtonSetThreadsCount) NSTUB(int, NewtonGetThreadsCount)
NSTUB(void, NewtonSetMultiThreadSolverOnSingleIsland) NSTUB(void, NewtonSetIslandUpdateEvent) NSTUB(void, NewtonSetDestroyBodyByExeciveForce) NSTUB(void, NewtonSetCollisionDestructor)
NSTUB(void, NewtonWorldCriticalSectionLock) NSTUB(void, NewtonWorldCriticalSectionUnlock) NSTUB(void, NewtonInvalidateCache) NSTUB(void, NewtonCollisionUpdate) NSTUB(void, NewtonBodySetCollisionShape)
#undef NSTUB
#ifdef __cplusplus
}
#endif
