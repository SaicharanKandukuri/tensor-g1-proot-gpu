/*
 * EXPERIMENTAL: VK_SAMPLER_REDUCTION_MODE_MIN/MAX and VK_FILTER_CUBIC_EXT
 * emulation for Valhall v9.
 *
 * Cubic: v9 (like every Mali) has no cubic filter. A sampler with a CUBIC
 * min or mag filter is tagged in word 3 as well and the emulation path
 * below takes the 4x4 Catmull-Rom footprint (2D and 2D array only),
 * weighted for WEIGHTED_AVERAGE, unweighted min/max otherwise. Between mip
 * levels the two results are blended (average) or reduced (min/max).
 *
 * v9 samplers have no reduction mode field (v10 added one), so for samplers
 * created with MIN or MAX the filtering is redone in the shader:
 *
 *  - panvk_vX_sampler.c puts a tag, the mode and the min/mag/mip filters in
 *    sampler descriptor word 3 (unused by v9 hardware). Every other sampler
 *    keeps word 3 = 0.
 *  - For every implicit-LOD, biased or explicit-LOD float sample, this pass
 *    loads word 3. If the tag is set, it computes the level of detail the same
 *    way va_lower_lod() does (sampler min/max LOD, level count, mipmap mode),
 *    fetches the texel footprint the selected filter would use with
 *    texelFetch at that level, applies the sampler's address modes in the
 *    shader, and takes the component-wise min or max. For a LINEAR mipmap
 *    mode with a non-zero fraction both levels are reduced together.
 *  - Otherwise the original hardware sample is used unchanged.
 *
 * Cube and cube array views: the direction picks a face (Vulkan face
 * selection table), the footprint is taken on that face, and with LINEAR
 * filtering texels past an edge come from the adjacent face (seamless cube
 * map edge handling). At a cube corner one of the three corner texels is
 * used. Texels are read with a cube sample at the texel centre (see
 * mm_cube_texel(): TEX_FETCH cannot select a cube face on v9).
 *
 * Not handled (the hardware weighted average is returned, i.e. wrong for a
 * MIN/MAX sampler): depth compare, explicit gradients (txd), non-32-bit
 * results.
 *
 * Runs right before pan_postprocess_nir(), so the raw descriptor loads take
 * the same path as the ones va_lower_lod() emits.
 */

#include "pan_nir.h"
#include "panvk_sampler.h"
#include "util/ralloc.h"

/* The pass runs only when the emulation is opted in and the device enabled
 * a feature that needs it. Hashed into the shader cache key
 * (panvk_hash_state) so cached shaders compiled without it are not reused. */
static inline bool
panvk_v9_sampler_emulation_active(const struct vk_device *dev)
{
   return panvk_v9_sampler_emulation_exposed(9) &&
          (dev->enabled_features.samplerFilterMinmax ||
           dev->enabled_extensions.EXT_sampler_filter_minmax ||
           dev->enabled_extensions.EXT_filter_cubic);
}

static nir_def *
mm_handle(nir_builder *b, nir_tex_instr *tex, nir_tex_src_type off_type,
          uint32_t index)
{
   int i = nir_tex_instr_src_index(tex, off_type);
   if (i >= 0)
      return nir_iadd_imm(b, tex->src[i].src.ssa, index);
   return nir_imm_int(b, index);
}

/* New tex instruction on the same texture/sampler. The caller fills the
 * last extra_srcs sources. */
static nir_tex_instr *
mm_new_tex(nir_builder *b, nir_tex_instr *orig, nir_texop op,
           unsigned extra_srcs, unsigned *first_free)
{
   int ti = nir_tex_instr_src_index(orig, nir_tex_src_texture_offset);
   int si = nir_tex_instr_src_index(orig, nir_tex_src_sampler_offset);
   unsigned n = extra_srcs + (ti >= 0) + (si >= 0);
   nir_tex_instr *t = nir_tex_instr_create(b->shader, n);
   t->op = op;
   t->sampler_dim = orig->sampler_dim;
   t->is_array = orig->is_array;
   t->texture_index = orig->texture_index;
   t->sampler_index = orig->sampler_index;
   t->dest_type = orig->dest_type;
   unsigned k = 0;
   if (ti >= 0)
      t->src[k++] = nir_tex_src_for_ssa(nir_tex_src_texture_offset,
                                        orig->src[ti].src.ssa);
   if (si >= 0)
      t->src[k++] = nir_tex_src_for_ssa(nir_tex_src_sampler_offset,
                                        orig->src[si].src.ssa);
   *first_free = k;
   return t;
}

static unsigned
mm_spatial_dims(enum glsl_sampler_dim dim)
{
   switch (dim) {
   case GLSL_SAMPLER_DIM_1D: return 1;
   case GLSL_SAMPLER_DIM_2D: return 2;
   case GLSL_SAMPLER_DIM_3D: return 3;
   case GLSL_SAMPLER_DIM_CUBE: return 2; /* footprint is on one face */
   default: return 0;
   }
}

static bool
mm_wanted(nir_tex_instr *tex)
{
   if (tex->op != nir_texop_tex && tex->op != nir_texop_txb &&
       tex->op != nir_texop_txl)
      return false;
   if (!mm_spatial_dims(tex->sampler_dim) || tex->is_shadow || tex->is_sparse)
      return false;
   if (nir_alu_type_get_base_type(tex->dest_type) != nir_type_float ||
       tex->def.bit_size != 32)
      return false;
   if (nir_tex_instr_src_index(tex, nir_tex_src_projector) >= 0 ||
       nir_tex_instr_src_index(tex, nir_tex_src_ddx) >= 0 ||
       nir_tex_instr_src_index(tex, nir_tex_src_coord) < 0)
      return false;
   return true;
}

/* Address mode applied in the shader. Returns the wrapped coordinate and sets
 * *border when the texel is outside a CLAMP_TO_BORDER image. Mode values are
 * MALI_WRAP_MODE_* (v9.xml "Wrap Mode"). */
static nir_def *
mm_wrap(nir_builder *b, nir_def *i, nir_def *w, nir_def *mode, nir_def **border)
{
   nir_def *wm1 = nir_iadd_imm(b, w, -1);
   nir_def *rep = nir_imod(b, i, w);
   nir_def *clamp = nir_imin(b, nir_imax(b, i, nir_imm_int(b, 0)), wm1);
   nir_def *w2 = nir_ishl_imm(b, w, 1);
   nir_def *t = nir_imod(b, i, w2);
   nir_def *mrep = nir_bcsel(b, nir_ilt(b, t, w), t,
                             nir_isub(b, nir_iadd_imm(b, w2, -1), t));
   nir_def *m = nir_bcsel(b, nir_ilt_imm(b, i, 0),
                          nir_isub(b, nir_imm_int(b, -1), i), i);
   nir_def *mclamp = nir_imin(b, m, wm1);

   nir_def *is_rep = nir_ieq_imm(b, mode, 8);
   nir_def *is_mrep = nir_ieq_imm(b, mode, 12);
   nir_def *is_mirror_clamp = nir_ior(b, nir_ieq_imm(b, mode, 13),
                                      nir_ieq_imm(b, mode, 15));

   nir_def *res = clamp; /* 9 clamp to edge, 11 clamp to border */
   res = nir_bcsel(b, is_mirror_clamp, mclamp, res);
   res = nir_bcsel(b, is_mrep, mrep, res);
   res = nir_bcsel(b, is_rep, rep, res);

   nir_def *out = nir_ior(b, nir_ilt_imm(b, i, 0), nir_ige(b, i, w));
   nir_def *mout = nir_ige(b, m, w);
   nir_def *bd = nir_iand(b, nir_ieq_imm(b, mode, 11), out);
   bd = nir_ior(b, bd, nir_iand(b, nir_ieq_imm(b, mode, 15), mout));
   *border = nir_ior(b, *border, bd);
   return res;
}

/* Vulkan cube face selection. Works on float directions and on the integer
 * texel-centre vectors used for edge remapping. Ties pick X, then Y, then Z. */
static void
mm_cube_select(nir_builder *b, bool is_int, nir_def *x, nir_def *y,
               nir_def *z, nir_def **face, nir_def **sc, nir_def **tc)
{
#define MM_ABS(v) (is_int ? nir_iabs(b, v) : nir_fabs(b, v))
#define MM_GE(u, v) (is_int ? nir_ige(b, u, v) : nir_fge(b, u, v))
#define MM_NEG(v) (is_int ? nir_ineg(b, v) : nir_fneg(b, v))
   nir_def *zero = is_int ? nir_imm_int(b, 0) : nir_imm_float(b, 0);
   nir_def *ax = MM_ABS(x), *ay = MM_ABS(y), *az = MM_ABS(z);
   nir_def *is_x = nir_iand(b, MM_GE(ax, ay), MM_GE(ax, az));
   nir_def *is_y = nir_iand(b, nir_inot(b, is_x), MM_GE(ay, az));
   nir_def *px = MM_GE(x, zero), *py = MM_GE(y, zero), *pz = MM_GE(z, zero);

   *face = nir_bcsel(b, is_x, nir_bcsel(b, px, nir_imm_int(b, 0), nir_imm_int(b, 1)),
           nir_bcsel(b, is_y, nir_bcsel(b, py, nir_imm_int(b, 2), nir_imm_int(b, 3)),
                     nir_bcsel(b, pz, nir_imm_int(b, 4), nir_imm_int(b, 5))));
   /* +X: (-z,-y)  -X: (z,-y)  +Y: (x,z)  -Y: (x,-z)  +Z: (x,-y)  -Z: (-x,-y) */
   *sc = nir_bcsel(b, is_x, nir_bcsel(b, px, MM_NEG(z), z),
         nir_bcsel(b, is_y, x, nir_bcsel(b, pz, x, MM_NEG(x))));
   *tc = nir_bcsel(b, is_y, nir_bcsel(b, py, z, MM_NEG(z)), MM_NEG(y));
   if (!is_int) {
      nir_def *ma = nir_bcsel(b, is_x, ax, nir_bcsel(b, is_y, ay, az));
      nir_def *half_inv = nir_fdiv(b, nir_imm_float(b, 0.5), ma);
      *sc = nir_fsat(b, nir_ffma(b, *sc, half_inv, nir_imm_float(b, 0.5)));
      *tc = nir_fsat(b, nir_ffma(b, *tc, half_inv, nir_imm_float(b, 0.5)));
   }
#undef MM_ABS
#undef MM_GE
#undef MM_NEG
}

/* Seamless edge handling: texel (i, j) of face `face` (size n), where i or j
 * may be one texel outside the face. The texel centre is turned into an
 * integer 3D vector in half-texel units (the face spans [-n, n]), the face
 * is selected again from that vector, and the coordinates on the new face
 * are mapped back to texels. A coordinate that was in range keeps its texel
 * exactly; the old major axis lands on the edge texel. */
static void
mm_cube_remap(nir_builder *b, nir_def *face, nir_def *i, nir_def *j,
              nir_def *n, nir_def **of, nir_def **oi, nir_def **oj)
{
   nir_def *sc = nir_isub(b, nir_iadd_imm(b, nir_ishl_imm(b, i, 1), 1), n);
   nir_def *tc = nir_isub(b, nir_iadd_imm(b, nir_ishl_imm(b, j, 1), 1), n);
   nir_def *nn = nir_ineg(b, n);
   nir_def *f[6];
   for (unsigned k = 0; k < 6; k++)
      f[k] = nir_ieq_imm(b, face, k);

   nir_def *x = nir_bcsel(b, f[0], n, nir_bcsel(b, f[1], nn,
                nir_bcsel(b, f[5], nir_ineg(b, sc), sc)));
   nir_def *y = nir_bcsel(b, f[2], n, nir_bcsel(b, f[3], nn, nir_ineg(b, tc)));
   nir_def *z = nir_bcsel(b, f[4], n, nir_bcsel(b, f[5], nn,
                nir_bcsel(b, f[0], nir_ineg(b, sc), nir_bcsel(b, f[1], sc,
                nir_bcsel(b, f[2], tc, nir_ineg(b, tc))))));

   nir_def *sc2, *tc2;
   mm_cube_select(b, true, x, y, z, of, &sc2, &tc2);
   nir_def *nm1 = nir_iadd_imm(b, n, -1), *zero = nir_imm_int(b, 0);
   *oi = nir_imin(b, nir_imax(b, nir_ishr_imm(b, nir_iadd(b, sc2, nm1), 1), zero), nm1);
   *oj = nir_imin(b, nir_imax(b, nir_ishr_imm(b, nir_iadd(b, tc2, nm1), 1), zero), nm1);
}

struct mm_state {
   nir_tex_instr *tex;
   bool cube;
   nir_def *face, *cube_index, *lod_bias; /* cube only */
   nir_def *coord, *offset, *size0, *border_color;
   nir_def *wrap[3];
   nir_def *normalized, *is_min, *is_avg;
   /* log2 of the footprint per dimension: 0 nearest, 1 linear, 2 cubic */
   nir_def *l2;
   unsigned dims;
};

static nir_def *
mm_reduce(nir_builder *b, nir_def *is_min, nir_def *a, nir_def *c)
{
   return nir_bcsel(b, is_min, nir_fmin(b, a, c), nir_fmax(b, a, c));
}

/* Weight of tap k (0..2^l2-1) along one dimension, fraction a = frac(u - 0.5).
 * Cubic is Catmull-Rom (the VK_EXT_filter_cubic default):
 *   w = 1/2 * [1 a a^2 a^3] * [ 0  2  0  0
 *                              -1  0  1  0
 *                               2 -5  4 -1
 *                              -1  3 -3  1 ] */
static nir_def *
mm_tap_weight(nir_builder *b, nir_def *l2, nir_def *k, nir_def *a)
{
   nir_def *a2 = nir_fmul(b, a, a), *a3 = nir_fmul(b, a2, a);
   nir_def *c0 = nir_fmul_imm(b, nir_fadd(b, nir_fsub(b, nir_fmul_imm(b, a2, 2.0), a), nir_fneg(b, a3)), 0.5);
   nir_def *c1 = nir_fmul_imm(b, nir_fadd_imm(b, nir_fsub(b, nir_fmul_imm(b, a3, 3.0), nir_fmul_imm(b, a2, 5.0)), 2.0), 0.5);
   nir_def *c2 = nir_fmul_imm(b, nir_fsub(b, nir_fadd(b, a, nir_fmul_imm(b, a2, 4.0)), nir_fmul_imm(b, a3, 3.0)), 0.5);
   nir_def *c3 = nir_fmul_imm(b, nir_fsub(b, a3, a2), 0.5);
   nir_def *cub = nir_bcsel(b, nir_ieq_imm(b, k, 0), c0,
                  nir_bcsel(b, nir_ieq_imm(b, k, 1), c1,
                  nir_bcsel(b, nir_ieq_imm(b, k, 2), c2, c3)));
   nir_def *lin = nir_bcsel(b, nir_ieq_imm(b, k, 0), nir_fsub_imm(b, 1.0, a), a);
   return nir_bcsel(b, nir_ieq_imm(b, l2, 0), nir_imm_float(b, 1.0),
                    nir_bcsel(b, nir_ieq_imm(b, l2, 1), lin, cub));
}

/* Texel (face, i, j) of level lvl of a cube.
 *
 * TEX_FETCH cannot reach cube faces on v9: on a cube descriptor its array
 * index selects the cube and the face is always +X (measured: index 1 of a
 * two-cube array returns cube 1 face 0, index 2 is out of bounds, and a
 * cube-dimension TEX_FETCH returns zero). So the texel is read with a
 * filtered cube sample aimed at the texel centre, at explicit LOD lvl:
 *  - the direction is built with a major axis of exactly 1, so the hardware
 *    face coordinate is sc * 0.5 + 0.5 with no division error, and the
 *    face is unambiguous (|sc|, |tc| < 1);
 *  - at a texel centre the bilinear weights are 1, 0, 0, 0 (exact for
 *    power-of-two sizes; otherwise the error is within float rounding of
 *    the centre);
 *  - the sampler LOD bias is subtracted, so the hardware sees lvl again.
 * The sampler min/max LOD still clamps the explicit LOD: with LINEAR
 * mipmapping and a fractional minLod or maxLod the hardware can blend two
 * levels here. That case is a known limitation. */
static nir_def *
mm_cube_texel(nir_builder *b, struct mm_state *s, nir_def *face, nir_def *i,
              nir_def *j, nir_def *n, nir_def *lvl)
{
   nir_def *rn = nir_frcp(b, nir_i2f32(b, n));
   nir_def *sc = nir_fadd_imm(b, nir_fmul(b, nir_i2f32(b,
                    nir_iadd_imm(b, nir_ishl_imm(b, i, 1), 1)), rn), -1.0);
   nir_def *tc = nir_fadd_imm(b, nir_fmul(b, nir_i2f32(b,
                    nir_iadd_imm(b, nir_ishl_imm(b, j, 1), 1)), rn), -1.0);
   nir_def *one = nir_imm_float(b, 1.0), *mone = nir_imm_float(b, -1.0);
   nir_def *f[6];
   for (unsigned k = 0; k < 6; k++)
      f[k] = nir_ieq_imm(b, face, k);
   /* +X: (1,-tc,-sc) -X: (-1,-tc,sc) +Y: (sc,1,tc) -Y: (sc,-1,-tc)
    * +Z: (sc,-tc,1)  -Z: (-sc,-tc,-1) */
   nir_def *x = nir_bcsel(b, f[0], one, nir_bcsel(b, f[1], mone,
                nir_bcsel(b, f[5], nir_fneg(b, sc), sc)));
   nir_def *y = nir_bcsel(b, f[2], one, nir_bcsel(b, f[3], mone,
                nir_fneg(b, tc)));
   nir_def *z = nir_bcsel(b, f[4], one, nir_bcsel(b, f[5], mone,
                nir_bcsel(b, f[0], nir_fneg(b, sc), nir_bcsel(b, f[1], sc,
                nir_bcsel(b, f[2], tc, nir_fneg(b, tc))))));

   nir_def *comps[4] = { x, y, z, NULL };
   unsigned nc = 3;
   if (s->tex->is_array)
      comps[nc++] = nir_i2f32(b, s->cube_index);

   unsigned k;
   nir_tex_instr *t = mm_new_tex(b, s->tex, nir_texop_txl, 2, &k);
   t->coord_components = nc;
   t->src[k] = nir_tex_src_for_ssa(nir_tex_src_coord, nir_vec(b, comps, nc));
   t->src[k + 1] = nir_tex_src_for_ssa(nir_tex_src_lod,
                                       nir_fsub(b, lvl, s->lod_bias));
   nir_def_init(&t->instr, &t->def, 4, 32);
   nir_builder_instr_insert(b, &t->instr);
   return &t->def;
}

/* Reduce the filter footprint at integer level lvl (float). */
static nir_def *
mm_level(nir_builder *b, struct mm_state *s, nir_def *lvl)
{
   nir_def *li = nir_f2i32(b, lvl);
   nir_def *base[3], *frac[3], *sz[3];

   /* Footprint of 2^l2 taps per dimension starting at
    *   nearest: floor(u)   linear: floor(u - 1/2)   cubic: floor(u - 3/2) */
   nir_def *start = nir_bcsel(b, nir_ieq_imm(b, s->l2, 0), nir_imm_float(b, 0.0),
                    nir_bcsel(b, nir_ieq_imm(b, s->l2, 1), nir_imm_float(b, -0.5),
                              nir_imm_float(b, -1.5)));
   for (unsigned d = 0; d < s->dims; d++) {
      sz[d] = nir_imax(b, nir_ushr(b, nir_channel(b, s->size0, d), li),
                       nir_imm_int(b, 1));
      nir_def *u = nir_channel(b, s->coord, d);
      u = nir_bcsel(b, s->normalized, nir_fmul(b, u, nir_i2f32(b, sz[d])), u);
      base[d] = nir_f2i32(b, nir_ffloor(b, nir_fadd(b, u, start)));
      frac[d] = nir_ffract(b, nir_fadd_imm(b, u, -0.5));
      if (s->offset)
         base[d] = nir_iadd(b, base[d], nir_channel(b, s->offset, d));
   }

   nir_def *layer = NULL;
   if (!s->cube && s->tex->is_array) {
      nir_def *nl = nir_channel(b, s->size0, s->dims);
      layer = nir_f2i32(b, nir_ffloor(b, nir_fadd_imm(b,
                    nir_channel(b, s->coord, s->dims), 0.5)));
      layer = nir_imin(b, nir_imax(b, layer, nir_imm_int(b, 0)),
                       nir_iadd_imm(b, nl, -1));
   }

   /* One texel fetch per loop iteration. Unrolled, the scheduler issued all
    * fetches up front and the register pressure made the backend spill. */
   nir_variable *acc_var =
      nir_local_variable_create(b->impl, glsl_vec4_type(), "mm_acc");
   nir_variable *cnr_var =
      nir_local_variable_create(b->impl, glsl_uint_type(), "mm_corner");
   nir_store_var(b, cnr_var, nir_imm_int(b, 0), 1);

   nir_push_loop(b);
   {
      nir_def *corner = nir_load_var(b, cnr_var);
      nir_break_if(b, nir_uge(b, corner, nir_ishl(b, nir_imm_int(b, 1),
                                  nir_imul_imm(b, s->l2, s->dims))));

      /* tap index per dimension, and the product of the tap weights */
      nir_def *tap[3], *weight = nir_imm_float(b, 1.0);
      nir_def *tap_mask = nir_iadd_imm(b, nir_ishl(b, nir_imm_int(b, 1), s->l2), -1);
      for (unsigned d = 0; d < s->dims; d++) {
         tap[d] = nir_iand(b, nir_ushr(b, corner, nir_imul_imm(b, s->l2, d)),
                           tap_mask);
         weight = nir_fmul(b, weight, mm_tap_weight(b, s->l2, tap[d], frac[d]));
      }

      nir_def *border = nir_imm_false(b);
      nir_def *c[4], *cube_face = NULL;
      unsigned nc = s->dims;
      if (s->cube) {
         /* Address modes are ignored for cubes: NEAREST clamps to the edge,
          * LINEAR crosses into the adjacent face. */
         nir_def *nm1 = nir_iadd_imm(b, sz[0], -1), *zero = nir_imm_int(b, 0);
         nir_def *ij[2], *fc;
         for (unsigned d = 0; d < 2; d++) {
            nir_def *v = nir_iadd(b, base[d], tap[d]);
            ij[d] = nir_bcsel(b, nir_ieq_imm(b, s->l2, 0),
                              nir_imin(b, nir_imax(b, v, zero), nm1), v);
         }
         mm_cube_remap(b, s->face, ij[0], ij[1], sz[0], &fc, &c[0], &c[1]);
         cube_face = fc;
      } else {
         for (unsigned d = 0; d < s->dims; d++)
            c[d] = mm_wrap(b, nir_iadd(b, base[d], tap[d]), sz[d],
                           s->wrap[d], &border);
         if (layer)
            c[nc++] = layer;
      }

      nir_def *fetched;
      if (s->cube) {
         fetched = mm_cube_texel(b, s, cube_face, c[0], c[1], sz[0], lvl);
      } else {
         unsigned k;
         nir_tex_instr *f = mm_new_tex(b, s->tex, nir_texop_txf, 2, &k);
         f->coord_components = nc;
         f->src[k] = nir_tex_src_for_ssa(nir_tex_src_coord, nir_vec(b, c, nc));
         f->src[k + 1] = nir_tex_src_for_ssa(nir_tex_src_lod, li);
         nir_def_init(&f->instr, &f->def, 4, 32);
         nir_builder_instr_insert(b, &f->instr);
         fetched = &f->def;
      }
      nir_def *texel = nir_bcsel(b, border, s->border_color, fetched);

      /* MIN/MAX take every texel of the footprint regardless of its weight
       * (spec: "the texels that would be used"); WEIGHTED_AVERAGE sums. */
      nir_def *acc = nir_load_var(b, acc_var);
      nir_def *first = nir_ieq_imm(b, corner, 0);
      nir_def *wt = nir_fmul(b, texel, weight);
      nir_def *avg = nir_bcsel(b, first, wt, nir_fadd(b, acc, wt));
      nir_def *red = nir_bcsel(b, first, texel,
                               mm_reduce(b, s->is_min, acc, texel));
      acc = nir_bcsel(b, s->is_avg, avg, red);
      nir_store_var(b, acc_var, acc, 0xf);
      nir_store_var(b, cnr_var, nir_iadd_imm(b, corner, 1), 1);
   }
   nir_pop_loop(b, NULL);

   return nir_load_var(b, acc_var);
}

static void
mm_lower_one(nir_builder *b, nir_tex_instr *tex)
{
   /* The hardware sample moves into the else branch so its result is not
    * live across the (register-heavy) emulation path. */
   b->cursor = nir_before_instr(&tex->instr);
   nir_def *orig = &tex->def;

   nir_def *samp_h = mm_handle(b, tex, nir_tex_src_sampler_offset,
                               tex->sampler_index);
   nir_def *tex_h = mm_handle(b, tex, nir_tex_src_texture_offset,
                              tex->texture_index);
   nir_def *w3 = pan_nir_load_va_desc(b, 1, 32, samp_h, 12);

   nir_def *cond = nir_test_mask(b, w3, PANVK_V9_MINMAX_TAG);
   nir_if *nif = nir_push_if(b, cond);

   struct mm_state s = { .tex = tex, .dims = mm_spatial_dims(tex->sampler_dim),
                         .cube = tex->sampler_dim == GLSL_SAMPLER_DIM_CUBE };
   const unsigned lod_dims = s.cube ? 3 : s.dims;
   nir_def *sw = pan_nir_load_va_desc(b, 3, 32, samp_h, 0);
   nir_def *w0 = nir_channel(b, sw, 0), *w1 = nir_channel(b, sw, 1),
           *w2 = nir_channel(b, sw, 2);

   s.coord = tex->src[nir_tex_instr_src_index(tex, nir_tex_src_coord)].src.ssa;
   int oi = nir_tex_instr_src_index(tex, nir_tex_src_offset);
   s.offset = oi >= 0 ? tex->src[oi].src.ssa : NULL;

   /* lambda' */
   nir_def *lam;
   int li = nir_tex_instr_src_index(tex, nir_tex_src_lod);
   if (tex->op == nir_texop_txl && li >= 0) {
      nir_def *bias = nir_fmul_imm(b, nir_i2f32(b,
                         nir_ibitfield_extract_imm(b, w2, 0, 16)), 1.0 / 256.0);
      lam = nir_fadd(b, tex->src[li].src.ssa, bias);
   } else {
      unsigned k;
      nir_tex_instr *q = mm_new_tex(b, tex, nir_texop_lod, 1, &k);
      q->is_array = false;
      q->coord_components = lod_dims;
      q->dest_type = nir_type_float32;
      q->src[k] = nir_tex_src_for_ssa(nir_tex_src_coord,
                     nir_trim_vector(b, s.coord, lod_dims));
      nir_def_init(&q->instr, &q->def, 2, 32);
      nir_builder_instr_insert(b, &q->instr);
      lam = nir_channel(b, &q->def, 1);
      int bi = nir_tex_instr_src_index(tex, nir_tex_src_bias);
      if (bi >= 0)
         lam = nir_fadd(b, lam, tex->src[bi].src.ssa);
   }

   /* Level selection, same as va_lower_lod(). */
   nir_def *min_lod = nir_fmul_imm(b, nir_u2f32(b,
                         nir_ubitfield_extract_imm(b, w1, 0, 13)), 1.0 / 256.0);
   nir_def *max_lod = nir_fmul_imm(b, nir_u2f32(b,
                         nir_ubitfield_extract_imm(b, w1, 16, 13)), 1.0 / 256.0);
   nir_def *maxl = nir_u2f32(b, nir_iadd_imm(b,
                      pan_nir_load_va_tex_levels(b, tex_h), -1));
   max_lod = nir_fmin(b, max_lod, maxl);
   int mi = nir_tex_instr_src_index(tex, nir_tex_src_min_lod);
   if (mi >= 0)
      min_lod = nir_fmax(b, min_lod, tex->src[mi].src.ssa);
   nir_def *lod = nir_fmin(b, nir_fmax(b, lam, min_lod), max_lod);

   nir_def *mip_linear = nir_test_mask(b, w3, PANVK_V9_MINMAX_MIP_LINEAR);
   nir_def *lnear = nir_fmax(b, nir_fadd_imm(b, nir_fceil(b,
                       nir_fadd_imm(b, lod, 0.5)), -1.0), nir_imm_float(b, 0));
   nir_def *lfloor = nir_ffloor(b, lod);
   nir_def *l0 = nir_bcsel(b, mip_linear, lfloor, lnear);
   nir_def *frac = nir_bcsel(b, mip_linear, nir_fsub(b, lod, lfloor),
                             nir_imm_float(b, 0));
   nir_def *l1 = nir_fmin(b, nir_fadd_imm(b, l0, 1.0), maxl);

   nir_def *mag = nir_fle_imm(b, lam, 0.0);
   nir_def *lin = nir_bcsel(b, mag,
                            nir_test_mask(b, w3, PANVK_V9_MINMAX_MAG_LINEAR),
                            nir_test_mask(b, w3, PANVK_V9_MINMAX_MIN_LINEAR));
   nir_def *cub = nir_bcsel(b, mag,
                            nir_test_mask(b, w3, PANVK_V9_MINMAX_MAG_CUBIC),
                            nir_test_mask(b, w3, PANVK_V9_MINMAX_MIN_CUBIC));
   /* Cubic is only exposed for 2D and 2D array views; anything else gets
    * the linear footprint. */
   if (s.dims != 2 || s.cube)
      cub = nir_imm_false(b);
   s.l2 = nir_bcsel(b, cub, nir_imm_int(b, 2),
                    nir_bcsel(b, lin, nir_imm_int(b, 1), nir_imm_int(b, 0)));
   s.is_min = nir_test_mask(b, w3, PANVK_V9_MINMAX_MIN);
   s.is_avg = nir_inot(b, nir_test_mask(b, w3, PANVK_V9_MINMAX_MIN |
                                                PANVK_V9_MINMAX_MAX));
   s.normalized = nir_test_mask(b, w0, 1u << 25);
   s.wrap[0] = nir_ubitfield_extract_imm(b, w0, 16, 4); /* S */
   s.wrap[1] = nir_ubitfield_extract_imm(b, w0, 12, 4); /* T */
   s.wrap[2] = nir_ubitfield_extract_imm(b, w0, 8, 4);  /* R */
   s.size0 = pan_nir_load_va_tex_size(b, tex_h, tex->sampler_dim,
                                      tex->is_array);
   if (s.cube) {
      /* Face and face coordinates from the direction. Cubes always use
       * normalized coordinates. The array size counts cubes. */
      nir_def *fs, *ft;
      mm_cube_select(b, false, nir_channel(b, s.coord, 0),
                     nir_channel(b, s.coord, 1), nir_channel(b, s.coord, 2),
                     &s.face, &fs, &ft);
      s.cube_index = NULL;
      if (tex->is_array) {
         nir_def *cube = nir_f2i32(b, nir_ffloor(b, nir_fadd_imm(b,
                            nir_channel(b, s.coord, 3), 0.5)));
         s.cube_index = nir_imin(b, nir_imax(b, cube, nir_imm_int(b, 0)),
                                 nir_iadd_imm(b, nir_channel(b, s.size0, 2), -1));
      }
      s.lod_bias = nir_fmul_imm(b, nir_i2f32(b,
                      nir_ibitfield_extract_imm(b, w2, 0, 16)), 1.0 / 256.0);
      s.coord = nir_vec2(b, fs, ft);
      s.normalized = nir_imm_true(b);
   }
   /* Border texel value. The raw sampler words are not it: the hardware
    * converts the border colour to the image format first, e.g. a format
    * without alpha reads alpha 1 (CTS unnormal e5b9g9r9 cubic failed on
    * alpha with the raw words). So ask the hardware: sample far outside the
    * image along every CLAMP_TO_BORDER axis, where all taps are border
    * texels. Not used when no axis clamps to border, or for cubes. */
   if (s.cube) {
      s.border_color = nir_imm_vec4(b, 0, 0, 0, 0);
   } else {
      nir_def *bc[4];
      for (unsigned d = 0; d < s.dims; d++) {
         nir_def *is_b = nir_ior(b, nir_ieq_imm(b, s.wrap[d], 11),
                                    nir_ieq_imm(b, s.wrap[d], 15));
         bc[d] = nir_bcsel(b, is_b, nir_imm_float(b, -1024.0),
                           nir_imm_float(b, 0.5));
      }
      unsigned nbc = s.dims;
      if (tex->is_array)
         bc[nbc++] = nir_channel(b, s.coord, s.dims);
      unsigned k;
      nir_tex_instr *bt = mm_new_tex(b, tex, nir_texop_txl, 2, &k);
      bt->coord_components = nbc;
      bt->src[k] = nir_tex_src_for_ssa(nir_tex_src_coord, nir_vec(b, bc, nbc));
      bt->src[k + 1] = nir_tex_src_for_ssa(nir_tex_src_lod, nir_imm_float(b, 0.0));
      nir_def_init(&bt->instr, &bt->def, 4, 32);
      nir_builder_instr_insert(b, &bt->instr);
      s.border_color = &bt->def;
   }

   nir_def *r0 = mm_level(b, &s, l0);
   nir_if *nif2 = nir_push_if(b, nir_fgt_imm(b, frac, 0.0));
   nir_def *r1 = mm_level(b, &s, l1);
   nir_def *r01 = nir_bcsel(b, s.is_avg, nir_flrp(b, r0, r1, frac),
                            mm_reduce(b, s.is_min, r0, r1));
   nir_pop_if(b, nif2);
   nir_def *res = nir_if_phi(b, r01, r0);
   res = nir_trim_vector(b, res, orig->num_components);

   nir_push_else(b, nif);
   nir_instr_remove(&tex->instr);
   nir_builder_instr_insert(b, &tex->instr);
   nir_pop_if(b, nif);
   nir_def *phi = nir_if_phi(b, res, orig);
   /* Every use of the hardware result except the phi itself now reads the
    * phi. (nir_def_rewrite_uses_after only works within one block.) */
   nir_foreach_use_including_if_safe(src, orig) {
      if (!nir_src_is_if(src) && nir_src_use_instr(src) == nir_def_instr(phi))
         continue;
      nir_src_rewrite(src, phi);
   }
}

static bool
panvk_v9_lower_sampler_minmax(nir_shader *nir)
{
   bool progress = false;

   nir_foreach_function_impl(impl, nir) {
      unsigned n = 0;
      nir_foreach_block(block, impl)
         nir_foreach_instr(instr, block)
            n += instr->type == nir_instr_type_tex &&
                 mm_wanted(nir_instr_as_tex(instr));
      if (!n) {
         nir_no_progress(impl);
         continue;
      }

      nir_tex_instr **list = ralloc_array(NULL, nir_tex_instr *, n);
      unsigned k = 0;
      nir_foreach_block(block, impl)
         nir_foreach_instr(instr, block)
            if (instr->type == nir_instr_type_tex &&
                mm_wanted(nir_instr_as_tex(instr)))
               list[k++] = nir_instr_as_tex(instr);

      nir_builder b = nir_builder_create(impl);
      for (unsigned i = 0; i < n; i++)
         mm_lower_one(&b, list[i]);
      ralloc_free(list);

      progress |= nir_progress(true, impl, nir_metadata_none);
   }
   if (progress)
      NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   return progress;
}
