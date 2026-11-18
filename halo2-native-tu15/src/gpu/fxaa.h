/* FXAA post-process applied at present time (Halo 2 Native 1.5). */
#ifndef HALO2_FXAA_H
#define HALO2_FXAA_H

#define COBJMACROS
#include <d3d11.h>

void halo2_fxaa_set_enabled(int enabled);
int  halo2_fxaa_enabled(void);
void halo2_fxaa_shutdown(void);
int  halo2_fxaa_apply(ID3D11Device *device, ID3D11DeviceContext *ctx,
                      ID3D11Texture2D *target, ID3D11RenderTargetView *target_rtv,
                      UINT width, UINT height);

#endif
