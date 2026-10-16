// SPDX-License-Identifier: GPL-3.0-only
struct Input { float3 position : POSITION; float3 color : COLOR; };
struct Output { float4 position : SV_Position; float3 color : COLOR; };
Output VSMain(Input v) {
    Output o; o.position = float4(v.position, 1.0); o.color = v.color; return o;
}
float4 PSMain(Output v) : SV_Target { return float4(v.color, 1.0); }
