#version 450
// Video (YUV 4:2:0, BT.709 limited) -> RGB with the picture filters, plus the UI overlay.
layout(set = 0, binding = 0) uniform sampler2D texY;
layout(set = 0, binding = 1) uniform sampler2D texU;
layout(set = 0, binding = 2) uniform sampler2D texV;
layout(set = 0, binding = 3) uniform sampler2D texUI;
layout(push_constant) uniform PC
{
	vec4 rect;      // video rect in output uv (x0, y0, x1, y1)
	float sharpen;  // 0..1
	float deband;   // 0 / 1
	float restore;  // 0, 1 (low), 2 (high)
	float boost;    // 0, 1, 2
	float hasVideo;
	float uiOn;
	float time;
	float pad;
} pc;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;

float Y(vec2 t) { return texture(texY, t).r; }

vec3 yuv2rgb(float y, float u, float v)
{
	y = (y - 16.0 / 255.0) * (255.0 / 219.0);
	u = (u - 128.0 / 255.0) * (255.0 / 224.0);
	v = (v - 128.0 / 255.0) * (255.0 / 224.0);
	return vec3(y + 1.5748 * v, y - 0.1873 * u - 0.4681 * v, y + 1.8556 * u);
}

void main()
{
	vec3 c = vec3(0.035, 0.035, 0.05);
	if(pc.hasVideo > 0.5)
	{
		vec2 t = (uv - pc.rect.xy) / (pc.rect.zw - pc.rect.xy);
		if(all(greaterThanEqual(t, vec2(0.0))) && all(lessThanEqual(t, vec2(1.0))))
		{
			vec2 px = 1.0 / vec2(textureSize(texY, 0));
			float y0 = Y(t);
			float n = Y(t + vec2(0.0, -px.y)), s = Y(t + vec2(0.0, px.y));
			float e = Y(t + vec2(px.x, 0.0)), w = Y(t + vec2(-px.x, 0.0));

			// Detail restore: smooth compression noise/blocking but keep real edges.
			if(pc.restore > 0.0)
			{
				float th = 0.015 + 0.02 * pc.restore;
				float acc = y0, wsum = 1.0;
				float nb[4] = float[](n, s, e, w);
				for(int i = 0; i < 4; i++)
				{
					float d = abs(nb[i] - y0);
					float wt = 0.35 * pc.restore * clamp(1.0 - d / th, 0.0, 1.0);
					acc += nb[i] * wt;
					wsum += wt;
				}
				y0 = acc / wsum;
			}

			// Deband: flat gradients get a wider average plus a little dither.
			if(pc.deband > 0.5)
			{
				vec2 r = px * 6.0;
				float a = (Y(t + vec2(r.x, r.y)) + Y(t + vec2(-r.x, r.y)) + Y(t + vec2(r.x, -r.y)) + Y(t + vec2(-r.x, -r.y))) * 0.25;
				if(abs(a - y0) < 0.006)
					y0 = mix(y0, a, 0.6);
				float dn = fract(sin(dot(uv * 1000.0 + pc.time, vec2(12.9898, 78.233))) * 43758.5453) - 0.5;
				y0 += dn / 255.0;
			}

			// Sharpen: contrast-adaptive (CAS-style), stronger where it won't ring.
			if(pc.sharpen > 0.0)
			{
				float mn = min(y0, min(min(n, s), min(e, w)));
				float mx = max(y0, max(max(n, s), max(e, w)));
				float amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, 1e-4), 0.0, 1.0));
				float wg = -amp * mix(0.04, 0.2, pc.sharpen);
				y0 = clamp((y0 + (n + s + e + w) * wg) / (1.0 + 4.0 * wg), 0.0, 1.0);
			}

			c = yuv2rgb(y0, texture(texU, t).r, texture(texV, t).r);

			// Colour boost for the TN panel: a bit more saturation and contrast.
			if(pc.boost > 0.0)
			{
				float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
				c = mix(vec3(l), c, 1.0 + 0.12 * pc.boost);
				c = (c - 0.5) * (1.0 + 0.05 * pc.boost) + 0.5;
			}
			c = clamp(c, 0.0, 1.0);
		}
	}
	if(pc.uiOn > 0.5)
	{
		vec4 ui = texture(texUI, uv);
		c = mix(c, ui.rgb, ui.a);
	}
	o = vec4(c, 1.0);
}
