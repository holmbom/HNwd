#pragma once

// Ett fragment ur FragmentStore med alla fält utlagda, så att proven kan
// jämföra fält för fält.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "fragments.h"

struct FragmentView {
    double t[3], q[4], s, m[9];
    float bbox[6];
    int32_t record, a, seq;
    uint32_t look;
    uint8_t pose_class;
};

inline FragmentView fragment_view(const HNwd::detail::FragmentStore& st, size_t i) {
    FragmentView v{};
    HNwd::detail::PoseD p;
    st.pose(i, p);
    for (int k = 0; k < 3; ++k) v.t[k] = p.t[k];
    for (int k = 0; k < 4; ++k) v.q[k] = p.q[k];
    v.s = p.s;
    for (int k = 0; k < 9; ++k) v.m[k] = p.m[k];
    const HNwd::detail::FragmentStore::Item& it = st.items[i];
    const HNwd::detail::GeometryRefData& g = st.geomrefs[it.geomref];
    for (int k = 0; k < 6; ++k) v.bbox[k] = g.bbox[k];
    v.record = g.record;
    v.a = it.primitives;
    v.seq = it.seq;
    v.look = it.look;
    v.pose_class = it.pose_class;
    return v;
}

inline std::vector<FragmentView> fragment_views(const HNwd::detail::FragmentStore& st) {
    std::vector<FragmentView> out;
    out.reserve(st.size());
    for (size_t i = 0; i < st.size(); ++i) out.push_back(fragment_view(st, i));
    return out;
}
