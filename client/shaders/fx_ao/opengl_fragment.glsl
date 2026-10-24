// fx: ambient occlusion, half resolution, from the depth buffer.
// Normal-weighted horizon sampling (HBAO-style): 4 directions x 4 steps,
// rotated per pixel and per frame so TAA averages the noise away.
#define depthMap texture0

uniform sampler2D depthMap;
uniform mat4 fxInvViewProj;   // clip -> scene space (jittered, matches the depth buffer)
uniform vec3 fxEyePos;        // camera in scene space
uniform float fxProjScale;    // pixels per scene unit at view distance 1
uniform float fxAoRadius;     // nodes
uniform float fxAoStrength;
uniform float fxFrame;
uniform mat4 fxViewProj;      // scene space -> clip (jittered, matches the depth buffer)
uniform vec3 fxSunDir;        // towards the sun
uniform float fxSunStrength;
uniform float fxContactShadows;

CENTROID_ VARYING_ mediump vec2 varTexCoord;

const float FX_BS = 10.0;

vec3 scenePos(vec2 uv)
{
	float d = texture2D(depthMap, uv).r;
	vec4 p = fxInvViewProj * vec4(vec3(uv, d) * 2.0 - 1.0, 1.0);
	return p.xyz / p.w;
}

void main(void)
{
	vec2 uv = varTexCoord.st;
	float d = texture2D(depthMap, uv).r;
	if (d >= 0.99999) { // sky
		gl_FragColor = vec4(1.0);
		return;
	}
	vec2 px = 1.0 / vec2(textureSize(depthMap, 0));
	vec3 p = scenePos(uv);

	// Normal from the neighbours with the smaller depth step (keeps edges
	// sharp). 2 px apart, and degenerate steps are skipped: at grazing angles
	// 24-bit depth quantizes neighbours to the same value, which would make a
	// zero-length tangent and a garbage normal (visible as stripes).
	vec3 pr = scenePos(uv + vec2(2.0 * px.x, 0.0)), pl = scenePos(uv - vec2(2.0 * px.x, 0.0));
	vec3 pu = scenePos(uv + vec2(0.0, 2.0 * px.y)), pd = scenePos(uv - vec2(0.0, 2.0 * px.y));
	vec3 ax = pr - p, bx = p - pl, ay = pu - p, by = p - pd;
	// On a continuous surface both one-sided steps agree: use the central
	// difference (4 px baseline, robust to quantization). At a depth edge they
	// disagree: use the smaller step so the normal belongs to this surface.
	vec3 dx = length(ax - bx) < 0.5 * length(ax + bx) ? ax + bx
		: (length(ax) < length(bx) ? ax : bx);
	vec3 dy = length(ay - by) < 0.5 * length(ay + by) ? ay + by
		: (length(ay) < length(by) ? ay : by);
	vec3 n = normalize(cross(dx, dy));
	vec3 to_eye = fxEyePos - p;
	if (dot(n, to_eye) < 0.0)
		n = -n;

	float radius = fxAoRadius * FX_BS;
	float dist = max(length(to_eye), 1.0);
	float r_px = clamp(radius * fxProjScale / dist, 2.0, 96.0);

	// interleaved gradient noise + golden-angle rotation per frame
	float ign = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
	float rot = ign * 6.2831853 + fxFrame * 2.3999632;

	// grazing surfaces get more bias: their depth samples are the least precise
	float grazing = 1.0 - abs(dot(n, normalize(to_eye)));
	float bias = 0.2 + dist / (FX_BS * 400.0) + grazing * grazing * 0.3;

	float occ = 0.0;
	for (int i = 0; i < 4; i++) {
		float a = rot + float(i) * 1.5707963;
		vec2 dir = vec2(cos(a), sin(a));
		for (int j = 1; j <= 4; j++) {
			float t = (float(j) - ign * 0.5) / 4.0;
			vec3 s = scenePos(uv + dir * r_px * t * px);
			vec3 v = s - p;
			float l = length(v);
			if (l < 1e-3)
				continue;
			float falloff = clamp(1.0 - (l * l) / (radius * radius), 0.0, 1.0);
			// bias grows with distance: depth precision drops and grazing
			// walls would otherwise shadow themselves in stripes
			occ += max(dot(n, v) / l - bias, 0.0) * falloff;
		}
	}
	float ao = clamp(1.0 - occ / 16.0 * 3.0 * fxAoStrength, 0.0, 1.0);
	ao = ao * ao; // contact shadows read darker, open areas stay at 1

	// Contact shadows: a short screen-space march towards the sun. Catches
	// the small detail (grass at a wall, an item on the floor, the step of a
	// slab) that vertex lighting and the shadow map miss. Stored in G.
	float cs = 1.0;
	// faces turned away from the sun are already unlit by it: marching from
	// them would hit their own block and darken them
	float sun = smoothstep(0.1, 0.45, fxSunStrength) * step(0.05, fxSunDir.y)
		* smoothstep(0.05, 0.25, dot(n, fxSunDir))
		* (1.0 - smoothstep(32.0 * FX_BS, 48.0 * FX_BS, dist));
	if (fxContactShadows > 0.0 && sun > 0.0) {
		vec3 start = p + n * (0.03 * FX_BS + dist * 0.002);
		const int STEPS = 10;
		float len = 0.7 * FX_BS;
		float hit = 0.0;
		for (int i = 1; i <= STEPS; i++) {
			float t = (float(i) - ign) / float(STEPS);
			vec3 q = start + fxSunDir * len * t;
			vec4 c = fxViewProj * vec4(q, 1.0);
			if (c.w <= 0.0)
				break;
			vec2 suv = c.xy / c.w * 0.5 + 0.5;
			if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0))))
				break;
			vec3 b = scenePos(suv);
			float dq = length(q - fxEyePos), db = length(b - fxEyePos);
			// occluder in front of the ray point, but not by much (thickness)
			float thick = 0.6 * FX_BS + dq * 0.01;
			if (db < dq - 0.02 * FX_BS - dq * 0.0015 && db > dq - thick) {
				hit = 1.0 - t * 0.5; // closer occluders shadow harder
				break;
			}
		}
		cs = 1.0 - hit * fxContactShadows * sun * 0.6;
	}
	gl_FragColor = vec4(ao, cs, ao, 1.0);
}
