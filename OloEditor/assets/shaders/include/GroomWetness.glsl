// GroomWetness.glsl — waterline-driven wet coats, issue #1615.
//
// EVERY FUNCTION HERE HAS A C++ TWIN in OloEngine/Groom/GroomWetness.h, written
// operation for operation in the same order: GroomWetnessParityTest renders
// this file's arithmetic and compares it with the C++ side. What wetness is,
// and why each response is what it is, is documented once, there.

#ifndef GROOM_WETNESS_GLSL
#define GROOM_WETNESS_GLSL

// Twin of GroomWetnessLimits::MinBand.
#define OLO_GROOM_WET_MIN_BAND 0.005

// Twin of GroomWetnessAt: wetness at coat-local height `height` (metres) under
// a soaked line `line` with an edge `band` either side, or the rain's, whichever
// is wetter.
float oloGroomWetness(float height, float rain, float line, float band)
{
	float rainWet = clamp(rain, 0.0, 1.0);
	float edge = max(band, OLO_GROOM_WET_MIN_BAND);
	float x = clamp((height - (line - edge)) / (2.0 * edge), 0.0, 1.0);
	float above = x * x * (3.0 - (2.0 * x));
	return max(1.0 - above, rainWet);
}

// Twin of GroomWetFibre. `response`: x = absorption, y = longitudinal roughness
// scale, z = azimuthal roughness scale, all at full wetness.
struct OloGroomWetScales
{
	float SigmaA;
	float V0;
	float S;
	float AlbedoPower;
};

OloGroomWetScales oloGroomFibreWet(float wetness, vec4 response)
{
	float w = clamp(wetness, 0.0, 1.0);
	OloGroomWetScales scales;
	scales.SigmaA = 1.0 + ((response.x - 1.0) * w);
	float roughness = 1.0 + ((response.y - 1.0) * w);
	scales.V0 = roughness * roughness;
	scales.S = 1.0 + ((response.z - 1.0) * w);
	scales.AlbedoPower = scales.SigmaA;
	return scales;
}

// Twins of GroomWetness.cpp's kWetLift and kMinDownhill.
#define OLO_GROOM_WET_LIFT 0.0874887
#define OLO_GROOM_WET_MIN_DOWNHILL 0.05

// Twin of GroomWetLocal: one point of a strand in its root's bind frame (+Z the
// skin normal), wet. `geometry`: x = flatten, y = shorten, z = clump.
vec3 oloGroomWetLocal(vec3 local, float t, float wetness, vec3 gravityLocal, vec3 clumpGrowth, vec4 geometry)
{
	if (!(wetness > 0.0))
	{
		return local;
	}
	float w = min(wetness, 1.0);
	if (!(length(local) > 1.0e-7))
	{
		return local;
	}

	vec3 p = local;
	if (length(clumpGrowth) > 1.0e-7)
	{
		float clump = clamp(geometry.z * w, 0.0, 1.0) * t * t;
		p = p + (((clumpGrowth * t) - p) * clump);
	}

	float hang = max(gravityLocal.z, 0.0);
	vec2 downhill = gravityLocal.xy;
	float downhillLength = length(downhill);
	vec2 direction = vec2(1.0, 0.0);
	if (downhillLength > OLO_GROOM_WET_MIN_DOWNHILL)
	{
		direction = downhill / downhillLength;
	}
	else
	{
		vec2 own = p.xy;
		float ownLength = length(own);
		if (ownLength > 1.0e-7)
		{
			direction = own / ownLength;
		}
	}
	float tangential = sqrt(max(0.0, 1.0 - (hang * hang)));
	vec3 target = normalize(vec3(direction * tangential, max(hang, OLO_GROOM_WET_LIFT)));
	float facingUp = clamp(-gravityLocal.z, 0.0, 1.0);
	float lie = clamp(geometry.x * w * (0.5 + (0.5 * facingUp)), 0.0, 1.0);
	float reach = length(p);
	vec3 q = p + (((target * reach) - p) * lie);
	float qLength = length(q);
	q = qLength > 1.0e-7 ? q * (reach / qLength) : p;

	return q * (1.0 - clamp(geometry.y * w, 0.0, 0.9));
}

#endif // GROOM_WETNESS_GLSL
