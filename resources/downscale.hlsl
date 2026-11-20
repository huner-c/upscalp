// Filtering happens in linear light when linearLight=1. The intermediate
// textures are RGBA16F so negative reconstruction lobes survive both passes.
cbuffer Settings : register(b0) {
    float2 sourceSize;
    float2 targetSize;
    uint horizontal;
    uint filterMode; // 0 area, 1 Mitchell, 2 Lanczos2, 3 bilinear, 4 guarded Lanczos3
    uint linearLight;
    uint comparison;
    float sharpness;
    float time;
    float2 padding;
};
Texture2D<float4> image : register(t0);
Texture2D<float4> original : register(t1);
SamplerState clampLinear : register(s0);

struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex VS(uint id : SV_VertexID) {
    Vertex v;
    v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv * float2(2,-2) + float2(-1,1), 0, 1);
    return v;
}
float3 Decode(float3 c) {
    return lerp(c / 12.92, pow(max((c + .055) / 1.055, 0), 2.4), step(.04045, c));
}
float3 Encode(float3 c) {
    c = saturate(c);
    return lerp(c * 12.92, 1.055 * pow(c, 1.0/2.4) - .055, step(.0031308,c));
}
float Sinc(float x) {
    x = abs(x);
    if (x < .00001) return 1;
    return sin(3.141592654*x) / (3.141592654*x);
}
float Kernel(float x) {
    x = abs(x);
    if (filterMode == 2) return x < 2 ? Sinc(x)*Sinc(x/2) : 0;
    if (filterMode == 4) return x < 3 ? Sinc(x)*Sinc(x/3) : 0;
    // Mitchell-Netravali B=C=1/3; gentler negative lobes than Catmull-Rom.
    if (x < 1) return ((7*x-12)*x*x+5.33333333)/6;
    if (x < 2) return (((-2.33333333*x+12)*x-20)*x+10.66666667)/6;
    return 0;
}
float3 Read(int2 p) {
    p = clamp(p, int2(0,0), int2(sourceSize)-1);
    float3 c = image.Load(int3(p,0)).rgb;
    return horizontal && linearLight ? Decode(c) : c;
}
float3 ClampToCenter(float3 color, int2 first, int2 direction) {
    float3 a = Read(first);
    float3 b = Read(first + direction);
    return clamp(color, min(a,b), max(a,b));
}
float3 SamplePair(float2 center, float2 direction, float distance) {
    float2 step = direction * distance;
    return image.SampleLevel(clampLinear,center-step,0).rgb
         + image.SampleLevel(clampLinear,center+step,0).rgb;
}
float4 FilterPS(Vertex v) : SV_Target {
    float2 center = v.uv * sourceSize - .5;
    float axis = horizontal ? center.x : center.y;
    float ratio = horizontal ? sourceSize.x / targetSize.x : sourceSize.y / targetSize.y;
    float footprint = max(ratio, 1);
    int2 p = int2(floor(center + .5));
    int2 direction = horizontal ? int2(1,0) : int2(0,1);
    int2 firstCenter = horizontal ? int2(floor(axis),p.y) : int2(p.x,floor(axis));
    if (ratio == 2 && (filterMode == 2 || filterMode == 4)) {
        // At 2:1 the phase is fixed. Pair adjacent same-sign taps through the
        // texture sampler. An sRGB view decodes the first pass before blending.
        float2 uv = (center + .5) / sourceSize;
        float2 texel = direction / sourceSize;
        if (filterMode == 2) {
            float3 color = .550803366 * SamplePair(uv,texel,.711509409)
                         - .050803366 * SamplePair(uv,texel,2.674463481);
            return float4(color,1);
        }
        float3 color = .581890671 * SamplePair(uv,texel,.732870693)
                     - .100635949 * SamplePair(uv,texel,2.837837838)
                     + .018745278 * SamplePair(uv,texel,4.696803451);
        return float4(ClampToCenter(color,firstCenter,direction),1);
    }
    if (filterMode == 3) {
        return float4(lerp(Read(firstCenter),Read(firstCenter+direction),frac(axis)),1);
    }
    float radius = filterMode == 0 ? footprint*.5+.5 : footprint*(filterMode == 4 ? 3 : 2);
    int first = (int)ceil(axis-radius);
    int last = (int)floor(axis+radius);
    float3 sum = 0;
    float total = 0;
    [loop] for (int i=first; i<=last; ++i) {
        float w;
        if (filterMode == 0) {
            // Exact source-pixel overlap for fractional as well as integer ratios.
            w = max(0, min(i+.5,axis+ratio*.5)-max(i-.5,axis-ratio*.5));
        } else { w = Kernel((i-axis)/footprint); }
        sum += Read(horizontal ? int2(i,p.y) : int2(p.x,i)) * w;
        total += w;
    }
    float3 color = sum/max(total,.00001);
    if (filterMode == 4) color = ClampToCenter(color,firstCenter,direction);
    return float4(color,1);
}
float3 Sharpen(float3 c, float2 uv) {
    float2 d = 1.0 / sourceSize;
    float3 n = image.SampleLevel(clampLinear,uv-float2(0,d.y),0).rgb;
    float3 s = image.SampleLevel(clampLinear,uv+float2(0,d.y),0).rgb;
    float3 e = image.SampleLevel(clampLinear,uv+float2(d.x,0),0).rgb;
    float3 w = image.SampleLevel(clampLinear,uv-float2(d.x,0),0).rgb;
    float3 lo = min(c,min(min(n,s),min(e,w)));
    float3 hi = max(c,max(max(n,s),max(e,w)));
    float contrast = max(hi.r-lo.r,max(hi.g-lo.g,hi.b-lo.b));
    return clamp(c + (c-(n+s+e+w)*.25)*sharpness/(1+contrast*4),lo,hi);
}
float4 PresentPS(Vertex v) : SV_Target {
    // Left is ordinary bilinear from the SAME captured frame, not native 1080p.
    if (comparison && abs(v.uv.x-.5) < 1.0/targetSize.x) return float4(.2,.9,.75,1);
    if (comparison && v.uv.x < .5) return float4(original.SampleLevel(clampLinear,v.uv,0).rgb,1);
    float3 c = image.SampleLevel(clampLinear,v.uv,0).rgb;
    if (sharpness > 0) c = Sharpen(c,v.uv);
    c = linearLight ? Encode(c) : saturate(c);
    return float4(c,1);
}
float4 PatternPS(Vertex v) : SV_Target {
    float2 p = v.uv * targetSize;
    float3 c = float3(.035,.055,.08);
    if (v.uv.y < .33) {
        float lineDistance = abs(frac((p.x+p.y*.31+time*45)/13)-.5);
        c = lineDistance < .08 ? float3(.95,.95,.95) : c;
    } else if (v.uv.y < .66) {
        float check = fmod(floor(p.x)+floor(p.y),2);
        c = check.xxx;
    } else {
        c = float3(v.uv.x,.2,1-v.uv.x);
        float r = length((v.uv-float2(.5,.83))*targetSize);
        if (abs(r-170) < 1.1) c = 1;
    }
    return float4(c,1);
}
