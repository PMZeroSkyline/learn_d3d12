# Using DXC (Shader Model 6.0):
dxc.exe -T vs_6_0 -E VSMain -Fo bindless_vs.cso bindless.shader
dxc.exe -T ps_6_0 -E PSMain -Fo bindless_ps.cso bindless.shader

# OR using FXC (Shader Model 5.1):
# fxc.exe /T vs_5_1 /E VSMain /Fo bindless_vs.cso bindless.shader /enableed_descriptor_tables
# fxc.exe /T ps_5_1 /E PSMain /Fo bindless_ps.cso bindless.shader /enableed_descriptor_tables