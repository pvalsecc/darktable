/*
   This file is part of darktable,
   Copyright (C) 2019-2020 darktable developers.

   darktable is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   darktable is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/gaussian.h"
#include "common/image.h"
#include "common/iop_profile.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop_gui.h"
#include "develop/imageop_math.h"
#include "develop/noise_generator.h"
#include "develop/openmp_maths.h"
#include "dtgtk/button.h"
#include "dtgtk/drawingarea.h"
#include "dtgtk/expander.h"
#include "dtgtk/paint.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

#include <complex.h>

DT_MODULE_INTROSPECTION(1, dt_iop_diffuse_params_t)

#define MAX_NUM_SCALES 12
typedef struct dt_iop_diffuse_params_t
{
  // global parameters
  int iterations;           // $MIN: 1   $MAX: 128   $DEFAULT: 1  $DESCRIPTION: "iterations"
  float sharpness;          // $MIN: -1.  $MAX: 1.   $DEFAULT: 0. $DESCRIPTION: "sharpness"
  int radius;               // $MIN: 1   $MAX: 256   $DEFAULT: 8  $DESCRIPTION: "radius"
  float regularization;     // $MIN: 0. $MAX: 6.   $DEFAULT: 0. $DESCRIPTION: "edge sensitivity"
  float variance_threshold; // $MIN: -1. $MAX: 1.   $DEFAULT: 0. $DESCRIPTION: "edge threshold"

  float anisotropy_first;         // $MIN: -4. $MAX: 4.   $DEFAULT: 0. $DESCRIPTION: "1st order anisotropy"
  float anisotropy_second;        // $MIN: -4. $MAX: 4.   $DEFAULT: 0. $DESCRIPTION: "2nd order anisotropy"
  float anisotropy_third;         // $MIN: -4. $MAX: 4.   $DEFAULT: 0. $DESCRIPTION: "3rd order anisotropy"
  float anisotropy_fourth;        // $MIN: -4. $MAX: 4.   $DEFAULT: 0. $DESCRIPTION: "4th order anisotropy"

  float threshold; // $MIN: 0.  $MAX: 8.   $DEFAULT: 0. $DESCRIPTION: "luminance masking threshold"

  float first; // $MIN: -1. $MAX: 1.   $DEFAULT: 0. $DESCRIPTION: "1st order (gradient)"
  float second; // $MIN: -1. $MAX: 1.   $DEFAULT: 0. $DESCRIPTION: "2nd order (laplacian)"
  float third; // $MIN: -1. $MAX: 1.   $DEFAULT: 0. $DESCRIPTION: "3rd order (gradient of laplacian)"
  float fourth; // $MIN: -1. $MAX: 1.   $DEFAULT: 0. $DESCRIPTION: "4th order (laplacian of laplacian)"
} dt_iop_diffuse_params_t;


typedef struct dt_iop_diffuse_gui_data_t
{
  GtkWidget *iterations, *fourth, *third, *second, *radius, *sharpness, *threshold, *regularization, *first,
      *anisotropy_first, *anisotropy_second, *anisotropy_third, *anisotropy_fourth, *regularization_first, *variance_threshold;
} dt_iop_diffuse_gui_data_t;

typedef struct dt_iop_diffuse_global_data_t
{
  int kernel_wavelets_decompose;
  int kernel_diffuse;
  int kernel_init;
} dt_iop_diffuse_global_data_t;


// only copy params struct to avoid a commit_params()
typedef struct dt_iop_diffuse_params_t dt_iop_diffuse_data_t;


typedef enum dt_isotropy_t
{
  DT_ISOTROPY_ISOTROPE = 0, // diffuse in all directions with same intensity
  DT_ISOTROPY_ISOPHOTE = 1, // diffuse more in the isophote direction (orthogonal to gradient)
  DT_ISOTROPY_GRADIENT = 2  // diffuse more in the gradient direction
} dt_isotropy_t;


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline dt_isotropy_t check_isotropy_mode(const float anisotropy)
{
  // user param is negative, positive or zero. The sign encodes the direction of diffusion, the magnitude encodes the ratio of anisotropy
  // ultimately, the anisotropy factor needs to be positive before going into the exponential
  if(anisotropy == 0.f) return DT_ISOTROPY_ISOTROPE;
  else if(anisotropy > 0.f) return DT_ISOTROPY_ISOPHOTE;
  else return DT_ISOTROPY_GRADIENT; // if(anisotropy > 0.f)
}


const char *name()
{
  return _("diffuse or sharpen");
}

const char *aliases()
{
  return _("diffusion|deconvolution|blur|sharpening");
}

const char *description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self,
                                _("simulate directional diffusion of light with heat transfer model\n"
                                  "to apply an iterative edge-oriented blur, \n"
                                  "inpaint damaged parts of the image,\n"
                                  "or to remove blur with blind deconvolution."),
                                _("corrective and creative"), _("linear, RGB, scene-referred"), _("linear, RGB"),
                                _("linear, RGB, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_EFFECTS;
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_diffuse_params_t p;
  memset(&p, 0, sizeof(p));

  // deblurring presets
  p.sharpness = 0.0f;
  p.threshold = 0.0f;
  p.variance_threshold = 0.f;

  p.anisotropy_first = -4.f;
  p.anisotropy_second = -4.f;
  p.anisotropy_third = +2.f;
  p.anisotropy_fourth = -4.f;

  p.first = -0.25f;
  p.second = -0.50f;
  p.third = +0.40f;
  p.fourth = -0.40f;

  p.iterations = 4;
  p.radius = 4;
  p.regularization = 4.5f;
  dt_gui_presets_add_generic(_("remove soft lens blur"), self->op, self->version(), &p, sizeof(p), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  p.iterations = 8;
  p.radius = 8;
  p.regularization = 5.5f;
  dt_gui_presets_add_generic(_("remove medium lens blur"), self->op, self->version(), &p, sizeof(p), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  p.iterations = 12;
  p.radius = 12;
  p.regularization = 5.7f;
  dt_gui_presets_add_generic(_("remove heavy lens blur"), self->op, self->version(), &p, sizeof(p), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  p.iterations = 20;
  p.radius = 16.f;
  p.sharpness = 0.f;
  p.variance_threshold = 0.0f;

  p.anisotropy_first = 0.f;
  p.anisotropy_second = 0.f;
  p.anisotropy_third = 0.f;
  p.anisotropy_fourth = 0.f;

  dt_gui_presets_add_generic(_("remove hazing"), self->op, self->version(), &p, sizeof(p), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  p.iterations = 5;
  p.radius = 8;
  p.sharpness = 0.0f;
  p.threshold = 0.0f;
  p.variance_threshold = 0.f;
  p.regularization = 5.f;

  p.anisotropy_first = -1.f;
  p.anisotropy_second = -1.f;
  p.anisotropy_third = 1.f;
  p.anisotropy_fourth = 1.f;

  p.first = -0.10f;
  p.second = -0.10f;
  p.third = +0.10f;
  p.fourth = +0.10f;
  dt_gui_presets_add_generic(_("denoise"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  p.iterations = 2;
  p.radius = 32;
  p.sharpness = 0.0f;
  p.threshold = 0.0f;
  p.variance_threshold = 0.f;
  p.regularization = 4.f;

  p.anisotropy_first = 0.f;
  p.anisotropy_second = +4.f;
  p.anisotropy_third = +4.f;
  p.anisotropy_fourth = +4.f;

  p.first = +0.0f;
  p.second = +0.25f;
  p.third = +0.25f;
  p.fourth = +0.25f;
  dt_gui_presets_add_generic(_("surface blur"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);


  p.iterations = 2;
  p.radius = 16;
  p.sharpness = 0.0f;
  p.threshold = 0.0f;
  p.variance_threshold = 0.f;
  p.regularization = 0.f;

  p.anisotropy_first = 0.f;
  p.anisotropy_second = 0.f;
  p.anisotropy_third = 0.f;
  p.anisotropy_fourth = 0.f;

  p.first = +0.25f;
  p.second = +0.25f;
  p.third = +0.25f;
  p.fourth = +0.25f;
  dt_gui_presets_add_generic(_("diffuse"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  p.iterations = 1;
  p.radius = 8;
  p.sharpness = 0.5f;
  p.threshold = 0.0f;
  p.variance_threshold = 0.25f;
  p.regularization = 1.f;

  p.anisotropy_first = +4.f;
  p.anisotropy_second = +4.f;
  p.anisotropy_third = +4.f;
  p.anisotropy_fourth = +4.f;

  p.first = +0.25f;
  p.second = +0.25f;
  p.third = +0.25f;
  p.fourth = +0.25f;
  dt_gui_presets_add_generic(_("increase perceptual acutance"), self->op, self->version(), &p, sizeof(p), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  p.iterations = 4;
  p.radius = 64;
  p.sharpness = -0.05f;
  p.threshold = 0.0f;
  p.variance_threshold = 0.f;
  p.regularization = 4.f;

  p.anisotropy_first = -4.f;
  p.anisotropy_second = +4.f;
  p.anisotropy_third = +4.f;
  p.anisotropy_fourth = +4.f;

  p.first = -0.50f;
  p.second = 0.f;
  p.third = +0.25f;
  p.fourth = +0.25f;
  dt_gui_presets_add_generic(_("simulate watercolour"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);
}

// B spline filter
#define FSIZE 5

// The B spline best approximate a Gaussian of standard deviation :
// see https://eng.aurelienpierre.com/2021/03/rotation-invariant-laplacian-for-2d-grids/
#define B_SPLINE_SIGMA 1.0553651328015339f

static inline float normalize_laplacian(const float sigma)
{
  // Normalize the wavelet scale to approximate a laplacian
  // see https://eng.aurelienpierre.com/2021/03/rotation-invariant-laplacian-for-2d-grids/#Scaling-coefficient
  return 2.f * M_PI_F / (sqrtf(M_PI_F) * sqf(sigma));
}

static inline float equivalent_sigma_at_step(const float sigma, const unsigned int s)
{
  // If we stack several gaussian blurs of standard deviation sigma on top of each other,
  // this is the equivalent standard deviation we get at the end (after s steps)
  // First step is s = 0
  // see
  // https://eng.aurelienpierre.com/2021/03/rotation-invariant-laplacian-for-2d-grids/#Multi-scale-iterative-scheme
  if(s == 0)
    return sigma;
  else
    return sqrtf(sqf(equivalent_sigma_at_step(sigma, s - 1)) + sqf(exp2f((float)s) * sigma));
}

static inline unsigned int num_steps_to_reach_equivalent_sigma(const float sigma_filter, const float sigma_final)
{
  // The inverse of the above : compute the number of scales needed to reach the desired equivalent sigma_final
  // after sequential blurs of constant sigma_filter
  unsigned int s = 0;
  float radius = sigma_filter;
  while(radius < sigma_final)
  {
    ++s;
    radius = sqrtf(sqf(radius) + sqf((float)(1 << s) * sigma_filter));
  }
  return s + 1;
}

inline static void blur_2D_Bspline(const float *const restrict in, float *const restrict HF,
                                   float *const restrict LF, const int mult, const size_t width,
                                   const size_t height)
{
  // see https://arxiv.org/pdf/1711.09791.pdf
#ifdef _OPENMP
#pragma omp parallel for default(none) dt_omp_firstprivate(width, height, in, LF, HF, mult) schedule(simd         \
                                                                                                     : static)    \
    collapse(2)
#endif
  for(size_t i = 0; i < height; i++)
  {
    for(size_t j = 0; j < width; j++)
    {
      const size_t index = (i * width + j) * 4;
      float DT_ALIGNED_PIXEL acc[4] = { 0.f };

      for(size_t ii = 0; ii < FSIZE; ++ii)
        for(size_t jj = 0; jj < FSIZE; ++jj)
        {
          const size_t row = CLAMP((int)i + mult * (int)(ii - (FSIZE - 1) / 2), (int)0, (int)height - 1);
          const size_t col = CLAMP((int)j + mult * (int)(jj - (FSIZE - 1) / 2), (int)0, (int)width - 1);
          const size_t k_index = (row * width + col) * 4;

          const float DT_ALIGNED_ARRAY filter[FSIZE]
              = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };
          const float filters = filter[ii] * filter[jj];

          for_four_channels(c, aligned(in : 64) aligned(acc : 16)) acc[c] += filters * in[k_index + c];
        }

      for_four_channels(c, aligned(in, HF, LF : 64) aligned(acc : 16))
      {
        LF[index + c] = acc[c];
        HF[index + c] = in[index + c] - acc[c];
      }
    }
  }
}

static inline void init_reconstruct(float *const restrict reconstructed, const size_t width, const size_t height)
{
// init the reconstructed buffer with non-clipped and partially clipped pixels
#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(reconstructed, width, height)                 \
    schedule(simd:static) aligned(reconstructed:64)
#endif
  for(size_t k = 0; k < height * width * 4; k++) reconstructed[k] = 0.f;
}


// Discretization parameters for the Partial Derivative Equation solver
#define H 1         // spatial step
#define KAPPA 0.25f // 0.25 if h = 1, 1 if h = 2


#ifdef _OPENMP
#pragma omp declare simd aligned(pixels:64) uniform(pixels)
#endif
static inline complex float find_gradient(const float pixels[9][4], const size_t c)
{
  // Compute the gradient with centered finite differences in a 3×3 stencil
  // warning : x is vertical, y is horizontal, same orientations as C buffers
  // We store the gradient as a complex numper : real = du(i, j) / dx ; imaginary = du(i, j) / dy
  return ((pixels[7][c] - pixels[1][c]) + I * (pixels[5][c] - pixels[3][c])) / 2.f;
}

#ifdef _OPENMP
#pragma omp declare simd aligned(pixels:64) uniform(pixels)
#endif
static inline complex float find_laplacian(const float pixels[9][4], const size_t c)
{
  // Compute the laplacian with centered finite differences in a 3×3 stencil
  // warning : x is vertical, y is horizontal
  // We store the laplacian as a complex numper : real = d²u(i, j) / dx² ; imaginary = d²u(i, j) / dy²
  return (pixels[7][c] + pixels[1][c]) + I * (pixels[5][c] + pixels[3][c]) - 2.f * pixels[4][c] * (1.f + I);
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void compute_anisotropic_direction(const complex float gradient, const float anisotropy,
                                                 float *const restrict c2,
                                                 float *const restrict cos_theta, float *const restrict sin_theta,
                                                 float *const restrict cos_theta2, float *const restrict sin_theta2)
{
  // find the argument and magnitude of the gradient
  // output the factor of anisotropy c2
  // output cos(argument) and sin(argument) to build the matrix of rotation
  const float magnitude = cabsf(gradient);
  const float theta = cargf(gradient);

  *sin_theta = sinf(theta);
  *cos_theta = cosf(theta);

  *cos_theta2 = sqf(*cos_theta);
  *sin_theta2 = sqf(*sin_theta);

  *c2 = expf(-magnitude / anisotropy); // c² in https://www.researchgate.net/publication/220663968
}

#ifdef _OPENMP
#pragma omp declare simd aligned(a:16)
#endif
static inline void rotation_matrix_isophote(const float c2,
                                            const float cos_theta, const float sin_theta,
                                            const float cos_theta2, const float sin_theta2,
                                            float a[2][2])
{
  // Write the coefficients of a square symmetrical matrice of rotation of the gradient :
  // [[ a11, a12 ],
  //  [ a12, a22 ]]
  // taken from https://www.researchgate.net/publication/220663968
  // c dampens the gradient direction
  a[0][0] = cos_theta2 + c2 * sin_theta2;
  a[1][1] = c2 * cos_theta2 + sin_theta2;
  a[0][1] = a[1][0] = (c2 - 1.0f) * cos_theta * sin_theta;
}

#ifdef _OPENMP
#pragma omp declare simd aligned(a:16)
#endif
static inline void rotation_matrix_gradient(const float c2,
                                            const float cos_theta, const float sin_theta,
                                            const float cos_theta2, const float sin_theta2,
                                            float a[2][2])
{
  // Write the coefficients of a square symmetrical matrice of rotation of the gradient :
  // [[ a11, a12 ],
  //  [ a12, a22 ]]
  // based on https://www.researchgate.net/publication/220663968 and inverted
  // c dampens the isophote direction
  a[0][0] = c2 * cos_theta2 + sin_theta2;
  a[1][1] = cos_theta2 + c2 * sin_theta2;
  a[0][1] = a[1][0] = (1.0f - c2) * cos_theta * sin_theta;
}


#ifdef _OPENMP
#pragma omp declare simd aligned(kernel: 64) aligned(a:16)
#endif
static inline void build_matrix(const float a[2][2], float kernel[9])
{
  const float b11 = -a[0][1] / 2.0f;
  const float b13 = -b11;
  const float b22 = -2.0f * (a[0][0] + a[1][1]);

  // build the kernel of rotated anisotropic laplacian
  // from https://www.researchgate.net/publication/220663968 :
  // [ [ -a12 / 2,  a22,           a12 / 2  ],
  //   [ a11,      -2 (a11 + a22), a11      ],
  //   [ a12 / 2,   a22,          -a12 / 2  ] ]
  kernel[0] = b11;
  kernel[1] = a[1][1];
  kernel[2] = b13;
  kernel[3] = a[0][0];
  kernel[4] = b22;
  kernel[5] = a[0][0];
  kernel[6] = b13;
  kernel[7] = a[1][1];
  kernel[8] = b11;
}

#ifdef _OPENMP
#pragma omp declare simd aligned(kernel: 64)
#endif
static inline void isotrope_laplacian(float kernel[9])
{
  // see in https://eng.aurelienpierre.com/2021/03/rotation-invariant-laplacian-for-2d-grids/#Second-order-isotropic-finite-differences
  // for references (Oono & Puri)
  kernel[0] = 0.25f;
  kernel[1] = 0.5f;
  kernel[2] = 0.25f;
  kernel[3] = 0.5f;
  kernel[4] = -3.f;
  kernel[5] = 0.5f;
  kernel[6] = 0.25f;
  kernel[7] = 0.5f;
  kernel[8] = 0.25f;
}

#ifdef _OPENMP
#pragma omp declare simd aligned(pixels, kernel: 64) uniform(pixels, anisotropy, isotropy_type)
#endif
static inline void compute_kernel(const float pixels[9][4], const size_t c,
                                   const float anisotropy, const dt_isotropy_t isotropy_type,
                                   const int gradient, float kernel[9])
{
  // if gradient == TRUE, follow the direction of the gradient
  // if gradient == FALSE, follow the direction of the derivative of the gradient

  // Build the matrix of rotation with anisotropy

  switch(isotropy_type)
  {
    case(DT_ISOTROPY_ISOTROPE):
    default:
    {
      isotrope_laplacian(kernel);
      break;
    }
    case(DT_ISOTROPY_ISOPHOTE):
    {
      float DT_ALIGNED_ARRAY a[2][2] = { { 0.f } };
      float c2, cos_theta, cos_theta2, sin_theta, sin_theta2;
      compute_anisotropic_direction((gradient) ? find_gradient(pixels, c)
                                               : find_laplacian(pixels, c),
                                     anisotropy, &c2, &cos_theta, &sin_theta, &cos_theta2, &sin_theta2);
      rotation_matrix_isophote(c2, cos_theta, sin_theta, cos_theta2, sin_theta2, a);
      build_matrix(a, kernel);
      break;
    }
    case(DT_ISOTROPY_GRADIENT):
    {
      float DT_ALIGNED_ARRAY a[2][2] = { { 0.f } };
      float c2, cos_theta, cos_theta2, sin_theta, sin_theta2;
      compute_anisotropic_direction((gradient) ? find_gradient(pixels, c)
                                               : find_laplacian(pixels, c),
                                     anisotropy, &c2, &cos_theta, &sin_theta, &cos_theta2, &sin_theta2);
      rotation_matrix_gradient(c2, cos_theta, sin_theta, cos_theta2, sin_theta2, a);
      build_matrix(a, kernel);
      break;
    }
  }
}

static inline void heat_PDE_diffusion(const float *const restrict high_freq, const float *const restrict low_freq,
                                      const uint8_t *const restrict mask, const int has_mask,
                                      float *const restrict output, const size_t width, const size_t height,
                                      const float anisotropy[4], const dt_isotropy_t isotropy_type[4],
                                      const float regularization, const float variance_threshold,
                                      const int current_radius, const int is_last_step,
                                      const float ABCD[4], const float strength,
                                      const int compute_first, const int compute_second,
                                      const int compute_third, const int compute_fourth,
                                      const int compute_variance)
{
  // Simultaneous inpainting for image structure and texture using anisotropic heat transfer model
  // https://www.researchgate.net/publication/220663968
  // modified as follow :
  //  * apply it in a multi-scale wavelet setup : we basically solve it twice, on the wavelets LF and HF layers.
  //  * replace the manual texture direction/distance selection by an automatic detection similar to the structure one,
  //  * generalize the framework for isotropic diffusion and anisotropic weighted on the isophote direction
  //  * add a variance regularization to better avoid edges.
  // The sharpness setting mimics the contrast equalizer effect by simply multiplying the HF by some gain.

  float *const restrict out = DT_IS_ALIGNED(output);
  const float *const restrict LF = DT_IS_ALIGNED(low_freq);
  const float *const restrict HF = DT_IS_ALIGNED(high_freq);

#ifdef _OPENMP
#pragma omp parallel for default(none)                                                                            \
    dt_omp_firstprivate(out, mask, HF, LF, height, width, ABCD, has_mask, current_radius, compute_first,          \
                        compute_second, compute_third, compute_fourth, compute_variance, variance_threshold,      \
                        anisotropy, regularization, is_last_step, strength, isotropy_type) schedule(simd:static) collapse(2)
#endif
  for(size_t i = 0; i < height; ++i)
    for(size_t j = 0; j < width; ++j)
    {
      const size_t idx = (i * width + j);
      const size_t index = idx * 4;
      uint8_t opacity = (has_mask) ? mask[idx] : 1;

      if(opacity)
      {
        // non-local neighbours coordinates
        const size_t j_neighbours[3]
            = { CLAMP((int)(j - current_radius * H), (int)0, (int)width - 1),   // y - mult
                j,                                                              // y
                CLAMP((int)(j + current_radius * H), (int)0, (int)width - 1) }; // y + mult

        const size_t i_neighbours[3]
            = { CLAMP((int)(i - current_radius * H), (int)0, (int)height - 1),   // x - mult
                i,                                                               // x
                CLAMP((int)(i + current_radius * H), (int)0, (int)height - 1) }; // x + mult

        // fetch non-local pixels and store them locally and contiguously
        float DT_ALIGNED_ARRAY neighbour_pixel_HF[9][4];
        float DT_ALIGNED_ARRAY neighbour_pixel_LF[9][4];

        for(size_t ii = 0; ii < 3; ii++)
          for(size_t jj = 0; jj < 3; jj++)
            for_four_channels(c, aligned(neighbour_pixel_LF, neighbour_pixel_HF, HF, LF : 64))
            {
              neighbour_pixel_HF[3 * ii + jj][c] = HF[(i_neighbours[ii] * width + j_neighbours[jj]) * 4 + c];
              neighbour_pixel_LF[3 * ii + jj][c] = LF[(i_neighbours[ii] * width + j_neighbours[jj]) * 4 + c];
            }

        for_four_channels(c, aligned(neighbour_pixel_LF, neighbour_pixel_HF, out, LF, HF : 64) \
            aligned(anisotropy, isotropy_type, ABCD :16))
        {
          // build the local anisotropic convolution filters for gradients and laplacians
          float DT_ALIGNED_ARRAY kern_first[9], kern_second[9], kern_third[9], kern_fourth[9];
          compute_kernel(neighbour_pixel_LF, c, anisotropy[0], isotropy_type[0], TRUE, kern_first);
          compute_kernel(neighbour_pixel_LF, c, anisotropy[1], isotropy_type[1], FALSE, kern_second);
          compute_kernel(neighbour_pixel_HF, c, anisotropy[2], isotropy_type[2], TRUE, kern_third);
          compute_kernel(neighbour_pixel_HF, c, anisotropy[3], isotropy_type[3], FALSE, kern_fourth);

          // convolve filters and compute the variance and the regularization term
          float DT_ALIGNED_PIXEL derivatives[4] = { 0.f };
          float variance = 0.f;
          for(size_t k = 0; k < 9; k++)
          {
            derivatives[0] += kern_first[k] * neighbour_pixel_LF[k][c];
            derivatives[1] += kern_second[k] * neighbour_pixel_LF[k][c];
            derivatives[2] += kern_third[k] * neighbour_pixel_HF[k][c];
            derivatives[3] += kern_fourth[k] * neighbour_pixel_HF[k][c];
            variance += sqf(neighbour_pixel_HF[k][c]);
          }
          variance = variance_threshold + variance / 9.f * regularization;

          // compute the update
          float acc = 0.f;
          for(size_t k = 0; k < 4; k++) acc += derivatives[k] * ABCD[k];
          acc = (HF[index + c] + acc / variance) * strength;

          // update the solution
          out[index + c] += (is_last_step) ? acc + LF[index + c] : acc;
        }
      }
      else
      {
        // only copy input to output, do nothing
        for_four_channels(c, aligned(out, HF, LF : 64))
          out[index + c] += (is_last_step) ? HF[index + c] + LF[index + c] : HF[index + c];
      }
    }
}

static inline float compute_anisotropy_factor(const float user_param)
{
  // compute the K param in c evaluation from https://www.researchgate.net/publication/220663968
  // but in a perceptually-even way, for better GUI interaction
  const float normalize = expf(1.f) - 1.f;
  if(user_param == 0.f) return FLT_MAX;
  else return expf(fabsf(1.f / user_param) - 1.f) / normalize;
}

static inline gint wavelets_process(const float *const restrict in, float *const restrict reconstructed,
                                    const uint8_t *const restrict mask, const size_t width,
                                    const size_t height, const dt_iop_diffuse_data_t *const data,
                                    const float final_radius, const float zoom, const int scales)
{
  gint success = TRUE;

  const float DT_ALIGNED_PIXEL anisotropy[4]
      = { compute_anisotropy_factor(data->anisotropy_first),
          compute_anisotropy_factor(data->anisotropy_second),
          compute_anisotropy_factor(data->anisotropy_third),
          compute_anisotropy_factor(data->anisotropy_fourth)};

  const dt_isotropy_t DT_ALIGNED_PIXEL isotropy_type[4]
      = { check_isotropy_mode(data->anisotropy_first),
          check_isotropy_mode(data->anisotropy_second),
          check_isotropy_mode(data->anisotropy_third),
          check_isotropy_mode(data->anisotropy_fourth) };

  float regularization = powf(10.f, data->regularization) - 1.f;
  float variance_threshold = powf(10.f, data->variance_threshold);

  // wavelets scales buffers
  float *const restrict LF_even = dt_alloc_align_float(width * height * 4); // low-frequencies RGB
  float *const restrict LF_odd = dt_alloc_align_float(width * height * 4);  // low-frequencies RGB
  float *const restrict HF = dt_alloc_align_float(width * height * 4);      // high-frequencies RGB

  // Init reconstructed with valid parts of image
  init_reconstruct(reconstructed, width, height);

  // À trous wavelet decompose
  // there is a paper from a guy we know that explains it : https://jo.dreggn.org/home/2010_atrous.pdf
  // the wavelets decomposition here is the same as the equalizer/atrous module,
  for(int s = 0; s < scales; ++s)
  {
    const float *restrict detail; // buffer containing this scale's input
    float *restrict LF;           // output buffer for the current scale

    // swap buffers so we only need 2 LF buffers : the LF at scale (s-1) and the one at current scale (s)
    if(s == 0)
    {
      detail = in;
      LF = LF_odd;
    }
    else if(s % 2 != 0)
    {
      detail = LF_odd;
      LF = LF_even;
    }
    else
    {
      detail = LF_even;
      LF = LF_odd;
    }

    const float current_radius = equivalent_sigma_at_step(B_SPLINE_SIGMA, s);
    const float real_radius = current_radius * zoom;

    const float norm = expf(-sqf(real_radius) / sqf(data->radius));
    const float DT_ALIGNED_ARRAY ABCD[4] = { data->first * KAPPA * norm, data->second * KAPPA * norm,
                                             data->third * KAPPA * norm, data->fourth * KAPPA * norm };
    const float strength = data->sharpness * norm + 1.f;
    const int is_last_step = (scales - 1) == s;

    /* Debug
    fprintf(stdout, "scale %i : mult = %i ; current rad = %.0f ; real rad = %.0f ; norm = %f ; strength = %f\n", s,
            1 << s, current_radius, real_radius, norm, strength);
    */

    // Compute wavelets low-frequency scales
    blur_2D_Bspline(detail, HF, LF, 1 << s, width, height);
    heat_PDE_diffusion(HF, LF, mask, (mask != NULL), reconstructed, width, height, anisotropy, isotropy_type, regularization,
                       variance_threshold, current_radius, is_last_step, ABCD, strength, (data->first != 0.f),
                       (data->second != 0.f), (data->third != 0.f), (data->fourth != 0.f),
                       (data->regularization != 0.f || data->variance_threshold != 0.f || TRUE));
  }

  if(HF) dt_free_align(HF);
  if(LF_even) dt_free_align(LF_even);
  if(LF_odd) dt_free_align(LF_odd);
  return success;
}


static inline void build_mask(const float *const restrict input, uint8_t *const restrict mask,
                              const float threshold, const size_t width, const size_t height)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(input, mask, height, width, threshold)        \
    schedule(simd:static) aligned(mask, input : 64)
#endif
  for(size_t k = 0; k < height * width * 4; k += 4)
  {
    // TRUE if any channel is above threshold
    mask[k / 4] = (input[k] > threshold || input[k + 1] > threshold || input[k + 2] > threshold);
  }
}

static inline void inpaint_mask(float *const restrict inpainted, const float *const restrict original,
                                const uint8_t *const restrict mask, const float noise,
                                const size_t width, const size_t height)
{
  // init the reconstruction with noise inside the masked areas
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(inpainted, original, mask, width, height, noise) schedule(simd:static)
#endif
  for(size_t k = 0; k < height * width * 4; k += 4)
  {
    if(mask[k / 4])
    {
      const uint32_t i = k / width;
      const uint32_t j = k - i;
      uint32_t DT_ALIGNED_ARRAY state[4]
          = { splitmix32(j + 1), splitmix32((j + 1) * (i + 3)),
              splitmix32(1337), splitmix32(666) };
      xoshiro128plus(state);
      xoshiro128plus(state);
      xoshiro128plus(state);
      xoshiro128plus(state);

      for_four_channels(c, aligned(inpainted, state:64)) inpainted[k + c] = fmaxf(gaussian_noise(1.f, noise, i % 2 || j % 2, state), 0.f);
    }
    else
    {
      for_four_channels(c, aligned(original, inpainted:64)) inpainted[k + c] = original[k + c];
    }
  }
}

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const restrict ivoid,
             void *const restrict ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_diffuse_data_t *const data = (dt_iop_diffuse_data_t *)piece->data;

  float *restrict in = DT_IS_ALIGNED((float *const restrict)ivoid);
  float *const restrict out = DT_IS_ALIGNED((float *const restrict)ovoid);

  float *const restrict temp1 = dt_alloc_align_float(roi_out->width * roi_out->height * 4);
  float *const restrict temp2 = dt_alloc_align_float(roi_out->width * roi_out->height * 4);

  float *restrict temp_in = NULL;
  float *restrict temp_out = NULL;

  uint8_t *restrict mask = NULL;

  const float scale = fmaxf(piece->iscale / roi_in->scale, 1.f);
  const float final_radius = data->radius * 2.f / scale;

  const int iterations = MAX(ceilf((float)data->iterations / scale), 1);
  const int diffusion_scales = num_steps_to_reach_equivalent_sigma(B_SPLINE_SIGMA, final_radius);
  const int scales = CLAMP(diffusion_scales, 1, MAX_NUM_SCALES);

  if(data->threshold > 0.f)
  {
    // build a boolean mask, TRUE where image is above threshold, FALSE otherwise
    mask = dt_alloc_align(64, roi_out->width * roi_out->height * sizeof(float));
    build_mask(in, mask, data->threshold, roi_out->width, roi_out->height);

    // init the inpainting area with noise
    inpaint_mask(temp1, in, mask, 0.2f, roi_out->width, roi_out->height);

    in = temp1;
  }

  for(int it = 0; it < iterations; it++)
  {
    if(it == 0)
    {
      temp_in = in;
      temp_out = temp2;
    }
    else if(it % 2 == 0)
    {
      temp_in = temp1;
      temp_out = temp2;
    }
    else
    {
      temp_in = temp2;
      temp_out = temp1;
    }

    if(it == (int)iterations - 1) temp_out = out;
    wavelets_process(temp_in, temp_out, mask, roi_out->width, roi_out->height, data, final_radius, scale, scales);
  }

  if(mask) dt_free_align(mask);
  if(temp1) dt_free_align(temp1);
  if(temp2) dt_free_align(temp2);
}

#if FALSE // HAVE_OPENCL
static inline cl_int wavelets_process_cl(cl_mem in, cl_mem reconstructed, cl_mem mask, const size_t ch,
                                          const dt_iop_diffuse_data_t *const data, dt_dev_pixelpipe_iop_t *piece,
                                          const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  gint success = TRUE;

  const float zoom = fmaxf(piece->iscale / roi_in->scale, 1.0f);
  const float final_radius = data->radius * 3.0f / zoom;
  const int diffusion_scales = num_steps_to_reach_equivalent_sigma(B_SPLINE_SIGMA, final_radius);
  const int scales = CLAMP(diffusion_scales, 1, MAX_NUM_SCALES);
  fprintf(stdout, "scales : %i\n", scales);

  /* TODO */

  dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_diffuse, 3, sizeof(int), (void *)&height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_diffuse, sizes);

  return success;
}

int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_diffuse_data_t *const data = (dt_iop_diffuse_data_t *)piece->data;
  dt_iop_diffuse_global_data_t *const gd = (dt_iop_diffuse_global_data_t *)self->global_data;

  cl_int err = -999;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

  cl_mem temp_in = NULL;
  cl_mem temp_out = NULL;
  cl_mem temp1 = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float));
  cl_mem temp2 = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float));
  if(!temp1 || !temp2) goto error;

  const float scale = fmaxf(piece->iscale / roi_in->scale, 1.f);
  const int iterations = CLAMP(ceilf((float)data->iterations / scale), 1, MAX_NUM_SCALES);

  for(int it = 0; it < iterations; it++)
  {
    if(it == 0)
    {
      temp_in = in;
      temp_out = temp2;
    }
    else if(it % 2 == 0)
    {
      temp_in = temp1;
      temp_out = temp2;
    }
    else
    {
      temp_in = temp2;
      temp_out = temp1;
    }

    if(it == (int)iterations - 1) temp_out = out;
    err = wavelets_process_cl(temp_in, temp_out, mask, ch, data, piece, roi_in, roi_out);
  }


  if(err != CL_SUCCESS) goto error;

  // cleanup and exit on success
  if(temp1) dt_opencl_release_mem_object(temp1);
  if(temp2) dt_opencl_release_mem_object(temp2);
  return TRUE;

error:
  if(temp1) dt_opencl_release_mem_object(temp1);
  if(temp2) dt_opencl_release_mem_object(temp2);
  dt_print(DT_DEBUG_OPENCL, "[opencl_diffuse] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 33; // extended.cl in programs.conf
  dt_iop_diffuse_global_data_t *gd = (dt_iop_diffuse_global_data_t *)malloc(sizeof(dt_iop_diffuse_global_data_t));

  module->data = gd;
  gd->kernel_diffuse = dt_opencl_create_kernel(program, "diffuse");
}


void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_diffuse_global_data_t *gd = (dt_iop_diffuse_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_diffuse);
  free(module->data);
  module->data = NULL;
}
#endif


void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_diffuse_gui_data_t *g = (dt_iop_diffuse_gui_data_t *)self->gui_data;
  dt_iop_diffuse_params_t *p = (dt_iop_diffuse_params_t *)self->params;
  dt_bauhaus_slider_set_soft(g->iterations, p->iterations);
  dt_bauhaus_slider_set_soft(g->fourth, p->fourth);
  dt_bauhaus_slider_set_soft(g->third, p->third);
  dt_bauhaus_slider_set_soft(g->second, p->second);
  dt_bauhaus_slider_set_soft(g->first, p->first);

  dt_bauhaus_slider_set_soft(g->variance_threshold, p->variance_threshold);
  dt_bauhaus_slider_set_soft(g->regularization, p->regularization);
  dt_bauhaus_slider_set_soft(g->radius, p->radius);
  dt_bauhaus_slider_set_soft(g->sharpness, p->sharpness);
  dt_bauhaus_slider_set_soft(g->threshold, p->threshold);

  dt_bauhaus_slider_set_soft(g->anisotropy_first, p->anisotropy_first);
  dt_bauhaus_slider_set_soft(g->anisotropy_second, p->anisotropy_second);
  dt_bauhaus_slider_set_soft(g->anisotropy_third, p->anisotropy_third);
  dt_bauhaus_slider_set_soft(g->anisotropy_fourth, p->anisotropy_fourth);
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_diffuse_gui_data_t *g = IOP_GUI_ALLOC(diffuse);
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("diffusion properties")), FALSE, FALSE, 0);

  g->iterations = dt_bauhaus_slider_from_params(self, "iterations");
  gtk_widget_set_tooltip_text(g->iterations,
                              _("more iterations make the effect stronger but the module slower.\n"
                                "this is analogous to giving more time to the diffusion reaction.\n"
                                "if you plan on sharpening or inpainting, more iterations help reconstruction."));

  g->radius = dt_bauhaus_slider_from_params(self, "radius");
  dt_bauhaus_slider_set_format(g->radius, "%.0f px");
  gtk_widget_set_tooltip_text(
      g->radius, _("scale of the diffusion.\n"
                   "high values diffuse farther, at the expense of computation time.\n"
                   "low values diffuse closer.\n"
                   "if you plan on denoising, the radius should be around the width of your lens blur."));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("diffusion typology")), FALSE, FALSE, 0);

  g->first = dt_bauhaus_slider_from_params(self, "first");
  dt_bauhaus_slider_set_factor(g->first, 100.0f);
  dt_bauhaus_slider_set_digits(g->first, 4);
  dt_bauhaus_slider_set_format(g->first, "%+.2f %%");
  gtk_widget_set_tooltip_text(g->first, _("smoothing or sharpening of smooth details (gradients).\n"
                                          "positive values diffuse and blur.\n"
                                          "negative values sharpen.\n"
                                          "zero does nothing."));

  g->second = dt_bauhaus_slider_from_params(self, "second");
  dt_bauhaus_slider_set_digits(g->second, 4);
  dt_bauhaus_slider_set_factor(g->second, 100.0f);
  dt_bauhaus_slider_set_format(g->second, "%+.2f %%");

  g->third = dt_bauhaus_slider_from_params(self, "third");
  dt_bauhaus_slider_set_digits(g->third, 4);
  dt_bauhaus_slider_set_factor(g->third, 100.0f);
  dt_bauhaus_slider_set_format(g->third, "%+.2f %%");
  gtk_widget_set_tooltip_text(g->third, _("smoothing or sharpening of sharp details.\n"
                                          "positive values diffuse and blur.\n"
                                          "negative values sharpen.\n"
                                          "zero does nothing."));

  g->fourth = dt_bauhaus_slider_from_params(self, "fourth");
  dt_bauhaus_slider_set_digits(g->fourth, 4);
  dt_bauhaus_slider_set_factor(g->fourth, 100.0f);
  dt_bauhaus_slider_set_format(g->fourth, "%+.2f %%");
  gtk_widget_set_tooltip_text(g->fourth, _("smoothing or sharpening of sharp details (gradients).\n"
                                           "positive values diffuse and blur.\n"
                                           "negative values sharpen.\n"
                                           "zero does nothing."));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("diffusion directionnality")), FALSE, FALSE, 0);

  g->anisotropy_first = dt_bauhaus_slider_from_params(self, "anisotropy_first");
  dt_bauhaus_slider_set_digits(g->anisotropy_first, 4);
  dt_bauhaus_slider_set_factor(g->anisotropy_first, 100.0f);
  dt_bauhaus_slider_set_format(g->anisotropy_first, "%+.2f %%");
  gtk_widget_set_tooltip_text(g->anisotropy_first,
                              _("anisotropy of the diffusion.\n"
                                "zero makes the diffusion isotrope (same in all directions)\n"
                                "positives make the diffusion follow isophotes more closely\n"
                                "negatives make the diffusion follow gradients more closely"));

  g->anisotropy_second = dt_bauhaus_slider_from_params(self, "anisotropy_second");
  dt_bauhaus_slider_set_digits(g->anisotropy_second, 4);
  dt_bauhaus_slider_set_factor(g->anisotropy_second, 100.0f);
  dt_bauhaus_slider_set_format(g->anisotropy_second, "%+.2f %%");
  gtk_widget_set_tooltip_text(g->anisotropy_second,
                              _("anisotropy of the diffusion.\n"
                                "zero makes the diffusion isotrope (same in all directions)\n"
                                "positives make the diffusion follow isophotes more closely\n"
                                "negatives make the diffusion follow gradients more closely"));

  g->anisotropy_third = dt_bauhaus_slider_from_params(self, "anisotropy_third");
  dt_bauhaus_slider_set_digits(g->anisotropy_third, 4);
  dt_bauhaus_slider_set_factor(g->anisotropy_third, 100.0f);
  dt_bauhaus_slider_set_format(g->anisotropy_third, "%+.2f %%");
  gtk_widget_set_tooltip_text(g->anisotropy_third,
                              _("anisotropy of the diffusion.\n"
                                "zero makes the diffusion isotrope (same in all directions)\n"
                                "positives make the diffusion follow isophotes more closely\n"
                                "negatives make the diffusion follow gradients more closely"));

  g->anisotropy_fourth = dt_bauhaus_slider_from_params(self, "anisotropy_fourth");
  dt_bauhaus_slider_set_digits(g->anisotropy_fourth, 4);
  dt_bauhaus_slider_set_factor(g->anisotropy_fourth, 100.0f);
  dt_bauhaus_slider_set_format(g->anisotropy_fourth, "%+.2f %%");
  gtk_widget_set_tooltip_text(g->anisotropy_fourth,
                              _("anisotropy of the diffusion.\n"
                                "zero makes the diffusion isotrope (same in all directions)\n"
                                "positives make the diffusion follow isophotes more closely\n"
                                "negatives make the diffusion follow gradients more closely"));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("edges management")), FALSE, FALSE, 0);

  g->sharpness = dt_bauhaus_slider_from_params(self, "sharpness");
  dt_bauhaus_slider_set_factor(g->sharpness, 100.0f);
  dt_bauhaus_slider_set_format(g->sharpness, "%.2f %%");
  gtk_widget_set_tooltip_text(g->sharpness,
                              _("increase or decrease the sharpness of the highest frequencies"));


  g->regularization = dt_bauhaus_slider_from_params(self, "regularization");
  g->variance_threshold = dt_bauhaus_slider_from_params(self, "variance_threshold");


  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("diffusion spatiality")), FALSE, FALSE, 0);

  g->threshold = dt_bauhaus_slider_from_params(self, "threshold");
  dt_bauhaus_slider_set_factor(g->threshold, 100.0f);
  dt_bauhaus_slider_set_format(g->threshold, "%.2f %%");
  gtk_widget_set_tooltip_text(g->threshold,
                              _("luminance threshold for the mask.\n"
                                "0. disables the luminance masking and applies the module on the whole image.\n"
                                "any higher value excludes pixels whith luminance lower than the threshold.\n"
                                "this can be used to inpaint highlights."));
}
