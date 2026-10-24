// fx: GPU particle update. One fragment per slot: reads the current state,
// writes the next (S0..S2; S3 is static). Forces, the shared wind field,
// curl turbulence, the player's wake, block-exact collision against the
// world texture, kind changes (rain -> splash), resting and waking leaves.
#include "fx_common/uniforms.glsl"
#include "fx_common/color.glsl"
#define fxpS0 texture0
#define fxpS1 texture1
#define fxpS2 texture2
#define fxpS3 texture3
#define fxpWorldTex texture7
uniform sampler2D fxpS0;
uniform sampler2D fxpS1;
uniform sampler2D fxpS2;
uniform sampler2D fxpS3;
uniform sampler2D fxpWorldTex;
#include "fx_common/particles.glsl"

bool solidAt(vec3 p) { return fxpWorldAt(p).r > 0.5; }

void kill()
{
	gl_FragData[0] = vec4(0.0);
	gl_FragData[1] = vec4(0.0);
	gl_FragData[2] = vec4(0.0);
}

void main(void)
{
	ivec2 tc = ivec2(gl_FragCoord.xy);
	vec4 s0 = texelFetch(fxpS0, tc, 0);
	vec4 s1 = texelFetch(fxpS1, tc, 0);
	vec4 s2 = texelFetch(fxpS2, tc, 0);
	float kind = s1.w;
	if (kind < 0.5) {
		kill();
		return;
	}
	float dt = fxpDt;
	vec3 p = s0.xyz + fxpShift.xyz; // the camera offset moved: keep the world position
	vec3 v = s1.xyz;
	float age = s0.w + dt;
	float life = s2.x, seed = s2.y, param = s2.z, rest = s2.w;
	if (age >= life || length(p - fxpCamPos.xyz) > 72.0) {
		kill();
		return;
	}
	vec4 here = fxpWorldAt(p);
	bool resting = rest >= 0.0;
	if (here.r > 0.5 && !resting) {
		kill(); // a block appeared where it was (or it spawned inside one)
		return;
	}

	bool spark = fxpIs(kind, FXP_SPARK), leaf = fxpIs(kind, FXP_LEAF);
	bool rain = fxpIs(kind, FXP_RAIN), drip = fxpIs(kind, FXP_DRIP);
	bool snow = fxpIs(kind, FXP_SNOW), splash = fxpIs(kind, FXP_SPLASH);
	bool dust = fxpIs(kind, FXP_DUST), ash = fxpIs(kind, FXP_ASH), firefly = fxpIs(kind, FXP_FIREFLY);
	if (splash) {
		gl_FragData[0] = vec4(p, age);
		gl_FragData[1] = s1;
		gl_FragData[2] = s2;
		return;
	}

	vec3 wp = p + fxpAnchor.xyz; // world nodes
	// the wind blows under the open sky, little indoors
	vec3 wind = fxpWind(wp) * mix(0.12, 1.0, here.g);
	vec3 curl = fxpCurl(wp * 0.7, fxTime * 0.5 + seed * 11.0);

	// the player's wake: the air near a moving player moves with them, swirled
	vec3 dpl = p - fxpPlayer.xyz;
	float speed = fxpPlayer.w;
	float wake = fxpWake * (1.0 - smoothstep(0.3, 2.4, length(dpl * vec3(1.0, 0.6, 1.0)))) *
			smoothstep(0.8, 3.5, speed);
	vec3 wake_air = fxpPlayerVel.xyz * 0.9 + curl * speed * 0.5 +
			normalize(dpl + vec3(0.0, 0.2, 0.0)) * speed * 0.35;

	vec3 acc = vec3(0.0);
	vec3 air = wind;
	float drag = 0.0, turb = 0.0, gravity = 0.0, bounce = 0.3, wake_k = 1.0;
	if (spark) {
		// hot air rises and cools: buoyancy fades with age
		float heat = 1.0 - age / life;
		acc.y += 7.5 * heat * heat * (0.6 + seed * 0.8);
		gravity = 5.0;
		drag = 0.9;
		turb = 0.7;
		bounce = 0.35;
	} else if (leaf) {
		gravity = 3.0;
		drag = 3.2; // terminal speed ~0.9 nodes/s
		turb = 0.6;
		bounce = 0.0;
		// flutter: swinging side to side, a little lift on each swing
		float ph = age * (2.2 + seed * 1.6) + seed * 40.0;
		float a = seed * 6.2832;
		acc += vec3(cos(a), 0.0, sin(a)) * sin(ph) * 2.4;
		acc.y += abs(cos(ph)) * 1.4;
		wake_k = 1.6;
	} else if (rain || drip) {
		gravity = 9.8;
		drag = rain ? 0.08 : 0.02;
		bounce = 0.0;
		wake_k = 0.0;
	} else if (snow) {
		gravity = 1.6;
		drag = 1.6; // terminal ~1 node/s
		turb = 0.7;
		bounce = 0.0;
		acc.xz += vec2(cos(age * 1.7 + param), sin(age * 1.3 + param)) * 0.6; // sway
	} else if (dust) {
		drag = 1.2;
		turb = 0.25;
		air *= 0.35;
		bounce = 0.2;
	} else if (ash) {
		drag = 1.0;
		turb = 0.4;
		air = air * 0.6 + vec3(0.0, param, 0.0); // param: rise (cinders) or fall (ash)
		bounce = 0.2;
	} else if (firefly) {
		drag = 1.0;
		air = curl * 0.6 + wind * 0.2 + vec3(0.0, 0.08 * sin(age * 0.9 + seed * 9.0), 0.0);
		bounce = 0.4;
		wake_k = 0.6;
	}
	air += curl * turb;
	float wk = clamp(wake * wake_k, 0.0, 1.0);
	air = mix(air, wake_air, wk);
	drag += wk * 6.0;

	if (resting) {
		// leaves and snow lying on a top face (or leaves floating on water)
		rest += dt;
		vec4 below = fxpWorldAt(p - vec3(0.0, 0.1, 0.0));
		bool supported = below.r > 0.5 || below.b > 0.75;
		if (!supported) {
			rest = -1.0; // the block went away: fall again
			v = vec3(0.0);
		} else if (leaf) {
			float gust = length(wind);
			float h = fxHash13(vec3(gl_FragCoord.xy, fract(fxTime * 7.31) * 100.0));
			float kick = max(gust - (1.5 + seed * 1.2), 0.0);
			if (wk > 0.2 || h < kick * dt * 1.5) {
				// kicked up: by a gust or the player running past
				v = air * 0.8 + vec3(0.0, 1.0 + seed * 1.4 + wk * 2.2, 0.0);
				rest = -1.0;
			} else {
				// sliding along with the gusts, friction otherwise
				float slide = below.b > 0.75 ? 0.15 : smoothstep(1.0 + seed * 0.8, 2.5, gust) * 0.3;
				v.xz = mix(v.xz, wind.xz * slide, 1.0 - exp(-2.0 * dt));
				v.y = 0.0;
			}
		} else {
			v = vec3(0.0);
		}
		if (rest >= 0.0 && dot(v.xz, v.xz) < 1e-6) {
			gl_FragData[0] = vec4(p, age);
			gl_FragData[1] = vec4(v, kind);
			gl_FragData[2] = vec4(life, seed, param, rest);
			return;
		}
	} else {
		v += (acc - vec3(0.0, gravity, 0.0)) * dt;
		v += (air - v) * (1.0 - exp(-drag * dt));
	}

	// move with block-exact collision, axis by axis, sub-stepped so nothing
	// tunnels through a block at low frame rates
	bool landed = false, in_water = false;
	float steps = clamp(ceil(length(v * dt) / 0.4), 1.0, 4.0);
	float sdt = dt / steps;
	for (int i = 0; i < 4; i++) {
		if (float(i) >= steps)
			break;
		vec3 q = p;
		q.y = p.y + v.y * sdt;
		vec4 wy = fxpWorldAt(q);
		bool water = wy.b > 0.75;
		bool liquid_stops = rain || drip || snow || leaf || spark;
		if (wy.r > 0.5 || (water && liquid_stops && rest < 0.0)) {
			if (v.y < 0.0) {
				landed = true;
				in_water = water && wy.r < 0.5;
				q.y = floor(p.y + 0.5) - 0.5 + 0.012; // on the face below
			} else {
				q.y = p.y;
			}
			v.y = -v.y * bounce;
		}
		q.x = p.x + v.x * sdt;
		if (solidAt(q)) {
			q.x = p.x;
			v.x = -v.x * bounce;
		}
		q.z = p.z + v.z * sdt;
		if (solidAt(q)) {
			q.z = p.z;
			v.z = -v.z * bounce;
		}
		p = q;
		if (landed)
			break;
	}

	if (landed) {
		if (rain || drip) {
			// a splash where it hit: flat ring, short-lived (param: size)
			kind = FXP_SPLASH;
			age = 0.0;
			life = 0.2 + seed * 0.15;
			param = drip ? 0.6 : 1.0;
			v = vec3(0.0);
		} else if (spark) {
			if (in_water) {
				kill();
				return;
			}
			v.xz *= 0.55; // friction on the bounce
		} else if ((leaf || snow) && rest < 0.0) {
			rest = 0.0;
			v = vec3(v.x * 0.3, 0.0, v.z * 0.3);
			if (snow)
				v = vec3(0.0);
			// resting leaves and settled snow fade after a while
			life = min(life, age + (leaf ? 10.0 + seed * 10.0 : 2.5 + seed * 3.0));
		} else if (rest < 0.0) {
			v.xz *= 0.7;
		}
	}

	gl_FragData[0] = vec4(p, age);
	gl_FragData[1] = vec4(v, kind);
	gl_FragData[2] = vec4(life, seed, param, rest);
}
