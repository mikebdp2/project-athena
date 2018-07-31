//
//  ParticleEffectEntityItem.h
//  libraries/entities/src
//
//  Created by Jason Rickwald on 3/2/15.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_ParticleEffectEntityItem_h
#define hifi_ParticleEffectEntityItem_h

#include <deque>

#include "EntityItem.h"

#include "ColorUtils.h"

namespace particle {
    static const float SCRIPT_MAXIMUM_PI = 3.1416f;  // Round up so that reasonable property values work
    static const float UNINITIALIZED = NAN;
    static const ScriptVec3UChar DEFAULT_COLOR = { 255, 255, 255 };
    static const vec3 DEFAULT_COLOR_UNINITIALIZED = { UNINITIALIZED, UNINITIALIZED, UNINITIALIZED };
    static const ScriptVec3UChar DEFAULT_COLOR_SPREAD = { 0, 0, 0 };
    static const float DEFAULT_ALPHA = 1.0f;
    static const float DEFAULT_ALPHA_SPREAD = 0.0f;
    static const float DEFAULT_ALPHA_START = UNINITIALIZED;
    static const float DEFAULT_ALPHA_FINISH = UNINITIALIZED;
    static const float MINIMUM_ALPHA = 0.0f;
    static const float MAXIMUM_ALPHA = 1.0f;
    static const quint32 DEFAULT_MAX_PARTICLES = 1000;
    static const quint32 MINIMUM_MAX_PARTICLES = 1;
    static const quint32 MAXIMUM_MAX_PARTICLES = 100000;
    static const float DEFAULT_LIFESPAN = 3.0f;
    static const float MINIMUM_LIFESPAN = 0.0f;
    static const float MAXIMUM_LIFESPAN = 86400.0f;  // 1 day
    static const float DEFAULT_EMIT_RATE = 15.0f;
    static const float MINIMUM_EMIT_RATE = 0.0f;
    static const float MAXIMUM_EMIT_RATE = 100000.0f;
    static const float DEFAULT_EMIT_SPEED = 5.0f;
    static const float MINIMUM_EMIT_SPEED = -1000.0f;
    static const float MAXIMUM_EMIT_SPEED = 1000.0f;  // Approx mach 3
    static const float DEFAULT_SPEED_SPREAD = 1.0f;
    static const glm::quat DEFAULT_EMIT_ORIENTATION = glm::angleAxis(-PI_OVER_TWO, Vectors::UNIT_X);  // Vertical
    static const glm::vec3 DEFAULT_EMIT_DIMENSIONS = Vectors::ZERO;  // Emit from point
    static const float MINIMUM_EMIT_DIMENSION = 0.0f;
    static const float MAXIMUM_EMIT_DIMENSION = (float)TREE_SCALE;
    static const float DEFAULT_EMIT_RADIUS_START = 1.0f;  // Emit from surface (when emitDimensions > 0)
    static const float MINIMUM_EMIT_RADIUS_START = 0.0f;
    static const float MAXIMUM_EMIT_RADIUS_START = 1.0f;
    static const float MINIMUM_POLAR = 0.0f;
    static const float MAXIMUM_POLAR = SCRIPT_MAXIMUM_PI;
    static const float DEFAULT_POLAR_START = 0.0f;  // Emit along z-axis
    static const float DEFAULT_POLAR_FINISH = 0.0f; // ""
    static const float MINIMUM_AZIMUTH = -SCRIPT_MAXIMUM_PI;
    static const float MAXIMUM_AZIMUTH = SCRIPT_MAXIMUM_PI;
    static const float DEFAULT_AZIMUTH_START = -PI;  // Emit full circumference (when polarFinish > 0)
    static const float DEFAULT_AZIMUTH_FINISH = PI;  // ""
    static const glm::vec3 DEFAULT_EMIT_ACCELERATION(0.0f, -9.8f, 0.0f);
    static const float MINIMUM_EMIT_ACCELERATION = -100.0f; // ~ 10g
    static const float MAXIMUM_EMIT_ACCELERATION = 100.0f;
    static const glm::vec3 DEFAULT_ACCELERATION_SPREAD(0.0f, 0.0f, 0.0f);
    static const float MINIMUM_ACCELERATION_SPREAD = 0.0f;
    static const float MAXIMUM_ACCELERATION_SPREAD = 100.0f;
    static const float DEFAULT_PARTICLE_RADIUS = 0.025f;
    static const float MINIMUM_PARTICLE_RADIUS = 0.0f;
    static const float MAXIMUM_PARTICLE_RADIUS = (float)TREE_SCALE;
    static const float DEFAULT_RADIUS_SPREAD = 0.0f;
    static const float DEFAULT_RADIUS_START = UNINITIALIZED;
    static const float DEFAULT_RADIUS_FINISH = UNINITIALIZED;
    static const float DEFAULT_PARTICLE_SPIN = 0.0f;
    static const float DEFAULT_SPIN_START = UNINITIALIZED;
    static const float DEFAULT_SPIN_FINISH = UNINITIALIZED;
    static const float DEFAULT_SPIN_SPREAD = 0.0f;
    static const float MINIMUM_PARTICLE_SPIN = -2.0f * SCRIPT_MAXIMUM_PI;
    static const float MAXIMUM_PARTICLE_SPIN = 2.0f * SCRIPT_MAXIMUM_PI;
    static const QString DEFAULT_TEXTURES = "";
    static const bool DEFAULT_EMITTER_SHOULD_TRAIL = false;
    static const bool DEFAULT_ROTATE_WITH_ENTITY = false;

    template <typename T>
    struct Range {
        Range() {}
        Range(const Range& other) { *this = other; }
        Range(const T& defaultStart, const T& defaultFinish)
            : start(defaultStart), finish(defaultFinish) {
        }

        Range& operator=(const Range& other) {
            start = other.start;
            finish = other.finish;
            return *this;
        }

        T start;
        T finish;
    };

    template <typename T>
    struct Gradient {
        Gradient() {}
        Gradient(const Gradient& other) { *this = other; }
        Gradient(const T& defaultTarget, const T& defaultSpread)
            : target(defaultTarget), spread(defaultSpread) {}

        Gradient& operator=(const Gradient& other) {
            target = other.target;
            spread = other.spread;
            return *this;
        }

        T target;
        T spread;
    };

    template <typename T>
    struct RangeGradient {
        RangeGradient() {}
        RangeGradient(const RangeGradient& other) { *this = other; }
        RangeGradient(const T& defaultValue, const T& defaultStart, const T& defaultFinish, const T& defaultSpread)
            : range(defaultStart, defaultFinish), gradient(defaultValue, defaultSpread) {
        }

        RangeGradient& operator=(const RangeGradient& other) {
            range = other.range;
            gradient = other.gradient;
            return *this;
        }

        Range<T> range;
        Gradient<T> gradient;
    };

    struct EmitProperties {
        float rate { DEFAULT_EMIT_RATE };
        Gradient<float> speed { DEFAULT_EMIT_SPEED, DEFAULT_SPEED_SPREAD };
        Gradient<vec3> acceleration { DEFAULT_EMIT_ACCELERATION, DEFAULT_ACCELERATION_SPREAD };
        glm::quat orientation { DEFAULT_EMIT_ORIENTATION };
        glm::vec3 dimensions { DEFAULT_EMIT_DIMENSIONS };
        bool shouldTrail { DEFAULT_EMITTER_SHOULD_TRAIL };

        EmitProperties() {};
        EmitProperties(const EmitProperties& other) { *this = other; }
        EmitProperties& operator=(const EmitProperties& other) {
            rate = other.rate;
            speed = other.speed;
            acceleration = other.acceleration;
            orientation = other.orientation;
            dimensions = other.dimensions;
            shouldTrail = other.shouldTrail;
            return *this;
        }
    };

    struct Properties {
        RangeGradient<vec3> color { DEFAULT_COLOR.toGlm(), DEFAULT_COLOR_UNINITIALIZED, DEFAULT_COLOR_UNINITIALIZED, DEFAULT_COLOR_SPREAD.toGlm() };
        RangeGradient<float> alpha { DEFAULT_ALPHA, DEFAULT_ALPHA_START, DEFAULT_ALPHA_FINISH, DEFAULT_ALPHA_SPREAD };
        float radiusStart { DEFAULT_EMIT_RADIUS_START };
        RangeGradient<float> radius { DEFAULT_PARTICLE_RADIUS, DEFAULT_RADIUS_START, DEFAULT_RADIUS_FINISH, DEFAULT_RADIUS_SPREAD };
        RangeGradient<float> spin { DEFAULT_PARTICLE_SPIN, DEFAULT_SPIN_START, DEFAULT_SPIN_FINISH, DEFAULT_SPIN_SPREAD };
        bool rotateWithEntity { DEFAULT_ROTATE_WITH_ENTITY };
        float lifespan { DEFAULT_LIFESPAN };
        uint32_t maxParticles { DEFAULT_MAX_PARTICLES };
        EmitProperties emission;
        Range<float> polar { DEFAULT_POLAR_START, DEFAULT_POLAR_FINISH };
        Range<float> azimuth { DEFAULT_AZIMUTH_START, DEFAULT_AZIMUTH_FINISH };
        QString textures;


        Properties() {};
        Properties(const Properties& other) { *this = other; }
        bool valid() const;
        bool emitting() const;
        uint64_t emitIntervalUsecs() const;
        
        Properties& operator =(const Properties& other) {
            color = other.color;
            alpha = other.alpha;
            spin = other.spin;
            rotateWithEntity = other.rotateWithEntity;
            radius = other.radius;
            lifespan = other.lifespan;
            maxParticles = other.maxParticles;
            emission = other.emission;
            polar = other.polar;
            azimuth = other.azimuth;
            textures = other.textures;
            return *this;
        }

        vec4 getColorStart() const { return vec4(ColorUtils::sRGBToLinearVec3(toGlm(color.range.start)), alpha.range.start); }
        vec4 getColorMiddle() const { return vec4(ColorUtils::sRGBToLinearVec3(toGlm(color.gradient.target)), alpha.gradient.target); }
        vec4 getColorFinish() const { return vec4(ColorUtils::sRGBToLinearVec3(toGlm(color.range.finish)), alpha.range.finish); }
        vec4 getColorSpread() const { return vec4(ColorUtils::sRGBToLinearVec3(toGlm(color.gradient.spread)), alpha.gradient.spread); }
    };
} // namespace particles


bool operator==(const particle::Properties& a, const particle::Properties& b);
bool operator!=(const particle::Properties& a, const particle::Properties& b);


class ParticleEffectEntityItem : public EntityItem {
public:
    ALLOW_INSTANTIATION // This class can be instantiated

    static EntityItemPointer factory(const EntityItemID& entityID, const EntityItemProperties& properties);

    ParticleEffectEntityItem(const EntityItemID& entityItemID);

    // methods for getting/setting all properties of this entity
    virtual EntityItemProperties getProperties(EntityPropertyFlags desiredProperties = EntityPropertyFlags()) const override;
    virtual bool setProperties(const EntityItemProperties& properties) override;

    virtual EntityPropertyFlags getEntityProperties(EncodeBitstreamParams& params) const override;

    virtual void appendSubclassData(OctreePacketData* packetData, EncodeBitstreamParams& params,
                                    EntityTreeElementExtraEncodeDataPointer entityTreeElementExtraEncodeData,
                                    EntityPropertyFlags& requestedProperties,
                                    EntityPropertyFlags& propertyFlags,
                                    EntityPropertyFlags& propertiesDidntFit,
                                    int& propertyCount,
                                    OctreeElement::AppendState& appendState) const override;

    virtual int readEntitySubclassDataFromBuffer(const unsigned char* data, int bytesLeftToRead,
                                                 ReadBitstreamToTreeParams& args,
                                                 EntityPropertyFlags& propertyFlags, bool overwriteLocalData,
                                                 bool& somethingChanged) override;

    void setColor(const ScriptVec3UChar& value);
    ScriptVec3UChar getColor() const { return _particleProperties.color.gradient.target; }

    void setColorStart(const vec3& colorStart);
    void setColorStart(const ScriptVec3Float& colorStart) { setColorStart(colorStart.toGlm()); }
    vec3 getColorStart() const { return _particleProperties.color.range.start; }
    ScriptVec3Float getScriptColorStart() const { return getColorStart(); }

    void setColorFinish(const vec3& colorFinish);
    void setColorFinish(const ScriptVec3Float& colorFinish) { setColorFinish(colorFinish.toGlm()); }
    vec3 getColorFinish() const { return _particleProperties.color.range.finish; }
    ScriptVec3Float getScriptColorFinish() const { return getColorFinish(); }

    void setColorSpread(const ScriptVec3UChar& colorSpread);
    ScriptVec3UChar getColorSpread() const { return _particleProperties.color.gradient.spread; }

    void setAlpha(float alpha);
    float getAlpha() const { return _particleProperties.alpha.gradient.target; }

    void setAlphaStart(float alphaStart);
    float getAlphaStart() const { return _particleProperties.alpha.range.start; }

    void setAlphaFinish(float alphaFinish);
    float getAlphaFinish() const { return _particleProperties.alpha.range.finish; }

    void setAlphaSpread(float alphaSpread);
    float getAlphaSpread() const { return _particleProperties.alpha.gradient.spread; }

    void setShapeType(ShapeType type) override;
    virtual ShapeType getShapeType() const override { return _shapeType; }

    virtual void debugDump() const override;

    bool getIsEmitting() const { return _isEmitting; }
    void setIsEmitting(bool isEmitting) { _isEmitting = isEmitting; }

    void setMaxParticles(quint32 maxParticles);
    quint32 getMaxParticles() const { return _particleProperties.maxParticles; }

    void setLifespan(float lifespan);
    float getLifespan() const { return _particleProperties.lifespan; }

    void setEmitRate(float emitRate);
    float getEmitRate() const { return _particleProperties.emission.rate; }

    void setEmitSpeed(float emitSpeed);
    float getEmitSpeed() const { return _particleProperties.emission.speed.target; }

    void setSpeedSpread(float speedSpread);
    float getSpeedSpread() const { return _particleProperties.emission.speed.spread; }

    void setEmitOrientation(const glm::quat& emitOrientation);
    const glm::quat& getEmitOrientation() const { return _particleProperties.emission.orientation; }

    void setEmitDimensions(const glm::vec3& emitDimensions);
    void setEmitDimensions(const ScriptVec3Float& emitDimensions) { setEmitDimensions(emitDimensions.toGlm()); }
    const glm::vec3& getEmitDimensions() const { return _particleProperties.emission.dimensions; }
    ScriptVec3Float getScriptEmitDimensions() const { return getEmitDimensions(); }

    void setEmitRadiusStart(float emitRadiusStart);
    float getEmitRadiusStart() const { return _particleProperties.radiusStart; }

    void setPolarStart(float polarStart);
    float getPolarStart() const { return _particleProperties.polar.start; }

    void setPolarFinish(float polarFinish);
    float getPolarFinish() const { return _particleProperties.polar.finish; }

    void setAzimuthStart(float azimuthStart);
    float getAzimuthStart() const { return _particleProperties.azimuth.start; }

    void setAzimuthFinish(float azimuthFinish);
    float getAzimuthFinish() const { return _particleProperties.azimuth.finish; }

    void setEmitAcceleration(const glm::vec3& emitAcceleration);
    void setEmitAcceleration(const ScriptVec3Float& emitAcceleration) { setEmitAcceleration(emitAcceleration.toGlm()); }
    const glm::vec3& getEmitAcceleration() const { return _particleProperties.emission.acceleration.target; }
    ScriptVec3Float getScriptEmitAcceleration() const { return getEmitAcceleration(); }
    
    void setAccelerationSpread(const glm::vec3& accelerationSpread);
    void setAccelerationSpread(const ScriptVec3Float& accelerationSpread) { setAccelerationSpread(accelerationSpread.toGlm()); }
    const glm::vec3& getAccelerationSpread() const { return _particleProperties.emission.acceleration.spread; }
    ScriptVec3Float getScriptAccelerationSpread() const { return getAccelerationSpread(); }

    void setParticleRadius(float particleRadius);
    float getParticleRadius() const { return _particleProperties.radius.gradient.target; }

    void setRadiusStart(float radiusStart);
    float getRadiusStart() const { return _particleProperties.radius.range.start; }

    void setRadiusFinish(float radiusFinish);
    float getRadiusFinish() const { return _particleProperties.radius.range.finish; }

    void setRadiusSpread(float radiusSpread);
    float getRadiusSpread() const { return _particleProperties.radius.gradient.spread; }

    void setParticleSpin(float particleSpin);
    float getParticleSpin() const { return _particleProperties.spin.gradient.target; }

    void setSpinStart(float spinStart);
    float getSpinStart() const { return _particleProperties.spin.range.start; }

    void setSpinFinish(float spinFinish);
    float getSpinFinish() const { return _particleProperties.spin.range.finish; }

    void setSpinSpread(float spinSpread);
    float getSpinSpread() const { return _particleProperties.spin.gradient.spread; }

    void setRotateWithEntity(bool rotateWithEntity);
    bool getRotateWithEntity() const { return _particleProperties.rotateWithEntity; }

    void computeAndUpdateDimensions();

    void setTextures(const QString& textures);
    QString getTextures() const { return _particleProperties.textures; }

    bool getEmitterShouldTrail() const { return _particleProperties.emission.shouldTrail; }
    void setEmitterShouldTrail(bool emitterShouldTrail);

    virtual bool supportsDetailedIntersection() const override { return false; }

    particle::Properties getParticleProperties() const;

protected:
    particle::Properties _particleProperties;
    bool _isEmitting { true };

    ShapeType _shapeType { SHAPE_TYPE_NONE };
};

#endif // hifi_ParticleEffectEntityItem_h
