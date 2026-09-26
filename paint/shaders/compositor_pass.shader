
cbuffer constants {
	float vignette_strength;
	float grain_strength;
	float tonemap_strength; // 0.0 or 1.0
	float lut_size;
	float contrast_strength;
	float gamma_strength;
	float4 ocio_lut_data; // STUDIO-PATCH(ocio-viewport): nodes, tile columns, tile rows, 0 ; nodes 0 = OCIO off
};

sampler sampler_linear;

tex2d tex;

tex2d lut_tex;

tex2d ocio_lut_tex; // STUDIO-PATCH(ocio-viewport): OCIO display transform, 3D LUT in tiles (plugins/external/studio_ocio_lut.h)

// tex2d histogram;

struct vert_in {
	float2 pos;
};

struct vert_out {
	float4 pos;
	float2 tex;
};

vert_out vert(vert_in input) {
	vert_out output;
	output.tex = input.pos.xy * 0.5 + 0.5;
	output.tex.y = 1.0 - output.tex.y;
	output.pos = float4(input.pos.xy, 0.0, 1.0);
	return output;
}

float3 tonemap_filmic(float3 color) {
	// Based on Filmic Tonemapping Operators http://filmicgames.com/archives/75
	// float3 x = max(float3(0.0, 0.0, 0.0), color - 0.004);
	float3 x;
	x.x = max(0.0, color.x - 0.004);
	x.y = max(0.0, color.y - 0.004);
	x.z = max(0.0, color.z - 0.004);
	return (x * (x * 6.2 + 0.5)) / (x * (x * 6.2 + 1.7) + 0.06);
}

// STUDIO-PATCH(ocio-viewport): ACEScct curve, the LUT input shaper (studio_ocio_lut_shaper) ; kong writes the literals
// with 6 decimals, < 1e-6 from the CPU curve, far below a LUT cell (1/128)
float ocio_shaper(float x) {
	float y = (log(max(x, 0.0078125)) * 1.4426950408889634 + 9.72) / 17.52;
	if (x <= 0.0078125) {
		y = 10.5402377416545 * x + 0.0729055341958355;
	}
	return y;
}

float4 frag(vert_out input) {
	float4 color = sample_lod(tex, sampler_linear, input.tex, 0.0);

	// Static grain
	float x = (input.tex.x + 4.0) * (input.tex.y + 4.0) * 10.0;
	float g = (((x % 13.0) + 1.0) * ((x % 123.0) + 1.0) % 0.01) - 0.005;
	color.rgb = color.rgb + (float3(g, g, g) * constants.grain_strength);

	// Vignette
	color.rgb = color.rgb * ((1.0 - constants.vignette_strength) + constants.vignette_strength * pow(16.0 * input.tex.x * input.tex.y * (1.0 - input.tex.x) * (1.0 - input.tex.y), 0.2));

	// Auto exposure
	// const float auto_exposure_strength = 1.0;
	// float expo = 2.0 - clamp(length(sample_lod(histogram, sampler_linear, float2(0.5, 0.5), 0.0).rgb), 0.0, 1.0);
	// color.rgb *= pow(expo, auto_exposure_strength * 2.0);

	// STUDIO-PATCH(ocio-viewport): OCIO display transform instead of the filmic tonemap, then the .cube lut on the display
	// values ; same sampling as studio_ocio_lut_sample
	bool ocio = constants.ocio_lut_data.x > 0.0 && constants.tonemap_strength > 0.0;
	if (ocio) {
		float n = constants.ocio_lut_data.x;
		float cols = constants.ocio_lut_data.y;
		float r_o = min(max(ocio_shaper(color.x), 0.0), 1.0) * (n - 1.0);
		float g_o = min(max(ocio_shaper(color.y), 0.0), 1.0) * (n - 1.0);
		float b_o = min(max(ocio_shaper(color.z), 0.0), 1.0) * (n - 1.0);
		float c0 = floor(b_o);
		float c1 = min(c0 + 1.0, n - 1.0);
		float c_frac = b_o - c0;
		float ty0 = floor((c0 + 0.5) / cols);
		float ty1 = floor((c1 + 0.5) / cols);
		float w = cols * n;
		float h = constants.ocio_lut_data.z * n;
		float2 uv0 = float2(((c0 - ty0 * cols) * n + r_o + 0.5) / w, (ty0 * n + g_o + 0.5) / h);
		float2 uv1 = float2(((c1 - ty1 * cols) * n + r_o + 0.5) / w, (ty1 * n + g_o + 0.5) / h);
		float4 o0 = sample_lod(ocio_lut_tex, sampler_linear, uv0, 0.0);
		float4 o1 = sample_lod(ocio_lut_tex, sampler_linear, uv1, 0.0);
		color.rgb = lerp(o0.rgb, o1.rgb, float3(c_frac, c_frac, c_frac));
	}

	if (constants.lut_size > 0.0) {
		// .cube lut color grading
		if (!ocio) { // STUDIO-PATCH(ocio-viewport): OCIO output is already display encoded
			color.rgb = pow(max(color.rgb, float3(0.0, 0.0, 0.0)), float3(0.4545, 0.4545, 0.4545));
		}
		float r_s = min(max(color.x, 0.0), 1.0) * (constants.lut_size - 1.0);
		float g_s = min(max(color.y, 0.0), 1.0) * (constants.lut_size - 1.0);
		float b_s = min(max(color.z, 0.0), 1.0) * (constants.lut_size - 1.0);
		float b0 = floor(b_s);
		float b1 = min(b0 + 1.0, constants.lut_size - 1.0);
		float b_frac = b_s - b0;
		float strip_w = constants.lut_size * constants.lut_size;
		float u0 = (b0 * constants.lut_size + r_s + 0.5) / strip_w;
		float u1 = (b1 * constants.lut_size + r_s + 0.5) / strip_w;
		float v = (g_s + 0.5) / constants.lut_size;
		float4 s0 = sample_lod(lut_tex, sampler_linear, float2(u0, v), 0.0);
		float4 s1 = sample_lod(lut_tex, sampler_linear, float2(u1, v), 0.0);
		color.rgb = lerp(s0.rgb, s1.rgb, float3(b_frac, b_frac, b_frac));
	}
	else if (!ocio) { // STUDIO-PATCH(ocio-viewport)
		// Tonemap with gamma
		color.rgb = lerp(color.rgb, tonemap_filmic(color.rgb), constants.tonemap_strength);
	}

	// Contrast
	color.rgb = (color.rgb - float3(0.5, 0.5, 0.5)) * constants.contrast_strength + float3(0.5, 0.5, 0.5);

	// Gamma
	float inv_gamma = 1.0 / max(constants.gamma_strength, 0.01);
	color.rgb = pow(max(color.rgb, float3(0.0, 0.0, 0.0)), float3(inv_gamma, inv_gamma, inv_gamma));

	return color;
}
