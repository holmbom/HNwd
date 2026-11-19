#include "pose.h"

#include <cmath>

namespace HNwd::detail {

namespace {

// R(q) för det normerade q, radvis. Enhetsmatrisen om |q| är 0 eller inte ändlig.
void rotation(const PoseD& pose, double m[9]) {
    const double x = pose.q[0], y = pose.q[1], z = pose.q[2], w = pose.q[3];
    const double n = std::sqrt(x * x + y * y + z * z + w * w);
    if (!(n > 0) || !std::isfinite(n)) {
        for (int i = 0; i < 9; ++i) m[i] = (i % 4 == 0) ? 1.0 : 0.0;
        return;
    }
    const double qx = x / n, qy = y / n, qz = z / n, qw = w / n;
    m[0] = 1 - 2 * (qy * qy + qz * qz);
    m[1] = 2 * (qx * qy - qw * qz);
    m[2] = 2 * (qx * qz + qw * qy);
    m[3] = 2 * (qx * qy + qw * qz);
    m[4] = 1 - 2 * (qx * qx + qz * qz);
    m[5] = 2 * (qy * qz - qw * qx);
    m[6] = 2 * (qx * qz - qw * qy);
    m[7] = 2 * (qy * qz + qw * qx);
    m[8] = 1 - 2 * (qx * qx + qy * qy);
}

double determinant(const double m[9]) {
    return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
           m[2] * (m[3] * m[7] - m[4] * m[6]);
}

} // namespace

void pose_linear(const PoseD& pose, double m[9]) {
    if (pose.affine) {
        for (int i = 0; i < 9; ++i) m[i] = pose.m[i];
        return;
    }
    rotation(pose, m);
    for (int i = 0; i < 9; ++i) m[i] *= pose.s;
}

void pose_point(const PoseD& pose, const float v[3], double out[3]) {
    double m[9];
    pose_linear(pose, m);
    for (int r = 0; r < 3; ++r)
        out[r] = m[3 * r] * v[0] + m[3 * r + 1] * v[1] + m[3 * r + 2] * v[2] + pose.t[r];
}

void pose_normal(const PoseD& pose, const float n[3], float out[3]) {
    double m[9];
    pose_linear(pose, m);
    // En normal följer inte matrisen utan dess inverterade transponat, som är
    // kofaktormatrisen delad med determinanten. Bara tecknet på determinanten
    // behövs, eftersom resultatet normeras.
    const double c[9] = {m[4] * m[8] - m[5] * m[7], m[5] * m[6] - m[3] * m[8],
                         m[3] * m[7] - m[4] * m[6], m[2] * m[7] - m[1] * m[8],
                         m[0] * m[8] - m[2] * m[6], m[1] * m[6] - m[0] * m[7],
                         m[1] * m[5] - m[2] * m[4], m[2] * m[3] - m[0] * m[5],
                         m[0] * m[4] - m[1] * m[3]};
    const double sign = determinant(m) < 0 ? -1.0 : 1.0;
    double r[3];
    for (int i = 0; i < 3; ++i)
        r[i] = sign * (c[3 * i] * n[0] + c[3 * i + 1] * n[1] + c[3 * i + 2] * n[2]);
    const double length = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (length > 0)
        for (double& v : r) v /= length;
    for (int i = 0; i < 3; ++i) out[i] = static_cast<float>(r[i]);
}

bool pose_mirrors(const PoseD& pose) {
    if (!pose.affine) return pose.s < 0;
    return determinant(pose.m) < 0;
}

void pose_box(const PoseD& pose, const float box[6], double out_min[3], double out_max[3]) {
    double m[9];
    pose_linear(pose, m);
    // De åtta hörnen räknas med samma uttryck som pose_point, så att varje
    // hörn som pose_point ger ligger i lådan på biten när.
    for (int r = 0; r < 3; ++r) {
        out_min[r] = INFINITY;
        out_max[r] = -INFINITY;
    }
    for (int corner = 0; corner < 8; ++corner) {
        const float v[3] = {box[(corner & 1) ? 3 : 0], box[(corner & 2) ? 4 : 1],
                            box[(corner & 4) ? 5 : 2]};
        for (int r = 0; r < 3; ++r) {
            const double w =
                m[3 * r] * v[0] + m[3 * r + 1] * v[1] + m[3 * r + 2] * v[2] + pose.t[r];
            if (w < out_min[r]) out_min[r] = w;
            if (w > out_max[r]) out_max[r] = w;
        }
    }
}

} // namespace HNwd::detail
