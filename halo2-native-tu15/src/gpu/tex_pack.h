/* Hi-res texture pack: DDS files that replace the game's textures on the GPU
 * (tex_pack.c). The guest keeps its own texture; only the view the D3D11
 * back end binds changes. */
#ifndef TEX_PACK_H
#define TEX_PACK_H

#define COBJMACROS
#include <d3d11.h>
#include <stdint.h>

/* Nonzero when a pack folder with at least one usable DDS was found. The
 * folder is RECOMP_TEX_PACK, or "textures" in the current directory (the
 * install folder); RECOMP_TEX_PACK=0 turns the pack off. */
int texpack_enabled(void);

/* The hi-res textures setting (settings.c), switchable at any time: off,
 * texpack_enabled() is 0 and no lookups match. The folder is scanned the
 * first time it is on. The D3D11 back end reloads its cached textures when
 * this changes (halo2_gpu_set_hires). */
void texpack_set_active(int on);
int texpack_active(void);

/* Looks up the texture whose face-0 level-0 guest bytes hash to `hash` at
 * width x height (hash_bytes() in d3d11_backend.c; the key in the DDS file
 * name), directly or through mip_aliases.txt. On a match returns 1 with
 * references the caller owns (*tex and *srv), the view's level count and
 * the number of levels the replacement adds over the guest size
 * (log2 of the scale). 0: no replacement, nothing returned. */
int texpack_lookup(ID3D11Device *dev, uint64_t hash, unsigned width, unsigned height,
                   int cube, ID3D11Texture2D **tex, ID3D11ShaderResourceView **srv,
                   unsigned *levels, unsigned *lod_extra);

#endif
