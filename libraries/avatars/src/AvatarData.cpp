//
//  AvatarData.cpp
//  libraries/avatars/src
//
//  Created by Stephen Birarda on 4/9/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//


#include "AvatarData.h"

#include <cstdio>
#include <cstring>
#include <stdint.h>

#include <QtCore/QDataStream>
#include <QtCore/QThread>
#include <QtCore/QUuid>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <QVariantGLM.h>
#include <Transform.h>
#include <NetworkAccessManager.h>
#include <NodeList.h>
#include <udt/PacketHeaders.h>
#include <GLMHelpers.h>
#include <StreamUtils.h>
#include <UUID.h>
#include <shared/JSONHelpers.h>
#include <ShapeInfo.h>

#include "AvatarLogging.h"

//#define WANT_DEBUG

quint64 DEFAULT_FILTERED_LOG_EXPIRY = 2 * USECS_PER_SECOND;

using namespace std;

const glm::vec3 DEFAULT_LOCAL_AABOX_CORNER(-0.5f);
const glm::vec3 DEFAULT_LOCAL_AABOX_SCALE(1.0f);

const QString AvatarData::FRAME_NAME = "com.highfidelity.recording.AvatarData";

static const int TRANSLATION_COMPRESSION_RADIX = 12;
static const int SENSOR_TO_WORLD_SCALE_RADIX = 10;
static const int AUDIO_LOUDNESS_RADIX = 2;
static const int MODEL_OFFSET_RADIX = 6;

#define ASSERT(COND)  do { if (!(COND)) { abort(); } } while(0)

AvatarData::AvatarData() :
    SpatiallyNestable(NestableType::Avatar, QUuid()),
    _handPosition(0.0f),
    _targetScale(1.0f),
    _handState(0),
    _keyState(NO_KEY_DOWN),
    _forceFaceTrackerConnected(false),
    _hasNewJointRotations(true),
    _hasNewJointTranslations(true),
    _headData(NULL),
    _displayNameTargetAlpha(1.0f),
    _displayNameAlpha(1.0f),
    _errorLogExpiry(0),
    _owningAvatarMixer(),
    _targetVelocity(0.0f),
    _localAABox(DEFAULT_LOCAL_AABOX_CORNER, DEFAULT_LOCAL_AABOX_SCALE)
{
    setBodyPitch(0.0f);
    setBodyYaw(-90.0f);
    setBodyRoll(0.0f);

    ASSERT(sizeof(AvatarDataPacket::Header) == AvatarDataPacket::HEADER_SIZE);
    ASSERT(sizeof(AvatarDataPacket::AvatarGlobalPosition) == AvatarDataPacket::AVATAR_GLOBAL_POSITION_SIZE);
    ASSERT(sizeof(AvatarDataPacket::AvatarLocalPosition) == AvatarDataPacket::AVATAR_LOCAL_POSITION_SIZE);
    ASSERT(sizeof(AvatarDataPacket::AvatarDimensions) == AvatarDataPacket::AVATAR_DIMENSIONS_SIZE);
    ASSERT(sizeof(AvatarDataPacket::AvatarOrientation) == AvatarDataPacket::AVATAR_ORIENTATION_SIZE);
    ASSERT(sizeof(AvatarDataPacket::AvatarScale) == AvatarDataPacket::AVATAR_SCALE_SIZE);
    ASSERT(sizeof(AvatarDataPacket::LookAtPosition) == AvatarDataPacket::LOOK_AT_POSITION_SIZE);
    ASSERT(sizeof(AvatarDataPacket::AudioLoudness) == AvatarDataPacket::AUDIO_LOUDNESS_SIZE);
    ASSERT(sizeof(AvatarDataPacket::SensorToWorldMatrix) == AvatarDataPacket::SENSOR_TO_WORLD_SIZE);
    ASSERT(sizeof(AvatarDataPacket::AdditionalFlags) == AvatarDataPacket::ADDITIONAL_FLAGS_SIZE);
    ASSERT(sizeof(AvatarDataPacket::ParentInfo) == AvatarDataPacket::PARENT_INFO_SIZE);
    ASSERT(sizeof(AvatarDataPacket::FaceTrackerInfo) == AvatarDataPacket::FACE_TRACKER_INFO_SIZE);

    // Old format...
    ASSERT(sizeof(AvatarDataPacket::AvatarInfo) == AvatarDataPacket::AVATAR_INFO_SIZE);

}

AvatarData::~AvatarData() {
    delete _headData;
}

// We cannot have a file-level variable (const or otherwise) in the AvatarInfo if it uses PathUtils, because that references Application, which will not yet initialized.
// Thus we have a static class getter, referencing a static class var.
QUrl AvatarData::_defaultFullAvatarModelUrl = {}; // In C++, if this initialization were in the AvatarInfo, every file would have it's own copy, even for class vars.
const QUrl& AvatarData::defaultFullAvatarModelUrl() {
    if (_defaultFullAvatarModelUrl.isEmpty()) {
        _defaultFullAvatarModelUrl = QUrl::fromLocalFile(PathUtils::resourcesPath() + "meshes/defaultAvatar_full.fst");
    }
    return _defaultFullAvatarModelUrl;
}

// There are a number of possible strategies for this set of tools through endRender, below.
void AvatarData::nextAttitude(glm::vec3 position, glm::quat orientation) {
    bool success;
    Transform trans = getTransform(success);
    if (!success) {
        qCWarning(avatars) << "Warning -- AvatarData::nextAttitude failed";
        return;
    }
    trans.setTranslation(position);
    trans.setRotation(orientation);
    SpatiallyNestable::setTransform(trans, success);
    if (!success) {
        qCWarning(avatars) << "Warning -- AvatarData::nextAttitude failed";
    }
    updateAttitude();
}

float AvatarData::getTargetScale() const {
    return _targetScale;
}

void AvatarData::setTargetScale(float targetScale) {
    _targetScale = glm::clamp(targetScale, MIN_AVATAR_SCALE, MAX_AVATAR_SCALE);
}

void AvatarData::setTargetScaleVerbose(float targetScale) {
    setTargetScale(targetScale);
    qCDebug(avatars) << "Changed scale to " << _targetScale;
}

glm::vec3 AvatarData::getHandPosition() const {
    return getOrientation() * _handPosition + getPosition();
}

void AvatarData::setHandPosition(const glm::vec3& handPosition) {
    // store relative to position/orientation
    _handPosition = glm::inverse(getOrientation()) * (handPosition - getPosition());
}

QByteArray AvatarData::toByteArray(AvatarDataDetail dataDetail) {
    return toByteArray_NEW(dataDetail);
}

QByteArray AvatarData::toByteArray_OLD(AvatarDataDetail dataDetail) {
    bool cullSmallChanges = (dataDetail == CullSmallData);
    bool sendAll = (dataDetail == SendAllData);
    bool sendMinimum = (dataDetail == MinimumData);
    // TODO: DRY this up to a shared method
    // that can pack any type given the number of bytes
    // and return the number of bytes to push the pointer

    // lazily allocate memory for HeadData in case we're not an Avatar instance
    if (!_headData) {
        _headData = new HeadData(this);
    }
    if (_forceFaceTrackerConnected) {
        _headData->_isFaceTrackerConnected = true;
    }

    QByteArray avatarDataByteArray(udt::MAX_PACKET_SIZE, 0);

    unsigned char* destinationBuffer = reinterpret_cast<unsigned char*>(avatarDataByteArray.data());
    unsigned char* startPosition = destinationBuffer;

    // Leading flags, to indicate how much data is actually included in the packet...
    uint8_t packetStateFlags = 0;
    if (sendMinimum) {
        setAtBit(packetStateFlags, AVATARDATA_FLAGS_MINIMUM);
    }

    memcpy(destinationBuffer, &packetStateFlags, sizeof(packetStateFlags));
    destinationBuffer += sizeof(packetStateFlags);

    if (sendMinimum) {
        memcpy(destinationBuffer, &_globalPosition, sizeof(_globalPosition));
        destinationBuffer += sizeof(_globalPosition);
    } else {
        auto avatarInfo = reinterpret_cast<AvatarDataPacket::AvatarInfo*>(destinationBuffer);
        avatarInfo->globalPosition[0] = _globalPosition.x;
        avatarInfo->globalPosition[1] = _globalPosition.y;
        avatarInfo->globalPosition[2] = _globalPosition.z;

        avatarInfo->position[0] = getLocalPosition().x;
        avatarInfo->position[1] = getLocalPosition().y;
        avatarInfo->position[2] = getLocalPosition().z;

        avatarInfo->globalBoundingBoxCorner[0] = getPosition().x - _globalBoundingBoxCorner.x;
        avatarInfo->globalBoundingBoxCorner[1] = getPosition().y - _globalBoundingBoxCorner.y;
        avatarInfo->globalBoundingBoxCorner[2] = getPosition().z - _globalBoundingBoxCorner.z;

        glm::vec3 bodyEulerAngles = glm::degrees(safeEulerAngles(getLocalOrientation()));
        packFloatAngleToTwoByte((uint8_t*)(avatarInfo->localOrientation + 0), bodyEulerAngles.y);
        packFloatAngleToTwoByte((uint8_t*)(avatarInfo->localOrientation + 1), bodyEulerAngles.x);
        packFloatAngleToTwoByte((uint8_t*)(avatarInfo->localOrientation + 2), bodyEulerAngles.z);
        packFloatRatioToTwoByte((uint8_t*)(&avatarInfo->scale), getDomainLimitedScale());
        avatarInfo->lookAtPosition[0] = _headData->_lookAtPosition.x;
        avatarInfo->lookAtPosition[1] = _headData->_lookAtPosition.y;
        avatarInfo->lookAtPosition[2] = _headData->_lookAtPosition.z;

        packFloatScalarToSignedTwoByteFixed((uint8_t*)&avatarInfo->audioLoudness, 
                                                glm::min(_headData->_audioLoudness, MAX_AUDIO_LOUDNESS), AUDIO_LOUDNESS_RADIX);

        glm::mat4 sensorToWorldMatrix = getSensorToWorldMatrix();
        packOrientationQuatToSixBytes(avatarInfo->sensorToWorldQuat, glmExtractRotation(sensorToWorldMatrix));
        glm::vec3 scale = extractScale(sensorToWorldMatrix);
        packFloatScalarToSignedTwoByteFixed((uint8_t*)&avatarInfo->sensorToWorldScale, scale.x, SENSOR_TO_WORLD_SCALE_RADIX);
        avatarInfo->sensorToWorldTrans[0] = sensorToWorldMatrix[3][0];
        avatarInfo->sensorToWorldTrans[1] = sensorToWorldMatrix[3][1];
        avatarInfo->sensorToWorldTrans[2] = sensorToWorldMatrix[3][2];

        setSemiNibbleAt(avatarInfo->flags, KEY_STATE_START_BIT, _keyState);
        // hand state
        bool isFingerPointing = _handState & IS_FINGER_POINTING_FLAG;
        setSemiNibbleAt(avatarInfo->flags, HAND_STATE_START_BIT, _handState & ~IS_FINGER_POINTING_FLAG);
        if (isFingerPointing) {
            setAtBit(avatarInfo->flags, HAND_STATE_FINGER_POINTING_BIT);
        }
        // faceshift state
        if (_headData->_isFaceTrackerConnected) {
            setAtBit(avatarInfo->flags, IS_FACESHIFT_CONNECTED);
        }
        // eye tracker state
        if (_headData->_isEyeTrackerConnected) {
            setAtBit(avatarInfo->flags, IS_EYE_TRACKER_CONNECTED);
        }
        // referential state
        QUuid parentID = getParentID();
        if (!parentID.isNull()) {
            setAtBit(avatarInfo->flags, HAS_REFERENTIAL);
        }
        destinationBuffer += sizeof(AvatarDataPacket::AvatarInfo);

        #if 0 // debugging
        #define COMPARE_MEMBER_V3(L, R, M) { if (L.M[0] != R.M[0] || L.M[1] != R.M[1] || L.M[2] != R.M[2]) {  qCDebug(avatars) << #M " changed - old:" << "{" << L.M[0] << "," << L.M[1] << ", " << L.M[2] << "}" << " new:" "{" << R.M[0] << "," << R.M[1] << ", " << R.M[2] << "}"; } }
        #define COMPARE_MEMBER_F(L, R, M) { if (L.M != R.M) {  qCDebug(avatars) << #M " changed - old:" << L.M << " new:" << R.M; } }

        qCDebug(avatars) << "--------------";
        COMPARE_MEMBER_V3(_lastAvatarInfo, (*avatarInfo), position);
        COMPARE_MEMBER_V3(_lastAvatarInfo, (*avatarInfo), globalPosition);
        COMPARE_MEMBER_V3(_lastAvatarInfo, (*avatarInfo), globalBoundingBoxCorner);
        COMPARE_MEMBER_V3(_lastAvatarInfo, (*avatarInfo), localOrientation);
        COMPARE_MEMBER_F(_lastAvatarInfo, (*avatarInfo), scale);
        COMPARE_MEMBER_V3(_lastAvatarInfo, (*avatarInfo), lookAtPosition);
        COMPARE_MEMBER_F(_lastAvatarInfo, (*avatarInfo), audioLoudness);

        if (_lastSensorToWorldMatrix != sensorToWorldMatrix) {
            qCDebug(avatars) << "sensorToWorldMatrix changed - old:" << _lastSensorToWorldMatrix << "new:" << sensorToWorldMatrix;
        }
        //COMPARE_MEMBER_V3(_lastAvatarInfo, (*avatarInfo), sensorToWorldQuat);
        COMPARE_MEMBER_F(_lastAvatarInfo, (*avatarInfo), sensorToWorldScale);
        COMPARE_MEMBER_V3(_lastAvatarInfo, (*avatarInfo), sensorToWorldTrans);
        COMPARE_MEMBER_F(_lastAvatarInfo, (*avatarInfo), flags);

        memcpy(&_lastAvatarInfo, avatarInfo, sizeof(_lastAvatarInfo));
        _lastSensorToWorldMatrix = sensorToWorldMatrix;

        #endif


        if (!parentID.isNull()) {
            auto parentInfo = reinterpret_cast<AvatarDataPacket::ParentInfo*>(destinationBuffer);
            QByteArray referentialAsBytes = parentID.toRfc4122();
            memcpy(parentInfo->parentUUID, referentialAsBytes.data(), referentialAsBytes.size());
            parentInfo->parentJointIndex = _parentJointIndex;
            destinationBuffer += sizeof(AvatarDataPacket::ParentInfo);
        }

        // If it is connected, pack up the data
        if (_headData->_isFaceTrackerConnected) {
            auto faceTrackerInfo = reinterpret_cast<AvatarDataPacket::FaceTrackerInfo*>(destinationBuffer);

            faceTrackerInfo->leftEyeBlink = _headData->_leftEyeBlink;
            faceTrackerInfo->rightEyeBlink = _headData->_rightEyeBlink;
            faceTrackerInfo->averageLoudness = _headData->_averageLoudness;
            faceTrackerInfo->browAudioLift = _headData->_browAudioLift;
            faceTrackerInfo->numBlendshapeCoefficients = _headData->_blendshapeCoefficients.size();
            destinationBuffer += sizeof(AvatarDataPacket::FaceTrackerInfo);

            // followed by a variable number of float coefficients
            memcpy(destinationBuffer, _headData->_blendshapeCoefficients.data(), _headData->_blendshapeCoefficients.size() * sizeof(float));
            destinationBuffer += _headData->_blendshapeCoefficients.size() * sizeof(float);
        }

        QReadLocker readLock(&_jointDataLock);

        // joint rotation data
        *destinationBuffer++ = _jointData.size();
        unsigned char* validityPosition = destinationBuffer;
        unsigned char validity = 0;
        int validityBit = 0;

        #ifdef WANT_DEBUG
        int rotationSentCount = 0;
        unsigned char* beforeRotations = destinationBuffer;
        #endif

        _lastSentJointData.resize(_jointData.size());

        for (int i=0; i < _jointData.size(); i++) {
            const JointData& data = _jointData[i];
            if (sendAll || _lastSentJointData[i].rotation != data.rotation) {
                if (sendAll ||
                    !cullSmallChanges ||
                    fabsf(glm::dot(data.rotation, _lastSentJointData[i].rotation)) <= AVATAR_MIN_ROTATION_DOT) {
                    if (data.rotationSet) {
                        validity |= (1 << validityBit);
                        #ifdef WANT_DEBUG
                        rotationSentCount++;
                        #endif
                    }
                }
            }
            if (++validityBit == BITS_IN_BYTE) {
                *destinationBuffer++ = validity;
                validityBit = validity = 0;
            }
        }
        if (validityBit != 0) {
            *destinationBuffer++ = validity;
        }

        validityBit = 0;
        validity = *validityPosition++;
        for (int i = 0; i < _jointData.size(); i ++) {
            const JointData& data = _jointData[i];
            if (validity & (1 << validityBit)) {
                destinationBuffer += packOrientationQuatToSixBytes(destinationBuffer, data.rotation);
            }
            if (++validityBit == BITS_IN_BYTE) {
                validityBit = 0;
                validity = *validityPosition++;
            }
        }


        // joint translation data
        validityPosition = destinationBuffer;
        validity = 0;
        validityBit = 0;

        #ifdef WANT_DEBUG
        int translationSentCount = 0;
        unsigned char* beforeTranslations = destinationBuffer;
        #endif

        float maxTranslationDimension = 0.0;
        for (int i=0; i < _jointData.size(); i++) {
            const JointData& data = _jointData[i];
            if (sendAll || _lastSentJointData[i].translation != data.translation) {
                if (sendAll ||
                    !cullSmallChanges ||
                    glm::distance(data.translation, _lastSentJointData[i].translation) > AVATAR_MIN_TRANSLATION) {
                    if (data.translationSet) {
                        validity |= (1 << validityBit);
                        #ifdef WANT_DEBUG
                        translationSentCount++;
                        #endif
                        maxTranslationDimension = glm::max(fabsf(data.translation.x), maxTranslationDimension);
                        maxTranslationDimension = glm::max(fabsf(data.translation.y), maxTranslationDimension);
                        maxTranslationDimension = glm::max(fabsf(data.translation.z), maxTranslationDimension);
                    }
                }
            }
            if (++validityBit == BITS_IN_BYTE) {
                *destinationBuffer++ = validity;
                validityBit = validity = 0;
            }
        }

        if (validityBit != 0) {
            *destinationBuffer++ = validity;
        }

        validityBit = 0;
        validity = *validityPosition++;
        for (int i = 0; i < _jointData.size(); i ++) {
            const JointData& data = _jointData[i];
            if (validity & (1 << validityBit)) {
                destinationBuffer +=
                    packFloatVec3ToSignedTwoByteFixed(destinationBuffer, data.translation, TRANSLATION_COMPRESSION_RADIX);
            }
            if (++validityBit == BITS_IN_BYTE) {
                validityBit = 0;
                validity = *validityPosition++;
            }
        }

        // faux joints
        Transform controllerLeftHandTransform = Transform(getControllerLeftHandMatrix());
        destinationBuffer += packOrientationQuatToSixBytes(destinationBuffer, controllerLeftHandTransform.getRotation());
        destinationBuffer += packFloatVec3ToSignedTwoByteFixed(destinationBuffer, controllerLeftHandTransform.getTranslation(),
                                                               TRANSLATION_COMPRESSION_RADIX);
        Transform controllerRightHandTransform = Transform(getControllerRightHandMatrix());
        destinationBuffer += packOrientationQuatToSixBytes(destinationBuffer, controllerRightHandTransform.getRotation());
        destinationBuffer += packFloatVec3ToSignedTwoByteFixed(destinationBuffer, controllerRightHandTransform.getTranslation(),
                                                               TRANSLATION_COMPRESSION_RADIX);

        #ifdef WANT_DEBUG
        if (sendAll) {
            qCDebug(avatars) << "AvatarData::toByteArray" << cullSmallChanges << sendAll
                     << "rotations:" << rotationSentCount << "translations:" << translationSentCount
                     << "largest:" << maxTranslationDimension
                     << "size:"
                     << (beforeRotations - startPosition) << "+"
                     << (beforeTranslations - beforeRotations) << "+"
                     << (destinationBuffer - beforeTranslations) << "="
                     << (destinationBuffer - startPosition);
        }
        #endif
    }

    return avatarDataByteArray.left(destinationBuffer - startPosition);
}

void AvatarData::lazyInitHeadData() {
    // lazily allocate memory for HeadData in case we're not an Avatar instance
    if (!_headData) {
        _headData = new HeadData(this);
    }
    if (_forceFaceTrackerConnected) {
        _headData->_isFaceTrackerConnected = true;
    }
}


bool AvatarData::avatarLocalPositionChanged() {
    return _lastSentLocalPosition != getLocalPosition();
}

bool AvatarData::avatarDimensionsChanged() {
    auto avatarDimensions = getPosition() - _globalBoundingBoxCorner;
    return _lastSentAvatarDimensions != avatarDimensions;
}

bool AvatarData::avatarOrientationChanged() {
    return _lastSentLocalOrientation != getLocalOrientation();
}

bool AvatarData::avatarScaleChanged() {
    return _lastSentScale != getDomainLimitedScale();
}

bool AvatarData::lookAtPositionChanged() {
    return _lastSentLookAt != _headData->_lookAtPosition;
}

bool AvatarData::audioLoudnessChanged() {
    return _lastSentAudioLoudness != glm::min(_headData->_audioLoudness, MAX_AUDIO_LOUDNESS);
}

bool AvatarData::sensorToWorldMatrixChanged() {
    return _lastSentSensorToWorldMatrix != getSensorToWorldMatrix();
}

bool AvatarData::additionalFlagsChanged() {
    return true; // FIXME!
}

bool AvatarData::parentInfoChanged() {
    return (_lastSentParentID != getParentID()) || (_lastSentParentJointIndex != _parentJointIndex);
}

bool AvatarData::faceTrackerInfoChanged() {
    return true; // FIXME!
}

QByteArray AvatarData::toByteArray_NEW(AvatarDataDetail dataDetail) {
    bool cullSmallChanges = (dataDetail == CullSmallData);
    bool sendAll = (dataDetail == SendAllData);
    bool sendMinimum = (dataDetail == MinimumData);

    // TODO: DRY this up to a shared method
    // that can pack any type given the number of bytes
    // and return the number of bytes to push the pointer
    lazyInitHeadData();

    QByteArray avatarDataByteArray(udt::MAX_PACKET_SIZE, 0);
    unsigned char* destinationBuffer = reinterpret_cast<unsigned char*>(avatarDataByteArray.data());
    unsigned char* startPosition = destinationBuffer;
    unsigned char* packetStateFlagsAt = startPosition;

    // psuedo code....
    //   - determine which sections will be included
    //   - create the packet has flags
    //   - include each section in order

    // FIXME - things to consider
    //   - how to dry up this code?
    //
    //   - the sections below are basically little repeats of each other, where they
    //     cast the destination pointer to the section struct type, set the struct
    //     members in some specific way (not just assigning), then advance the buffer,
    //     and then remember the last value sent. This could be macro-ized and/or
    //     templatized or lambda-ized
    //
    //   - also, we could determine the "hasXXX" flags in the little sections,
    //     and then set the actual flag values AFTER the rest are done...
    //
    //   - this toByteArray() side-effects the AvatarData, is that safe? in particular
    //     is it possible we'll call toByteArray() and then NOT actually use the result?


    bool hasAvatarGlobalPosition = true; // always include global position
    bool hasAvatarLocalPosition = sendAll || avatarLocalPositionChanged();
    bool hasAvatarDimensions = sendAll || avatarDimensionsChanged();
    bool hasAvatarOrientation = sendAll || avatarOrientationChanged();
    bool hasAvatarScale = sendAll || avatarScaleChanged();
    bool hasLookAtPosition = sendAll || lookAtPositionChanged();
    bool hasAudioLoudness = sendAll || audioLoudnessChanged();
    bool hasSensorToWorldMatrix = sendAll || sensorToWorldMatrixChanged();
    bool hasAdditionalFlags = sendAll || additionalFlagsChanged();
    bool hasParentInfo = hasParent() && (sendAll || parentInfoChanged());
    bool hasFaceTrackerInfo = hasFaceTracker() && (sendAll || faceTrackerInfoChanged());
    bool hasJointData = !sendMinimum;

    //qDebug() << __FUNCTION__ << "sendAll:" << sendAll;
    //qDebug() << "hasAvatarGlobalPosition:" << hasAvatarGlobalPosition;
    //qDebug() << "hasAvatarOrientation:" << hasAvatarOrientation;

    // Leading flags, to indicate how much data is actually included in the packet...
    AvatarDataPacket::HasFlags packetStateFlags =
            (hasAvatarGlobalPosition ? AvatarDataPacket::PACKET_HAS_AVATAR_GLOBAL_POSITION : 0)
          | (hasAvatarLocalPosition  ? AvatarDataPacket::PACKET_HAS_AVATAR_LOCAL_POSITION : 0)
          | (hasAvatarDimensions     ? AvatarDataPacket::PACKET_HAS_AVATAR_DIMENSIONS : 0)
          | (hasAvatarOrientation    ? AvatarDataPacket::PACKET_HAS_AVATAR_ORIENTATION : 0)
          | (hasAvatarScale          ? AvatarDataPacket::PACKET_HAS_AVATAR_SCALE : 0)
          | (hasLookAtPosition       ? AvatarDataPacket::PACKET_HAS_LOOK_AT_POSITION : 0)
          | (hasAudioLoudness        ? AvatarDataPacket::PACKET_HAS_AUDIO_LOUDNESS : 0)
          | (hasSensorToWorldMatrix  ? AvatarDataPacket::PACKET_HAS_SENSOR_TO_WORLD_MATRIX : 0)
          | (hasAdditionalFlags      ? AvatarDataPacket::PACKET_HAS_ADDITIONAL_FLAGS : 0)
          | (hasParentInfo           ? AvatarDataPacket::PACKET_HAS_PARENT_INFO : 0)
          | (hasFaceTrackerInfo      ? AvatarDataPacket::PACKET_HAS_FACE_TRACKER_INFO : 0)
          | (hasJointData            ? AvatarDataPacket::PACKET_HAS_JOINT_DATA : 0);

    //qDebug() << __FUNCTION__ << "packetStateFlags:" << packetStateFlags;

    memcpy(destinationBuffer, &packetStateFlags, sizeof(packetStateFlags));
    destinationBuffer += sizeof(packetStateFlags);

    if (hasAvatarGlobalPosition) {
        auto data = reinterpret_cast<AvatarDataPacket::AvatarGlobalPosition*>(destinationBuffer);
        data->globalPosition[0] = _globalPosition.x;
        data->globalPosition[1] = _globalPosition.y;
        data->globalPosition[2] = _globalPosition.z;
        destinationBuffer += sizeof(AvatarDataPacket::AvatarGlobalPosition);
        _lastSentGlobalPosition = _globalPosition;

        //qDebug() << "hasAvatarGlobalPosition _globalPosition:" << _globalPosition;
    }

    // FIXME - I was told by tony this was "skeletal model position"-- but it seems to be 
    // SpatiallyNestable::getLocalPosition() ... which AFAICT is almost always the same as
    // the global position (unless presumably you're on a parent)... we might be able to
    // include this in the parent info record
    if (hasAvatarLocalPosition) {
        auto data = reinterpret_cast<AvatarDataPacket::AvatarLocalPosition*>(destinationBuffer);
        auto localPosition = getLocalPosition();
        data->localPosition[0] = localPosition.x;
        data->localPosition[1] = localPosition.y;
        data->localPosition[2] = localPosition.z;
        destinationBuffer += sizeof(AvatarDataPacket::AvatarLocalPosition);
        _lastSentLocalPosition = localPosition;
    }

    if (hasAvatarDimensions) {
        auto data = reinterpret_cast<AvatarDataPacket::AvatarDimensions*>(destinationBuffer);
        auto avatarDimensions = getPosition() - _globalBoundingBoxCorner;
        data->avatarDimensions[0] = avatarDimensions.x;
        data->avatarDimensions[1] = avatarDimensions.y;
        data->avatarDimensions[2] = avatarDimensions.z;
        destinationBuffer += sizeof(AvatarDataPacket::AvatarDimensions);
        _lastSentAvatarDimensions = avatarDimensions;
    }

    if (hasAvatarOrientation) {
        auto data = reinterpret_cast<AvatarDataPacket::AvatarOrientation*>(destinationBuffer);
        auto localOrientation = getLocalOrientation();
        glm::vec3 bodyEulerAngles = glm::degrees(safeEulerAngles(localOrientation));
        packFloatAngleToTwoByte((uint8_t*)(data->localOrientation + 0), bodyEulerAngles.y);
        packFloatAngleToTwoByte((uint8_t*)(data->localOrientation + 1), bodyEulerAngles.x);
        packFloatAngleToTwoByte((uint8_t*)(data->localOrientation + 2), bodyEulerAngles.z);
        destinationBuffer += sizeof(AvatarDataPacket::AvatarOrientation);
        _lastSentLocalOrientation = localOrientation;
    }

    if (hasAvatarScale) {
        auto data = reinterpret_cast<AvatarDataPacket::AvatarScale*>(destinationBuffer);
        auto scale = getDomainLimitedScale();
        packFloatRatioToTwoByte((uint8_t*)(&data->scale), scale);
        destinationBuffer += sizeof(AvatarDataPacket::AvatarScale);
        _lastSentScale = scale;
    }

    if (hasLookAtPosition) {
        auto data = reinterpret_cast<AvatarDataPacket::LookAtPosition*>(destinationBuffer);
        auto lookAt = _headData->_lookAtPosition;
        data->lookAtPosition[0] = lookAt.x;
        data->lookAtPosition[1] = lookAt.y;
        data->lookAtPosition[2] = lookAt.z;
        destinationBuffer += sizeof(AvatarDataPacket::LookAtPosition);
        _lastSentLookAt = lookAt;
    }

    if (hasAudioLoudness) {
        auto data = reinterpret_cast<AvatarDataPacket::AudioLoudness*>(destinationBuffer);
        auto audioLoudness = glm::min(_headData->_audioLoudness, MAX_AUDIO_LOUDNESS);
        packFloatScalarToSignedTwoByteFixed((uint8_t*)&data->audioLoudness, audioLoudness, AUDIO_LOUDNESS_RADIX);
        destinationBuffer += sizeof(AvatarDataPacket::AudioLoudness);
        _lastSentAudioLoudness = audioLoudness;
    }

    if (hasSensorToWorldMatrix) {
        auto data = reinterpret_cast<AvatarDataPacket::SensorToWorldMatrix*>(destinationBuffer);
        glm::mat4 sensorToWorldMatrix = getSensorToWorldMatrix();
        packOrientationQuatToSixBytes(data->sensorToWorldQuat, glmExtractRotation(sensorToWorldMatrix));
        glm::vec3 scale = extractScale(sensorToWorldMatrix);
        packFloatScalarToSignedTwoByteFixed((uint8_t*)&data->sensorToWorldScale, scale.x, SENSOR_TO_WORLD_SCALE_RADIX);
        data->sensorToWorldTrans[0] = sensorToWorldMatrix[3][0];
        data->sensorToWorldTrans[1] = sensorToWorldMatrix[3][1];
        data->sensorToWorldTrans[2] = sensorToWorldMatrix[3][2];
        destinationBuffer += sizeof(AvatarDataPacket::SensorToWorldMatrix);
        _lastSentSensorToWorldMatrix = sensorToWorldMatrix;
    }

    QUuid parentID = getParentID();

    if (hasAdditionalFlags) {
        auto data = reinterpret_cast<AvatarDataPacket::AdditionalFlags*>(destinationBuffer);

        uint8_t flags { 0 };

        setSemiNibbleAt(flags, KEY_STATE_START_BIT, _keyState);

        // hand state
        bool isFingerPointing = _handState & IS_FINGER_POINTING_FLAG;
        setSemiNibbleAt(flags, HAND_STATE_START_BIT, _handState & ~IS_FINGER_POINTING_FLAG);
        if (isFingerPointing) {
            setAtBit(flags, HAND_STATE_FINGER_POINTING_BIT);
        }
        // faceshift state
        if (_headData->_isFaceTrackerConnected) {
            setAtBit(flags, IS_FACESHIFT_CONNECTED);
        }
        // eye tracker state
        if (_headData->_isEyeTrackerConnected) {
            setAtBit(flags, IS_EYE_TRACKER_CONNECTED);
        }
        // referential state
        if (!parentID.isNull()) {
            setAtBit(flags, HAS_REFERENTIAL);
        }
        data->flags = flags;
        destinationBuffer += sizeof(AvatarDataPacket::AdditionalFlags);
        _lastSentAdditionalFlags = flags;
    }

    if (hasParentInfo) {
        auto parentInfo = reinterpret_cast<AvatarDataPacket::ParentInfo*>(destinationBuffer);
        QByteArray referentialAsBytes = parentID.toRfc4122();
        memcpy(parentInfo->parentUUID, referentialAsBytes.data(), referentialAsBytes.size());
        parentInfo->parentJointIndex = _parentJointIndex;
        destinationBuffer += sizeof(AvatarDataPacket::ParentInfo);
        _lastSentParentID = parentID;
        _lastSentParentJointIndex = _parentJointIndex;
    }

    // If it is connected, pack up the data
    if (hasFaceTrackerInfo) {
        auto faceTrackerInfo = reinterpret_cast<AvatarDataPacket::FaceTrackerInfo*>(destinationBuffer);

        faceTrackerInfo->leftEyeBlink = _headData->_leftEyeBlink;
        faceTrackerInfo->rightEyeBlink = _headData->_rightEyeBlink;
        faceTrackerInfo->averageLoudness = _headData->_averageLoudness;
        faceTrackerInfo->browAudioLift = _headData->_browAudioLift;
        faceTrackerInfo->numBlendshapeCoefficients = _headData->_blendshapeCoefficients.size();
        destinationBuffer += sizeof(AvatarDataPacket::FaceTrackerInfo);

        // followed by a variable number of float coefficients
        memcpy(destinationBuffer, _headData->_blendshapeCoefficients.data(), _headData->_blendshapeCoefficients.size() * sizeof(float));
        destinationBuffer += _headData->_blendshapeCoefficients.size() * sizeof(float);
    }

    // If it is connected, pack up the data
    if (hasJointData) {
        QReadLocker readLock(&_jointDataLock);

        // joint rotation data
        *destinationBuffer++ = _jointData.size();
        unsigned char* validityPosition = destinationBuffer;
        unsigned char validity = 0;
        int validityBit = 0;

#ifdef WANT_DEBUG
        int rotationSentCount = 0;
        unsigned char* beforeRotations = destinationBuffer;
#endif

        _lastSentJointData.resize(_jointData.size());

        for (int i = 0; i < _jointData.size(); i++) {
            const JointData& data = _jointData[i];
            if (sendAll || _lastSentJointData[i].rotation != data.rotation) {
                if (sendAll ||
                    !cullSmallChanges ||
                    fabsf(glm::dot(data.rotation, _lastSentJointData[i].rotation)) <= AVATAR_MIN_ROTATION_DOT) {
                    if (data.rotationSet) {
                        validity |= (1 << validityBit);
#ifdef WANT_DEBUG
                        rotationSentCount++;
#endif
                    }
                }
            }
            if (++validityBit == BITS_IN_BYTE) {
                *destinationBuffer++ = validity;
                validityBit = validity = 0;
            }
        }
        if (validityBit != 0) {
            *destinationBuffer++ = validity;
        }

        validityBit = 0;
        validity = *validityPosition++;
        for (int i = 0; i < _jointData.size(); i++) {
            const JointData& data = _jointData[i];
            if (validity & (1 << validityBit)) {
                destinationBuffer += packOrientationQuatToSixBytes(destinationBuffer, data.rotation);
            }
            if (++validityBit == BITS_IN_BYTE) {
                validityBit = 0;
                validity = *validityPosition++;
            }
        }


        // joint translation data
        validityPosition = destinationBuffer;
        validity = 0;
        validityBit = 0;

#ifdef WANT_DEBUG
        int translationSentCount = 0;
        unsigned char* beforeTranslations = destinationBuffer;
#endif

        float maxTranslationDimension = 0.0;
        for (int i = 0; i < _jointData.size(); i++) {
            const JointData& data = _jointData[i];
            if (sendAll || _lastSentJointData[i].translation != data.translation) {
                if (sendAll ||
                    !cullSmallChanges ||
                    glm::distance(data.translation, _lastSentJointData[i].translation) > AVATAR_MIN_TRANSLATION) {
                    if (data.translationSet) {
                        validity |= (1 << validityBit);
#ifdef WANT_DEBUG
                        translationSentCount++;
#endif
                        maxTranslationDimension = glm::max(fabsf(data.translation.x), maxTranslationDimension);
                        maxTranslationDimension = glm::max(fabsf(data.translation.y), maxTranslationDimension);
                        maxTranslationDimension = glm::max(fabsf(data.translation.z), maxTranslationDimension);
                    }
                }
            }
            if (++validityBit == BITS_IN_BYTE) {
                *destinationBuffer++ = validity;
                validityBit = validity = 0;
            }
        }

        if (validityBit != 0) {
            *destinationBuffer++ = validity;
        }

        validityBit = 0;
        validity = *validityPosition++;
        for (int i = 0; i < _jointData.size(); i++) {
            const JointData& data = _jointData[i];
            if (validity & (1 << validityBit)) {
                destinationBuffer +=
                    packFloatVec3ToSignedTwoByteFixed(destinationBuffer, data.translation, TRANSLATION_COMPRESSION_RADIX);
            }
            if (++validityBit == BITS_IN_BYTE) {
                validityBit = 0;
                validity = *validityPosition++;
            }
        }

        // faux joints
        Transform controllerLeftHandTransform = Transform(getControllerLeftHandMatrix());
        destinationBuffer += packOrientationQuatToSixBytes(destinationBuffer, controllerLeftHandTransform.getRotation());
        destinationBuffer += packFloatVec3ToSignedTwoByteFixed(destinationBuffer, controllerLeftHandTransform.getTranslation(),
            TRANSLATION_COMPRESSION_RADIX);
        Transform controllerRightHandTransform = Transform(getControllerRightHandMatrix());
        destinationBuffer += packOrientationQuatToSixBytes(destinationBuffer, controllerRightHandTransform.getRotation());
        destinationBuffer += packFloatVec3ToSignedTwoByteFixed(destinationBuffer, controllerRightHandTransform.getTranslation(),
            TRANSLATION_COMPRESSION_RADIX);

#ifdef WANT_DEBUG
        if (sendAll) {
            qCDebug(avatars) << "AvatarData::toByteArray" << cullSmallChanges << sendAll
                << "rotations:" << rotationSentCount << "translations:" << translationSentCount
                << "largest:" << maxTranslationDimension
                << "size:"
                << (beforeRotations - startPosition) << "+"
                << (beforeTranslations - beforeRotations) << "+"
                << (destinationBuffer - beforeTranslations) << "="
                << (destinationBuffer - startPosition);
        }
#endif
    }

    return avatarDataByteArray.left(destinationBuffer - startPosition);
}

void AvatarData::doneEncoding(bool cullSmallChanges) {
    // The server has finished sending this version of the joint-data to other nodes.  Update _lastSentJointData.
    QReadLocker readLock(&_jointDataLock);
    _lastSentJointData.resize(_jointData.size());
    for (int i = 0; i < _jointData.size(); i ++) {
        const JointData& data = _jointData[ i ];
        if (_lastSentJointData[i].rotation != data.rotation) {
            if (!cullSmallChanges ||
                fabsf(glm::dot(data.rotation, _lastSentJointData[i].rotation)) <= AVATAR_MIN_ROTATION_DOT) {
                if (data.rotationSet) {
                    _lastSentJointData[i].rotation = data.rotation;
                }
            }
        }
        if (_lastSentJointData[i].translation != data.translation) {
            if (!cullSmallChanges ||
                glm::distance(data.translation, _lastSentJointData[i].translation) > AVATAR_MIN_TRANSLATION) {
                if (data.translationSet) {
                    _lastSentJointData[i].translation = data.translation;
                }
            }
        }
    }
}

bool AvatarData::shouldLogError(const quint64& now) {
#ifdef WANT_DEBUG
    if (now > 0) {
        return true;
    }
#endif

    if (now > _errorLogExpiry) {
        _errorLogExpiry = now + DEFAULT_FILTERED_LOG_EXPIRY;
        return true;
    }
    return false;
}


const unsigned char* unpackFauxJoint(const unsigned char* sourceBuffer, ThreadSafeValueCache<glm::mat4>& matrixCache) {
    glm::quat orientation;
    glm::vec3 position;
    Transform transform;
    sourceBuffer += unpackOrientationQuatFromSixBytes(sourceBuffer, orientation);
    sourceBuffer += unpackFloatVec3FromSignedTwoByteFixed(sourceBuffer, position, TRANSLATION_COMPRESSION_RADIX);
    transform.setTranslation(position);
    transform.setRotation(orientation);
    matrixCache.set(transform.getMatrix());
    return sourceBuffer;
}


#define PACKET_READ_CHECK(ITEM_NAME, SIZE_TO_READ)                                        \
    if ((endPosition - sourceBuffer) < (int)SIZE_TO_READ) {                               \
        if (shouldLogError(now)) {                                                        \
            qCWarning(avatars) << "AvatarData packet too small, attempting to read " <<   \
                #ITEM_NAME << ", only " << (endPosition - sourceBuffer) <<                \
                " bytes left, " << getSessionUUID();                                      \
        }                                                                                 \
        return buffer.size();                                                             \
    }

// read data in packet starting at byte offset and return number of bytes parsed
int AvatarData::parseDataFromBuffer(const QByteArray& buffer) {
    return parseDataFromBuffer_NEW(buffer);
}

// read data in packet starting at byte offset and return number of bytes parsed
int AvatarData::parseDataFromBuffer_OLD(const QByteArray& buffer) {
    // lazily allocate memory for HeadData in case we're not an Avatar instance
    if (!_headData) {
        _headData = new HeadData(this);
    }

    uint8_t packetStateFlags = buffer.at(0);
    bool minimumSent = oneAtBit(packetStateFlags, AVATARDATA_FLAGS_MINIMUM);

    const unsigned char* startPosition = reinterpret_cast<const unsigned char*>(buffer.data());
    const unsigned char* endPosition = startPosition + buffer.size();
    const unsigned char* sourceBuffer = startPosition + sizeof(packetStateFlags); // skip the flags!!

    // if this is the minimum, then it only includes the flags
    if (minimumSent) {
        memcpy(&_globalPosition, sourceBuffer, sizeof(_globalPosition));
        sourceBuffer += sizeof(_globalPosition);
        int numBytesRead = (sourceBuffer - startPosition);
        _averageBytesReceived.updateAverage(numBytesRead);
        return numBytesRead;
    }

    quint64 now = usecTimestampNow();

    PACKET_READ_CHECK(AvatarInfo, sizeof(AvatarDataPacket::AvatarInfo));
    auto avatarInfo = reinterpret_cast<const AvatarDataPacket::AvatarInfo*>(sourceBuffer);
    sourceBuffer += sizeof(AvatarDataPacket::AvatarInfo);

    _globalPosition = glm::vec3(avatarInfo->globalPosition[0], avatarInfo->globalPosition[1], avatarInfo->globalPosition[2]);
    glm::vec3 position = glm::vec3(avatarInfo->position[0], avatarInfo->position[1], avatarInfo->position[2]);
    _globalBoundingBoxCorner = glm::vec3(avatarInfo->globalBoundingBoxCorner[0], avatarInfo->globalBoundingBoxCorner[1], avatarInfo->globalBoundingBoxCorner[2]);
    if (isNaN(position)) {
        if (shouldLogError(now)) {
            qCWarning(avatars) << "Discard AvatarData packet: position NaN, uuid " << getSessionUUID();
        }
        return buffer.size();
    }
    setLocalPosition(position);

    float pitch, yaw, roll;
    unpackFloatAngleFromTwoByte(avatarInfo->localOrientation + 0, &yaw);
    unpackFloatAngleFromTwoByte(avatarInfo->localOrientation + 1, &pitch);
    unpackFloatAngleFromTwoByte(avatarInfo->localOrientation + 2, &roll);
    if (isNaN(yaw) || isNaN(pitch) || isNaN(roll)) {
        if (shouldLogError(now)) {
            qCWarning(avatars) << "Discard AvatarData packet: localOriention is NaN, uuid " << getSessionUUID();
        }
        return buffer.size();
    }

    glm::quat currentOrientation = getLocalOrientation();
    glm::vec3 newEulerAngles(pitch, yaw, roll);
    glm::quat newOrientation = glm::quat(glm::radians(newEulerAngles));
    if (currentOrientation != newOrientation) {
        _hasNewJointRotations = true;
        setLocalOrientation(newOrientation);
    }

    float scale;
    unpackFloatRatioFromTwoByte((uint8_t*)&avatarInfo->scale, scale);
    if (isNaN(scale)) {
        if (shouldLogError(now)) {
            qCWarning(avatars) << "Discard AvatarData packet: scale NaN, uuid " << getSessionUUID();
        }
        return buffer.size();
    }
    setTargetScale(scale);

    glm::vec3 lookAt = glm::vec3(avatarInfo->lookAtPosition[0], avatarInfo->lookAtPosition[1], avatarInfo->lookAtPosition[2]);
    if (isNaN(lookAt)) {
        if (shouldLogError(now)) {
            qCWarning(avatars) << "Discard AvatarData packet: lookAtPosition is NaN, uuid " << getSessionUUID();
        }
        return buffer.size();
    }
    _headData->_lookAtPosition = lookAt;


    float audioLoudness;
    unpackFloatScalarFromSignedTwoByteFixed((int16_t*)&avatarInfo->audioLoudness, &audioLoudness, AUDIO_LOUDNESS_RADIX);

    // FIXME - is this really needed?
    if (isNaN(audioLoudness)) {
        if (shouldLogError(now)) {
            qCWarning(avatars) << "Discard AvatarData packet: audioLoudness is NaN, uuid " << getSessionUUID();
        }
        return buffer.size();
    }
    _headData->_audioLoudness = audioLoudness;

    glm::quat sensorToWorldQuat;
    unpackOrientationQuatFromSixBytes(avatarInfo->sensorToWorldQuat, sensorToWorldQuat);
    float sensorToWorldScale;
    unpackFloatScalarFromSignedTwoByteFixed((int16_t*)&avatarInfo->sensorToWorldScale, &sensorToWorldScale, SENSOR_TO_WORLD_SCALE_RADIX);
    glm::vec3 sensorToWorldTrans(avatarInfo->sensorToWorldTrans[0], avatarInfo->sensorToWorldTrans[1], avatarInfo->sensorToWorldTrans[2]);
    glm::mat4 sensorToWorldMatrix = createMatFromScaleQuatAndPos(glm::vec3(sensorToWorldScale), sensorToWorldQuat, sensorToWorldTrans);

    _sensorToWorldMatrixCache.set(sensorToWorldMatrix);

    { // bitFlags and face data
        uint8_t bitItems = avatarInfo->flags;

        // key state, stored as a semi-nibble in the bitItems
        _keyState = (KeyState)getSemiNibbleAt(bitItems, KEY_STATE_START_BIT);

        // hand state, stored as a semi-nibble plus a bit in the bitItems
        // we store the hand state as well as other items in a shared bitset. The hand state is an octal, but is split
        // into two sections to maintain backward compatibility. The bits are ordered as such (0-7 left to right).
        //     +---+-----+-----+--+
        //     |x,x|H0,H1|x,x,x|H2|
        //     +---+-----+-----+--+
        // Hand state - H0,H1,H2 is found in the 3rd, 4th, and 8th bits
        _handState = getSemiNibbleAt(bitItems, HAND_STATE_START_BIT)
            + (oneAtBit(bitItems, HAND_STATE_FINGER_POINTING_BIT) ? IS_FINGER_POINTING_FLAG : 0);

        _headData->_isFaceTrackerConnected = oneAtBit(bitItems, IS_FACESHIFT_CONNECTED);
        _headData->_isEyeTrackerConnected = oneAtBit(bitItems, IS_EYE_TRACKER_CONNECTED);
        bool hasReferential = oneAtBit(bitItems, HAS_REFERENTIAL);

        if (hasReferential) {
            PACKET_READ_CHECK(ParentInfo, sizeof(AvatarDataPacket::ParentInfo));
            auto parentInfo = reinterpret_cast<const AvatarDataPacket::ParentInfo*>(sourceBuffer);
            sourceBuffer += sizeof(AvatarDataPacket::ParentInfo);

            QByteArray byteArray((const char*)parentInfo->parentUUID, NUM_BYTES_RFC4122_UUID);
            _parentID = QUuid::fromRfc4122(byteArray);
            _parentJointIndex = parentInfo->parentJointIndex;
        } else {
            _parentID = QUuid();
        }

        if (_headData->_isFaceTrackerConnected) {
            PACKET_READ_CHECK(FaceTrackerInfo, sizeof(AvatarDataPacket::FaceTrackerInfo));
            auto faceTrackerInfo = reinterpret_cast<const AvatarDataPacket::FaceTrackerInfo*>(sourceBuffer);
            sourceBuffer += sizeof(AvatarDataPacket::FaceTrackerInfo);

            _headData->_leftEyeBlink = faceTrackerInfo->leftEyeBlink;
            _headData->_rightEyeBlink = faceTrackerInfo->rightEyeBlink;
            _headData->_averageLoudness = faceTrackerInfo->averageLoudness;
            _headData->_browAudioLift = faceTrackerInfo->browAudioLift;

            int numCoefficients = faceTrackerInfo->numBlendshapeCoefficients;
            const int coefficientsSize = sizeof(float) * numCoefficients;
            PACKET_READ_CHECK(FaceTrackerCoefficients, coefficientsSize);
            _headData->_blendshapeCoefficients.resize(numCoefficients);  // make sure there's room for the copy!
            memcpy(_headData->_blendshapeCoefficients.data(), sourceBuffer, coefficientsSize);
            sourceBuffer += coefficientsSize;
        }
    }

    PACKET_READ_CHECK(NumJoints, sizeof(uint8_t));
    int numJoints = *sourceBuffer++;

    const int bytesOfValidity = (int)ceil((float)numJoints / (float)BITS_IN_BYTE);
    PACKET_READ_CHECK(JointRotationValidityBits, bytesOfValidity);

    int numValidJointRotations = 0;
    QVector<bool> validRotations;
    validRotations.resize(numJoints);
    { // rotation validity bits
        unsigned char validity = 0;
        int validityBit = 0;
        for (int i = 0; i < numJoints; i++) {
            if (validityBit == 0) {
                validity = *sourceBuffer++;
            }
            bool valid = (bool)(validity & (1 << validityBit));
            if (valid) {
                ++numValidJointRotations;
            }
            validRotations[i] = valid;
            validityBit = (validityBit + 1) % BITS_IN_BYTE;
        }
    }

    // each joint rotation is stored in 6 bytes.
    QWriteLocker writeLock(&_jointDataLock);
    _jointData.resize(numJoints);

    const int COMPRESSED_QUATERNION_SIZE = 6;
    PACKET_READ_CHECK(JointRotations, numValidJointRotations * COMPRESSED_QUATERNION_SIZE);
    for (int i = 0; i < numJoints; i++) {
        JointData& data = _jointData[i];
        if (validRotations[i]) {
            sourceBuffer += unpackOrientationQuatFromSixBytes(sourceBuffer, data.rotation);
            _hasNewJointRotations = true;
            data.rotationSet = true;
        }
    }

    PACKET_READ_CHECK(JointTranslationValidityBits, bytesOfValidity);

    // get translation validity bits -- these indicate which translations were packed
    int numValidJointTranslations = 0;
    QVector<bool> validTranslations;
    validTranslations.resize(numJoints);
    { // translation validity bits
        unsigned char validity = 0;
        int validityBit = 0;
        for (int i = 0; i < numJoints; i++) {
            if (validityBit == 0) {
                validity = *sourceBuffer++;
            }
            bool valid = (bool)(validity & (1 << validityBit));
            if (valid) {
                ++numValidJointTranslations;
            }
            validTranslations[i] = valid;
            validityBit = (validityBit + 1) % BITS_IN_BYTE;
        }
    } // 1 + bytesOfValidity bytes

    // each joint translation component is stored in 6 bytes.
    const int COMPRESSED_TRANSLATION_SIZE = 6;
    PACKET_READ_CHECK(JointTranslation, numValidJointTranslations * COMPRESSED_TRANSLATION_SIZE);

    for (int i = 0; i < numJoints; i++) {
        JointData& data = _jointData[i];
        if (validTranslations[i]) {
            sourceBuffer += unpackFloatVec3FromSignedTwoByteFixed(sourceBuffer, data.translation, TRANSLATION_COMPRESSION_RADIX);
            _hasNewJointTranslations = true;
            data.translationSet = true;
        }
    }

    #ifdef WANT_DEBUG
    if (numValidJointRotations > 15) {
        qCDebug(avatars) << "RECEIVING -- rotations:" << numValidJointRotations
                 << "translations:" << numValidJointTranslations
                 << "size:" << (int)(sourceBuffer - startPosition);
    }
    #endif

    // faux joints
    sourceBuffer = unpackFauxJoint(sourceBuffer, _controllerLeftHandMatrixCache);
    sourceBuffer = unpackFauxJoint(sourceBuffer, _controllerRightHandMatrixCache);

    int numBytesRead = sourceBuffer - startPosition;
    _averageBytesReceived.updateAverage(numBytesRead);
    return numBytesRead;
}


// read data in packet starting at byte offset and return number of bytes parsed
int AvatarData::parseDataFromBuffer_NEW(const QByteArray& buffer) {
    // lazily allocate memory for HeadData in case we're not an Avatar instance
    lazyInitHeadData();

    AvatarDataPacket::HasFlags packetStateFlags;

    const unsigned char* startPosition = reinterpret_cast<const unsigned char*>(buffer.data());
    const unsigned char* endPosition = startPosition + buffer.size();
    const unsigned char* sourceBuffer = startPosition;

    // read the packet flags
    memcpy(&packetStateFlags, sourceBuffer, sizeof(packetStateFlags));

    #define HAS_FLAG(B,F) ((B & F) == F)

    bool hasAvatarGlobalPosition = HAS_FLAG(packetStateFlags, AvatarDataPacket::PACKET_HAS_AVATAR_GLOBAL_POSITION);
    bool hasAvatarLocalPosition  = HAS_FLAG(packetStateFlags, AvatarDataPacket::PACKET_HAS_AVATAR_LOCAL_POSITION);
    bool hasAvatarDimensions     = HAS_FLAG(packetStateFlags, AvatarDataPacket::PACKET_HAS_AVATAR_DIMENSIONS);
    bool hasAvatarOrientation    = HAS_FLAG(packetStateFlags, AvatarDataPacket::PACKET_HAS_AVATAR_ORIENTATION);
    bool hasAvatarScale          = HAS_FLAG(packetStateFlags, AvatarDataPacket::PACKET_HAS_AVATAR_SCALE);
    bool hasLookAtPosition       = HAS_FLAG(packetStateFlags, AvatarDataPacket::PACKET_HAS_LOOK_AT_POSITION);
    bool hasAudioLoudness        = HAS_FLAG(packetStateFlags, AvatarDataPacket::PACKET_HAS_AUDIO_LOUDNESS);
    bool hasSensorToWorldMatrix  = HAS_FLAG(packetStateFlags, AvatarDataPacket::PACKET_HAS_SENSOR_TO_WORLD_MATRIX);
    bool hasAdditionalFlags      = HAS_FLAG(packetStateFlags, AvatarDataPacket::PACKET_HAS_ADDITIONAL_FLAGS);
    bool hasParentInfo           = HAS_FLAG(packetStateFlags, AvatarDataPacket::PACKET_HAS_PARENT_INFO);
    bool hasFaceTrackerInfo      = HAS_FLAG(packetStateFlags, AvatarDataPacket::PACKET_HAS_FACE_TRACKER_INFO);
    bool hasJointData            = HAS_FLAG(packetStateFlags, AvatarDataPacket::PACKET_HAS_JOINT_DATA);

    sourceBuffer += sizeof(AvatarDataPacket::HasFlags);

    quint64 now = usecTimestampNow();

    if (hasAvatarGlobalPosition) {
        PACKET_READ_CHECK(AvatarGlobalPosition, sizeof(AvatarDataPacket::AvatarGlobalPosition));
        auto data = reinterpret_cast<const AvatarDataPacket::AvatarGlobalPosition*>(sourceBuffer);
        _globalPosition = glm::vec3(data->globalPosition[0], data->globalPosition[1], data->globalPosition[2]);
        sourceBuffer += sizeof(AvatarDataPacket::AvatarGlobalPosition);
        //qDebug() << "hasAvatarGlobalPosition _globalPosition:" << _globalPosition;
    }

    if (hasAvatarLocalPosition) {
        PACKET_READ_CHECK(AvatarLocalPosition, sizeof(AvatarDataPacket::AvatarLocalPosition));
        auto data = reinterpret_cast<const AvatarDataPacket::AvatarLocalPosition*>(sourceBuffer);
        glm::vec3 position = glm::vec3(data->localPosition[0], data->localPosition[1], data->localPosition[2]);
        if (isNaN(position)) {
            if (shouldLogError(now)) {
                qCWarning(avatars) << "Discard AvatarData packet: position NaN, uuid " << getSessionUUID();
            }
            return buffer.size();
        }
        setLocalPosition(position);
        sourceBuffer += sizeof(AvatarDataPacket::AvatarLocalPosition);
        //qDebug() << "hasAvatarLocalPosition position:" << position;
    }

    if (hasAvatarDimensions) {
        PACKET_READ_CHECK(AvatarDimensions, sizeof(AvatarDataPacket::AvatarDimensions));
        auto data = reinterpret_cast<const AvatarDataPacket::AvatarDimensions*>(sourceBuffer);

        // FIXME - this is suspicious looking!
        _globalBoundingBoxCorner = glm::vec3(data->avatarDimensions[0], data->avatarDimensions[1], data->avatarDimensions[2]);
        sourceBuffer += sizeof(AvatarDataPacket::AvatarDimensions);
        //qDebug() << "hasAvatarDimensions _globalBoundingBoxCorner:" << _globalBoundingBoxCorner;
    }

    if (hasAvatarOrientation) {
        PACKET_READ_CHECK(AvatarOrientation, sizeof(AvatarDataPacket::AvatarOrientation));
        auto data = reinterpret_cast<const AvatarDataPacket::AvatarOrientation*>(sourceBuffer);
        float pitch, yaw, roll;
        unpackFloatAngleFromTwoByte(data->localOrientation + 0, &yaw);
        unpackFloatAngleFromTwoByte(data->localOrientation + 1, &pitch);
        unpackFloatAngleFromTwoByte(data->localOrientation + 2, &roll);
        if (isNaN(yaw) || isNaN(pitch) || isNaN(roll)) {
            if (shouldLogError(now)) {
                qCWarning(avatars) << "Discard AvatarData packet: localOriention is NaN, uuid " << getSessionUUID();
            }
            return buffer.size();
        }

        glm::quat currentOrientation = getLocalOrientation();
        glm::vec3 newEulerAngles(pitch, yaw, roll);
        glm::quat newOrientation = glm::quat(glm::radians(newEulerAngles));
        if (currentOrientation != newOrientation) {
            _hasNewJointRotations = true;
            setLocalOrientation(newOrientation);
        }
        sourceBuffer += sizeof(AvatarDataPacket::AvatarOrientation);
        //qDebug() << "hasAvatarOrientation newOrientation:" << newOrientation;
    }

    if (hasAvatarScale) {
        PACKET_READ_CHECK(AvatarScale, sizeof(AvatarDataPacket::AvatarScale));
        auto data = reinterpret_cast<const AvatarDataPacket::AvatarScale*>(sourceBuffer);
        float scale;
        unpackFloatRatioFromTwoByte((uint8_t*)&data->scale, scale);
        if (isNaN(scale)) {
            if (shouldLogError(now)) {
                qCWarning(avatars) << "Discard AvatarData packet: scale NaN, uuid " << getSessionUUID();
            }
            return buffer.size();
        }
        setTargetScale(scale);
        sourceBuffer += sizeof(AvatarDataPacket::AvatarScale);
        //qDebug() << "hasAvatarOrientation scale:" << scale;
    }

    if (hasLookAtPosition) {
        PACKET_READ_CHECK(LookAtPosition, sizeof(AvatarDataPacket::LookAtPosition));
        auto data = reinterpret_cast<const AvatarDataPacket::LookAtPosition*>(sourceBuffer);
        glm::vec3 lookAt = glm::vec3(data->lookAtPosition[0], data->lookAtPosition[1], data->lookAtPosition[2]);
        if (isNaN(lookAt)) {
            if (shouldLogError(now)) {
                qCWarning(avatars) << "Discard AvatarData packet: lookAtPosition is NaN, uuid " << getSessionUUID();
            }
            return buffer.size();
        }
        _headData->_lookAtPosition = lookAt;
        sourceBuffer += sizeof(AvatarDataPacket::LookAtPosition);
        //qDebug() << "hasLookAtPosition lookAt:" << lookAt;
    }

    if (hasAudioLoudness) {
        PACKET_READ_CHECK(AudioLoudness, sizeof(AvatarDataPacket::AudioLoudness));
        auto data = reinterpret_cast<const AvatarDataPacket::AudioLoudness*>(sourceBuffer);
        float audioLoudness;
        unpackFloatScalarFromSignedTwoByteFixed((int16_t*)&data->audioLoudness, &audioLoudness, AUDIO_LOUDNESS_RADIX);

        if (isNaN(audioLoudness)) {
            if (shouldLogError(now)) {
                qCWarning(avatars) << "Discard AvatarData packet: audioLoudness is NaN, uuid " << getSessionUUID();
            }
            return buffer.size();
        }
        _headData->_audioLoudness = audioLoudness;
        sourceBuffer += sizeof(AvatarDataPacket::AudioLoudness);
        //qDebug() << "hasAudioLoudness audioLoudness:" << audioLoudness;
    }

    if (hasSensorToWorldMatrix) {
        PACKET_READ_CHECK(SensorToWorldMatrix, sizeof(AvatarDataPacket::SensorToWorldMatrix));
        auto data = reinterpret_cast<const AvatarDataPacket::SensorToWorldMatrix*>(sourceBuffer);
        glm::quat sensorToWorldQuat;
        unpackOrientationQuatFromSixBytes(data->sensorToWorldQuat, sensorToWorldQuat);
        float sensorToWorldScale;
        unpackFloatScalarFromSignedTwoByteFixed((int16_t*)&data->sensorToWorldScale, &sensorToWorldScale, SENSOR_TO_WORLD_SCALE_RADIX);
        glm::vec3 sensorToWorldTrans(data->sensorToWorldTrans[0], data->sensorToWorldTrans[1], data->sensorToWorldTrans[2]);
        glm::mat4 sensorToWorldMatrix = createMatFromScaleQuatAndPos(glm::vec3(sensorToWorldScale), sensorToWorldQuat, sensorToWorldTrans);
        _sensorToWorldMatrixCache.set(sensorToWorldMatrix);
        sourceBuffer += sizeof(AvatarDataPacket::SensorToWorldMatrix);
        //qDebug() << "hasSensorToWorldMatrix sensorToWorldMatrix:" << sensorToWorldMatrix;
    }

    if (hasAdditionalFlags) {
        PACKET_READ_CHECK(AdditionalFlags, sizeof(AvatarDataPacket::AdditionalFlags));
        auto data = reinterpret_cast<const AvatarDataPacket::AdditionalFlags*>(sourceBuffer);
        uint8_t bitItems = data->flags;

        // key state, stored as a semi-nibble in the bitItems
        _keyState = (KeyState)getSemiNibbleAt(bitItems, KEY_STATE_START_BIT);

        // hand state, stored as a semi-nibble plus a bit in the bitItems
        // we store the hand state as well as other items in a shared bitset. The hand state is an octal, but is split
        // into two sections to maintain backward compatibility. The bits are ordered as such (0-7 left to right).
        //     +---+-----+-----+--+
        //     |x,x|H0,H1|x,x,x|H2|
        //     +---+-----+-----+--+
        // Hand state - H0,H1,H2 is found in the 3rd, 4th, and 8th bits
        _handState = getSemiNibbleAt(bitItems, HAND_STATE_START_BIT)
            + (oneAtBit(bitItems, HAND_STATE_FINGER_POINTING_BIT) ? IS_FINGER_POINTING_FLAG : 0);

        _headData->_isFaceTrackerConnected = oneAtBit(bitItems, IS_FACESHIFT_CONNECTED);
        _headData->_isEyeTrackerConnected = oneAtBit(bitItems, IS_EYE_TRACKER_CONNECTED);

        //qDebug() << "hasAdditionalFlags bitItems:" << bitItems;
    }

    // FIXME -- make sure to handle the existance of a parent vs a change in the parent...
    //bool hasReferential = oneAtBit(bitItems, HAS_REFERENTIAL);
    if (hasParentInfo) {
        PACKET_READ_CHECK(ParentInfo, sizeof(AvatarDataPacket::ParentInfo));
        auto parentInfo = reinterpret_cast<const AvatarDataPacket::ParentInfo*>(sourceBuffer);
        sourceBuffer += sizeof(AvatarDataPacket::ParentInfo);

        QByteArray byteArray((const char*)parentInfo->parentUUID, NUM_BYTES_RFC4122_UUID);
        _parentID = QUuid::fromRfc4122(byteArray);
        _parentJointIndex = parentInfo->parentJointIndex;
        //qDebug() << "hasParentInfo _parentID:" << _parentID;
    } else {
        _parentID = QUuid();
    }

    if (hasFaceTrackerInfo) {
        PACKET_READ_CHECK(FaceTrackerInfo, sizeof(AvatarDataPacket::FaceTrackerInfo));
        auto faceTrackerInfo = reinterpret_cast<const AvatarDataPacket::FaceTrackerInfo*>(sourceBuffer);
        sourceBuffer += sizeof(AvatarDataPacket::FaceTrackerInfo);

        _headData->_leftEyeBlink = faceTrackerInfo->leftEyeBlink;
        _headData->_rightEyeBlink = faceTrackerInfo->rightEyeBlink;
        _headData->_averageLoudness = faceTrackerInfo->averageLoudness;
        _headData->_browAudioLift = faceTrackerInfo->browAudioLift;

        int numCoefficients = faceTrackerInfo->numBlendshapeCoefficients;
        const int coefficientsSize = sizeof(float) * numCoefficients;
        PACKET_READ_CHECK(FaceTrackerCoefficients, coefficientsSize);
        _headData->_blendshapeCoefficients.resize(numCoefficients);  // make sure there's room for the copy!
        memcpy(_headData->_blendshapeCoefficients.data(), sourceBuffer, coefficientsSize);
        sourceBuffer += coefficientsSize;
        //qDebug() << "hasFaceTrackerInfo numCoefficients:" << numCoefficients;
    }

    if (hasJointData) {
        PACKET_READ_CHECK(NumJoints, sizeof(uint8_t));
        int numJoints = *sourceBuffer++;
        //qDebug() << "hasJointData numJoints:" << numJoints;

        const int bytesOfValidity = (int)ceil((float)numJoints / (float)BITS_IN_BYTE);
        PACKET_READ_CHECK(JointRotationValidityBits, bytesOfValidity);

        int numValidJointRotations = 0;
        QVector<bool> validRotations;
        validRotations.resize(numJoints);
        { // rotation validity bits
            unsigned char validity = 0;
            int validityBit = 0;
            for (int i = 0; i < numJoints; i++) {
                if (validityBit == 0) {
                    validity = *sourceBuffer++;
                }
                bool valid = (bool)(validity & (1 << validityBit));
                if (valid) {
                    ++numValidJointRotations;
                }
                validRotations[i] = valid;
                validityBit = (validityBit + 1) % BITS_IN_BYTE;
            }
        }

        // each joint rotation is stored in 6 bytes.
        QWriteLocker writeLock(&_jointDataLock);
        _jointData.resize(numJoints);

        const int COMPRESSED_QUATERNION_SIZE = 6;
        PACKET_READ_CHECK(JointRotations, numValidJointRotations * COMPRESSED_QUATERNION_SIZE);
        for (int i = 0; i < numJoints; i++) {
            JointData& data = _jointData[i];
            if (validRotations[i]) {
                sourceBuffer += unpackOrientationQuatFromSixBytes(sourceBuffer, data.rotation);
                _hasNewJointRotations = true;
                data.rotationSet = true;
            }
        }

        PACKET_READ_CHECK(JointTranslationValidityBits, bytesOfValidity);

        // get translation validity bits -- these indicate which translations were packed
        int numValidJointTranslations = 0;
        QVector<bool> validTranslations;
        validTranslations.resize(numJoints);
        { // translation validity bits
            unsigned char validity = 0;
            int validityBit = 0;
            for (int i = 0; i < numJoints; i++) {
                if (validityBit == 0) {
                    validity = *sourceBuffer++;
                }
                bool valid = (bool)(validity & (1 << validityBit));
                if (valid) {
                    ++numValidJointTranslations;
                }
                validTranslations[i] = valid;
                validityBit = (validityBit + 1) % BITS_IN_BYTE;
            }
        } // 1 + bytesOfValidity bytes

        // each joint translation component is stored in 6 bytes.
        const int COMPRESSED_TRANSLATION_SIZE = 6;
        PACKET_READ_CHECK(JointTranslation, numValidJointTranslations * COMPRESSED_TRANSLATION_SIZE);

        for (int i = 0; i < numJoints; i++) {
            JointData& data = _jointData[i];
            if (validTranslations[i]) {
                sourceBuffer += unpackFloatVec3FromSignedTwoByteFixed(sourceBuffer, data.translation, TRANSLATION_COMPRESSION_RADIX);
                _hasNewJointTranslations = true;
                data.translationSet = true;
            }
        }

#ifdef WANT_DEBUG
        if (numValidJointRotations > 15) {
            qCDebug(avatars) << "RECEIVING -- rotations:" << numValidJointRotations
                << "translations:" << numValidJointTranslations
                << "size:" << (int)(sourceBuffer - startPosition);
        }
#endif
        // faux joints
        sourceBuffer = unpackFauxJoint(sourceBuffer, _controllerLeftHandMatrixCache);
        sourceBuffer = unpackFauxJoint(sourceBuffer, _controllerRightHandMatrixCache);
    }

    int numBytesRead = sourceBuffer - startPosition;
    _averageBytesReceived.updateAverage(numBytesRead);
    return numBytesRead;
}

int AvatarData::getAverageBytesReceivedPerSecond() const {
    return lrint(_averageBytesReceived.getAverageSampleValuePerSecond());
}

int AvatarData::getReceiveRate() const {
    return lrint(1.0f / _averageBytesReceived.getEventDeltaAverage());
}

std::shared_ptr<Transform> AvatarData::getRecordingBasis() const {
    return _recordingBasis;
}

void AvatarData::setRawJointData(QVector<JointData> data) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "setRawJointData", Q_ARG(QVector<JointData>, data));
        return;
    }
    QWriteLocker writeLock(&_jointDataLock);
    _jointData = data;
}

void AvatarData::setJointData(int index, const glm::quat& rotation, const glm::vec3& translation) {
    if (index == -1) {
        return;
    }
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "setJointData", Q_ARG(int, index), Q_ARG(const glm::quat&, rotation));
        return;
    }
    QWriteLocker writeLock(&_jointDataLock);
    if (_jointData.size() <= index) {
        _jointData.resize(index + 1);
    }
    JointData& data = _jointData[index];
    data.rotation = rotation;
    data.rotationSet = true;
    data.translation = translation;
    data.translationSet = true;
}

void AvatarData::clearJointData(int index) {
    if (index == -1) {
        return;
    }
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "clearJointData", Q_ARG(int, index));
        return;
    }
    QWriteLocker writeLock(&_jointDataLock);
    // FIXME: I don't understand how this "clears" the joint data at index
    if (_jointData.size() <= index) {
        _jointData.resize(index + 1);
    }
}

bool AvatarData::isJointDataValid(int index) const {
    if (index == -1) {
        return false;
    }
    if (QThread::currentThread() != thread()) {
        bool result;
        QMetaObject::invokeMethod(const_cast<AvatarData*>(this), "isJointDataValid", Qt::BlockingQueuedConnection,
            Q_RETURN_ARG(bool, result), Q_ARG(int, index));
        return result;
    }
    return index < _jointData.size();
}

glm::quat AvatarData::getJointRotation(int index) const {
    if (index == -1) {
        return glm::quat();
    }
    if (QThread::currentThread() != thread()) {
        glm::quat result;
        QMetaObject::invokeMethod(const_cast<AvatarData*>(this), "getJointRotation", Qt::BlockingQueuedConnection,
            Q_RETURN_ARG(glm::quat, result), Q_ARG(int, index));
        return result;
    }
    QReadLocker readLock(&_jointDataLock);
    return index < _jointData.size() ? _jointData.at(index).rotation : glm::quat();
}


glm::vec3 AvatarData::getJointTranslation(int index) const {
    if (index == -1) {
        return glm::vec3();
    }
    if (QThread::currentThread() != thread()) {
        glm::vec3 result;
        QMetaObject::invokeMethod(const_cast<AvatarData*>(this), "getJointTranslation", Qt::BlockingQueuedConnection,
            Q_RETURN_ARG(glm::vec3, result), Q_ARG(int, index));
        return result;
    }
    QReadLocker readLock(&_jointDataLock);
    return index < _jointData.size() ? _jointData.at(index).translation : glm::vec3();
}

glm::vec3 AvatarData::getJointTranslation(const QString& name) const {
    if (QThread::currentThread() != thread()) {
        glm::vec3 result;
        QMetaObject::invokeMethod(const_cast<AvatarData*>(this), "getJointTranslation", Qt::BlockingQueuedConnection,
            Q_RETURN_ARG(glm::vec3, result), Q_ARG(const QString&, name));
        return result;
    }
    return getJointTranslation(getJointIndex(name));
}

void AvatarData::setJointData(const QString& name, const glm::quat& rotation, const glm::vec3& translation) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "setJointData", Q_ARG(const QString&, name), Q_ARG(const glm::quat&, rotation),
            Q_ARG(const glm::vec3&, translation));
        return;
    }
    setJointData(getJointIndex(name), rotation, translation);
}

void AvatarData::setJointRotation(const QString& name, const glm::quat& rotation) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "setJointRotation", Q_ARG(const QString&, name), Q_ARG(const glm::quat&, rotation));
        return;
    }
    setJointRotation(getJointIndex(name), rotation);
}

void AvatarData::setJointTranslation(const QString& name, const glm::vec3& translation) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "setJointTranslation", Q_ARG(const QString&, name),
            Q_ARG(const glm::vec3&, translation));
        return;
    }
    setJointTranslation(getJointIndex(name), translation);
}

void AvatarData::setJointRotation(int index, const glm::quat& rotation) {
    if (index == -1) {
        return;
    }
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "setJointRotation", Q_ARG(int, index), Q_ARG(const glm::quat&, rotation));
        return;
    }
    QWriteLocker writeLock(&_jointDataLock);
    if (_jointData.size() <= index) {
        _jointData.resize(index + 1);
    }
    JointData& data = _jointData[index];
    data.rotation = rotation;
    data.rotationSet = true;
}

void AvatarData::setJointTranslation(int index, const glm::vec3& translation) {
    if (index == -1) {
        return;
    }
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "setJointTranslation", Q_ARG(int, index), Q_ARG(const glm::vec3&, translation));
        return;
    }
    QWriteLocker writeLock(&_jointDataLock);
    if (_jointData.size() <= index) {
        _jointData.resize(index + 1);
    }
    JointData& data = _jointData[index];
    data.translation = translation;
    data.translationSet = true;
}

void AvatarData::clearJointData(const QString& name) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "clearJointData", Q_ARG(const QString&, name));
        return;
    }
    clearJointData(getJointIndex(name));
}

bool AvatarData::isJointDataValid(const QString& name) const {
    if (QThread::currentThread() != thread()) {
        bool result;
        QMetaObject::invokeMethod(const_cast<AvatarData*>(this), "isJointDataValid", Qt::BlockingQueuedConnection,
            Q_RETURN_ARG(bool, result), Q_ARG(const QString&, name));
        return result;
    }
    return isJointDataValid(getJointIndex(name));
}

glm::quat AvatarData::getJointRotation(const QString& name) const {
    if (QThread::currentThread() != thread()) {
        glm::quat result;
        QMetaObject::invokeMethod(const_cast<AvatarData*>(this), "getJointRotation", Qt::BlockingQueuedConnection,
            Q_RETURN_ARG(glm::quat, result), Q_ARG(const QString&, name));
        return result;
    }
    return getJointRotation(getJointIndex(name));
}

QVector<glm::quat> AvatarData::getJointRotations() const {
    if (QThread::currentThread() != thread()) {
        QVector<glm::quat> result;
        QMetaObject::invokeMethod(const_cast<AvatarData*>(this),
                                  "getJointRotations", Qt::BlockingQueuedConnection,
                                  Q_RETURN_ARG(QVector<glm::quat>, result));
        return result;
    }
    QReadLocker readLock(&_jointDataLock);
    QVector<glm::quat> jointRotations(_jointData.size());
    for (int i = 0; i < _jointData.size(); ++i) {
        jointRotations[i] = _jointData[i].rotation;
    }
    return jointRotations;
}

void AvatarData::setJointRotations(QVector<glm::quat> jointRotations) {
    if (QThread::currentThread() != thread()) {
        QVector<glm::quat> result;
        QMetaObject::invokeMethod(const_cast<AvatarData*>(this),
                                  "setJointRotations", Qt::BlockingQueuedConnection,
                                  Q_ARG(QVector<glm::quat>, jointRotations));
    }
    QWriteLocker writeLock(&_jointDataLock);
    if (_jointData.size() < jointRotations.size()) {
        _jointData.resize(jointRotations.size());
    }
    for (int i = 0; i < jointRotations.size(); ++i) {
        if (i < _jointData.size()) {
            setJointRotation(i, jointRotations[i]);
        }
    }
}

void AvatarData::setJointTranslations(QVector<glm::vec3> jointTranslations) {
    if (QThread::currentThread() != thread()) {
        QVector<glm::quat> result;
        QMetaObject::invokeMethod(const_cast<AvatarData*>(this),
                                  "setJointTranslations", Qt::BlockingQueuedConnection,
                                  Q_ARG(QVector<glm::vec3>, jointTranslations));
    }
    QWriteLocker writeLock(&_jointDataLock);
    if (_jointData.size() < jointTranslations.size()) {
        _jointData.resize(jointTranslations.size());
    }
    for (int i = 0; i < jointTranslations.size(); ++i) {
        if (i < _jointData.size()) {
            setJointTranslation(i, jointTranslations[i]);
        }
    }
}

void AvatarData::clearJointsData() {
    // FIXME: this method is terribly inefficient and probably doesn't even work
    // (see implementation of clearJointData(index))
    for (int i = 0; i < _jointData.size(); ++i) {
        clearJointData(i);
    }
}

int AvatarData::getFauxJointIndex(const QString& name) const {
    if (name == "_SENSOR_TO_WORLD_MATRIX") {
        return SENSOR_TO_WORLD_MATRIX_INDEX;
    }
    if (name == "_CONTROLLER_LEFTHAND") {
        return CONTROLLER_LEFTHAND_INDEX;
    }
    if (name == "_CONTROLLER_RIGHTHAND") {
        return CONTROLLER_RIGHTHAND_INDEX;
    }
    if (name == "_CAMERA_RELATIVE_CONTROLLER_LEFTHAND") {
        return CAMERA_RELATIVE_CONTROLLER_LEFTHAND_INDEX;
    }
    if (name == "_CAMERA_RELATIVE_CONTROLLER_RIGHTHAND") {
        return CAMERA_RELATIVE_CONTROLLER_RIGHTHAND_INDEX;
    }
    return -1;
}

int AvatarData::getJointIndex(const QString& name) const {
    int result = getFauxJointIndex(name);
    if (result != -1) {
        return result;
    }
    QReadLocker readLock(&_jointDataLock);
    return _jointIndices.value(name) - 1;
}

QStringList AvatarData::getJointNames() const {
    QReadLocker readLock(&_jointDataLock);
    return _jointNames;
}

void AvatarData::parseAvatarIdentityPacket(const QByteArray& data, Identity& identityOut) {
    QDataStream packetStream(data);

    packetStream >> identityOut.uuid >> identityOut.skeletonModelURL >> identityOut.attachmentData >> identityOut.displayName >> identityOut.sessionDisplayName >> identityOut.avatarEntityData;
}

static const QUrl emptyURL("");
const QUrl& AvatarData::cannonicalSkeletonModelURL(const QUrl& emptyURL) {
    // We don't put file urls on the wire, but instead convert to empty.
    return _skeletonModelURL.scheme() == "file" ? emptyURL : _skeletonModelURL;
}

bool AvatarData::processAvatarIdentity(const Identity& identity) {
    bool hasIdentityChanged = false;

    if (_firstSkeletonCheck || (identity.skeletonModelURL != cannonicalSkeletonModelURL(emptyURL))) {
        setSkeletonModelURL(identity.skeletonModelURL);
        hasIdentityChanged = true;
        _firstSkeletonCheck = false;
    }

    if (identity.displayName != _displayName) {
        setDisplayName(identity.displayName);
        hasIdentityChanged = true;
    }
    maybeUpdateSessionDisplayNameFromTransport(identity.sessionDisplayName);

    if (identity.attachmentData != _attachmentData) {
        setAttachmentData(identity.attachmentData);
        hasIdentityChanged = true;
    }

    bool avatarEntityDataChanged = false;
    _avatarEntitiesLock.withReadLock([&] {
        avatarEntityDataChanged = (identity.avatarEntityData != _avatarEntityData);
    });
    if (avatarEntityDataChanged) {
        setAvatarEntityData(identity.avatarEntityData);
        hasIdentityChanged = true;
    }

    return hasIdentityChanged;
}

QByteArray AvatarData::identityByteArray() {
    QByteArray identityData;
    QDataStream identityStream(&identityData, QIODevice::Append);
    const QUrl& urlToSend = cannonicalSkeletonModelURL(emptyURL);

    _avatarEntitiesLock.withReadLock([&] {
        identityStream << getSessionUUID() << urlToSend << _attachmentData << _displayName << getSessionDisplayNameForTransport() << _avatarEntityData;
    });

    return identityData;
}

void AvatarData::setSkeletonModelURL(const QUrl& skeletonModelURL) {
    const QUrl& expanded = skeletonModelURL.isEmpty() ? AvatarData::defaultFullAvatarModelUrl() : skeletonModelURL;
    if (expanded == _skeletonModelURL) {
        return;
    }
    _skeletonModelURL = expanded;
    qCDebug(avatars) << "Changing skeleton model for avatar" << getSessionUUID() << "to" << _skeletonModelURL.toString();

    updateJointMappings();
}

void AvatarData::setDisplayName(const QString& displayName) {
    _displayName = displayName;

    qCDebug(avatars) << "Changing display name for avatar to" << displayName;
}

QVector<AttachmentData> AvatarData::getAttachmentData() const {
    if (QThread::currentThread() != thread()) {
        QVector<AttachmentData> result;
        QMetaObject::invokeMethod(const_cast<AvatarData*>(this), "getAttachmentData", Qt::BlockingQueuedConnection,
            Q_RETURN_ARG(QVector<AttachmentData>, result));
        return result;
    }
    return _attachmentData;
}

void AvatarData::setAttachmentData(const QVector<AttachmentData>& attachmentData) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "setAttachmentData", Q_ARG(const QVector<AttachmentData>&, attachmentData));
        return;
    }
    _attachmentData = attachmentData;
}

void AvatarData::attach(const QString& modelURL, const QString& jointName,
                        const glm::vec3& translation, const glm::quat& rotation,
                        float scale, bool isSoft,
                        bool allowDuplicates, bool useSaved) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "attach", Q_ARG(const QString&, modelURL), Q_ARG(const QString&, jointName),
                                  Q_ARG(const glm::vec3&, translation), Q_ARG(const glm::quat&, rotation),
                                  Q_ARG(float, scale), Q_ARG(bool, isSoft),
                                  Q_ARG(bool, allowDuplicates), Q_ARG(bool, useSaved));
        return;
    }
    QVector<AttachmentData> attachmentData = getAttachmentData();
    if (!allowDuplicates) {
        foreach (const AttachmentData& data, attachmentData) {
            if (data.modelURL == modelURL && (jointName.isEmpty() || data.jointName == jointName)) {
                return;
            }
        }
    }
    AttachmentData data;
    data.modelURL = modelURL;
    data.jointName = jointName;
    data.translation = translation;
    data.rotation = rotation;
    data.scale = scale;
    data.isSoft = isSoft;
    attachmentData.append(data);
    setAttachmentData(attachmentData);
}

void AvatarData::detachOne(const QString& modelURL, const QString& jointName) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "detachOne", Q_ARG(const QString&, modelURL), Q_ARG(const QString&, jointName));
        return;
    }
    QVector<AttachmentData> attachmentData = getAttachmentData();
    for (QVector<AttachmentData>::iterator it = attachmentData.begin(); it != attachmentData.end(); ++it) {
        if (it->modelURL == modelURL && (jointName.isEmpty() || it->jointName == jointName)) {
            attachmentData.erase(it);
            setAttachmentData(attachmentData);
            return;
        }
    }
}

void AvatarData::detachAll(const QString& modelURL, const QString& jointName) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "detachAll", Q_ARG(const QString&, modelURL), Q_ARG(const QString&, jointName));
        return;
    }
    QVector<AttachmentData> attachmentData = getAttachmentData();
    for (QVector<AttachmentData>::iterator it = attachmentData.begin(); it != attachmentData.end(); ) {
        if (it->modelURL == modelURL && (jointName.isEmpty() || it->jointName == jointName)) {
            it = attachmentData.erase(it);
        } else {
            ++it;
        }
    }
    setAttachmentData(attachmentData);
}

void AvatarData::setJointMappingsFromNetworkReply() {
    QNetworkReply* networkReply = static_cast<QNetworkReply*>(sender());

    {
        QWriteLocker writeLock(&_jointDataLock);
        QByteArray line;
        while (!(line = networkReply->readLine()).isEmpty()) {
            line = line.trimmed();
            if (line.startsWith("filename")) {
                int filenameIndex = line.indexOf('=') + 1;
                    if (filenameIndex > 0) {
                        _skeletonFBXURL = _skeletonModelURL.resolved(QString(line.mid(filenameIndex).trimmed()));
                    }
                }
            if (!line.startsWith("jointIndex")) {
                continue;
            }
            int jointNameIndex = line.indexOf('=') + 1;
            if (jointNameIndex == 0) {
                continue;
            }
            int secondSeparatorIndex = line.indexOf('=', jointNameIndex);
            if (secondSeparatorIndex == -1) {
                continue;
            }
            QString jointName = line.mid(jointNameIndex, secondSeparatorIndex - jointNameIndex).trimmed();
            bool ok;
            int jointIndex = line.mid(secondSeparatorIndex + 1).trimmed().toInt(&ok);
            if (ok) {
                while (_jointNames.size() < jointIndex + 1) {
                    _jointNames.append(QString());
                }
                _jointNames[jointIndex] = jointName;
            }
        }
        for (int i = 0; i < _jointNames.size(); i++) {
            _jointIndices.insert(_jointNames.at(i), i + 1);
        }
    }

    networkReply->deleteLater();
}

void AvatarData::sendAvatarDataPacket() {
    auto nodeList = DependencyManager::get<NodeList>();

    // about 2% of the time, we send a full update (meaning, we transmit all the joint data), even if nothing has changed.
    // this is to guard against a joint moving once, the packet getting lost, and the joint never moving again.
    QByteArray avatarByteArray = toByteArray((randFloat() < AVATAR_SEND_FULL_UPDATE_RATIO) ? SendAllData : CullSmallData);

    doneEncoding(true); // FIXME - doneEncoding() takes a bool for culling small changes, that's janky!

    static AvatarDataSequenceNumber sequenceNumber = 0;

    auto avatarPacket = NLPacket::create(PacketType::AvatarData, avatarByteArray.size() + sizeof(sequenceNumber));
    avatarPacket->writePrimitive(sequenceNumber++);
    avatarPacket->write(avatarByteArray);

    nodeList->broadcastToNodes(std::move(avatarPacket), NodeSet() << NodeType::AvatarMixer);
}

void AvatarData::sendIdentityPacket() {
    auto nodeList = DependencyManager::get<NodeList>();

    QByteArray identityData = identityByteArray();

    auto packetList = NLPacketList::create(PacketType::AvatarIdentity, QByteArray(), true, true);
    packetList->write(identityData);
    nodeList->eachMatchingNode(
        [&](const SharedNodePointer& node)->bool {
            return node->getType() == NodeType::AvatarMixer && node->getActiveSocket();
        },
        [&](const SharedNodePointer& node) {
            nodeList->sendPacketList(std::move(packetList), *node);
        });

    _avatarEntityDataLocallyEdited = false;
}

void AvatarData::updateJointMappings() {
    {
        QWriteLocker writeLock(&_jointDataLock);
        _jointIndices.clear();
        _jointNames.clear();
        _jointData.clear();
    }

    if (_skeletonModelURL.fileName().toLower().endsWith(".fst")) {
        QNetworkAccessManager& networkAccessManager = NetworkAccessManager::getInstance();
        QNetworkRequest networkRequest = QNetworkRequest(_skeletonModelURL);
        networkRequest.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
        networkRequest.setHeader(QNetworkRequest::UserAgentHeader, HIGH_FIDELITY_USER_AGENT);
        QNetworkReply* networkReply = networkAccessManager.get(networkRequest);
        connect(networkReply, &QNetworkReply::finished, this, &AvatarData::setJointMappingsFromNetworkReply);
    }
}

static const QString JSON_ATTACHMENT_URL = QStringLiteral("modelUrl");
static const QString JSON_ATTACHMENT_JOINT_NAME = QStringLiteral("jointName");
static const QString JSON_ATTACHMENT_TRANSFORM = QStringLiteral("transform");
static const QString JSON_ATTACHMENT_IS_SOFT = QStringLiteral("isSoft");

QJsonObject AttachmentData::toJson() const {
    QJsonObject result;
    if (modelURL.isValid() && !modelURL.isEmpty()) {
        result[JSON_ATTACHMENT_URL] = modelURL.toString();
    }
    if (!jointName.isEmpty()) {
        result[JSON_ATTACHMENT_JOINT_NAME] = jointName;
    }
    // FIXME the transform constructor that takes rot/scale/translation
    // doesn't return the correct value for isIdentity()
    Transform transform;
    transform.setRotation(rotation);
    transform.setScale(scale);
    transform.setTranslation(translation);
    if (!transform.isIdentity()) {
        result[JSON_ATTACHMENT_TRANSFORM] = Transform::toJson(transform);
    }
    result[JSON_ATTACHMENT_IS_SOFT] = isSoft;
    return result;
}

void AttachmentData::fromJson(const QJsonObject& json) {
    if (json.contains(JSON_ATTACHMENT_URL)) {
        const QString modelURLTemp = json[JSON_ATTACHMENT_URL].toString();
        if (modelURLTemp != modelURL.toString()) {
            modelURL = modelURLTemp;
        }
    }

    if (json.contains(JSON_ATTACHMENT_JOINT_NAME)) {
        const QString jointNameTemp = json[JSON_ATTACHMENT_JOINT_NAME].toString();
        if (jointNameTemp != jointName) {
            jointName = jointNameTemp;
        }
    }

    if (json.contains(JSON_ATTACHMENT_TRANSFORM)) {
        Transform transform = Transform::fromJson(json[JSON_ATTACHMENT_TRANSFORM]);
        translation = transform.getTranslation();
        rotation = transform.getRotation();
        scale = transform.getScale().x;
    }

    if (json.contains(JSON_ATTACHMENT_IS_SOFT)) {
        isSoft = json[JSON_ATTACHMENT_IS_SOFT].toBool();
    }
}

bool AttachmentData::operator==(const AttachmentData& other) const {
    return modelURL == other.modelURL && jointName == other.jointName && translation == other.translation &&
        rotation == other.rotation && scale == other.scale && isSoft == other.isSoft;
}

QDataStream& operator<<(QDataStream& out, const AttachmentData& attachment) {
    return out << attachment.modelURL << attachment.jointName <<
        attachment.translation << attachment.rotation << attachment.scale << attachment.isSoft;
}

QDataStream& operator>>(QDataStream& in, AttachmentData& attachment) {
    return in >> attachment.modelURL >> attachment.jointName >>
        attachment.translation >> attachment.rotation >> attachment.scale >> attachment.isSoft;
}

void AttachmentDataObject::setModelURL(const QString& modelURL) {
    AttachmentData data = qscriptvalue_cast<AttachmentData>(thisObject());
    data.modelURL = modelURL;
    thisObject() = engine()->toScriptValue(data);
}

QString AttachmentDataObject::getModelURL() const {
    return qscriptvalue_cast<AttachmentData>(thisObject()).modelURL.toString();
}

void AttachmentDataObject::setJointName(const QString& jointName) {
    AttachmentData data = qscriptvalue_cast<AttachmentData>(thisObject());
    data.jointName = jointName;
    thisObject() = engine()->toScriptValue(data);
}

QString AttachmentDataObject::getJointName() const {
    return qscriptvalue_cast<AttachmentData>(thisObject()).jointName;
}

void AttachmentDataObject::setTranslation(const glm::vec3& translation) {
    AttachmentData data = qscriptvalue_cast<AttachmentData>(thisObject());
    data.translation = translation;
    thisObject() = engine()->toScriptValue(data);
}

glm::vec3 AttachmentDataObject::getTranslation() const {
    return qscriptvalue_cast<AttachmentData>(thisObject()).translation;
}

void AttachmentDataObject::setRotation(const glm::quat& rotation) {
    AttachmentData data = qscriptvalue_cast<AttachmentData>(thisObject());
    data.rotation = rotation;
    thisObject() = engine()->toScriptValue(data);
}

glm::quat AttachmentDataObject::getRotation() const {
    return qscriptvalue_cast<AttachmentData>(thisObject()).rotation;
}

void AttachmentDataObject::setScale(float scale) {
    AttachmentData data = qscriptvalue_cast<AttachmentData>(thisObject());
    data.scale = scale;
    thisObject() = engine()->toScriptValue(data);
}

float AttachmentDataObject::getScale() const {
    return qscriptvalue_cast<AttachmentData>(thisObject()).scale;
}

void AttachmentDataObject::setIsSoft(bool isSoft) {
    AttachmentData data = qscriptvalue_cast<AttachmentData>(thisObject());
    data.isSoft = isSoft;
    thisObject() = engine()->toScriptValue(data);
}

bool AttachmentDataObject::getIsSoft() const {
    return qscriptvalue_cast<AttachmentData>(thisObject()).isSoft;
}

void registerAvatarTypes(QScriptEngine* engine) {
    qScriptRegisterSequenceMetaType<QVector<AttachmentData> >(engine);
    engine->setDefaultPrototype(qMetaTypeId<AttachmentData>(), engine->newQObject(
        new AttachmentDataObject(), QScriptEngine::ScriptOwnership));
}

void AvatarData::setRecordingBasis(std::shared_ptr<Transform> recordingBasis) {
    if (!recordingBasis) {
        recordingBasis = std::make_shared<Transform>();
        recordingBasis->setRotation(getOrientation());
        recordingBasis->setTranslation(getPosition());
        // TODO: find a  different way to record/playback the Scale of the avatar
        //recordingBasis->setScale(getTargetScale());
    }
    _recordingBasis = recordingBasis;
}

void AvatarData::clearRecordingBasis() {
    _recordingBasis.reset();
}

static const QString JSON_AVATAR_BASIS = QStringLiteral("basisTransform");
static const QString JSON_AVATAR_RELATIVE = QStringLiteral("relativeTransform");
static const QString JSON_AVATAR_JOINT_ARRAY = QStringLiteral("jointArray");
static const QString JSON_AVATAR_HEAD = QStringLiteral("head");
static const QString JSON_AVATAR_HEAD_MODEL = QStringLiteral("headModel");
static const QString JSON_AVATAR_BODY_MODEL = QStringLiteral("bodyModel");
static const QString JSON_AVATAR_DISPLAY_NAME = QStringLiteral("displayName");
// It isn't meaningful to persist sessionDisplayName.
static const QString JSON_AVATAR_ATTACHEMENTS = QStringLiteral("attachments");
static const QString JSON_AVATAR_ENTITIES = QStringLiteral("attachedEntities");
static const QString JSON_AVATAR_SCALE = QStringLiteral("scale");
static const QString JSON_AVATAR_VERSION = QStringLiteral("version");

static const int JSON_AVATAR_JOINT_ROTATIONS_IN_RELATIVE_FRAME_VERSION = 0;
static const int JSON_AVATAR_JOINT_ROTATIONS_IN_ABSOLUTE_FRAME_VERSION = 1;

QJsonValue toJsonValue(const JointData& joint) {
    QJsonArray result;
    result.push_back(toJsonValue(joint.rotation));
    result.push_back(toJsonValue(joint.translation));
    return result;
}

JointData jointDataFromJsonValue(const QJsonValue& json) {
    JointData result;
    if (json.isArray()) {
        QJsonArray array = json.toArray();
        result.rotation = quatFromJsonValue(array[0]);
        result.rotationSet = true;
        result.translation = vec3FromJsonValue(array[1]);
        result.translationSet = false;
    }
    return result;
}

QJsonObject AvatarData::toJson() const {
    QJsonObject root;

    root[JSON_AVATAR_VERSION] = JSON_AVATAR_JOINT_ROTATIONS_IN_ABSOLUTE_FRAME_VERSION;

    if (!getSkeletonModelURL().isEmpty()) {
        root[JSON_AVATAR_BODY_MODEL] = getSkeletonModelURL().toString();
    }
    if (!getDisplayName().isEmpty()) {
        root[JSON_AVATAR_DISPLAY_NAME] = getDisplayName();
    }
    if (!getAttachmentData().isEmpty()) {
        QJsonArray attachmentsJson;
        for (auto attachment : getAttachmentData()) {
            attachmentsJson.push_back(attachment.toJson());
        }
        root[JSON_AVATAR_ATTACHEMENTS] = attachmentsJson;
    }

    _avatarEntitiesLock.withReadLock([&] {
        if (!_avatarEntityData.empty()) {
            QJsonArray avatarEntityJson;
            for (auto entityID : _avatarEntityData.keys()) {
                QVariantMap entityData;
                entityData.insert("id", entityID);
                entityData.insert("properties", _avatarEntityData.value(entityID));
                avatarEntityJson.push_back(QVariant(entityData).toJsonObject());
            }
            root[JSON_AVATAR_ENTITIES] = avatarEntityJson;
        }
    });

    auto recordingBasis = getRecordingBasis();
    bool success;
    Transform avatarTransform = getTransform(success);
    if (!success) {
        qCWarning(avatars) << "Warning -- AvatarData::toJson couldn't get avatar transform";
    }
    avatarTransform.setScale(getDomainLimitedScale());
    if (recordingBasis) {
        root[JSON_AVATAR_BASIS] = Transform::toJson(*recordingBasis);
        // Find the relative transform
        auto relativeTransform = recordingBasis->relativeTransform(avatarTransform);
        if (!relativeTransform.isIdentity()) {
            root[JSON_AVATAR_RELATIVE] = Transform::toJson(relativeTransform);
        }
    } else {
        root[JSON_AVATAR_RELATIVE] = Transform::toJson(avatarTransform);
    }

    auto scale = getDomainLimitedScale();
    if (scale != 1.0f) {
        root[JSON_AVATAR_SCALE] = scale;
    }

    // Skeleton pose
    QJsonArray jointArray;
    for (const auto& joint : getRawJointData()) {
        jointArray.push_back(toJsonValue(joint));
    }
    root[JSON_AVATAR_JOINT_ARRAY] = jointArray;

    const HeadData* head = getHeadData();
    if (head) {
        auto headJson = head->toJson();
        if (!headJson.isEmpty()) {
            root[JSON_AVATAR_HEAD] = headJson;
        }
    }
    return root;
}

void AvatarData::fromJson(const QJsonObject& json) {

    int version;
    if (json.contains(JSON_AVATAR_VERSION)) {
        version = json[JSON_AVATAR_VERSION].toInt();
    } else {
        // initial data did not have a version field.
        version = JSON_AVATAR_JOINT_ROTATIONS_IN_RELATIVE_FRAME_VERSION;
    }

    // The head setOrientation likes to overwrite the avatar orientation,
    // so lets do the head first
    // Most head data is relative to the avatar, and needs no basis correction,
    // but the lookat vector does need correction
    if (json.contains(JSON_AVATAR_HEAD)) {
        if (!_headData) {
            _headData = new HeadData(this);
        }
        _headData->fromJson(json[JSON_AVATAR_HEAD].toObject());
    }

    if (json.contains(JSON_AVATAR_BODY_MODEL)) {
        auto bodyModelURL = json[JSON_AVATAR_BODY_MODEL].toString();
        if (bodyModelURL != getSkeletonModelURL().toString()) {
            setSkeletonModelURL(bodyModelURL);
        }
    }
    if (json.contains(JSON_AVATAR_DISPLAY_NAME)) {
        auto newDisplayName = json[JSON_AVATAR_DISPLAY_NAME].toString();
        if (newDisplayName != getDisplayName()) {
            setDisplayName(newDisplayName);
        }
    }

    if (json.contains(JSON_AVATAR_RELATIVE)) {
        // During playback you can either have the recording basis set to the avatar current state
        // meaning that all playback is relative to this avatars starting position, or
        // the basis can be loaded from the recording, meaning the playback is relative to the
        // original avatar location
        // The first is more useful for playing back recordings on your own avatar, while
        // the latter is more useful for playing back other avatars within your scene.

        auto currentBasis = getRecordingBasis();
        if (!currentBasis) {
            currentBasis = std::make_shared<Transform>(Transform::fromJson(json[JSON_AVATAR_BASIS]));
        }

        auto relativeTransform = Transform::fromJson(json[JSON_AVATAR_RELATIVE]);
        auto worldTransform = currentBasis->worldTransform(relativeTransform);
        setPosition(worldTransform.getTranslation());
        setOrientation(worldTransform.getRotation());
    }

    if (json.contains(JSON_AVATAR_SCALE)) {
        setTargetScale((float)json[JSON_AVATAR_SCALE].toDouble());
    }

    if (json.contains(JSON_AVATAR_ATTACHEMENTS) && json[JSON_AVATAR_ATTACHEMENTS].isArray()) {
        QJsonArray attachmentsJson = json[JSON_AVATAR_ATTACHEMENTS].toArray();
        QVector<AttachmentData> attachments;
        for (auto attachmentJson : attachmentsJson) {
            AttachmentData attachment;
            attachment.fromJson(attachmentJson.toObject());
            attachments.push_back(attachment);
        }
        setAttachmentData(attachments);
    }

    // if (json.contains(JSON_AVATAR_ENTITIES) && json[JSON_AVATAR_ENTITIES].isArray()) {
    //     QJsonArray attachmentsJson = json[JSON_AVATAR_ATTACHEMENTS].toArray();
    //     for (auto attachmentJson : attachmentsJson) {
    //         // TODO -- something
    //     }
    // }

    if (json.contains(JSON_AVATAR_JOINT_ARRAY)) {
        if (version == JSON_AVATAR_JOINT_ROTATIONS_IN_RELATIVE_FRAME_VERSION) {
            // because we don't have the full joint hierarchy skeleton of the model,
            // we can't properly convert from relative rotations into absolute rotations.
            quint64 now = usecTimestampNow();
            if (shouldLogError(now)) {
                qCWarning(avatars) << "Version 0 avatar recordings not supported. using default rotations";
            }
        } else {
            QVector<JointData> jointArray;
            QJsonArray jointArrayJson = json[JSON_AVATAR_JOINT_ARRAY].toArray();
            jointArray.reserve(jointArrayJson.size());
            int i = 0;
            for (const auto& jointJson : jointArrayJson) {
                auto joint = jointDataFromJsonValue(jointJson);
                jointArray.push_back(joint);
                setJointData(i, joint.rotation, joint.translation);
                i++;
            }
            setRawJointData(jointArray);
        }
    }
}

// Every frame will store both a basis for the recording and a relative transform
// This allows the application to decide whether playback should be relative to an avatar's
// transform at the start of playback, or relative to the transform of the recorded
// avatar
QByteArray AvatarData::toFrame(const AvatarData& avatar) {
    QJsonObject root = avatar.toJson();
#ifdef WANT_JSON_DEBUG
    {
        QJsonObject obj = root;
        obj.remove(JSON_AVATAR_JOINT_ARRAY);
        qCDebug(avatars).noquote() << QJsonDocument(obj).toJson(QJsonDocument::JsonFormat::Indented);
    }
#endif
    return QJsonDocument(root).toBinaryData();
}


void AvatarData::fromFrame(const QByteArray& frameData, AvatarData& result) {
    QJsonDocument doc = QJsonDocument::fromBinaryData(frameData);
#ifdef WANT_JSON_DEBUG
    {
        QJsonObject obj = doc.object();
        obj.remove(JSON_AVATAR_JOINT_ARRAY);
        qCDebug(avatars).noquote() << QJsonDocument(obj).toJson(QJsonDocument::JsonFormat::Indented);
    }
#endif
    result.fromJson(doc.object());
}

float AvatarData::getBodyYaw() const {
    glm::vec3 eulerAngles = glm::degrees(safeEulerAngles(getOrientation()));
    return eulerAngles.y;
}

void AvatarData::setBodyYaw(float bodyYaw) {
    glm::vec3 eulerAngles = glm::degrees(safeEulerAngles(getOrientation()));
    eulerAngles.y = bodyYaw;
    setOrientation(glm::quat(glm::radians(eulerAngles)));
}

float AvatarData::getBodyPitch() const {
    glm::vec3 eulerAngles = glm::degrees(safeEulerAngles(getOrientation()));
    return eulerAngles.x;
}

void AvatarData::setBodyPitch(float bodyPitch) {
    glm::vec3 eulerAngles = glm::degrees(safeEulerAngles(getOrientation()));
    eulerAngles.x = bodyPitch;
    setOrientation(glm::quat(glm::radians(eulerAngles)));
}

float AvatarData::getBodyRoll() const {
    glm::vec3 eulerAngles = glm::degrees(safeEulerAngles(getOrientation()));
    return eulerAngles.z;
}

void AvatarData::setBodyRoll(float bodyRoll) {
    glm::vec3 eulerAngles = glm::degrees(safeEulerAngles(getOrientation()));
    eulerAngles.z = bodyRoll;
    setOrientation(glm::quat(glm::radians(eulerAngles)));
}

void AvatarData::setPosition(const glm::vec3& position) {
    SpatiallyNestable::setPosition(position);
}

void AvatarData::setOrientation(const glm::quat& orientation) {
    SpatiallyNestable::setOrientation(orientation);
}

glm::quat AvatarData::getAbsoluteJointRotationInObjectFrame(int index) const {
    assert(false);
    return glm::quat();
}

glm::vec3 AvatarData::getAbsoluteJointTranslationInObjectFrame(int index) const {
    assert(false);
    return glm::vec3();
}

QVariant AttachmentData::toVariant() const {
    QVariantMap result;
    result["modelUrl"] = modelURL;
    result["jointName"] = jointName;
    result["translation"] = glmToQMap(translation);
    result["rotation"] = glmToQMap(glm::degrees(safeEulerAngles(rotation)));
    result["scale"] = scale;
    result["soft"] = isSoft;
    return result;
}

glm::vec3 variantToVec3(const QVariant& var) {
    auto map = var.toMap();
    glm::vec3 result;
    result.x = map["x"].toFloat();
    result.y = map["y"].toFloat();
    result.z = map["z"].toFloat();
    return result;
}

void AttachmentData::fromVariant(const QVariant& variant) {
    auto map = variant.toMap();
    if (map.contains("modelUrl")) {
        auto urlString = map["modelUrl"].toString();
        modelURL = urlString;
    }
    if (map.contains("jointName")) {
        jointName = map["jointName"].toString();
    }
    if (map.contains("translation")) {
        translation = variantToVec3(map["translation"]);
    }
    if (map.contains("rotation")) {
        rotation = glm::quat(glm::radians(variantToVec3(map["rotation"])));
    }
    if (map.contains("scale")) {
        scale = map["scale"].toFloat();
    }
    if (map.contains("soft")) {
        isSoft = map["soft"].toBool();
    }
}

QVariantList AvatarData::getAttachmentsVariant() const {
    QVariantList result;
    for (const auto& attachment : getAttachmentData()) {
        result.append(attachment.toVariant());
    }
    return result;
}

void AvatarData::setAttachmentsVariant(const QVariantList& variant) {
    QVector<AttachmentData> newAttachments;
    newAttachments.reserve(variant.size());
    for (const auto& attachmentVar : variant) {
        AttachmentData attachment;
        attachment.fromVariant(attachmentVar);
        if (!attachment.modelURL.isEmpty()) {
            newAttachments.append(attachment);
        }
    }
    setAttachmentData(newAttachments);
}

void AvatarData::updateAvatarEntity(const QUuid& entityID, const QByteArray& entityData) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "updateAvatarEntity", Q_ARG(const QUuid&, entityID), Q_ARG(QByteArray, entityData));
        return;
    }
    _avatarEntitiesLock.withWriteLock([&] {
        _avatarEntityData.insert(entityID, entityData);
        _avatarEntityDataLocallyEdited = true;
    });
}

void AvatarData::clearAvatarEntity(const QUuid& entityID) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "clearAvatarEntity", Q_ARG(const QUuid&, entityID));
        return;
    }

    _avatarEntitiesLock.withWriteLock([&] {
        _avatarEntityData.remove(entityID);
        _avatarEntityDataLocallyEdited = true;
    });
}

AvatarEntityMap AvatarData::getAvatarEntityData() const {
    AvatarEntityMap result;
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(const_cast<AvatarData*>(this), "getAvatarEntityData", Qt::BlockingQueuedConnection,
                                  Q_RETURN_ARG(AvatarEntityMap, result));
        return result;
    }

    _avatarEntitiesLock.withReadLock([&] {
        result = _avatarEntityData;
    });
    return result;
}

void AvatarData::setAvatarEntityData(const AvatarEntityMap& avatarEntityData) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "setAvatarEntityData", Q_ARG(const AvatarEntityMap&, avatarEntityData));
        return;
    }
    _avatarEntitiesLock.withWriteLock([&] {
        if (_avatarEntityData != avatarEntityData) {
            // keep track of entities that were attached to this avatar but no longer are
            AvatarEntityIDs previousAvatarEntityIDs = QSet<QUuid>::fromList(_avatarEntityData.keys());

            _avatarEntityData = avatarEntityData;
            setAvatarEntityDataChanged(true);

            foreach (auto entityID, previousAvatarEntityIDs) {
                if (!_avatarEntityData.contains(entityID)) {
                    _avatarEntityDetached.insert(entityID);
                }
            }
        }
    });
}

AvatarEntityIDs AvatarData::getAndClearRecentlyDetachedIDs() {
    AvatarEntityIDs result;
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(const_cast<AvatarData*>(this), "getAndClearRecentlyDetachedIDs", Qt::BlockingQueuedConnection,
                                  Q_RETURN_ARG(AvatarEntityIDs, result));
        return result;
    }
    _avatarEntitiesLock.withWriteLock([&] {
        result = _avatarEntityDetached;
        _avatarEntityDetached.clear();
    });
    return result;
}

// thread-safe
glm::mat4 AvatarData::getSensorToWorldMatrix() const {
    return _sensorToWorldMatrixCache.get();
}

// thread-safe
glm::mat4 AvatarData::getControllerLeftHandMatrix() const {
    return _controllerLeftHandMatrixCache.get();
}

// thread-safe
glm::mat4 AvatarData::getControllerRightHandMatrix() const {
    return _controllerRightHandMatrixCache.get();
}


QScriptValue RayToAvatarIntersectionResultToScriptValue(QScriptEngine* engine, const RayToAvatarIntersectionResult& value) {
    QScriptValue obj = engine->newObject();
    obj.setProperty("intersects", value.intersects);
    QScriptValue avatarIDValue = quuidToScriptValue(engine, value.avatarID);
    obj.setProperty("avatarID", avatarIDValue);
    obj.setProperty("distance", value.distance);
    QScriptValue intersection = vec3toScriptValue(engine, value.intersection);
    obj.setProperty("intersection", intersection);
    return obj;
}

void RayToAvatarIntersectionResultFromScriptValue(const QScriptValue& object, RayToAvatarIntersectionResult& value) {
    value.intersects = object.property("intersects").toVariant().toBool();
    QScriptValue avatarIDValue = object.property("avatarID");
    quuidFromScriptValue(avatarIDValue, value.avatarID);
    value.distance = object.property("distance").toVariant().toFloat();
    QScriptValue intersection = object.property("intersection");
    if (intersection.isValid()) {
        vec3FromScriptValue(intersection, value.intersection);
    }
}
