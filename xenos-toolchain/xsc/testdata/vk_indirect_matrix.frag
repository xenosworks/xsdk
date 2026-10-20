#version 450
// Two indirect texture stages read one matrix, as Dusklight's GX shaders do (1510c05add37f2b3.frag): each builds its
// rows from the uniform again, so the second's rows are the same constants moved into other temps.
layout(set = 0, binding = 0, std140) uniform View {
    vec4 ind_mtx[2];
} uview;
layout(location = 0) in vec4 in_ind;
layout(location = 1) in vec4 in_uv;
layout(location = 0) out vec4 o_coords;
void main() {
    vec3 ind0_biased = in_ind.xyz + vec3(1.0, 1.0, 1.0) * -128.0;
    vec4 ind0_c0 = uview.ind_mtx[0];
    vec4 ind0_c1 = uview.ind_mtx[1];
    vec2 tev_coord0 = in_uv.xy + (vec2(dot(vec3(ind0_c0.xz, ind0_c1.x), ind0_biased),
        dot(vec3(ind0_c0.yw, ind0_c1.y), ind0_biased)) * ind0_c1.z);
    vec3 ind1_biased = in_ind.xyz + vec3(1.0, 1.0, 1.0) * -128.0;
    vec4 ind1_c0 = uview.ind_mtx[0];
    vec4 ind1_c1 = uview.ind_mtx[1];
    vec2 tev_coord1 = in_uv.zw + (vec2(dot(vec3(ind1_c0.xz, ind1_c1.x), ind1_biased),
        dot(vec3(ind1_c0.yw, ind1_c1.y), ind1_biased)) * ind1_c1.z);
    o_coords = vec4(tev_coord0, tev_coord1);
}
