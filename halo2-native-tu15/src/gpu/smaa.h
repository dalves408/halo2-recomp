/* SMAA 1x post-process applied at present time, in place of FXAA (Halo 2 Native 1.5).
 * Uses the SMAA reference shader and lookup textures (src/gpu/smaa, MIT, iryoku/smaa). */
#ifndef HALO2_SMAA_H
#define HALO2_SMAA_H

#define COBJMACROS
#include <d3d11.h>

void halo2_smaa_set_enabled(int enabled);
int  halo2_smaa_enabled(void);
void halo2_smaa_shutdown(void);
int  halo2_smaa_apply(ID3D11Device *device, ID3D11DeviceContext *ctx,
                      ID3D11Texture2D *target, ID3D11RenderTargetView *target_rtv,
                      UINT width, UINT height);

#endif
