// fx: temporal anti-aliasing resolve.
// Reprojects last frame's resolve through the depth buffer (the world is
// static; moving objects are handled by neighbourhood clipping), clips it to
// the current 3x3 neighbourhood in YCoCg, and blends. AO, when enabled, is
// applied to the current frame here so its per-pixel noise averages out.
#define currentTex texture0
#define depthTex texture1
#define historyTex texture2
#define aoTex texture3

uniform sampler2D currentTex;
uniform sampler2D depthTex;
uniform sampler2D historyTex;
#ifdef FX_AO
uniform sampler2D aoTex;
#endif

uniform mat4 fxInvViewProj;   // current, jittered: clip -> scene space
uniform mat4 fxPrevViewProj;  // previous frame, unjittered, its own scene space
uniform vec3 fxOffsetDelta;   // camera offset change since last frame (scene units)
uniform vec2 fxJitter;        // this frame's jitter in NDC
uniform float fxTaaBlend;
uniform float fxTaaReset;

CENTROID_ VARYING_ mediump vec2 varTexCoord;

vec3 toYCoCg(vec3 c)
{
	return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
		0.5 * c.r - 0.5 * c.b,
		-0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

vec3 fromYCoCg(vec3 c)
{
	return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

vec3 sampleCurrent(vec2 uv)
{
	vec3 c = texture2D(currentTex, uv).rgb;
#ifdef FX_AO
	vec2 aocs = texture2D(aoTex, uv).rg;
	// emissive and sunlit-bright pixels keep their light
	float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
	c *= mix(aocs.r, 1.0, smoothstep(0.9, 3.0, lum));
	// contact shadows only matter where the sun lights the pixel
	c *= mix(1.0, aocs.g, smoothstep(0.1, 0.4, lum) * (1.0 - smoothstep(2.0, 4.0, lum)));
#endif
	return c;
}

// Catmull-Rom history fetch in 5 bilinear taps (Jimenez/Karis): keeps
// pixel-art texture detail that plain bilinear history smears away.
vec3 sampleHistory(vec2 uv, vec2 size)
{
	vec2 pos = uv * size;
	vec2 c = floor(pos - 0.5) + 0.5;
	vec2 f = pos - c;
	vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
	vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
	vec2 w3 = f * f * (-0.5 + 0.5 * f);
	vec2 w12 = w1 + w2;
	vec2 tc0 = (c - 1.0) / size, tc3 = (c + 2.0) / size;
	vec2 tc12 = (c + w2 / w12) / size;
	vec3 r = texture2D(historyTex, vec2(tc12.x, tc0.y)).rgb * (w12.x * w0.y)
		+ texture2D(historyTex, vec2(tc0.x, tc12.y)).rgb * (w0.x * w12.y)
		+ texture2D(historyTex, tc12).rgb * (w12.x * w12.y)
		+ texture2D(historyTex, vec2(tc3.x, tc12.y)).rgb * (w3.x * w12.y)
		+ texture2D(historyTex, vec2(tc12.x, tc3.y)).rgb * (w12.x * w3.y);
	float w = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
	return max(r / w, vec3(0.0));
}

void main(void)
{
	vec2 uv = varTexCoord.st;
	vec2 px = 1.0 / vec2(textureSize(currentTex, 0));

	vec3 cur = sampleCurrent(uv);

#if defined(FX_DEBUG_VIEW) && FX_DEBUG_VIEW == 7 && defined(FX_AO)
	gl_FragColor = vec4(vec3(texture2D(aoTex, uv).r), 1.0);
	return;
#endif

	// neighbourhood statistics (YCoCg, 3x3)
	vec3 m1 = vec3(0.0), m2 = vec3(0.0);
	for (int y = -1; y <= 1; y++)
	for (int x = -1; x <= 1; x++) {
		vec3 s = toYCoCg(x == 0 && y == 0 ? cur : sampleCurrent(uv + vec2(x, y) * px));
		m1 += s;
		m2 += s * s;
	}
	m1 /= 9.0;
	vec3 sigma = sqrt(max(m2 / 9.0 - m1 * m1, vec3(0.0)));

	// reproject
	float d = texture2D(depthTex, uv).r;
	vec4 p = fxInvViewProj * vec4(vec3(uv, d) * 2.0 - 1.0, 1.0);
	vec3 pos = p.xyz / p.w + fxOffsetDelta;
	vec4 prev = fxPrevViewProj * vec4(pos, 1.0);
	vec2 prev_uv = prev.xy / prev.w * 0.5 + 0.5;

	bool valid = fxTaaReset < 0.5 && prev.w > 0.0 &&
		all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThanEqual(prev_uv, vec2(1.0)));
	if (!valid) {
		gl_FragColor = vec4(cur, 1.0);
		return;
	}

	vec3 hist = toYCoCg(sampleHistory(prev_uv, vec2(textureSize(historyTex, 0))));
	// variance clipping: pull history into the neighbourhood's colour box
	vec3 lo = m1 - sigma * 1.25, hi = m1 + sigma * 1.25;
	vec3 center = (lo + hi) * 0.5, extent = max((hi - lo) * 0.5, vec3(1e-4));
	vec3 off = hist - center;
	vec3 ratio = abs(off / extent);
	float k = max(ratio.x, max(ratio.y, ratio.z));
	if (k > 1.0)
		hist = center + off / k;

	// less history when the camera moved a lot (fewer stale pixels)
	float motion = length((prev_uv - uv) / px);
	float blend = fxTaaBlend * clamp(1.0 - motion * 0.02, 0.6, 1.0);

	// weight by inverse luminance so HDR highlights don't flicker
	vec3 c_y = toYCoCg(cur);
	float wc = (1.0 - blend) / (1.0 + c_y.x);
	float wh = blend / (1.0 + hist.x);
	vec3 res = (c_y * wc + hist * wh) / (wc + wh);
	gl_FragColor = vec4(max(fromYCoCg(res), vec3(0.0)), 1.0);
}
