// Shared colour/AppSW depth intersection. Each shader supplies pc.a/b/c, col and dep.
// XR_RAY_BOUNDS enables the colour path acceleration; both paths use the same solver.
#pragma once
#define XR_DEPTH_RAY_GLSL \
"float idist(float u, float v) {\n" \
"  int f = int(pc.b.w);\n" \
"  vec2 q = pc.c.xy + vec2(u, (f & 4) != 0 ? 1.0 - v : v) * pc.c.zw;\n" \
"  float z = texture(dep, q).r;\n" \
"#ifdef XR_HELD_LAYER\n" \
"  if (pc.d.z > 0.5 && (f & 256) == 0 && z < pc.d.x) return -1.0;\n" \
"#endif\n" \
"  if ((f & 1) == 0) z = z * 2.0 - 1.0;\n" \
"  return max((z + pc.b.y) / pc.b.z, 0.0);\n" \
"}\n" \
"float xr_surface(float u0, float v, float K) {\n" \
"  float t = 0.0;\n" \
"  const float TMAX = 5.0;\n" \
"  bool accurate=(int(pc.b.w)&64)!=0, oracle=(int(pc.b.w)&128)!=0;\n" \
"  int steps=oracle?48:accurate?24:12; float nsteps=float(steps);\n" \
"  if (K != 0.0) {\n" \
"    int first=1;\n" \
"#ifdef XR_RAY_BOUNDS\n" \
"    if ((int(pc.b.w)&8)!=0) {\n" \
"      vec2 sz=vec2(textureSize(dep,0));\n" \
"      float yy=pc.c.y+((int(pc.b.w)&4)!=0?1.0-v:v)*pc.c.w;\n" \
"      int row=int(clamp(floor(yy*sz.y),0.0,sz.y-1.0))/(((int(pc.b.w)&32)!=0)?1:64);\n" \
"      float xa=(pc.c.x+u0*pc.c.z)*sz.x, xb=(pc.c.x+(u0+K*TMAX)*pc.c.z)*sz.x;\n" \
"      int lo=int(clamp(floor(min(xa,xb)),0.0,sz.x-1.0))/64;\n" \
"      int hi=int(clamp(floor(max(xa,xb)),0.0,sz.x-1.0))/64;\n" \
"      float bound=0.0; for(int x=lo;x<=hi;x++) bound=max(bound,texelFetch(bounds,ivec2(x,row),0).r*5.0);\n" \
"      first=clamp(int(floor(nsteps*(1.0-bound/TMAX))),1,steps);\n" \
"    }\n" \
"#endif\n" \
"    float tp = TMAX*(1.0-float(first-1)/nsteps), dp = idist(u0 + K * tp, v) - tp;\n" \
"    if (dp >= 0.0) t = tp;\n" \
"    else for (int i = first; i <= steps; i++) {\n" \
"      float tc = TMAX * (1.0 - float(i) / nsteps);\n" \
"      float dc = idist(u0 + K * tc, v) - tc;\n" \
"      if (dc >= 0.0) {\n" \
"        int refine=oracle?12:accurate?8:2;\n" \
"        for (int j = 0; j < refine; j++) {\n" \
"          float tm = tp + (tc - tp) * (-dp) / max(dc - dp, 1e-6);\n" \
"          if((accurate||oracle)&&j>0) tm=0.5*(tp+tc);\n" \
"          float dm = idist(u0 + K * tm, v) - tm;\n" \
"          if((accurate||oracle)&&abs(dm*K)*float(textureSize(col,0).x)<0.002) { tc=tm; break; }\n" \
"          if (dm >= 0.0) { tc = tm; dc = dm; } else { tp = tm; dp = dm; }\n" \
"          if((accurate||oracle)&&abs((tc-tp)*K)*float(textureSize(col,0).x)<(oracle?0.001:0.1)) break;\n" \
"        }\n" \
"        t = tc;\n" \
"        break;\n" \
"      }\n" \
"      tp = tc; dp = dc;\n" \
"    }\n" \
"  }\n" \
"  return t;\n" \
"}\n" \
""
