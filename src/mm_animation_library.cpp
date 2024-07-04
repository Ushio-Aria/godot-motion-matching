#include "mm_animation_library.h"

#include <godot_cpp/variant/utility_functions.hpp>

#include "common.h"
#include "features/mm_feature.h"
#include "math/stats.hpp"

using namespace godot;

MMAnimationLibrary::MMAnimationLibrary() : AnimationLibrary() {
}

MMAnimationLibrary::~MMAnimationLibrary() {
}

void MMAnimationLibrary::bake_data(const MMAnimationPlayer* p_player, const Skeleton3D* p_skeleton) {
    motion_data.clear();
    db_anim_index.clear();
    db_time_index.clear();

    size_t dim_count = 0;
    for (auto i = 0; i < features.size(); ++i) {
        MMFeature* f = Object::cast_to<MMFeature>(features[i]);
        dim_count += f->get_dimension_count();
        f->setup_skeleton(p_player, p_skeleton);
    }

    TypedArray<StringName> animation_list = get_animation_list();

    // Normalization data
    std::vector<std::vector<StatsAccumulator>> stats(features.size());

    PackedFloat32Array data;
    // For every animation
    for (size_t anim_index = 0; anim_index < animation_list.size(); anim_index++) {
        const StringName& anim_name = animation_list[anim_index];
        Ref<Animation> animation = get_animation(anim_name);

        // Initialize features
        for (size_t feature_index = 0; feature_index < features.size(); feature_index++) {
            MMFeature* feature = Object::cast_to<MMFeature>(features[feature_index]);
            stats[feature_index].resize(feature->get_dimension_count());
            feature->setup_for_animation(animation);
        }

        const float animation_length = animation->get_length();
        const float time_step = 1.0f / get_sampling_rate();
        // Every time step
        for (float time = 0; time < animation_length; time += time_step) {
            PackedFloat32Array pose_data;
            // For every feature
            for (size_t feature_index = 0; feature_index < features.size(); feature_index++) {
                const MMFeature* feature = Object::cast_to<MMFeature>(features[feature_index]);
                const PackedFloat32Array feature_data = feature->bake_animation_pose(animation, time);

                ERR_FAIL_COND(feature_data.size() != feature->get_dimension_count());

                // Update stats
                for (int feature_element_index = 0; feature_element_index < feature_data.size(); feature_element_index++) {
                    stats[feature_index][feature_element_index].add_sample(feature_data[feature_element_index]);
                }

                pose_data.append_array(feature_data);
            }

            ERR_FAIL_COND(pose_data.size() != dim_count);

            // Update dataset
            data.append_array(pose_data);
            db_anim_index.push_back(anim_index);
            db_time_index.push_back(time);
        }
    }

    // Compute mean and standard deviation
    for (size_t feature_index = 0; feature_index < features.size(); feature_index++) {
        MMFeature* feature = Object::cast_to<MMFeature>(features[feature_index]);

        PackedFloat32Array feature_means;
        feature_means.resize(feature->get_dimension_count());
        PackedFloat32Array feature_std_devs;
        feature_std_devs.resize(feature->get_dimension_count());
        PackedFloat32Array feature_mins;
        feature_mins.resize(feature->get_dimension_count());
        PackedFloat32Array feature_maxes;
        feature_maxes.resize(feature->get_dimension_count());

        for (size_t feature_element_index = 0; feature_element_index < feature->get_dimension_count(); feature_element_index++) {
            feature_means.set(feature_element_index, stats[feature_index][feature_element_index].get_mean());
            feature_std_devs.set(feature_element_index, stats[feature_index][feature_element_index].get_standard_deviation());
            feature_mins.set(feature_element_index, stats[feature_index][feature_element_index].get_min());
            feature_maxes.set(feature_element_index, stats[feature_index][feature_element_index].get_max());
        }

        feature->set_means(feature_means);
        feature->set_std_devs(feature_std_devs);
        feature->set_mins(feature_mins);
        feature->set_maxes(feature_maxes);
    }

    _normalize_data(data, dim_count);

    motion_data = data.duplicate();
}

MMQueryOutput MMAnimationLibrary::query(const MMQueryInput& p_query_input) {
    // TODO: Use fancier search algorithms
    // TODO: Do this using an offset array instead

    const TypedArray<StringName> animation_list = get_animation_list();

    int32_t dim_count = 0;
    PackedFloat32Array query_vector = PackedFloat32Array();
    for (size_t feature_index = 0; feature_index < features.size(); feature_index++) {
        const MMFeature* feature = Object::cast_to<MMFeature>(features[feature_index]);
        PackedFloat32Array feature_data = feature->evaluate_runtime_data(p_query_input);
        feature->normalize(feature_data);
        query_vector.append_array(feature_data);
        dim_count += feature->get_dimension_count();
    }

    float cost = FLT_MAX;
    MMQueryOutput result;
    int current_index = 0;

    for (size_t start_frame_index = 0; start_frame_index < motion_data.size(); start_frame_index += dim_count) {
        int start_feature_index = start_frame_index;
        float frame_cost = 0.f;
        for (size_t feature_index = 0; feature_index < features.size(); feature_index++) {
            const MMFeature* feature = Object::cast_to<MMFeature>(features[feature_index]);
            PackedFloat32Array frame_feature_data = motion_data.slice(start_feature_index, start_feature_index + feature->get_dimension_count());

            PackedFloat32Array query_feature_vector = query_vector.slice(start_feature_index - start_frame_index, start_feature_index - start_frame_index + feature->get_dimension_count());
            const float feature_cost = feature->compute_cost(query_feature_vector, frame_feature_data);
            frame_cost += feature_cost;
            start_feature_index += feature->get_dimension_count();
        }

        if (frame_cost < cost) {
            cost = frame_cost;
            result.cost = cost;
            result.matched_frame_data = motion_data.slice(start_frame_index, start_frame_index + dim_count);
            result.animation_match = get_animation_list()[db_anim_index[start_frame_index / dim_count]];
            result.time_match = db_time_index[start_frame_index / dim_count];
        }
    }

    return result;
}

void MMAnimationLibrary::_normalize_data(PackedFloat32Array& p_data, size_t p_dim_count) const {
    ERR_FAIL_COND(p_data.size() % p_dim_count != 0);

    for (size_t frame_index = 0; frame_index < p_data.size(); frame_index += p_dim_count) {

        int dim_index = 0;
        for (size_t feature_index = 0; feature_index < features.size(); feature_index++) {
            const MMFeature* feature = Object::cast_to<MMFeature>(features[feature_index]);
            PackedFloat32Array feature_data = p_data.slice(frame_index + dim_index, frame_index + dim_index + feature->get_dimension_count());

            feature->normalize(feature_data);

            for (size_t i = 0; i < feature_data.size(); i++) {
                p_data.set(frame_index + dim_index + i, feature_data[i]);
            }
            dim_index += feature->get_dimension_count();
        }
    }
}

void MMAnimationLibrary::_bind_methods() {
    BINDER_PROPERTY_PARAMS(MMAnimationLibrary, Variant::ARRAY, features, PROPERTY_HINT_TYPE_STRING, UtilityFunctions::str(Variant::OBJECT) + '/' + UtilityFunctions::str(Variant::BASIS) + ":MMFeature", PROPERTY_USAGE_DEFAULT);
    BINDER_PROPERTY_PARAMS(MMAnimationLibrary, Variant::FLOAT, sampling_rate);
    BINDER_PROPERTY_PARAMS(MMAnimationLibrary, Variant::PACKED_FLOAT32_ARRAY, motion_data);
    BINDER_PROPERTY_PARAMS(MMAnimationLibrary, Variant::PACKED_INT32_ARRAY, db_anim_index);
    BINDER_PROPERTY_PARAMS(MMAnimationLibrary, Variant::PACKED_FLOAT32_ARRAY, db_time_index);
}