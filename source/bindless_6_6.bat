# Compile Vertex Shader (Shader Model 6.6)
dxc.exe -T vs_6_6 -E VSMain -Fo bindless_vs_6_6.sco bindless_6_6.shader

# Compile Pixel Shader (Shader Model 6.6)
dxc.exe -T ps_6_6 -E PSMain -Fo bindless_ps_6_6.sco bindless_6_6.shader