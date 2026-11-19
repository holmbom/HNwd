#pragma once
#include <cstdint>

namespace HNwd::detail {

// En placering i dubbel precision, med samma fält som den publika Pose.
//
// Vanligen är värld = s · R(q) · v + t, där R(q) är rotationsmatrisen för det
// normerade q. Med `affine` gäller i stället värld = M · v + t, och q och s
// används inte.
struct PoseD {
    double t[3] = {0, 0, 0};
    double q[4] = {0, 0, 0, 1};   // kvaternion (x, y, z, w), som den står i filen
    double s = 1;                 // enhetlig skala; negativ speglar
    bool affine = false;          // sann: m gäller
    double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // radvis: m[3·rad + kolumn]
};

// Posens linjära del radvis: matrisen själv, eller s · R(q). En kvaternion med
// längden 0 eller som inte är ändlig ger R = enhetsmatrisen.
void pose_linear(const PoseD& pose, double m[9]);

// En punkt i världen.
void pose_point(const PoseD& pose, const float v[3], double out[3]);

// En normal i världen, med längden 1: vinkelrät mot ytan också efter olika
// skala per axel, och åt samma håll från ytan som i posten också när posen
// speglar. En normal som blir 0 lämnas som 0.
void pose_normal(const PoseD& pose, const float n[3], float out[3]);

// Sann om posen speglar: s < 0, eller en matris med negativ determinant.
bool pose_mirrors(const PoseD& pose);

// Världslådan kring en låda i postens egna koordinater (min xyz, max xyz):
// den minsta axelparallella lådan kring lådans åtta hörn i världen.
void pose_box(const PoseD& pose, const float box[6], double out_min[3], double out_max[3]);

} // namespace HNwd::detail
