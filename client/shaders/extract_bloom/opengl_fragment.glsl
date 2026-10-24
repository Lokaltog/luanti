#define rendered texture0

struct ExposureParams {
	float compensationFactor;
};

uniform sampler2D rendered;
uniform mediump float bloomStrength;
uniform ExposureParams exposureParams;

#ifdef FX_HDR_COMPOSITE
uniform float fxBloomThreshold;
uniform float fxBloomKnee;
uniform float fxExposure;
#endif

CENTROID_ VARYING_ mediump vec2 varTexCoord;

#ifdef ENABLE_AUTO_EXPOSURE
VARYING_ float exposure; // linear exposure factor, see vertex shader
#endif

void main(void)
{
	vec2 uv = varTexCoord.st;
	vec3 color = texture2D(rendered, uv).rgb;
	// translate to linear colorspace (approximate)
#ifdef GL_ES
	// clamp color to [0,1] range in lieu of centroids
	color = pow(clamp(color, 0.0, 1.0), vec3(2.2));
#else
	color = pow(color, vec3(2.2));
#endif

#ifdef FX_HDR_COMPOSITE
	// fx: only HDR energy above the threshold blooms (soft knee, as in
	// Karis 2014), so emissives glow and lit surfaces stay crisp.
	color *= exposureParams.compensationFactor * fxExposure;
#ifdef ENABLE_AUTO_EXPOSURE
	color *= exposure;
#endif
	float peak = max(color.r, max(color.g, color.b));
	float knee = fxBloomThreshold * fxBloomKnee + 1e-4;
	float soft = clamp(peak - fxBloomThreshold + knee, 0.0, 2.0 * knee);
	soft = soft * soft / (4.0 * knee);
	color *= max(soft, peak - fxBloomThreshold) / max(peak, 1e-4);
#else
	color *= exposureParams.compensationFactor * bloomStrength;

#ifdef ENABLE_AUTO_EXPOSURE
	color *= exposure;
#endif
#endif

	gl_FragColor = vec4(min(color, vec3(60000.0)), 1.0); // force full alpha to avoid holes in the image. fx: clamp below fp16 max so one hot pixel cannot become Inf
}
