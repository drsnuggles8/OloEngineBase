// GroomQuat.glsl — the groom's quaternion arithmetic.
//
// Quaternions are (x, y, z, w), the layout GroomDeformRootRecord and
// GroomDeformBindRecord pack. Each function is glm's operator of the same
// meaning, term for term (glm/detail/type_quat.inl), so a CPU twin evaluating
// the same packed bytes agrees with the shader to the rounding of the same
// operations. Shared by the vertex stage's deformation
// (include/GroomStrandDeform.glsl, which needs this included first) and the
// fragment stage's rest-volume lookup (#1533).

#ifndef GROOM_QUAT_GLSL
#define GROOM_QUAT_GLSL

// glm's quat * vec3.
vec3 oloGroomQuatRotate(vec4 q, vec3 v)
{
	vec3 uv = cross(q.xyz, v);
	vec3 uuv = cross(q.xyz, uv);
	return v + ((uv * q.w) + uuv) * 2.0;
}

// glm's quat * quat: rotating by the product rotates by b, then by a.
vec4 oloGroomQuatMul(vec4 a, vec4 b)
{
	return vec4((a.w * b.xyz) + (b.w * a.xyz) + cross(a.xyz, b.xyz), (a.w * b.w) - dot(a.xyz, b.xyz));
}

// glm's conjugate: the inverse of a unit quaternion.
vec4 oloGroomQuatConjugate(vec4 q)
{
	return vec4(-q.xyz, q.w);
}

#endif // GROOM_QUAT_GLSL
