/* -----------------------------------------------------------------------------
 * sift.c
 * -----------------------------------------------------------------------------
 * Copyright (c) 2015 Blaine Rister et al., see LICENSE for details.
 * -----------------------------------------------------------------------------
 * This file contains all routines needed to initialize, delete, 
 * and run the SIFT3D detector and descriptor. It also contains routines for
 * matching SIFT3D features and drawing the results.
 * -----------------------------------------------------------------------------
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <assert.h>
#include <float.h>
#include <getopt.h>
#include "types.h"
#include "macros.h"
#include "imutil.h"

#include "sift.h"

/* Implementation options */
//#define SIFT3D_ORI_SOLID_ANGLE_WEIGHT // Weight bins by solid angle
//#define SIFT3D_MATCH_MAX_DIST 0.3 // Maximum distance between matching features 
//#define CUBOID_EXTREMA // Search for extrema in a cuboid region

/* Safety checks */
#if defined ICOS_HIST && !defined EIG_ORI
#pragma error("sift.c: Cannot use ICOS_HIST without EIG_ORI")
#endif

/* Default SIFT3D parameters. These may be overriden by 
 * the calling appropriate functions. */
const int first_octave_default = 0; // Starting octave index
const double peak_thresh_default = 0.03; // DoG peak threshold
const int num_kp_levels_default = 3; // Number of levels per octave in which keypoints are found
const double corner_thresh_default = 0.5; // Minimum corner score
const double sigma_n_default = 1.15; // Nominal scale of input data
const double sigma0_default = 1.6; // Scale of the base octave

/* SIFT3D option names */
const char opt_first_octave[] = "first_octave";
const char opt_peak_thresh[] = "peak_thresh";
const char opt_corner_thresh[] = "corner_thresh";
const char opt_num_octaves[] = "num_octaves";
const char opt_num_kp_levels[] = "num_kp_levels";
const char opt_sigma_n[] = "sigma_n";
const char opt_sigma0[] = "sigma0";

/* Internal parameters */
const double max_eig_ratio =  0.90;	// Maximum ratio of eigenvalue magnitudes
const double ori_grad_thresh = 1E-10;   // Minimum norm of average gradient
const double bary_eps = FLT_EPSILON * 1E1;	// Error tolerance for barycentric coordinates
const double ori_sig_fctr = 1.5;        // Ratio of window parameter to keypoint scale
const double ori_rad_fctr =  3.0; // Ratio of window radius to parameter
const double desc_sig_fctr = 7.071067812; // See ori_sig_fctr, 5 * sqrt(2)
const double desc_rad_fctr = 2.0;  // See ori_rad_fctr
const double trunc_thresh = 0.2f * 128.0f / DESC_NUMEL; // Descriptor truncation threshold

/* Internal math constants */
const double gr = 1.6180339887; // Golden ratio

/* Keypoint data format constants */
const int kp_num_cols = IM_NDIMS * (IM_NDIMS + 1) + 1; // Number of columns
const int kp_x = 0; // column of x-coordinate
const int kp_y = 1; // column of y-coordinate
const int kp_z = 2; // column of z-coordinate
const int kp_s = 3; // column of s-coordinate
const int kp_ori = 4; // first column of the orientation matrix
const int ori_numel = IM_NDIMS * IM_NDIMS; // Number of orientaiton elements

/* Internal return codes */
#define REJECT 1

/* Get the index of bin j from triangle i */
#define MESH_GET_IDX(mesh, i, j) \
	((mesh)->tri[i].idx[j])

/* Get bin j from triangle i */
#define MESH_HIST_GET(mesh, hist, i, j) \
	((hist)->bins[MESH_GET_IDX(mesh, i, j)])

/* Clamp out of bounds polar accesses to the first or last element.
 * Note that the polar histogram is NOT circular. */
#define HIST_GET_PO(hist, a, p) \
			 ((p) < 0 ? \
			 HIST_GET(hist, ((a) + NBINS_AZ / 2) % NBINS_AZ, 1) : \
			 (p) >= NBINS_PO ? \
		 	 HIST_GET(hist, ((a) + NBINS_AZ / 2) % NBINS_AZ, \
			 NBINS_PO - 1) : \
		     HIST_GET(hist, a, p))

/* Convert out of bounds azimuthal accesses circularly, e.g. -1 goes
 * to NBINS_AZ - 1, NBINS_AZ goes to 0. This algorithm does not work
 * if the indices wrap around more than once. */
#define HIST_GET_AZ(hist, a, p)	\
			 HIST_GET_PO(hist, ((a) + NBINS_AZ) % NBINS_AZ, p)

/* Loop through a spherical image region. im and [x, y, z] are defined as
 * above. vcenter is a pointer to a Cvec specifying the center of the window.
 * rad is the radius of the window. vdisp is a pointer to a Cvec storing
 * the displacement from the window center. sqdisp is a float storing the
 * squared Euclidean distance from the window center.
 * 
 * Delimit with SIFT3D_IM_LOOP_END. */
#define IM_LOOP_SPHERE_START(im, x, y, z, vcenter, rad, vdisp, sq_dist) \
	const int x_start = (int) SIFT3D_MAX((int) (vcenter)->x - (int) ((rad) + 0.5), 1); \
	const int x_end   = (int) SIFT3D_MIN((int) (vcenter)->x + (int) ((rad) + 0.5), im->nx - 2); \
	const int y_start = (int) SIFT3D_MAX((int) (vcenter)->y - (int) ((rad) + 0.5), 1); \
	const int y_end   = (int) SIFT3D_MIN((int) (vcenter)->y + (int) ((rad) + 0.5), im->ny - 2); \
	const int z_start = (int) SIFT3D_MAX((int) (vcenter)->z - (int) ((rad) + 0.5), 1); \
	const int z_end   = (int) SIFT3D_MIN((int) (vcenter)->z + (int) ((rad) + 0.5), im->nz - 2); \
	SIFT3D_IM_LOOP_LIMITED_START(im, x, y, z, x_start, x_end, y_start, y_end, \
			      z_start, z_end) \
	    (vdisp)->x = ((float) x + 0.5f) - (vcenter)->x; \
	    (vdisp)->y = ((float) y + 0.5f) - (vcenter)->y; \
	    (vdisp)->z = ((float) z + 0.5f) - (vcenter)->z; \
	    (sq_dist) = SIFT3D_CVEC_L2_NORM_SQ(vdisp); \
	    if ((sq_dist) > (rad) * (rad)) \
		continue; \

// Loop over all bins in a gradient histogram. If ICOS_HIST is defined, p
// is not referenced
#ifdef ICOS_HIST
#define HIST_LOOP_START(a, p) \
	for ((a) = 0; (a) < HIST_NUMEL; (a)++) { p = p; {
#else
#define HIST_LOOP_START(a, p) \
	for ((p) = 0; (p) < NBINS_PO; (p)++) { \
	for ((a) = 0; (a) < NBINS_AZ; (a)++) {
#endif

// Delimit a HIST_LOOP
#define HIST_LOOP_END }}

// Get an element from a gradient histogram. If ICOS_HIST is defined, p
// is not referenced
#ifdef ICOS_HIST
#define HIST_GET_IDX(a, p) (a)
#else
#define HIST_GET_IDX(a, p) ((a) + (p) * NBINS_AZ)
#endif
#define HIST_GET(hist, a, p) ((hist)->bins[HIST_GET_IDX(a, p)])

/* Global variables */
extern CL_data cl_data;

/* Helper routines */
static int init_geometry(SIFT3D *sift3d);
static int set_im_SIFT3D(SIFT3D *const sift3d, const Image *const im);
static int resize_SIFT3D(SIFT3D *const sift3d);
static int build_gpyr(SIFT3D *sift3d);
static int build_dog(SIFT3D *dog);
static int detect_extrema(SIFT3D *sift3d, Keypoint_store *kp);
static int refine_keypoints(SIFT3D *sift3d, Keypoint_store *kp);
static int assign_eig_ori(SIFT3D *const sift3d, const Image *const im, 
                          const Cvec *const vcenter,
                          const double sigma, Mat_rm *const R);
static int assign_orientations(SIFT3D *sift3d, Keypoint_store *kp);
static int Cvec_to_sbins(const Cvec * const vd, Svec * const bins);
static void refine_Hist(Hist *hist);
static int init_cl_SIFT3D(SIFT3D *sift3d);
static int cart2bary(const Cvec * const cart, const Tri * const tri, 
		      Cvec * const bary, float * const k);
static void SIFT3D_desc_acc_interp(const SIFT3D * const sift3d, 
				   const Cvec * const vbins, 
				   const Cvec * const grad,
				   SIFT3D_Descriptor * const desc);
static void extract_descrip(SIFT3D *const sift3d, const Image *const im,
	   const Keypoint *const key, SIFT3D_Descriptor *const desc);
static int argv_remove(const int argc, char **argv, 
                        const unsigned char *processed);
static int extract_dense_descriptors_no_rotate(SIFT3D *const sift3d,
        const Image *const in, Image *const desc);
static int extract_dense_descriptors_rotate(SIFT3D *const sift3d,
        const Image *const in, Image *const desc);
static void extract_dense_descrip_rotate(SIFT3D *const sift3d, 
           const Image *const im, const Cvec *const vcenter, 
           const double sigma, const Mat_rm *const R, Hist *const hist);
static int vox2hist(const Image *const im, const int x, const int y,
        const int z, Hist *const hist);
static int hist2vox(Hist *const hist, const Image *const im, const int x, 
        const int y, const int z);

/* Initialize geometry tables. */
static int init_geometry(SIFT3D *sift3d) {

	Mat_rm V, F;
	Cvec temp1, temp2, temp3, n;
	float mag;
	int i, j;

	Mesh * const mesh = &sift3d->mesh;

	/* Verices of a regular icosahedron inscribed in the unit sphere. */
	const float vert[] = {  0,  1,  gr,
			        0, -1,  gr,
			        0,  1, -gr,
			        0, -1, -gr,
			        1,  gr,  0,
			       -1,  gr,  0,
			        1, -gr,  0,
			       -1, -gr,  0,
			       gr,   0,  1,
			      -gr,   0,  1,
			       gr,   0, -1, 
			      -gr,   0, -1 }; 

	/* Vertex triplets forming the faces of the icosahedron. */
	const float faces[] = {0, 1, 8,
    			       0, 8, 4,
    			       0, 4, 5,
    			       0, 5, 9,
    			       0, 9, 1,
    			       1, 6, 8,
			       8, 6, 10,
			       8, 10, 4,
			       4, 10, 2,
			       4, 2, 5,
			       5, 2, 11,
			       5, 11, 9,
			       9, 11, 7,
			       9, 7, 1,
			       1, 7, 6,
			       3, 6, 7,
			       3, 7, 11,
			       3, 11, 2,
			       3, 2, 10,
			       3, 10, 6};

	// Initialize matrices
	if (init_Mat_rm_p(&V, vert, ICOS_NVERT, 3, FLOAT, SIFT3D_FALSE) ||
	    init_Mat_rm_p(&F, faces, ICOS_NFACES, 3, FLOAT, SIFT3D_FALSE))
		return SIFT3D_FAILURE;
			    
	// Initialize triangle memory
        init_Mesh(mesh);
	if ((mesh->tri = (Tri *) malloc(ICOS_NFACES * sizeof(Tri))) == NULL)
		return SIFT3D_FAILURE;
 
	// Populate the triangle struct for each face
	for (i = 0; i < ICOS_NFACES; i++) {

		Tri * const tri = mesh->tri + i;	
		Cvec * const v = tri->v;

		// Initialize the vertices
		for (j = 0; j < 3; j++) {

			const float mag_expected = sqrt(1 + gr * gr);
			int * const idx = tri->idx + j;

			*idx = SIFT3D_MAT_RM_GET(&F, i, j, float);

			// Initialize the vector
			v[j].x = SIFT3D_MAT_RM_GET(&V, *idx, 0, float);
			v[j].y = SIFT3D_MAT_RM_GET(&V, *idx, 1, float);
			v[j].z = SIFT3D_MAT_RM_GET(&V, *idx, 2, float);

			// Normalize to unit length
			mag = SIFT3D_CVEC_L2_NORM(v + j);
			assert(fabsf(mag - mag_expected) < 1E-10);
			SIFT3D_CVEC_SCALE(v + j, 1.0f / mag);
		}

		// Compute the normal vector at v[0] as  (V2 - V1) X (V1 - V0)
		SIFT3D_CVEC_OP(v + 2, v + 1, -, &temp1);
		SIFT3D_CVEC_OP(v + 1, v, -, &temp2);
		SIFT3D_CVEC_CROSS(&temp1, &temp2, &n);

		// Ensure this vector is facing outward from the origin
		if (SIFT3D_CVEC_DOT(&n, v) < 0) {
			// Swap two vertices
			temp1 = v[0];
			v[0] = v[1];
			v[1] = temp1;

			// Compute the normal again
			SIFT3D_CVEC_OP(v + 2, v + 1, -, &temp1);
			SIFT3D_CVEC_OP(v + 1, v, -, &temp2);
			SIFT3D_CVEC_CROSS(&temp1, &temp2, &n);
		}
		assert(SIFT3D_CVEC_DOT(&n, v) >= 0);

		// Ensure the triangle is equilateral
		SIFT3D_CVEC_OP(v + 2, v, -, &temp3);
		assert(fabsf(SIFT3D_CVEC_L2_NORM(&temp1) - 
                        SIFT3D_CVEC_L2_NORM(&temp2)) < 1E-10);
		assert(fabsf(SIFT3D_CVEC_L2_NORM(&temp1) - 
                        SIFT3D_CVEC_L2_NORM(&temp3)) < 1E-10);
	}	
	
	return SIFT3D_SUCCESS;
}

/* Convert Cartesian coordinates to barycentric. bary is set to all zeros if
 * the problem is unstable. 
 *
 * The output value k is the constant by which the ray is multiplied to
 * intersect the supporting plane of the triangle.
 *
 * This code uses the Moller-Trumbore algorithm. */
static int cart2bary(const Cvec * const cart, const Tri * const tri, 
		      Cvec * const bary, float * const k) {

	Cvec e1, e2, t, p, q;
	float det, det_inv;

	const Cvec * const v = tri->v;

	SIFT3D_CVEC_OP(v + 1, v, -, &e1);
	SIFT3D_CVEC_OP(v + 2, v, -, &e2);
	SIFT3D_CVEC_CROSS(cart, &e2, &p);
	det = SIFT3D_CVEC_DOT(&e1, &p);

	// Reject unstable points
	if (fabsf(det) < bary_eps) {
		return SIFT3D_FAILURE;
	}

	det_inv = 1.0f / det;

	t = v[0];
	SIFT3D_CVEC_SCALE(&t, -1.0f);	

	SIFT3D_CVEC_CROSS(&t, &e1, &q);

	bary->y = det_inv * SIFT3D_CVEC_DOT(&t, &p);	
	bary->z = det_inv * SIFT3D_CVEC_DOT(cart, &q);
	bary->x = 1.0f - bary->y - bary->z;

	*k = SIFT3D_CVEC_DOT(&e2, &q) * det_inv;

#ifndef NDEBUG
	Cvec temp1, temp2, temp3;
        double residual;

        if (isnan(bary->x) || isnan(bary->y) || isnan(bary->z)) {
                printf("cart2bary: invalid bary (%f, %f, %f)\n", bary->x, 
                        bary->y, bary->z);
                //exit(1);
        }

	// Verify k * c = bary->x * v1 + bary->y * v2 + bary->z * v3
	temp1 = v[0];
	temp2 = v[1];
	temp3 = v[2];
	SIFT3D_CVEC_SCALE(&temp1, bary->x);
	SIFT3D_CVEC_SCALE(&temp2, bary->y);	
	SIFT3D_CVEC_SCALE(&temp3, bary->z);	
	SIFT3D_CVEC_OP(&temp1, &temp2, +, &temp1);
	SIFT3D_CVEC_OP(&temp1, &temp3, +, &temp1);
	SIFT3D_CVEC_SCALE(&temp1, 1.0f / *k);
	SIFT3D_CVEC_OP(&temp1, cart, -, &temp1);
        residual = SIFT3D_CVEC_L2_NORM(&temp1);
	if (residual > bary_eps) {
                printf("cart2bary: residual: %f\n", residual);
                exit(1);
        }
#endif
	return SIFT3D_SUCCESS;
}

/* Initialize a Keypoint_store for first use.
 * This does not need to be called to reuse the store
 * for a new image. */
void init_Keypoint_store(Keypoint_store *const kp) {
	init_Slab(&kp->slab);
	kp->buf = (Keypoint *) kp->slab.buf;
}

/* Free all memory associated with a Keypoint_store. kp cannot be
 * used after calling this function, unless re-initialized. */
void cleanup_Keypoint_store(Keypoint_store *const kp) {
        cleanup_Slab(&kp->slab);
}

/* Initialize a SIFT_Descriptor_store for first use.
 * This does not need to be called to reuse the store
 * for a new image. */
void init_SIFT3D_Descriptor_store(SIFT3D_Descriptor_store *const desc) {
	desc->buf = NULL;
}

/* Free all memory associated with a SIFT3D_Descriptor_store. desc
 * cannot be used after calling this function, unless re-initialized. */
void cleanup_SIFT3D_Descriptor_store(SIFT3D_Descriptor_store *const desc) {
        free(desc->buf);
}

/* Initializes the OpenCL data for this SIFT3D struct. This
 * increments the reference counts for shared data. */
static int init_cl_SIFT3D(SIFT3D *sift3d) {
#ifdef SIFT3D_USE_OPENCL
	cl_image_format image_format;

	// Initialize basic OpenCL platform and context info
	image_format.image_channel_order = CL_R;
	image_format.image_channel_data_type = CL_FLOAT;
	if (init_cl(&cl_data, PLATFORM_NAME_NVIDIA, CL_DEVICE_TYPE_GPU,
 		    CL_MEM_READ_WRITE | CL_MEM_ALLOC_HOST_PTR, 
                    image_format))
		return SIFT3D_FAILURE;

	// Load and compile the downsampling kernel

#endif
	return SIFT3D_SUCCESS;
}

/* Sets the first octave, resizing the internal data. */
int set_first_octave_SIFT3D(SIFT3D *const sift3d, 
                                const int first_octave) {

	sift3d->dog.first_octave = sift3d->gpyr.first_octave = first_octave;

        return resize_SIFT3D(sift3d);
}

/* Sets the peak threshold, checking that it is in the interval (0, inf) */
int set_peak_thresh_SIFT3D(SIFT3D *const sift3d,
                                const double peak_thresh) {
        if (peak_thresh <= 0.0) {
                fprintf(stderr, "SIFT3D peak_thresh must be greater than 0."
                        " Provided: %f \n", peak_thresh);
                return SIFT3D_FAILURE;
        }

        sift3d->peak_thresh = peak_thresh;
        return SIFT3D_SUCCESS;
}

/* Sets the corner threshold, checking that it is in the interval [0, 1]. */
int set_corner_thresh_SIFT3D(SIFT3D *const sift3d,
                                const double corner_thresh) {

        if (corner_thresh < 0.0 || corner_thresh > 1.0) {
                fprintf(stderr, "SIFT3D corner_thresh must be in the interval "
                        "[0, 1]. Provided: %f \n", corner_thresh);
                return SIFT3D_FAILURE;
        }

        sift3d->corner_thresh = corner_thresh;
        return SIFT3D_SUCCESS;
}

/* Sets the number octaves to be processed. If necessary, this function will 
 * resize the internal data. */
int set_num_octaves_SIFT3D(SIFT3D *const sift3d,
                                const unsigned int num_octaves) {

	sift3d->dog.num_octaves = sift3d->gpyr.num_octaves = (int) num_octaves;

        return resize_SIFT3D(sift3d);
}

/* Sets the number of levels per octave. This function will resize the
 * internal data. */
int set_num_kp_levels_SIFT3D(SIFT3D *const sift3d,
                                const unsigned int num_kp_levels) {

	const int num_dog_levels = (int) num_kp_levels + 2;
	const int num_gpyr_levels = num_dog_levels + 1;

        // Set the new parameter
	sift3d->dog.num_kp_levels = sift3d->gpyr.num_kp_levels = 
                (int) num_kp_levels;
	sift3d->dog.num_levels = num_dog_levels;
	sift3d->gpyr.num_levels = num_gpyr_levels;

        // Resize the data
        return resize_SIFT3D(sift3d);
}

/* Sets the nominal scale parameter of the input data, checking that it is 
 * nonnegative. */
int set_sigma_n_SIFT3D(SIFT3D *const sift3d,
                                const double sigma_n) {

        if (sigma_n < 0.0) {
                fprintf(stderr, "SIFT3D sigma_n must be nonnegative. Provided: "
                        "%f \n", sigma_n);
                return SIFT3D_FAILURE;
        }

	sift3d->dog.sigma_n = sift3d->gpyr.sigma_n = sigma_n;
        return SIFT3D_SUCCESS;
}

/* Sets the scale parameter of the first level of octave 0, checking that it
 * is nonnegative. */
int set_sigma0_SIFT3D(SIFT3D *const sift3d,
                                const double sigma0) {

        if (sigma0 < 0.0) {
                fprintf(stderr, "SIFT3D sigma0 must be nonnegative. Provided: "
                        "%f \n", sigma0);
                return SIFT3D_FAILURE; 
        } 

	sift3d->dog.sigma0 = sift3d->gpyr.sigma0 = sigma0;
        return SIFT3D_SUCCESS;
}

/* Initialize a SIFT3D struct with the default parameters. */
int init_SIFT3D(SIFT3D *sift3d) {

	int num_dog_levels, num_gpyr_levels;

        Pyramid *const dog = &sift3d->dog;
        Pyramid *const gpyr = &sift3d->gpyr;
        GSS_filters *const gss = &sift3d->gss;

	// Initialize to defaults
	const int first_octave = first_octave_default;
	const double peak_thresh = peak_thresh_default;
	const double corner_thresh = corner_thresh_default;
	const int num_octaves = -1;
	const int num_kp_levels = num_kp_levels_default;
	const double sigma_n = sigma_n_default;
	const double sigma0 = sigma0_default;
        const int dense_rotate = SIFT3D_FALSE;

	// First-time pyramid initialization
        init_Pyramid(dog);
        init_Pyramid(gpyr);

        // First-time filter initialization
        init_GSS_filters(gss);

        // Intialize the geometry tables
	if (init_geometry(sift3d))
		return SIFT3D_FAILURE;

	// init static OpenCL programs and contexts, if support is enabled
	if (init_cl_SIFT3D(sift3d))
		return SIFT3D_FAILURE;

	// Initialize image to null, to mark for resizing
	sift3d->im = NULL;

	// Save data
	dog->first_level = gpyr->first_level = -1;
        set_sigma_n_SIFT3D(sift3d, sigma_n);
        set_sigma0_SIFT3D(sift3d, sigma0);
        if (set_first_octave_SIFT3D(sift3d, first_octave) ||
            set_peak_thresh_SIFT3D(sift3d, peak_thresh) ||
            set_corner_thresh_SIFT3D(sift3d, corner_thresh) ||
            set_num_octaves_SIFT3D(sift3d, num_octaves) ||
            set_num_kp_levels_SIFT3D(sift3d, num_kp_levels))
                return SIFT3D_FAILURE;
        sift3d->dense_rotate = dense_rotate;

	return SIFT3D_SUCCESS;
}

/* Make a deep copy of a SIFT3D struct, including all internal images. */
int copy_SIFT3D(const SIFT3D *const src, SIFT3D *const dst) {

        // Copy the parameters
        dst->dog.first_level = dst->gpyr.first_level = src->dog.first_level;
        set_sigma_n_SIFT3D(dst, src->gpyr.sigma_n); 
        set_sigma0_SIFT3D(dst, src->gpyr.sigma0);
        if (set_first_octave_SIFT3D(dst, src->gpyr.first_octave) ||
            set_peak_thresh_SIFT3D(dst, src->peak_thresh) ||
            set_corner_thresh_SIFT3D(dst, src->corner_thresh) ||
            set_num_octaves_SIFT3D(dst, src->gpyr.num_octaves) ||
            set_num_kp_levels_SIFT3D(dst, src->gpyr.num_kp_levels))
                return SIFT3D_FAILURE;
        dst->dense_rotate = src->dense_rotate;

        // Initialize the image pointer, pyramid dimensions, and GSS
        set_im_SIFT3D(dst, src->im);

        // Copy the pyramids, if any
        if (copy_Pyramid(&src->gpyr, &dst->gpyr) ||
            copy_Pyramid(&src->dog, &dst->dog))
                return SIFT3D_FAILURE;

        return SIFT3D_SUCCESS;
}

/* Free all memory associated with a SIFT3D struct. sift3d cannot be reused
 * unless it is reinitialized. */
void cleanup_SIFT3D(SIFT3D *const sift3d) {

        // Clean up the pyramids
        cleanup_Pyramid(&sift3d->gpyr);
        cleanup_Pyramid(&sift3d->dog);

        // Clean up the GSS filters
        cleanup_GSS_filters(&sift3d->gss);

        // Clean up the triangle mesh 
        cleanup_Mesh(&sift3d->mesh);

#ifdef USE_OPENCL
        // Clean up the OpenCL kernels
        cleanup_SIFT3D_cl_kernels(&sift3d->kernels);
#endif
}

/* Helper function to remove the processed arguments from argv. 
 * Returns the number of remaining arguments. */
static int argv_remove(const int argc, char **argv, 
                        const unsigned char *processed) {

        int i, new_pos;

        // Remove the processed arguments in-place
        new_pos = 0;
        for (i = 0; i < argc; i++) {
                // Skip processed arguments
                if (processed[i])
                        continue;

                // Add the unprocessed arguments to the new argv
                argv[new_pos++] = argv[i];                 
        }

        return new_pos;
}

/* Print the options for a SIFT3D struct to stdout. */
void print_opts_SIFT3D(void) {

        printf("SIFT3D Options: \n"
               " --%s [value] \n"
               "    The first octave of the pyramid. Must be an integer. "
               "(default: %d) \n"
               " --%s [value] \n"
               "    The smallest allowed absolute DoG value, on the interval "
               "(0, inf). (default: %.2f) \n"
               " --%s [value] \n"
               "    The smallest allowed corner score, on the interval [0, 1]."
               " (default: %.2f) \n"
               " --%s [value] \n"
               "    The number of octaves to process. Must be a positive "
               "integer. (default: process as many as we can) \n"
               " --%s [value] \n"
               "    The number of pyramid levels per octave in which "
               "keypoints are found. Must be a positive integer. "
               "(default: %d) \n"
               " --%s [value] \n"
               "    The nominal scale parameter of the input data, on the "
               "interval (0, inf). (default: %.2f) \n"
               " --%s [value] \n"
               "    The scale parameter of the first level of octave 0, on "
               "the interval (0, inf). (default: %.2f) \n",
               opt_first_octave, first_octave_default,
               opt_peak_thresh, peak_thresh_default,
               opt_corner_thresh, corner_thresh_default,
               opt_num_octaves, 
               opt_num_kp_levels, num_kp_levels_default,
               opt_sigma_n, sigma_n_default,
               opt_sigma0, sigma0_default);

}

/* Set the parameters of a SIFT3D struct from the given command line 
 * arguments. The argument SIFT3D must be initialized with
 * init_SIFT3D prior to calling this function. 
 *
 * On return, all processed SIFT3D options will be removed from argv.
 * Use argc_ret to get the number of remaining options.
 *
 * Options:
 * --first_octave	 - the first octave (int)
 * --peak_thresh	 - threshold on DoG extrema magnitude (double)
 * --corner_thresh - threshold on edge score (double)
 * --num_octaves	 - total number of octaves (default: process
 *	  		until one dimension is less than 8, use 
 *			-1 for default) (int)
 * --num_kp_levels - number of levels per octave for keypoint
 *    			candidates (int)
 * --sigma_n - base level of blurring assumed in data (double)
 * --sigma0 - level to blur base of pyramid (double)
 *
 * Parameters:
 *      argc - The number of arguments
 *      argv - An array of strings of arguments. All unproccesed arguments are
 *              permuted to the end.
 *      sift3d - The struct to be initialized
 *      check_err - If nonzero, report unrecognized options as errors
 *
 * Return value: 
 *       Returns the new number of arguments in argv, or -1 on failure. */
int parse_args_SIFT3D(SIFT3D *const sift3d,
        const int argc, char **argv, const int check_err) {

        unsigned char *processed;
        double dval;
        int c, err, ival, argc_new;

#define FIRST_OCTAVE 'a'
#define PEAK_THRESH 'b'
#define CORNER_THRESH 'c'
#define NUM_OCTAVES 'd'
#define NUM_KP_LEVELS 'e'
#define SIGMA_N 'f'
#define SIGMA0 'g'

        // Options
        const struct option longopts[] = {
                {opt_first_octave, required_argument, NULL, FIRST_OCTAVE},
                {opt_peak_thresh, required_argument, NULL, PEAK_THRESH},
                {opt_corner_thresh, required_argument, NULL, CORNER_THRESH},
                {opt_num_octaves, required_argument, NULL, NUM_OCTAVES},
                {opt_num_kp_levels, required_argument, NULL, NUM_KP_LEVELS},
                {opt_sigma_n, required_argument, NULL, SIGMA_N},
                {opt_sigma0, required_argument, NULL, SIGMA0},
                {0, 0, 0, 0}
        };

        // Starting getopt variables 
        const int opterr_start = opterr;

        // Set the error checking behavior
        opterr = check_err;

        // Intialize intermediate data
        if ((processed = calloc(argc, sizeof(char *))) == NULL) {
                fprintf(stderr, "parse_args_SIFT3D: out of memory \n");
                return -1;
        }
        err = SIFT3D_FALSE;

        // Process the arguments
        while ((c = getopt_long(argc, argv, "-", longopts, NULL)) != -1) {

                const int idx = optind - 1;

                // Convert the value to double and integer
                if (optarg != NULL) {
                        dval = atof(optarg);
                        ival = atoi(optarg);
                }

                switch (c) {
                        case FIRST_OCTAVE:
                                if (set_first_octave_SIFT3D(sift3d, 
                                        ival))
                                        goto parse_args_quit;

                                processed[idx - 1] = SIFT3D_TRUE;
                                processed[idx] = SIFT3D_TRUE;
                                break;
                        case PEAK_THRESH:
                                if (set_peak_thresh_SIFT3D(sift3d, 
                                        dval))
                                        goto parse_args_quit;

                                processed[idx - 1] = SIFT3D_TRUE;
                                processed[idx] = SIFT3D_TRUE;
                                break;
                        case CORNER_THRESH:
                                if (set_corner_thresh_SIFT3D(sift3d, 
                                        dval))
                                        goto parse_args_quit;

                                processed[idx - 1] = SIFT3D_TRUE;
                                processed[idx] = SIFT3D_TRUE;
                                break;
                        case NUM_OCTAVES:
                                // Check for errors
                                if (ival <= 0) {
                                        fprintf(stderr, "SIFT3D num_octaves "
                                                "must be positive. Provided: "
                                                "%d \n", ival);
                                        goto parse_args_quit;
                                }

                                if (set_num_octaves_SIFT3D(sift3d, 
                                        ival))
                                        goto parse_args_quit;

                                processed[idx - 1] = SIFT3D_TRUE;
                                processed[idx] = SIFT3D_TRUE;
                                break;
                        case NUM_KP_LEVELS:
                                // Check for errors                        
                                if (ival <= 0) {
                                        fprintf(stderr, "SIFT3D num_kp_levels "
                                                "must be positive. Provided: "
                                                "%d \n", ival);
                                        goto parse_args_quit;
                                }

                                if (set_num_kp_levels_SIFT3D(sift3d, 
                                        ival))
                                        goto parse_args_quit;

                                processed[idx - 1] = SIFT3D_TRUE;
                                processed[idx] = SIFT3D_TRUE;
                                break;
                        case SIGMA_N:
                                set_sigma_n_SIFT3D(sift3d, dval);
                                processed[idx - 1] = SIFT3D_TRUE;
                                processed[idx] = SIFT3D_TRUE;
                                break;
                        case SIGMA0:
                                set_sigma0_SIFT3D(sift3d, dval);
                                processed[idx - 1] = SIFT3D_TRUE;
                                processed[idx] = SIFT3D_TRUE;
                                break;
                        case '?':
                        default:
                                if (!check_err)
                                        continue;
                                err = SIFT3D_TRUE;
                }
        }

#undef FIRST_OCTAVE
#undef PEAK_THRESH
#undef CORNER_THRESH
#undef NUM_OCTAVES
#undef NUM_KP_LEVELS
#undef SIGMA_N
#undef SIGMA0

        // Put all unprocessed options at the end
        argc_new = argv_remove(argc, argv, processed);

        // Return to the default settings
        opterr = opterr_start;

        // Clean up
        free(processed);

        // Return an error, if error checking is enabled
        if (check_err && err)
                return -1;
        
        // Reset the state of getopt
        optind = 0;

        return argc_new;

parse_args_quit:
        free(processed);
        return -1;
}

/* Helper routine to begin processing a new image. If the dimensions differ
 * from the last one, this function resizes the SIFT3D struct. */
static int set_im_SIFT3D(SIFT3D *const sift3d, const Image *const im) {


	Image const *im_old = sift3d->im;

        // Initialize the image
        sift3d->im = (Image *const) im;

        // Resize the internal data, if necessary
        if ((im_old == NULL || im->nx != im_old->nx || im->ny != im_old->ny || 
            im->nz != im_old->nz) && resize_SIFT3D(sift3d))
                return SIFT3D_FAILURE;

        return SIFT3D_SUCCESS;
}

/* Resize a SIFT3D struct, allocating temporary storage and recompiling the 
 * filters. Does nothing unless set_im_SIFT3D was previously called. */
static int resize_SIFT3D(SIFT3D *const sift3d) {

	int last_octave, num_octaves; 

        const Image *const im = sift3d->im;
	const int first_octave = sift3d->gpyr.first_octave;

        // Do nothing if we have no image
        if (im == NULL)
                return SIFT3D_SUCCESS;

	// Compute the number of octaves, if not specified by user
	if ((num_octaves = sift3d->gpyr.num_octaves) == -1) {
		last_octave = (int) log2((double) SIFT3D_MIN(
                        // The minimum size is 8 in any dimension
                        SIFT3D_MIN(im->nx, im->ny), im->nz)) - 3 - first_octave;

		num_octaves = last_octave - first_octave + 1;
	} else {
		// Number of octaves specified by user: compute last octave
		last_octave = num_octaves + first_octave - 1;
	}

	// Update pyramid data
	sift3d->gpyr.last_octave = sift3d->dog.last_octave = last_octave;
	sift3d->dog.num_octaves = sift3d->gpyr.num_octaves = num_octaves;

	// Resize the pyramid
	if (resize_Pyramid(&sift3d->gpyr, sift3d->im) ||
		resize_Pyramid(&sift3d->dog, sift3d->im))
		return SIFT3D_FAILURE;

	// Compute the Gaussian filters
	if (make_gss(&sift3d->gss, &sift3d->gpyr))
		return SIFT3D_FAILURE;

	return SIFT3D_SUCCESS;
}

/* Build the GSS pyramid on a single CPU thread */
static int build_gpyr(SIFT3D *sift3d) {

	Sep_FIR_filter *f;
	Image *cur, *prev;
	int o, s;

	Pyramid *const gpyr = &sift3d->gpyr;
	const GSS_filters *const gss = &sift3d->gss;
	const int s_start = gpyr->first_level + 1;
	const int s_end = gpyr->last_level;
	const int o_start = gpyr->first_octave;
	const int o_end = gpyr->last_octave;

	// Build the first image
	cur = SIFT3D_PYR_IM_GET(gpyr, o_start, s_start - 1);
	prev = sift3d->im;
#ifdef SIFT3D_USE_OPENCL
	if (im_load_cl(cur, SIFT3D_FALSE))
		return SIFT3D_FAILURE;	
#endif

	f = (Sep_FIR_filter *) &gss->first_gauss.f;
	if (apply_Sep_FIR_filter(prev, cur, f))
		return SIFT3D_FAILURE;

	// Build the rest of the pyramid
	SIFT3D_PYR_LOOP_LIMITED_START(o, s, o_start, o_end, s_start, s_end)
			cur = SIFT3D_PYR_IM_GET(gpyr, o, s);
			prev = SIFT3D_PYR_IM_GET(gpyr, o, s - 1);
			f = &gss->gauss_octave[s].f;
			if (apply_Sep_FIR_filter(prev, cur, f))
				return SIFT3D_FAILURE;
#ifdef SIFT3D_USE_OPENCL
			if (im_read_back(cur, SIFT3D_FALSE))
				return SIFT3D_FAILURE;
#endif
		SIFT3D_PYR_LOOP_SCALE_END
		// Downsample
		if (o != o_end) {
			prev = cur;
			cur = SIFT3D_PYR_IM_GET(gpyr, o + 1, s_start - 1);
			if (im_downsample_2x(prev, cur))
				return SIFT3D_FAILURE;
		}
	SIFT3D_PYR_LOOP_OCTAVE_END

#ifdef SIFT3D_USE_OPENCL
	clFinish_all();
#endif

	return SIFT3D_SUCCESS;
}

static int build_dog(SIFT3D *sift3d) {

	Image *gpyr_cur, *gpyr_next, *dog_level;
	int o, s;

	Pyramid *const dog = &sift3d->dog;
	Pyramid *const gpyr = &sift3d->gpyr;

	SIFT3D_PYR_LOOP_START(dog, o, s)
		gpyr_cur = SIFT3D_PYR_IM_GET(gpyr, o, s);
		gpyr_next = SIFT3D_PYR_IM_GET(gpyr, o, s + 1);			
		dog_level = SIFT3D_PYR_IM_GET(dog, o, s);
		
		if (im_subtract(gpyr_cur, gpyr_next, 
						dog_level))
			return SIFT3D_FAILURE;
	SIFT3D_PYR_LOOP_END

	return SIFT3D_SUCCESS;
}

/* Detect local extrema */
static int detect_extrema(SIFT3D *sift3d, Keypoint_store *kp) {

	Image *cur, *prev, *next;
	Keypoint *key;
	float pcur, dogmax, peak_thresh;
	int o, s, x, y, z, x_start, x_end, y_start, y_end, z_start,
		z_end, num;

	const Pyramid *const dog = &sift3d->dog;
	const int o_start = dog->first_octave;
	const int o_end = dog->last_octave;
	const int s_start = dog->first_level + 1;
	const int s_end = dog->last_level - 1;

	// Verify the inputs
	if (dog->num_levels < 3) {
		printf("detect_extrema: Requires at least 3 levels per octave, "
			   "proivded only %d", dog->num_levels);
		return SIFT3D_FAILURE;
	}

	// Initialize dimensions of keypoint store
	cur = SIFT3D_PYR_IM_GET(dog, o_start, s_start);
	kp->nx = cur->nx;
	kp->ny = cur->ny;
	kp->nz = cur->nz;

#ifdef CUBOID_EXTREMA
#define CMP_NEIGHBORS(im, x, y, z, CMP, IGNORESELF, val) ( \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x),     (y),     (z) - 1, 0) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x) - 1, (y),     (z) - 1, 0) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x) + 1, (y),     (z) - 1, 0) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x),     (y) - 1, (z) - 1, 0) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x),     (y) + 1, (z) - 1, 0) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x) - 1, (y) - 1, (z) - 1, 0) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x) + 1, (y) - 1, (z) - 1, 0) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x) - 1, (y) + 1, (z) - 1, 0) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x) + 1, (y) + 1, (z) - 1, 0) && \
	((val) CMP SIFT3D_IM_GET_VOX( (im), (x),     (y),    (z), 0   ) || \
	    IGNORESELF) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x) - 1, (y),     (z), 0    ) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x) + 1, (y),     (z), 0    ) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x),     (y) - 1, (z), 0    ) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x),     (y) + 1, (z), 0    ) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x) - 1, (y) - 1, (z), 0    ) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x) + 1, (y) - 1, (z), 0    ) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x) - 1, (y) + 1, (z), 0    ) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x) + 1, (y) + 1, (z), 0    ) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x),     (y),     (z) + 1, 0) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x) - 1, (y),     (z) + 1, 0) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x) + 1, (y),     (z) + 1, 0) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x),     (y) - 1, (z) + 1, 0) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x),     (y) + 1, (z) + 1, 0) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x) - 1, (y) - 1, (z) + 1, 0) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x) + 1, (y) - 1, (z) + 1, 0) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x) - 1, (y) + 1, (z) + 1, 0) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x) + 1, (y) + 1, (z) + 1, 0) )
#else
#define CMP_NEIGHBORS(im, x, y, z, CMP, IGNORESELF, val) ( \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x) + 1, (y),     (z), 0) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x) - 1, (y),     (z), 0) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x),     (y) + 1, (z), 0) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x),     (y) - 1, (z), 0) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x),     (y),     (z) - 1, 0) && \
	(val) CMP SIFT3D_IM_GET_VOX( (im), (x),     (y),     (z) + 1, 0) && \
	((val) CMP SIFT3D_IM_GET_VOX( (im), (x),     (y),    (z), 0) || \
	    IGNORESELF) )
#endif

	num = 0;
	SIFT3D_PYR_LOOP_LIMITED_START(o, s, o_start, o_end, s_start, s_end)  

		// Select current and neighboring levels
		prev = SIFT3D_PYR_IM_GET(dog, o, s - 1);
		cur = SIFT3D_PYR_IM_GET(dog, o, s);
		next = SIFT3D_PYR_IM_GET(dog, o, s + 1);

		// Find maximum DoG value at this level
		dogmax = 0.0f;
		SIFT3D_IM_LOOP_START(cur, x, y, z)
			dogmax = SIFT3D_MAX(dogmax, 
                                fabsf(SIFT3D_IM_GET_VOX(cur, x, y, z, 0)));
		SIFT3D_IM_LOOP_END
		// Adjust threshold
		peak_thresh = sift3d->peak_thresh * dogmax;

		// Loop through all non-boundary pixels
		x_start = y_start = z_start = 1;
		x_end = cur->nx - 2;
		y_end = cur->ny - 2;
		z_end = cur->nz - 2;
		SIFT3D_IM_LOOP_LIMITED_START(cur, x, y, z, x_start, x_end, y_start,
							  y_end, z_start, z_end)
			// Sample the center value
			pcur = SIFT3D_IM_GET_VOX(cur, x, y, z, 0);

			// Apply the peak threshold
			if ((pcur > peak_thresh || pcur < -peak_thresh) && ((
				// Compare to the neighbors
				CMP_NEIGHBORS(prev, x, y, z, >, SIFT3D_FALSE, pcur) &&
				CMP_NEIGHBORS(cur, x, y, z, >, SIFT3D_TRUE, pcur) &&
				CMP_NEIGHBORS(next, x, y, z, >, SIFT3D_FALSE, pcur)
				) || (
				CMP_NEIGHBORS(prev, x, y, z, <, SIFT3D_FALSE, pcur) &&
				CMP_NEIGHBORS(cur, x, y, z, <, SIFT3D_TRUE, pcur) &&
				CMP_NEIGHBORS(next, x, y, z, <, SIFT3D_FALSE, pcur))))
				{
					// Add a keypoint candidate
					num++;
					SIFT3D_RESIZE_KP_STORE(kp, num, 
                                                sizeof(Keypoint));
					key = kp->buf + num - 1;
					key->o = o;
					key->s = s;
					key->xi = x;
					key->yi = y;
					key->zi = z;

				}
		SIFT3D_IM_LOOP_END
	SIFT3D_PYR_LOOP_END
#undef CMP_NEIGHBORS

	return SIFT3D_SUCCESS;
}

/* Refine keypoint locations to sub-pixel accuracy. */
static int refine_keypoints(SIFT3D *sift3d, Keypoint_store *kp) {

	Mat_rm B, Hi, Hs, X;
SIFT3D_IGNORE_UNUSED
	Cvec vd;
	double xd, yd, zd, sd;
SIFT3D_IGNORE_UNUSED
	int x, y, z, c, xnew, ynew, znew, i, j, k, l;

	// Initialize data 
	init_Mat_rm(&B, 4, 1, DOUBLE, SIFT3D_FALSE);
	init_Mat_rm(&X, 4, 1, DOUBLE, SIFT3D_FALSE);
	init_Mat_rm(&Hi, 3, 3, DOUBLE, SIFT3D_FALSE);
	init_Mat_rm(&Hs, 4, 4, DOUBLE, SIFT3D_FALSE);

	for (k = 0; k < kp->slab.num; k++) {

		// Misc. constant data
		Keypoint *const key = kp->buf + k;
		const int o = key->o;
		const int s = key->s;
		const Image *const prev = 
                        SIFT3D_PYR_IM_GET(&sift3d->dog, o, s - 1);
		const Image *const cur = 
                        SIFT3D_PYR_IM_GET(&sift3d->dog, o, s);
		const Image *const next = 
                        SIFT3D_PYR_IM_GET(&sift3d->dog, o, s + 1);

		// Bound the translation to all non-boundary pixels
		const double xmin = 1;
		const double ymin = 1;
		const double zmin = 1;
		const double xmax = cur->nx - 2 - DBL_EPSILON;
		const double ymax = cur->ny - 2 - DBL_EPSILON;
		const double zmax = cur->nz - 2 - DBL_EPSILON;
	    
		// Bound the scale to that of the neighboring levels
		const double smin = prev->s;
		const double smax = next->s;
	
		// Initialize mutable data	
		x = key->xi;
		y = key->yi;
		z = key->zi;
		xd = (double) x + 0.5;
		yd = (double) y + 0.5;
		zd = (double) z + 0.5;
		sd = cur->s; 

		// Refine the keypoint for a fixed number of iterations
		for (l = 0; l < 5; l++) {

		assert(x >= 1 && y >= 1 && z >= 1 && x <= cur->nx - 2 &&
		       y <= cur->ny -2 && z <= cur->nz - 2); 

#define PARABOLA
#ifndef PARABOLA 
		// Form the gradient
		SIFT3D_IM_GET_GRAD(cur, x, y, z, 0, &vd);

		// Form the response vector as the negative gradient
		SIFT3D_MAT_RM_GET(&B, 0, 0, double) = (double) -vd.x;
		SIFT3D_MAT_RM_GET(&B, 1, 0, double) = (double) -vd.y;
		SIFT3D_MAT_RM_GET(&B, 2, 0, double) = (double) -vd.z;
		SIFT3D_MAT_RM_GET(&B, 3, 0, double) = 
		   (double) - 0.5 * (SIFT3D_IM_GET_VOX(next, x, y, z, 0) - 
			      SIFT3D_IM_GET_VOX(prev, x, y, z, 0));

		// Form the Hessian
		SIFT3D_IM_GET_HESSIAN(cur, x, y, z, c, 0, &Hi, double);
		SIFT3D_MAT_RM_LOOP_START(&Hi, i, j)
			SIFT3D_MAT_RM_GET(&Hs, i, j, double) = 
				SIFT3D_MAT_RM_GET(&Hi, i, j, double);
		SIFT3D_MAT_RM_LOOP_END

		// Dsx
		SIFT3D_MAT_RM_GET(&Hs, 0, 3, double) = 
		SIFT3D_MAT_RM_GET(&Hs, 3, 0, double) =
			(double) 0.25 * (SIFT3D_IM_GET_VOX(next, x + 1, y, z, 0) -
			 SIFT3D_IM_GET_VOX(prev, x + 1, y, z, 0) + 
			 SIFT3D_IM_GET_VOX(prev, x - 1, y, z, 0) - 
			 SIFT3D_IM_GET_VOX(next, x - 1, y, z, 0)); 

		// Dsy 
		SIFT3D_MAT_RM_GET(&Hs, 1, 3, double) = 
		SIFT3D_MAT_RM_GET(&Hs, 3, 1, double) = 
			(double) 0.25 * (SIFT3D_IM_GET_VOX(next, x, y + 1, z, 0) -
			 SIFT3D_IM_GET_VOX(prev, x, y + 1, z, 0) + 
			 SIFT3D_IM_GET_VOX(prev, x, y - 1, z, 0) - 
			 SIFT3D_IM_GET_VOX(next, x, y - 1, z, 0)); 

		// Dsz 
		SIFT3D_MAT_RM_GET(&Hs, 2, 3, double) = 
		SIFT3D_MAT_RM_GET(&Hs, 3, 2, double) = 
			(double) 0.25 * (SIFT3D_IM_GET_VOX(next, x, y, z + 1) -
			 SIFT3D_IM_GET_VOX(prev, x, y, z + 1) + 
			 SIFT3D_IM_GET_VOX(prev, x, y, z - 1) - 
			 SIFT3D_IM_GET_VOX(next, x, y, z - 1)); 

		// Dss  
		SIFT3D_MAT_RM_GET(&Hs, 3, 3, double) = 
			(double) 0.25 * (SIFT3D_IM_GET_VOX(next, x, y, z, 0) -
			2 * SIFT3D_IM_GET_VOX(cur, x, y, z, 0) +
			SIFT3D_IM_GET_VOX(prev, x, y, z, 0));

		// Solve the system
		switch(solve_Mat_rm(&Hs, &B, -1.0, &X)) {
		    case SIFT3D_SUCCESS:
			break;
		    case SINGULAR:
			// The system is singular: give up
			goto refine_quit;
		    default:
			puts("refine_keypoint: error solving system! \n");
			return SIFT3D_FAILURE;	
		}
	
#else
		// Parabolic interpolation
		SIFT3D_MAT_RM_GET(&X, 0, 0, double) = -0.5 * ( 
		    SIFT3D_IM_GET_VOX(cur, x + 1, y, z, 0) -
		    SIFT3D_IM_GET_VOX(cur, x - 1, y, z, 0)) / (
		    SIFT3D_IM_GET_VOX(cur, x + 1, y, z, 0) -
		    SIFT3D_IM_GET_VOX(cur, x - 1, y, z, 0) +
		    2 * SIFT3D_IM_GET_VOX(cur, x, y, z, 0));
		SIFT3D_MAT_RM_GET(&X, 1, 0, double) = -0.5 * ( 
		    SIFT3D_IM_GET_VOX(cur, x, y + 1, z, 0) -
		    SIFT3D_IM_GET_VOX(cur, x, y - 1, z, 0)) / (
		    SIFT3D_IM_GET_VOX(cur, x, y + 1, z, 0) -
		    SIFT3D_IM_GET_VOX(cur, x, y - 1, z, 0) +
		    2 * SIFT3D_IM_GET_VOX(cur, x, y, z, 0));
		SIFT3D_MAT_RM_GET(&X, 2, 0, double) = -0.5 * ( 
		    SIFT3D_IM_GET_VOX(cur, x, y, z + 1, 0) -
		    SIFT3D_IM_GET_VOX(cur, x, y, z - 1, 0)) / (
		    SIFT3D_IM_GET_VOX(cur, x, y, z + 1, 0) -
		    SIFT3D_IM_GET_VOX(cur, x, y, z - 1, 0) +
		    2 * SIFT3D_IM_GET_VOX(cur, x, y, z, 0));
		SIFT3D_MAT_RM_GET(&X, 3, 0, double) = -0.5 * ( 
		    SIFT3D_IM_GET_VOX(next, x, y, z, 0) -
		    SIFT3D_IM_GET_VOX(prev, x, y, z, 0)) / (
		    SIFT3D_IM_GET_VOX(next, x, y, z, 0) -
		    SIFT3D_IM_GET_VOX(prev, x, y, z, 0) +
		    2 * SIFT3D_IM_GET_VOX(cur, x, y, z, 0));
#endif
			// Update the coordinates
			xd = SIFT3D_MAX(SIFT3D_MIN(xd + 
                                SIFT3D_MAT_RM_GET(&X, 0, 0, double), xmax), xmin);
			yd = SIFT3D_MAX(SIFT3D_MIN(yd + 
                                SIFT3D_MAT_RM_GET(&X, 1, 0, double), ymax), ymin);
			zd = SIFT3D_MAX(SIFT3D_MIN(zd + 
                                SIFT3D_MAT_RM_GET(&X, 2, 0, double), zmax), zmin);
			sd = SIFT3D_MAX(SIFT3D_MIN(sd + 
                                SIFT3D_MAT_RM_GET(&X, 3, 0, double), smax), smin);

			// Compute the new pixel indices	
			xnew = (int) floor(xd);
			ynew = (int) floor(yd);
			znew = (int) floor(zd);

			// We are done if the pixel has not moved
			if (x == xnew && y == ynew && z == znew)
				break;

			// Update the pixel coordinates
			x = xnew;
			y = ynew;
			z = znew;
		}
#ifndef PARABOLA
refine_quit:
#else
#undef PARABOLA
#endif
		// Save the keypoint
		key->xi = x;
		key->yi = y;
		key->zi = z;
		key->xd = xd;
		key->yd = yd;
		key->zd = zd;
		key->sd = sd;
		key->sd_rel = sd * pow(2.0, -o);
	}

	return SIFT3D_SUCCESS;
}

/* Bin a Cartesian gradient into Spherical gradient bins */
static int Cvec_to_sbins(const Cvec * const vd, Svec * const bins) {

	// Convert to spherical coordinates
	SIFT3D_CVEC_TO_SVEC(vd, bins);
	//FIXME: Is this needed? SIFT3D_CVEC_TO_SVEC cannot divide by zero
	if (bins->mag < FLT_EPSILON * 1E2)
		return SIFT3D_FAILURE;

	// Compute bins
	bins->az *= (float) NBINS_AZ / SIFT3D_AZ_MAX_F; 
	bins->po *= (float) NBINS_PO / SIFT3D_PO_MAX_F;

	assert(bins->az < NBINS_AZ);
	assert(bins->po < NBINS_PO);

	return SIFT3D_SUCCESS;
}

/* Refine a gradient histogram with optional operations,
 * such as solid angle weighting. */
static void refine_Hist(Hist *hist) {

#ifndef ICOS_HIST

#ifdef SIFT3D_ORI_SOLID_ANGLE_WEIGHT
	{	
	float po;
	int a, p;
	// TODO: could accelerate this with a lookup table		

	// Weight by the solid angle of the bins, ignoring constants
	HIST_LOOP_START(a, p)
		po = p * po_max_f / NBINS_PO;
		HIST_GET(hist, a, p) /= cosf(po) - cosf(po + 
			po_max_f / NBINS_PO);
	HIST_LOOP_END
	}
#endif

#endif

}

/* As above, but using the eigenvector method */
static int assign_eig_ori(SIFT3D *const sift3d, const Image *const im, 
                          const Cvec *const vcenter,
                          const double sigma, Mat_rm *const R) {

    Cvec v[2];
    Mat_rm A, L, Q;
    Cvec vd, vd_win, vdisp, vr;
    double d, cos_ang;
    float weight, sq_dist, sgn;
    int i, x, y, z, m;
  
    const double win_radius = sigma * ori_rad_fctr; 

    // Initialize the intermediates
    if (init_Mat_rm(&A, 3, 3, DOUBLE, SIFT3D_TRUE) ||
	init_Mat_rm(&L, 0, 0, DOUBLE, SIFT3D_TRUE) ||
	init_Mat_rm(&Q, 0, 0, DOUBLE, SIFT3D_TRUE))
	goto eig_ori_fail;

    // Form the structure tensor and window gradient
    vd_win.x = 0.0f;
    vd_win.y = 0.0f;
    vd_win.z = 0.0f;
    IM_LOOP_SPHERE_START(im, x, y, z, vcenter, win_radius, &vdisp, sq_dist)
	// Compute Gaussian weighting, ignoring constant factor
	weight = expf(-0.5 * sq_dist / (sigma * sigma));		

	// Get the gradient	
	SIFT3D_IM_GET_GRAD(im, x, y, z, 0, &vd);

	// Update the structure tensor
	SIFT3D_MAT_RM_GET(&A, 0, 0, double) += (double) vd.x * vd.x * weight;
	SIFT3D_MAT_RM_GET(&A, 0, 1, double) += (double) vd.x * vd.y * weight;
	SIFT3D_MAT_RM_GET(&A, 0, 2, double) += (double) vd.x * vd.z * weight;
	SIFT3D_MAT_RM_GET(&A, 1, 1, double) += (double) vd.y * vd.y * weight;
	SIFT3D_MAT_RM_GET(&A, 1, 2, double) += (double) vd.y * vd.z * weight;
	SIFT3D_MAT_RM_GET(&A, 2, 2, double) += (double) vd.z * vd.z * weight;

	// Update the window gradient
	SIFT3D_CVEC_OP(&vd_win, &vd, +, &vd_win);
    SIFT3D_IM_LOOP_END

    // Fill in the remaining elements
    SIFT3D_MAT_RM_GET(&A, 1, 0, double) = SIFT3D_MAT_RM_GET(&A, 0, 1, double);
    SIFT3D_MAT_RM_GET(&A, 2, 0, double) = SIFT3D_MAT_RM_GET(&A, 0, 2, double);
    SIFT3D_MAT_RM_GET(&A, 2, 1, double) = SIFT3D_MAT_RM_GET(&A, 1, 2, double);

    // Reject keypoints with weak gradient 
    if (SIFT3D_CVEC_L2_NORM_SQ(&vd_win) < (float) ori_grad_thresh) {
	goto eig_ori_reject;
    } 

    // Get the eigendecomposition
    if (eigen_Mat_rm(&A, &Q, &L))
	goto eig_ori_fail;

    // Ensure we have distinct eigenvalues
    m = L.num_rows;
    if (m != 3)
	goto eig_ori_reject;

    // Test the eigenvectors for stability
    for (i = 0; i < m - 1; i++) {
	if (fabs(SIFT3D_MAT_RM_GET(&L, i, 0, double) /
		 SIFT3D_MAT_RM_GET(&L, i + 1, 0, double)) > max_eig_ratio)
	    goto eig_ori_reject;
    }

    // Assign signs to the first n - 1 vectors
    for (i = 0; i < m - 1; i++) {

	const int eig_idx = m - i - 1;

	// Get an eigenvector, in descending order
	vr.x = (float) SIFT3D_MAT_RM_GET(&Q, 0, eig_idx, double);
	vr.y = (float) SIFT3D_MAT_RM_GET(&Q, 1, eig_idx, double);
	vr.z = (float) SIFT3D_MAT_RM_GET(&Q, 2, eig_idx, double);

	// Get the directional derivative
	d = SIFT3D_CVEC_DOT(&vd, &vr);

        // Get the cosine of the angle between the eigenvector and the gradient
        cos_ang = d / (SIFT3D_CVEC_L2_NORM(&vr) * SIFT3D_CVEC_L2_NORM(&vd));

        // Reject points not meeting the corner score
        if (fabs(cos_ang) < sift3d->corner_thresh) 
                goto eig_ori_reject;

	// Get the sign of the derivative
	if (d > 0.0)
	    sgn = 1.0f;
	else
	    sgn = -1.0f;

	// Enforce positive directional derivative
	SIFT3D_CVEC_SCALE(&vr, sgn);

	// Add the vector to the rotation matrix
	SIFT3D_MAT_RM_GET(R, 0, i, float) = vr.x;
	SIFT3D_MAT_RM_GET(R, 1, i, float) = vr.y;
	SIFT3D_MAT_RM_GET(R, 2, i, float) = vr.z;

	// Save this vector for later use
	v[i] = vr;
    }

    // Take the cross product of the first two vectors
    SIFT3D_CVEC_CROSS(v, v + 1, &vr);

    // Add the last vector
    SIFT3D_MAT_RM_GET(R, 0, 2, float) = (float) vr.x;
    SIFT3D_MAT_RM_GET(R, 1, 2, float) = (float) vr.y;
    SIFT3D_MAT_RM_GET(R, 2, 2, float) = (float) vr.z;

    cleanup_Mat_rm(&A);
    cleanup_Mat_rm(&Q);
    cleanup_Mat_rm(&L);
    return SIFT3D_SUCCESS; 

eig_ori_reject:
    cleanup_Mat_rm(&A);
    cleanup_Mat_rm(&Q);
    cleanup_Mat_rm(&L);
    return REJECT;

eig_ori_fail:
    cleanup_Mat_rm(&A);
    cleanup_Mat_rm(&Q);
    cleanup_Mat_rm(&L);
    return SIFT3D_FAILURE;
}

/* Assign rotation matrices to the keypoints. 
 * 
 * Note that this stage will modify kp, likely
 * rejecting some keypoints as orientationally
 * unstable. */
static int assign_orientations(SIFT3D *sift3d, 
			       Keypoint_store *kp) {

	Keypoint *kp_pos;
	size_t num;
	int i; 

	// Iterate over the keypoints 
	kp_pos = kp->buf;
	for (i = 0; i < kp->slab.num; i++) {

		Keypoint *const key = kp->buf + i;
		const Image *const level = 
                        SIFT3D_PYR_IM_GET(&sift3d->gpyr, key->o, key->s);
                Mat_rm *const R = &key->R;
                const Cvec vcenter = {key->xd, key->yd, key->zd};
                const double sigma = ori_sig_fctr * key->sd_rel;

		// Initialize the orientation matrix
		if (init_Mat_rm_p(R, key->r_data, 3, 3, FLOAT, SIFT3D_FALSE))
			return SIFT3D_FAILURE;

		// Compute dominant orientations
		switch (assign_eig_ori(sift3d, level, &vcenter, sigma, R)) {
			case SIFT3D_SUCCESS:
				// Continue processing this keypoint
				break;
			case REJECT:
				// Skip this keypoint
				continue;
			default:
				// Any other return value is an error
				return SIFT3D_FAILURE;
		}
		
		// Rebuild the Keypoint buffer in place
		*kp_pos++ = *key; 
	}

	// Release unneeded keypoint memory
	num = kp_pos - kp->buf;
	SIFT3D_RESIZE_KP_STORE(kp, num, sizeof(Keypoint));

	return SIFT3D_SUCCESS;
}

/* Detect keypoint locations and orientations. You must initialize
 * the SIFT3D struct, image, and keypoint store with the appropriate
 * functions prior to calling this function. */
int SIFT3D_detect_keypoints(SIFT3D *const sift3d, const Image *const im,
			    Keypoint_store *const kp) {

        // Verify inputs
        if (im->nc != 1) {
                fprintf(stderr, "SIFT3D_detect_keypoints: invalid number "
                        "of image channels: %d -- only single-channel images "
                        "are supported \n", im->nc);
                return SIFT3D_FAILURE;
        }

        // Set the image       
        if (set_im_SIFT3D(sift3d, im))
                return SIFT3D_FAILURE;

	// Build the GSS pyramid
	if (build_gpyr(sift3d))
		return SIFT3D_FAILURE;

	// Build the DoG pyramid
	if (build_dog(sift3d))
		return SIFT3D_FAILURE;

	// Detect extrema
	if (detect_extrema(sift3d, kp))
		return SIFT3D_FAILURE;

	// Refine keypoints	
	if (refine_keypoints(sift3d, kp))
		return SIFT3D_FAILURE;

	// Assign orientations
	if (assign_orientations(sift3d, kp))
		return SIFT3D_FAILURE;

	return SIFT3D_SUCCESS;
}

/* Get the bin and barycentric coordinates of a vector in the icosahedral 
 * histogram. */
SIFT3D_IGNORE_UNUSED
static int icos_hist_bin(const SIFT3D * const sift3d,
			   const Cvec * const x, Cvec * const bary,
			   int * const bin) { 

	float k;
	int i;

	const Mesh * const mesh = &sift3d->mesh;

	// Check for very small vectors
	if (SIFT3D_CVEC_L2_NORM_SQ(x) < bary_eps)
		return SIFT3D_FAILURE;

	// Iterate through the faces
	for (i = 0; i < ICOS_NFACES; i++) {

		const Tri * const tri = mesh->tri + i;

		// Convert to barycentric coordinates
		if (cart2bary(x, tri, bary, &k))
			continue;

		// Test for intersection
		if (bary->x < -bary_eps || bary->y < -bary_eps ||
		    bary->z < -bary_eps || k < 0)
			continue;

		// Save the bin
		*bin = i;

		// No other triangles will be intersected
		return SIFT3D_SUCCESS;
	}	

	// Unreachable code
	assert(SIFT3D_FALSE);
	return SIFT3D_FAILURE;
}

/* Helper routine to interpolate over the histograms of a
 * SIFT3D descriptor. */
void SIFT3D_desc_acc_interp(const SIFT3D * const sift3d, 
				const Cvec * const vbins, 
				const Cvec * const grad,
				SIFT3D_Descriptor * const desc) {

	Cvec dvbins;
	Hist *hist;
	float weight;
	int dx, dy, dz, x, y, z;

#ifdef ICOS_HIST
	Cvec bary;
	float mag;
	int bin;	
#else
	Svec sbins, dsbins;
	int da, dp, a, p;
#endif

	const int y_stride = NHIST_PER_DIM;
	const int z_stride = NHIST_PER_DIM * NHIST_PER_DIM; 

	// Compute difference from integer bin values
	dvbins.x = vbins->x - floorf(vbins->x);
	dvbins.y = vbins->y - floorf(vbins->y);
	dvbins.z = vbins->z - floorf(vbins->z);

	// Compute the histogram bin
#ifdef ICOS_HIST
	const Mesh *const mesh = &sift3d->mesh;

	// Get the index of the intersecting face 
	if (icos_hist_bin(sift3d, grad, &bary, &bin))
		return;
	
	// Get the magnitude of the vector
	mag = SIFT3D_CVEC_L2_NORM(grad);

#else
	if (Cvec_to_sbins(grad, &sbins))
		return;
	dsbins.az = sbins.az - floorf(sbins.az);
	dsbins.po = sbins.po - floorf(sbins.po);
#endif
	
	for (dx = 0; dx < 2; dx++) {
		for (dy = 0; dy < 2; dy++) {
			for (dz = 0; dz < 2; dz++) {

				x = (int) vbins->x + dx;
				y = (int) vbins->y + dy;
				z = (int) vbins->z + dz;

				// Check boundaries
				if (x < 0 || x >= NHIST_PER_DIM ||
					y < 0 || y >= NHIST_PER_DIM ||
					z < 0 || z >= NHIST_PER_DIM)
					continue;

				// Get the histogram
				hist = desc->hists + x + y * y_stride + 
					   z * z_stride;	

				assert(x + y * y_stride + z * z_stride < DESC_NUM_TOTAL_HIST);

				// Get the spatial interpolation weight
				weight = ((dx == 0) ? (1.0f - dvbins.x) : 
					 	dvbins.x) *
					 ((dy == 0) ? (1.0f - dvbins.y) : 
						dvbins.y) *
					 ((dz == 0) ? (1.0f - dvbins.z) : 
						dvbins.z);

				/* Add the value into the histogram */
#ifdef ICOS_HIST
				assert(HIST_NUMEL == ICOS_NVERT);
				assert(bin >= 0 && bin < ICOS_NFACES);

				// Interpolate over three vertices
				MESH_HIST_GET(mesh, hist, bin, 0) += mag * weight * bary.x;
				MESH_HIST_GET(mesh, hist, bin, 1) += mag * weight * bary.y;
				MESH_HIST_GET(mesh, hist, bin, 2) += mag * weight * bary.z; 
#else
				// Iterate over all angles
				for (dp = 0; dp < 2; dp ++) {
					for (da = 0; da < 2; da ++) {

						a = ((int) sbins.az + da) % NBINS_AZ;
						p = (int) sbins.po + dp;
						if (p >= NBINS_PO) {
							// See HIST_GET_PO
							a = (a + NBINS_AZ / 2) % NBINS_AZ;
							p = NBINS_PO - 1;
						}
		
						assert(a >= 0);
						assert(a < NBINS_AZ);
						assert(p >= 0);
						assert(p < NBINS_PO);

						HIST_GET(hist, a, p) += sbins.mag * weight *
						((da == 0) ? (1.0f - dsbins.az) : dsbins.az) *
						((dp == 0) ? (1.0f - dsbins.po) : dsbins.po);
					}
				}	
#endif
	}}}

}

/* Normalize a descriptor */
static void normalize_desc(SIFT3D_Descriptor * const desc) {

	double norm; 
	int i, a, p;

	norm = 0.0;
	for (i = 0; i < DESC_NUM_TOTAL_HIST; i++) { 

                const Hist *const hist = desc->hists + i;

		HIST_LOOP_START(a, p) 
			const float el = HIST_GET(hist, a, p);
			norm += (double) el * el;
		HIST_LOOP_END 
	}

	norm = sqrt(norm) + DBL_EPSILON; 

	for (i = 0; i < DESC_NUM_TOTAL_HIST; i++) {

                Hist *const hist = desc->hists + i;
		const float norm_inv = 1.0f / norm; 

		HIST_LOOP_START(a, p) 
			HIST_GET(hist, a, p) *= norm_inv; 
		HIST_LOOP_END 
	}
}

/* Set a histogram to zero */
static void hist_zero(Hist *hist) {

        int a, p;

        HIST_LOOP_START(a, p)
                HIST_GET(hist, a, p) = 0.0f;
        HIST_LOOP_END
}

/* Helper routine to extract a single SIFT3D descriptor */
static void extract_descrip(SIFT3D *const sift3d, const Image *const im,
	   const Keypoint *const key, SIFT3D_Descriptor *const desc) {

	Cvec vcenter, vim, vkp, vbins, grad, grad_rot;
	Hist *hist;
	float weight, sq_dist;
	int i, x, y, z, a, p;

	// Compute basic parameters 
        const float sigma = key->sd_rel * desc_sig_fctr;
	const float win_radius = desc_rad_fctr * sigma;
	const float desc_width = win_radius / sqrt(2);
	const float desc_hw = desc_width / 2.0f;
	const float desc_bin_fctr = (float) NHIST_PER_DIM / desc_width;
	const double coord_factor = pow(2.0, key->o);

	// Zero the descriptor
	for (i = 0; i < DESC_NUM_TOTAL_HIST; i++) {
		hist = desc->hists + i;
                hist_zero(hist);
	}

	// Iterate over a sphere window in image space
	vcenter.x = key->xd;
	vcenter.y = key->yd;
	vcenter.z = key->zd;
	IM_LOOP_SPHERE_START(im, x, y, z, &vcenter, win_radius, &vim, sq_dist)

		// Rotate to keypoint space
		SIFT3D_MUL_MAT_RM_CVEC(&key->R, &vim, &vkp);		

		// Compute spatial bins
		vbins.x = (vkp.x + desc_hw) * desc_bin_fctr;
		vbins.y = (vkp.y + desc_hw) * desc_bin_fctr;
		vbins.z = (vkp.z + desc_hw) * desc_bin_fctr;

		// Reject points outside the rectangular descriptor 
		if (vbins.x < 0 || vbins.y < 0 || vbins.z < 0 ||
			vbins.x >= (float) NHIST_PER_DIM ||
			vbins.y >= (float) NHIST_PER_DIM ||
			vbins.z >= (float) NHIST_PER_DIM)
			continue;

		// Take the gradient
		SIFT3D_IM_GET_GRAD(im, x, y, z, 0, &grad);

		// Apply a Gaussian window
		weight = expf(-0.5f * sq_dist / (sigma * sigma));
		SIFT3D_CVEC_SCALE(&grad, weight);

                // Rotate the gradient to keypoint space
		SIFT3D_MUL_MAT_RM_CVEC(&key->R, &grad, &grad_rot);

		// Finally, accumulate bins by 5x linear interpolation
		SIFT3D_desc_acc_interp(sift3d, &vbins, &grad_rot, desc);
	SIFT3D_IM_LOOP_END

	// Histogram refinement steps
	for (i = 0; i < DESC_NUM_TOTAL_HIST; i++) {
		refine_Hist(&desc->hists[i]);
	}

	// Normalize the descriptor
	normalize_desc(desc);

	// Truncate
	for (i = 0; i < DESC_NUM_TOTAL_HIST; i++) {
		hist = desc->hists + i;
		HIST_LOOP_START(a, p)
			HIST_GET(hist, a, p) = SIFT3D_MIN(HIST_GET(hist, a, p), 
						   (float) trunc_thresh);
		HIST_LOOP_END
	}

	// Normalize again
	normalize_desc(desc);

	// Save the descriptor location in the original image
	// coordinates
	desc->xd = key->xd * coord_factor;
	desc->yd = key->yd * coord_factor;
	desc->zd = key->zd * coord_factor;
	desc->sd = key->sd;
}

/* Extract SIFT3D descriptors from a list of keypoints and an 
 * image.
 *
 * parameters:
 * 	sift3d - (initialized) struct defining parameters
 *  im - If use_gpyr == 0, this is a pointer to an Image
 *   	 and features will be extracted from the "raw" data.
 *  	 Else, this is a pointer to a Pyramid and features
 * 		 will be extracted at the nearest pyramid level to
 * 		 the keypoint. 
 *  kp - keypoint list populated by a feature detector 
 *  desc - (initialized) struct to hold the descriptors
 *  use_gpyr - see im for details */
int SIFT3D_extract_descriptors(SIFT3D *const sift3d, const void *const im,
	const Keypoint_store *const kp, SIFT3D_Descriptor_store *const desc,
        const int use_gpyr) {

	const Image *first_level;
	int i;

	// Parse inputs
	if (!use_gpyr) {
		puts("SIFT3D_extract_descriptors: This feature has not yet "
			 "been implemented! Please call this function with a "
			 "Pyramid rather than an Image. \n");
	}

	// Resize the descriptor store
	desc->num = kp->slab.num;
	if ((desc->buf = (SIFT3D_Descriptor *) realloc(desc->buf, desc->num * 
				sizeof(SIFT3D_Descriptor))) == NULL)
		return SIFT3D_FAILURE;

	// Initialize the image info
	if (use_gpyr) {
		const Pyramid *const gpyr = (Pyramid *) im;
		first_level = SIFT3D_PYR_IM_GET(gpyr, gpyr->first_octave, 
                        gpyr->first_level);
	} else
		first_level = im;
	desc->nx = first_level->nx;	
	desc->ny = first_level->ny;	
	desc->nz = first_level->nz;	

        // Extract the descriptors
	for (i = 0; i < desc->num; i++) {

		const Keypoint *const key = kp->buf + i;
		SIFT3D_Descriptor *const descrip = desc->buf + i;
		const Image *const level = 
                        SIFT3D_PYR_IM_GET((Pyramid *) im, key->o, key->s);
		extract_descrip(sift3d, level, key, descrip);
	}	

	return SIFT3D_SUCCESS;
}

/* L2-normalize a histogram */
static void normalize_hist(Hist *const hist) {

        double norm;
        float norm_inv;
        int a, p;

        norm = 0.0;
        HIST_LOOP_START(a, p)
                const float el = HIST_GET(hist, a, p);
                norm += (double) el * el;
        HIST_LOOP_END

        norm = sqrt(norm) + DBL_EPSILON;
        norm_inv = 1.0f / norm; 

        HIST_LOOP_START(a, p)
                 HIST_GET(hist, a, p) *= norm_inv; 
        HIST_LOOP_END 
}

/* Dense gradient histogram postprocessing steps */
static void postproc_Hist(Hist *const hist, const float norm) {

        int a, p;

        const float hist_trunc = trunc_thresh * DESC_NUMEL / HIST_NUMEL;

	// Histogram refinement steps
	refine_Hist(hist);

	// Normalize the descriptor
	normalize_hist(hist);

	// Truncate
	HIST_LOOP_START(a, p)
		HIST_GET(hist, a, p) = SIFT3D_MIN(HIST_GET(hist, a, p), 
					   hist_trunc);
	HIST_LOOP_END

	// Normalize again
	normalize_hist(hist);

        // Convert to the desired norm
        HIST_LOOP_START(a, p)
                HIST_GET(hist, a, p) *= norm;
        HIST_LOOP_END
}

/* Helper routine to extract a single SIFT3D histogram, with rotation. */
static void extract_dense_descrip_rotate(SIFT3D *const sift3d, 
           const Image *const im, const Cvec *const vcenter, 
           const double sigma, const Mat_rm *const R, Hist *const hist) {

	Cvec grad, grad_rot, bary, vim;
	float sq_dist, mag, weight;
	int a, p, x, y, z, bin;

        const Mesh *const mesh = &sift3d->mesh;
	const float win_radius = desc_rad_fctr * sigma;
	const float desc_width = win_radius / sqrt(2);
	const float desc_hw = desc_width / 2.0f;

	// Zero the descriptor
        hist_zero(hist);

	// Iterate over a sphere window in image space
	IM_LOOP_SPHERE_START(im, x, y, z, vcenter, win_radius, &vim, sq_dist)

		// Take the gradient and rotate
		SIFT3D_IM_GET_GRAD(im, x, y, z, 0, &grad);
		SIFT3D_MUL_MAT_RM_CVEC(R, &grad, &grad_rot);

                // Get the index of the intersecting face
                if (icos_hist_bin(sift3d, &grad_rot, &bary, &bin))
                        continue;

                // Get the magnitude of the vector
                mag = SIFT3D_CVEC_L2_NORM(&grad);

		// Get the Gaussian window weight
		weight = expf(-0.5f * sq_dist / (sigma * sigma));

                // Interpolate over three vertices
                MESH_HIST_GET(mesh, hist, bin, 0) += mag * weight * bary.x;
                MESH_HIST_GET(mesh, hist, bin, 1) += mag * weight * bary.y;
                MESH_HIST_GET(mesh, hist, bin, 2) += mag * weight * bary.z;

	SIFT3D_IM_LOOP_END
}

/* Get a descriptor with a single histogram at each voxel of an image.
 * The result is an image with HIST_NUMEL channels, where each channel is a
 * bin of the histogram.
 *
 * Parameters:
 * -sift3d The descriptor extractor.
 * -in The input image.
 * -out The output image.
 */
int SIFT3D_extract_dense_descriptors(SIFT3D *const sift3d, 
        const Image *const in, Image *const desc) {

        int (*extract_fun)(SIFT3D *const, const Image *const, Image *const);
        Image in_smooth;
        Gauss_filter gauss;
        int x, y, z;

        const double sigma_n = sift3d->gpyr.sigma_n;
        const double sigma0 = sift3d->gpyr.sigma0;

        // Verify inputs
        if (in->nc != 1) {
                fprintf(stderr, "SIFT3D_extract_dense_descriptors: invalid "
                        "number of channels: %d. This function only supports "
                        "single-channel images. \n", in->nc);
                return SIFT3D_FAILURE;
        }

        // Select the appropriate subroutine
        extract_fun = sift3d->dense_rotate ? 
                extract_dense_descriptors_rotate :
                extract_dense_descriptors_no_rotate;

        // Resize the output image
        memcpy(desc->dims, in->dims, IM_NDIMS * sizeof(int));
        desc->nc = HIST_NUMEL;
        im_default_stride(desc);
        if (im_resize(desc))
                return SIFT3D_FAILURE;

        // Initialize the smoothing filter
        if (init_Gauss_incremental_filter(&gauss, sigma_n, sigma0, 3))
                return SIFT3D_FAILURE;

        // Initialize the smoothed input image
        init_im(&in_smooth);
        if (im_copy_dims(in, &in_smooth))
                return SIFT3D_FAILURE;

        // Smooth the input image
        if (apply_Sep_FIR_filter(in, &in_smooth, &gauss.f))
                goto extract_dense_quit;

        // Extract the descriptors
        if (extract_fun(sift3d, &in_smooth, desc))
                return SIFT3D_FAILURE;

        // Post-process the descriptors
        SIFT3D_IM_LOOP_START(desc, x, y, z)

                Hist hist;

                // Get the image intensity at this voxel 
                const float val = SIFT3D_IM_GET_VOX(in, x, y, z, 0);

                // Copy to a Hist
                vox2hist(desc, x, y, z, &hist);

                // Post-process
                postproc_Hist(&hist, val);

                // Copy back to the image
                hist2vox(&hist, desc, x, y, z);

        SIFT3D_IM_LOOP_END


        // Clean up
        cleanup_Gauss_filter(&gauss);
        im_free(&in_smooth);

        return SIFT3D_SUCCESS;

extract_dense_quit:
        cleanup_Gauss_filter(&gauss);
        im_free(&in_smooth);
        return SIFT3D_FAILURE;
}

/* Helper function for SIFT3D_extract_dense_descriptors, without rotation 
 * invariance. This function is much faster than its rotation-invariant 
 * counterpart because histogram bins are pre-computed. */
static int extract_dense_descriptors_no_rotate(SIFT3D *const sift3d,
        const Image *const in, Image *const desc) {

        Image temp; 
        Gauss_filter gauss;
	Cvec grad, bary;
	float sq_dist, mag, weight;
        int i, x, y, z, bin, vert;

        const int x_start = 1;
        const int y_start = 1;
        const int z_start = 1;
        const int x_end = in->nx - 2;
        const int y_end = in->ny - 2;
        const int z_end = in->nz - 2;

        Mesh * const mesh = &sift3d->mesh;
        const double sigma_win = sift3d->gpyr.sigma0 * desc_sig_fctr / 
                                 NHIST_PER_DIM;

        // Initialize the intermediate image
        init_im(&temp);
        if (im_copy_dims(desc, &temp))
                return SIFT3D_FAILURE;

        // Initialize the filter
        if (init_Gauss_filter(&gauss, sigma_win, 3)) {
                im_free(&temp);
                return SIFT3D_FAILURE;
        }

        // Initialize the descriptors for each voxel
        im_zero(&temp);
        SIFT3D_IM_LOOP_LIMITED_START(in, x, y, z, x_start, x_end, y_start, 
                y_end, z_start, z_end)

                // Take the gradient
		SIFT3D_IM_GET_GRAD(in, x, y, z, 0, &grad);

                // Get the index of the intersecting face
                if (icos_hist_bin(sift3d, &grad, &bary, &bin))
                        continue;

                // Initialize each vertex
                SIFT3D_IM_GET_VOX(&temp, x, y, z, MESH_GET_IDX(mesh, bin, 0)) = 
                        bary.x;
                SIFT3D_IM_GET_VOX(&temp, x, y, z, MESH_GET_IDX(mesh, bin, 1)) = 
                        bary.y;
                SIFT3D_IM_GET_VOX(&temp, x, y, z, MESH_GET_IDX(mesh, bin, 2)) = 
                        bary.z;

        SIFT3D_IM_LOOP_END

        // Filter the descriptors
	if (apply_Sep_FIR_filter(&temp, desc, &gauss.f))
                goto dense_extract_quit;

        // Clean up
        im_free(&temp);
        cleanup_Gauss_filter(&gauss);

        return SIFT3D_SUCCESS;

dense_extract_quit:
        im_free(&temp);
        cleanup_Gauss_filter(&gauss);
        return SIFT3D_FAILURE;
}

/* Copy a voxel to a Hist. Does no bounds checking. */
static int vox2hist(const Image *const im, const int x, const int y,
        const int z, Hist *const hist) {

        int c;

        for (c = 0; c < HIST_NUMEL; c++) {
                hist->bins[c] = SIFT3D_IM_GET_VOX(im, x, y, z, c);
        }
}

/* Copy a Hist to a voxel. Does no bounds checking. */
static int hist2vox(Hist *const hist, const Image *const im, const int x, 
        const int y, const int z) {

        int c;
        
        for (c = 0; c < HIST_NUMEL; c++) {
                SIFT3D_IM_GET_VOX(im, x, y, z, c) = hist->bins[c];
        }
}

/* As in extract_dense_descrip, but with rotation invariance */
static int extract_dense_descriptors_rotate(SIFT3D *const sift3d,
        const Image *const in, Image *const desc) {

        Hist hist;
        Mat_rm R, Id;
        Mat_rm *ori;
        int i, x, y, z, c;

        // Initialize the identity matrix
        if (init_Mat_rm(&Id, 3, 3, FLOAT, SIFT3D_TRUE)) {
                return SIFT3D_FAILURE;
        }
        for (i = 0; i < 3; i++) {       
                SIFT3D_MAT_RM_GET(&Id, i, i, float) = 1.0f;
        }

        // Initialize the rotation matrix
        if (init_Mat_rm(&R, 3, 3, FLOAT, SIFT3D_TRUE)) {
                cleanup_Mat_rm(&Id);
                return SIFT3D_FAILURE;
        }

        // Iterate over each voxel
        SIFT3D_IM_LOOP_START(in, x, y, z)

                const Cvec vcenter = {(float) x + 0.5f, 
                                      (float) y + 0.5f, 
                                      (float) z + 0.5f};

                const double ori_sigma = sift3d->gpyr.sigma0 * ori_sig_fctr;
                const double desc_sigma = sift3d->gpyr.sigma0 * 
                        desc_sig_fctr / NHIST_PER_DIM;

                // Attempt to assign an orientation
                switch(assign_eig_ori(sift3d, in, &vcenter, ori_sigma, &R)) {
                        case SIFT3D_SUCCESS:
                                // Use the assigned orientation
                                ori = &R;
                                break;
                        case REJECT:
                                // Default to identity
                                ori = &Id;
                                break;
                        default:
                                // Unexpected error
                                goto dense_rotate_quit;
                }

                // Extract the descriptor
                extract_dense_descrip_rotate(sift3d, in, &vcenter, desc_sigma,
                        ori, &hist);

                // Copy the descriptor to the image channels
                hist2vox(&hist, desc, x, y, z);

        SIFT3D_IM_LOOP_END

        // Clean up
        cleanup_Mat_rm(&R);
        cleanup_Mat_rm(&Id);
        return SIFT3D_SUCCESS;

dense_rotate_quit:
        // Clean up and return an error condition 
        cleanup_Mat_rm(&R);
        cleanup_Mat_rm(&Id);
        return SIFT3D_FAILURE;
}

/* Convert a keypoint store to a matrix. 
 * Output format:
 *  [x1 y1 z1]
 *  |   ...  |
 *  [xn yn zn] 
 * 
 * mat must be initialized. */
int Keypoint_store_to_Mat_rm(const Keypoint_store *const kp, Mat_rm *const mat) {

    int i;

    const int num = kp->slab.num;

    // Resize mat
    mat->num_rows = num;
    mat->num_cols = IM_NDIMS;
    mat->type = DOUBLE;
    if (resize_Mat_rm(mat))
	return SIFT3D_FAILURE;

    // Build the matrix
    for (i = 0; i < num; i++) {

        const Keypoint *const key = kp->buf + i;

        // Adjust the coordinates to the base octave
        const double coord_factor = pow(2.0, key->o);

	SIFT3D_MAT_RM_GET(mat, i, 0, double) = coord_factor * key->xd;
	SIFT3D_MAT_RM_GET(mat, i, 1, double) = coord_factor * key->yd;
	SIFT3D_MAT_RM_GET(mat, i, 2, double) = coord_factor * key->zd;
    }

    return SIFT3D_SUCCESS;
}

/* Convert SIFT3D descriptors to a matrix.
 *
 * Output format:
 *  [x y z el0 el1 ... el767]
 * Each row is a feature descriptor. [x y z] are the coordinates in image
 * space, and [el0 el1 ... el767 are the 768 dimensions of the descriptor.
 *
 * mat must be initialized prior to calling this function. mat will be resized.
 * The type of mat will be changed to float.
 */
int SIFT3D_Descriptor_store_to_Mat_rm(const SIFT3D_Descriptor_store *const store, 
				      Mat_rm *const mat) {
	int i, j, a, p;

	const int num_rows = store->num;
	const int num_cols = IM_NDIMS + DESC_NUMEL;

	// Verify inputs
	if (num_rows < 1) {
		printf("SIFT3D_Descriptor_store_to_Mat_rm: invalid number of "
		       "descriptors: %d \n", num_rows);
		return SIFT3D_FAILURE;
	}

	// Resize inputs
	mat->type = FLOAT;
	mat->num_rows = num_rows;
	mat->num_cols = num_cols;
	if (resize_Mat_rm(mat))
		return SIFT3D_FAILURE;

	// Copy the data
	for (i = 0; i < num_rows; i++) {

		const SIFT3D_Descriptor *const desc = store->buf + i;

		// Copy the coordinates
		SIFT3D_MAT_RM_GET(mat, i, 0, float) = (float) desc->xd;
		SIFT3D_MAT_RM_GET(mat, i, 1, float) = (float) desc->yd;
		SIFT3D_MAT_RM_GET(mat, i, 2, float) = (float) desc->zd;

		// Copy the feature vector
		for (j = 0; j < DESC_NUM_TOTAL_HIST; j++) {
			const Hist *const hist = desc->hists + j;
			HIST_LOOP_START(a, p)
				SIFT3D_MAT_RM_GET(mat, i, j + IM_NDIMS, float) = 
					HIST_GET(hist, a, p);
			HIST_LOOP_END
		}
	}

	return SIFT3D_SUCCESS;
}

/* Convert a Mat_rm to a descriptor store. See 
 * SIFT3D_Descriptor_store_to_Mat_rm for the input format. */
int Mat_rm_to_SIFT3D_Descriptor_store(const Mat_rm *const mat, 
				      SIFT3D_Descriptor_store *const store) {

	int i, j, a, p;

	const int num_rows = mat->num_rows;
	const int num_cols = mat->num_cols;

	// Verify inputs
	if (num_rows < 1 || num_cols != IM_NDIMS + DESC_NUMEL) {
		printf("Mat_rm_to_SIFT3D_Descriptor_store: invalid matrix "
		       "dimensions: [%d X %d] \n", num_rows, num_cols);
		return SIFT3D_FAILURE;
	}

	// Resize the descriptor store
	store->num = num_rows;
	if ((store->buf = (SIFT3D_Descriptor *) realloc(store->buf, store->num * 
				sizeof(SIFT3D_Descriptor))) == NULL)
		return SIFT3D_FAILURE;

	// Copy the data
	for (i = 0; i < num_rows; i++) {

		SIFT3D_Descriptor *const desc = store->buf + i;

		// Copy the coordinates
		desc->xd = SIFT3D_MAT_RM_GET(mat, i, 0, float);
		desc->yd = SIFT3D_MAT_RM_GET(mat, i, 1, float);
		desc->zd = SIFT3D_MAT_RM_GET(mat, i, 2, float);

		// Copy the feature vector
		for (j = 0; j < DESC_NUM_TOTAL_HIST; j++) {
			Hist *const hist = desc->hists + j;
			HIST_LOOP_START(a, p)
				HIST_GET(hist, a, p) = SIFT3D_MAT_RM_GET(mat, i, 
					j + IM_NDIMS, float);
			HIST_LOOP_END
		}
	}
	
	return SIFT3D_SUCCESS;
}

/* Convert a list of matches to matrices of point coordinates.
 * Only valid matches will be included in the output matrices.
 *
 * The format of "matches" is specified in SIFT3D_nn_match.
 *
 * All matrices must be initialized prior to calling this function.
 *
 * Output format:
 *  m x 3 matrices [x11 y11 z11] [x21 y21 z21]
 * 		   |x12 y12 z12| |x22 y22 z22|
 *		        ...	      ...
 * 		   [x1N y1N z1N] [x2N y2N z2N] 
 *
 * Where points on corresponding rows are matches. */
int SIFT3D_matches_to_Mat_rm(SIFT3D_Descriptor_store *d1,
			     SIFT3D_Descriptor_store *d2,
			     const int *const matches,
			     Mat_rm *const match1, 
			     Mat_rm *const match2) {
    int i, num_matches;

    const int num = d1->num;

    // Resize matrices 
    match1->num_rows = match2->num_rows = d1->num;
    match1->num_cols = match2->num_cols = 3;
    match1->type = match2->type = DOUBLE;
    if (resize_Mat_rm(match1) || resize_Mat_rm(match2))
	    return SIFT3D_FAILURE;

    // Populate the matrices
    num_matches = 0;
    for (i = 0; i < num; i++) {

	    const SIFT3D_Descriptor *const desc1 = d1->buf + i;
	    const SIFT3D_Descriptor *const desc2 = d2->buf + matches[i];

	    if (matches[i] == -1)
		    continue;

	    // Save the match
	    SIFT3D_MAT_RM_GET(match1, num_matches, 0, double) = desc1->xd; 
	    SIFT3D_MAT_RM_GET(match1, num_matches, 1, double) = desc1->yd; 
	    SIFT3D_MAT_RM_GET(match1, num_matches, 2, double) = desc1->zd; 
	    SIFT3D_MAT_RM_GET(match2, num_matches, 0, double) = desc2->xd; 
	    SIFT3D_MAT_RM_GET(match2, num_matches, 1, double) = desc2->yd; 
	    SIFT3D_MAT_RM_GET(match2, num_matches, 2, double) = desc2->zd; 
	    num_matches++;
    }

    // Release extra memory
    match1->num_rows = match2->num_rows = num_matches;
    if (resize_Mat_rm(match1) || resize_Mat_rm(match2))
	    return SIFT3D_FAILURE;
    
    return SIFT3D_SUCCESS;
}

/* Like SIFT3D_nn_match, but also tests for forward-backward consistency.
 * That is, matching is performed from d1 to d2, and then d2 to d1, and
 * any matches that do not have the same result in each pass are rejected. 
 *
 * See SIFT3D_nn_match for descriptions of the parameters. */
int SIFT3D_nn_match_fb(const SIFT3D_Descriptor_store *const d1,
		       const SIFT3D_Descriptor_store *const d2,
		       const float nn_thresh, int **const matches) {
    int *matches2; 
    int i;

    // Run the matching passes
    matches2 = NULL;
    if (SIFT3D_nn_match(d1, d2, nn_thresh, matches) ||
	SIFT3D_nn_match(d2, d1, nn_thresh, &matches2))
	return SIFT3D_FAILURE;

    // Enforce forward-backward consistency
    for (i = 0; i < d1->num; i++) {

	int *const match1 = *matches + i;

	if (*match1 >= 0 && matches2[*match1] != i)
	    *match1 = -1;
    }

    return SIFT3D_SUCCESS;
}

/* Perform nearest neighbor matching on two sets of 
 * SIFT descriptors.
 *
 * This function will reallocate *matches. As such, *matches must be either
 * NULL or a pointer to previously-allocated array. Upon successful exit,
 * *matches is an array of size d1->num.
 * 
 * On return, the ith element of matches contains the index in d2 of the match
 * corresponding to the ith descriptor in d1, or -1 if no match was found.
 *
 * You may consider using SIFT3D_matches_to_Mat_rm to convert the matches to
 * coordinate matrices. */
int SIFT3D_nn_match(const SIFT3D_Descriptor_store *const d1,
		    const SIFT3D_Descriptor_store *const d2,
		    const float nn_thresh, int **const matches) {

	const SIFT3D_Descriptor *desc_best;
	float *match_ssds;
	float ssd, ssd_best, ssd_nearest;
	int i, j, k, a, p, desc2_idx;

	const int num = d1->num;

#ifdef SIFT3D_MATCH_MAX_DIST
		Cvec dims, dmatch;
		double dist_match;
				
		// Compute spatial distance rejection threshold
		dims.x = (float) d1->nx;	
		dims.y = (float) d1->ny;	
		dims.z = (float) d1->nz;	
		const double diag = SIFT3D_CVEC_L2_NORM(&dims);	
		const double dist_thresh = diag * SIFT3D_MATCH_MAX_DIST;
#endif

	// Initialize intermediate arrays 
	if ((*matches = (int *) realloc(*matches, num * sizeof(float))) == NULL ||
	    (match_ssds = (float *) malloc(num * sizeof(float))) == NULL) {
	    fprintf(stderr, "SIFT3D_nn_match: out of memory! \n");
	    return SIFT3D_FAILURE;
	}

	for (i = 0; i < d1->num; i++) {
	    // Mark -1 to signal there is no match
	    (*matches)[i] = -1;
	    match_ssds[i] = -1.0f;
	}
	
	// Exhaustive search for matches
	for (i = 0; i < d1->num; i++) {

	    const SIFT3D_Descriptor *const desc1 = d1->buf + i;

	    // Linear search for the best and second-best SSD matches 
	    ssd_best = ssd_nearest = 1e30f;
	    desc_best = NULL;
	    for (j = 0; j < d2->num; j++) { 

		    const SIFT3D_Descriptor *const desc2 = d2->buf + j;

		    // Find the SSD of the two descriptors
		    ssd = 0.0f;
		    for (k = 0; k < DESC_NUM_TOTAL_HIST; k++) {

			    const Hist *const hist1 = &desc1->hists[k];
			    const Hist *const hist2 = &desc2->hists[k];

			    HIST_LOOP_START(a, p)
				    const float diff = HIST_GET(hist1, a, p) - 
					       HIST_GET(hist2, a, p); 
				    ssd += diff * diff; 		
			    HIST_LOOP_END
		    }

		    // Compare to best matches
		    if (ssd < ssd_best) {
			    desc_best = desc2; 
			    ssd_nearest = ssd_best;
			    ssd_best = ssd;
		    } else 
			    ssd_nearest = SIFT3D_MIN(ssd_nearest, ssd);
	    }

	    // Reject match if nearest neighbor is too close 
	    if (ssd_best / ssd_nearest > nn_thresh * nn_thresh)
		    goto match_reject;

	    desc2_idx = desc_best - d2->buf;

#ifdef SIFT3D_MATCH_MAX_DIST
	    // Compute the spatial distance of the match
	    dmatch.x = (float) desc_best->xd - desc1->xd; 
	    dmatch.y = (float) desc_best->yd - desc1->yd; 
	    dmatch.z = (float) desc_best->zd - desc1->zd; 
	    dist_match = (double) SIFT3D_CVEC_L2_NORM(&dmatch);

	    // Reject matches of great distance
	    if (dist_match > dist_thresh)
		    goto match_reject;
#endif
			
	    // Save the match
	    (*matches)[i] = desc2_idx;
	    match_ssds[i] = ssd_best;

match_reject: ;
	}

	return SIFT3D_SUCCESS;
}

/* Draw the matches. 
 * 
 * Inputs:
 * -left - lefthand-side image
 * -right - righthand-side image
 * -keys_left - Keypoints from "left" (can be NULL if keys is NULL) 
 * -keys_right - Keypoints from "right" (can be NULL if keys is NULL)
 * -match_left - Matches from "left" (can be NULL if lines is NULL)
 * -match_right - Matches from "right" (can be NULL if lines is NULL)
 * 
 * Outputs:
 * -concat - Concatenated image (NULL if not needed)
 * -keys - Keypoints from concat (NULL is not needed)
 * -lines - Lines between matching keypoints in concat (NULL if not needed)
 * It is an error if all outputs are NULL.
 *
 * Return:
 * SIFT3D_SUCCESS or SIFT3D_FAILURE
 */
int draw_matches(const Image *const left, const Image *const right,
                 const Mat_rm *const keys_left, const Mat_rm *const keys_right,
		 const Mat_rm *const match_left, 
                 const Mat_rm *const match_right, Image *const concat, 
                 Image *const keys, Image *const lines) {

        Image concat_temp, left_padded, right_padded;
	Mat_rm keys_right_draw, keys_left_draw, keys_draw, match_right_draw;
	int i;

        const double right_pad = (double) left->nx;
        const int ny_pad = SIFT3D_MAX(right->ny, left->ny);
	const int nz_pad = SIFT3D_MAX(right->nz, left->nz);

        // Choose which image to use for concatenation 
        Image *const concat_arg = concat == NULL ? &concat_temp : concat;

        // Verify inputs
        if (concat == NULL && keys == NULL && lines == NULL) {
                fprintf(stderr, "draw_matches: all outputs are NULL \n");
                return SIFT3D_FAILURE;
        }
        if (keys_left == NULL && keys != NULL) {
                fprintf(stderr, "draw_matches: keys_left is NULL but keys is "
                        "not \n");
                return SIFT3D_FAILURE;
        }
        if (keys_right == NULL && keys != NULL) {
                fprintf(stderr, "draw_matches: keys_right is NULL but keys is "
                        "not \n");
                return SIFT3D_FAILURE;
        }
        if (match_left == NULL && lines != NULL) {
                fprintf(stderr, "draw_matches: match_left is NULL but lines "
                        "is not \n");
                return SIFT3D_FAILURE;
        }
        if (match_right == NULL && lines != NULL) {
                fprintf(stderr, "draw_matches: match_right is NULL but lines "
                        "is not \n");
                return SIFT3D_FAILURE;
        }

	// Initialize intermediates		
        init_im(&concat_temp);
        init_im(&left_padded);
        init_im(&right_padded);
        if (init_Mat_rm(&keys_right_draw, 0, 0, DOUBLE, SIFT3D_FALSE) ||
	        init_Mat_rm(&match_right_draw, 0, 0, DOUBLE, SIFT3D_FALSE) ||
                init_Mat_rm(&keys_left_draw, 0, 0, DOUBLE, SIFT3D_FALSE) ||
                init_Mat_rm(&keys_draw, 0, 0, DOUBLE, SIFT3D_FALSE))
	        return SIFT3D_FAILURE;

        // Pad the images to be the same in all dimensions but x
	if (init_im_with_dims(&right_padded, right->nx, ny_pad, nz_pad, 1) || 
	        init_im_with_dims(&left_padded, left->nx, ny_pad, nz_pad, 1) ||
	   	im_pad(right, &right_padded) || 
	    	im_pad(left, &left_padded)) {
                fprintf(stderr, "draw_matches: unable to pad images \n");
                return SIFT3D_FAILURE;
	}

	// Draw a concatenated image
	if (im_concat(&left_padded, &right_padded, 0, concat_arg)) {
                fprintf(stderr, "draw_matches: Could not concatenate the "
                        "images \n");
                goto draw_matches_quit;
        }

        // Optionally draw the keypoints
        if (keys != NULL) { 

                // Convert inputs to double
                if (convert_Mat_rm(keys_right, &keys_right_draw, DOUBLE) ||
                        convert_Mat_rm(keys_left, &keys_left_draw, DOUBLE))
                        goto draw_matches_quit;
       
                // Pad the x-coordinate 
                for (i = 0; i < keys_right->num_rows; i++) {
                        SIFT3D_MAT_RM_GET(&keys_right_draw, i, 0, double) += 
                                right_pad; 
                }

                // Concatenate the points
                if (concat_Mat_rm(&keys_left_draw, &keys_right_draw,
                        &keys_draw, 0))
                        goto draw_matches_quit;

                // Draw the points
                if (draw_points(&keys_draw, concat_arg->dims, 1, keys))
                        goto draw_matches_quit;
        }

	// Optionally draw the lines 
        if (lines != NULL) {

                // Convert input to double
                if (convert_Mat_rm(match_right, &match_right_draw, DOUBLE))
                        goto draw_matches_quit;

                // Pad the x-coordinate 
                for (i = 0; i < match_right->num_rows; i++) {
                        SIFT3D_MAT_RM_GET(&match_right_draw, i, 0, double) += 
                                right_pad;
                }

                // Draw the lines
                if (draw_lines(match_left, &match_right_draw, concat_arg->dims,
                        lines))
                        goto draw_matches_quit;
        }

        // Clean up
        im_free(&concat_temp);
        im_free(&left_padded);
        im_free(&right_padded); 
        cleanup_Mat_rm(&keys_right_draw); 
        cleanup_Mat_rm(&keys_left_draw); 
        cleanup_Mat_rm(&keys_draw); 
        cleanup_Mat_rm(&match_right_draw); 
	return SIFT3D_SUCCESS;

draw_matches_quit:
        im_free(&concat_temp);
        im_free(&left_padded);
        im_free(&right_padded); 
        cleanup_Mat_rm(&keys_right_draw);
        cleanup_Mat_rm(&keys_left_draw); 
        cleanup_Mat_rm(&keys_draw); 
        cleanup_Mat_rm(&match_right_draw);
        return SIFT3D_FAILURE;
}

/* Write a Keypoint_store to a text file. The keypoints are stored in a matrix
 * (.csv, .csv.gz), where each keypoint is a row. The elements of each row are
 * as follows:
 *
 * x y z s ori11 ori12 ... or1nn
 *
 * x - the x-coordinate
 * y - the y-coordinate
 * z - the z-coordinate
 * s - the scale coordinate
 * ori(ij) - the ith row, jth column of the orientation matrix */
int write_Keypoint_store(const char *path, const Keypoint_store *const kp) {

        Mat_rm mat;
	int i, i_R, j_R;

        const int num_rows = kp->slab.num;

        // Initialize the matrix
        if (init_Mat_rm(&mat, num_rows, kp_num_cols, DOUBLE, SIFT3D_FALSE))
                return SIFT3D_FAILURE;
       
        // Write the keypoints 
        for (i = 0; i < num_rows; i++) {

                const Keypoint *const key = kp->buf + i;
                const Mat_rm *const R = &key->R;

                // Write the coordinates 
                SIFT3D_MAT_RM_GET(&mat, i, kp_x, double) = key->xd;
                SIFT3D_MAT_RM_GET(&mat, i, kp_y, double) = key->yd;
                SIFT3D_MAT_RM_GET(&mat, i, kp_z, double) = key->zd;
                SIFT3D_MAT_RM_GET(&mat, i, kp_s, double) = key->sd;

                // Write the orientation matrix
                SIFT3D_MAT_RM_LOOP_START(R, i_R, j_R)

                        const int kp_idx = kp_ori + 
                                SIFT3D_MAT_RM_GET_IDX(R, i_R, j_R);
        
                        SIFT3D_MAT_RM_GET(&mat, i, kp_idx, double) = 
                                (double) SIFT3D_MAT_RM_GET(R, i_R, j_R, float);

                SIFT3D_MAT_RM_LOOP_END
        }

        // Write the matrix 
        if (write_Mat_rm(path, &mat))
                goto write_kp_quit;

        // Clean up
        cleanup_Mat_rm(&mat);

        return SIFT3D_SUCCESS;

write_kp_quit:
        cleanup_Mat_rm(&mat);
        return SIFT3D_FAILURE;
}

/* Write SIFT3D descriptors to a file. The descriptors are represented by a
 * matrix (.csv, .csv.gz) where each row is a descriptor. */
int write_SIFT3D_Descriptor_store(const char *path, 
        const SIFT3D_Descriptor_store *const desc) {

        const int num_rows = desc->num;

        Mat_rm mat;
        int i, j;

        // Initialize the matrix
        if (init_Mat_rm(&mat, num_rows, DESC_NUMEL, DOUBLE, SIFT3D_FALSE))
                return SIFT3D_FAILURE;
     
        // Write the data into the matrix 
        for (i = 0; i < num_rows; i++) { 

                int col;

                const SIFT3D_Descriptor *const d = desc->buf + i;

                for (j = 0; j < DESC_NUM_TOTAL_HIST; j++) {

                        int a, p;

                        const Hist *const hist = d->hists + j;

                        HIST_LOOP_START(a, p)
                                const int col = HIST_GET_IDX(a, p) + j * HIST_NUMEL;
                                SIFT3D_MAT_RM_GET(&mat, i, col, double) =   
                                        HIST_GET(hist, a, p);
                        HIST_LOOP_END
                }
        }

        // Write the matrix to the file
        if (write_Mat_rm(path, &mat))
                goto write_desc_quit;

        // Clean up
        cleanup_Mat_rm(&mat);
        return SIFT3D_SUCCESS;

write_desc_quit:
        cleanup_Mat_rm(&mat);
        return SIFT3D_FAILURE;
}

