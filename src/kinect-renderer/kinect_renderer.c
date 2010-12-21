#include <zlib.h>

#include <lcm/lcm.h>

#include <bot_core/bot_core.h>
#include <bot_vis/bot_vis.h>

#include <lcmtypes/kinect_frame_msg_t.h>
#include <kinect/kinect_calib.h>

#include "jpeg-utils-ijg.h"

typedef struct _KinectRenderer {
    BotRenderer renderer;
    BotGtkParamWidget *pw;
    BotViewer   *viewer;
    lcm_t     *lcm;

    kinect_frame_msg_t* msg;

    int width;
    int height;

    // raw disparity
    uint16_t* disparity;
    int need_to_recompute_frame_data;

    uint8_t* uncompress_buffer;
    int uncompress_buffer_size;

    uint8_t* rgb_data;

    KinectCalibration* kcal;
} KinectRenderer;

static void
recompute_frame_data(KinectRenderer* self);

#if 0
/** Given an array of colors, a palette is created that linearly interpolates through all the colors. **/
static void color_util_build_color_table(double color_palette[][3], int palette_size, float lut[][3], int lut_size)
{
    for (int idx = 0; idx < lut_size; idx++) {
        double znorm = ((double) idx) / lut_size;

        int color_index = (palette_size - 1) * znorm;
        double alpha = (palette_size - 1) * znorm - color_index;
        
        for (int i = 0; i < 3; i++) {
            lut[idx][i] = color_palette[color_index][i] * (1.0 - alpha) + color_palette[color_index+1][i]*alpha;
        }    
    }
}

#define JET_COLORS_LUT_SIZE 1024
static float jet_colors[JET_COLORS_LUT_SIZE][3];
static int jet_colors_initialized = 0;

static void init_color_table_jet()
{
    double jet[][3] = {{ 0,   0,   1 },
                       { 0,  .5,  .5 },
                       { .8, .8,   0 },
                       { 1,   0,   0 }};

    color_util_build_color_table(jet, sizeof(jet)/(sizeof(double)*3), jet_colors, JET_COLORS_LUT_SIZE);
    jet_colors_initialized = 1;
}

static inline float *color_util_jet(double v)
{
    if (!jet_colors_initialized)
        init_color_table_jet();

    v = fmax(0, v);
    v = fmin(1, v);

    int idx = (JET_COLORS_LUT_SIZE - 1) * v;
    return jet_colors[idx];
}
#endif

static void
recompute_frame_data(KinectRenderer* self)
{
    if(!self->msg) {
        self->need_to_recompute_frame_data = 0;
        return;
    }

    int npixels = self->width * self->height;

    const uint8_t* depth_data = self->msg->depth.depth_data;

    if(self->msg->depth.compression != KINECT_DEPTH_MSG_T_COMPRESSION_NONE) {
        if(self->msg->depth.uncompressed_size > self->uncompress_buffer_size) {
            self->uncompress_buffer_size = self->msg->depth.uncompressed_size;
            self->uncompress_buffer = (uint8_t*) realloc(self->uncompress_buffer, self->uncompress_buffer_size);
        }
        unsigned long dlen = self->msg->depth.uncompressed_size;
        int status = uncompress(self->uncompress_buffer, &dlen, 
                self->msg->depth.depth_data, self->msg->depth.depth_data_nbytes);
        if(status != Z_OK) {
            return;
        }
        depth_data = self->uncompress_buffer;
    }

    switch(self->msg->depth.depth_data_format) {
        case KINECT_DEPTH_MSG_T_DEPTH_11BIT:
            if(G_BYTE_ORDER == G_LITTLE_ENDIAN) {
                int16_t* rdd = (int16_t*) depth_data;
                int i;
                for(i=0; i<npixels; i++) {
                    int d = rdd[i];
                    self->disparity[i] = d;
                }
            } else {
                fprintf(stderr, "Big endian systems not supported\n");
            }
            break;
        case KINECT_DEPTH_MSG_T_DEPTH_10BIT:
            fprintf(stderr, "10-bit depth data not supported\n");
            break;
        default:
            break;
    }
}

static void 
on_kinect_frame (const lcm_recv_buf_t *rbuf, const char *channel,
        const kinect_frame_msg_t *msg, void *user_data )
{
    KinectRenderer *self = (KinectRenderer*) user_data;

    if(self->msg)
        kinect_frame_msg_t_destroy(self->msg);
    self->msg = kinect_frame_msg_t_copy(msg);

    // TODO check width, height

    if(msg->image.image_data_format == KINECT_IMAGE_MSG_T_VIDEO_RGB) {
        memcpy(self->rgb_data, msg->image.image_data, 
                self->width * self->height * 3);
    } else if(msg->image.image_data_format == KINECT_IMAGE_MSG_T_VIDEO_RGB_JPEG) {
        jpegijg_decompress_8u_rgb(msg->image.image_data, msg->image.image_data_nbytes,
                self->rgb_data, self->width, self->height, self->width * 3);
    }

    self->need_to_recompute_frame_data = 1;

    bot_viewer_request_redraw(self->viewer);
}

static void on_param_widget_changed (BotGtkParamWidget *pw, const char *name, void *user)
{
    KinectRenderer *self = (KinectRenderer*) user;

    // TODO

    bot_viewer_request_redraw(self->viewer);
}

static inline void
_matrix_vector_multiply_3x4_4d (const double m[12], const double v[4],
        double result[3])
{
    result[0] = m[0]*v[0] + m[1]*v[1] + m[2] *v[2] + m[3] *v[3];
    result[1] = m[4]*v[0] + m[5]*v[1] + m[6] *v[2] + m[7] *v[3];
    result[2] = m[8]*v[0] + m[9]*v[1] + m[10]*v[2] + m[11]*v[3];
}

static inline void
_matrix_transpose_4x4d (const double m[16], double result[16])
{
    result[0] = m[0];
    result[1] = m[4];
    result[2] = m[8];
    result[3] = m[12];
    result[4] = m[1];
    result[5] = m[5];
    result[6] = m[9];
    result[7] = m[13];
    result[8] = m[2];
    result[9] = m[6];
    result[10] = m[10];
    result[11] = m[14];
    result[12] = m[3];
    result[13] = m[7];
    result[14] = m[11];
    result[15] = m[15];
}

static void _draw(BotViewer *viewer, BotRenderer *renderer)
{
    KinectRenderer *self = (KinectRenderer*) renderer->user;
    if(!self->msg)
        return;

    if(self->need_to_recompute_frame_data)
        recompute_frame_data(self);

    // rotate so that X is forward and Z is up
    glRotatef(-90, 0, 0, 1);
    glRotatef(-90, 1, 0, 0);

    float so = self->kcal->shift_offset;
    double depth_to_rgb_uvd[12];
    kinect_calib_get_depth_uvd_to_rgb_uvw_3x4(self->kcal, depth_to_rgb_uvd);

    double depth_to_depth_xyz[16];
    kinect_calib_get_depth_uvd_to_depth_xyz_4x4(self->kcal, depth_to_depth_xyz);

    double depth_to_depth_xyz_trans[16];
    _matrix_transpose_4x4d(depth_to_depth_xyz, depth_to_depth_xyz_trans);

    glPushMatrix();
    glMultMatrixd(depth_to_depth_xyz_trans);
    glPointSize(2.0f);
    glBegin(GL_POINTS);
    glColor3f(0, 0, 0);
    for(int u=0; u<self->width; u++) {
        for(int v=0; v<self->height; v++) {
            uint16_t disparity = self->disparity[v*self->width+u];
            float d = 0.125 * (so - disparity);

            double uvd_depth[4] = { u, v, d, 1 };
            double uvd_rgb[3];
            _matrix_vector_multiply_3x4_4d(depth_to_rgb_uvd, uvd_depth, uvd_rgb);

            double uv_rect[2] = {
                uvd_rgb[0] / uvd_rgb[2],
                uvd_rgb[1] / uvd_rgb[2]
            };
            double uv_dist[2];

            // compute distorted pixel coordinates
            kinect_calib_distort_rgb_uv(self->kcal, uv_rect, uv_dist);
            int u_rgb = uv_dist[0] + 0.5;
            int v_rgb = uv_dist[1] + 0.5;

            uint8_t r, g, b;
            if(u_rgb >= self->width || u_rgb < 0 || v_rgb >= self->height || v_rgb < 0) {
                r = g = b = 0;
            } else {
                r = self->rgb_data[v_rgb*self->width*3 + u_rgb*3 + 0];
                g = self->rgb_data[v_rgb*self->width*3 + u_rgb*3 + 1];
                b = self->rgb_data[v_rgb*self->width*3 + u_rgb*3 + 2];
            }

            glColor3f(r / 255.0, g / 255.0, b / 255.0);

            glVertex3f(u, v, d);
        }
    }
    glEnd();
    glPopMatrix();
}

static void _free(BotRenderer *renderer)
{
    KinectRenderer *self = (KinectRenderer*) renderer;

    if(self->msg)
        kinect_frame_msg_t_destroy(self->msg);

    free(self->uncompress_buffer);
    self->uncompress_buffer_size = 0;

    kinect_calib_destroy(self->kcal);

    free(self->disparity);
    free(self->rgb_data);

    free(self);
}

void 
kinect_add_renderer_to_viewer(BotViewer* viewer, lcm_t* lcm, int priority)
{
    KinectRenderer *self = (KinectRenderer*) calloc(1, sizeof(KinectRenderer));

    self->need_to_recompute_frame_data = 0;
    self->width = 640;
    self->height = 480;
    self->disparity = (uint16_t*) malloc(self->width * self->height * sizeof(uint16_t));
    self->rgb_data = (uint8_t*) malloc(self->width * self->height * 3);

    self->msg = NULL;

    BotRenderer *renderer = &self->renderer;

    self->kcal = kinect_calib_new();
    self->kcal->width = 640;
    self->kcal->height = 480;
#if 0
    self->kcal->intrinsics_depth.fx = 579.97821782;
    self->kcal->intrinsics_depth.cx = 317.80483131;
    self->kcal->intrinsics_depth.cy = 243.16590456;

    self->kcal->intrinsics_rgb.fx = 518.12282951;
    self->kcal->intrinsics_rgb.cx = 322.60598443;
    self->kcal->intrinsics_rgb.cy = 262.56367112;
    self->kcal->intrinsics_rgb.k1 = 0.17730118;
    self->kcal->intrinsics_rgb.k2 = -0.35126598;

    self->kcal->shift_offset = 1101.5254;
    self->kcal->projector_depth_baseline = 0.07710;

    double R[9] = { 0.999997, -0.002517, -0.000720, 0.002518, 0.999995, 0.001772,  0.000716, -0.001774, 0.999998 };
    double T[3] = { -0.025318, 0.000415, -0.011306 };
#else
    self->kcal->intrinsics_depth.fx = 580.06887971;
    self->kcal->intrinsics_depth.cx = 324.15898680;
    self->kcal->intrinsics_depth.cy = 238.02660584;

    self->kcal->intrinsics_rgb.fx = 518.22278379;
    self->kcal->intrinsics_rgb.cx = 318.57466440;
    self->kcal->intrinsics_rgb.cy = 256.95963149;
    self->kcal->intrinsics_rgb.k1 = 0.15272473;
    self->kcal->intrinsics_rgb.k2 = -0.26339203;

    self->kcal->shift_offset = 1035.9168;
    self->kcal->projector_depth_baseline = 0.02663;

    double R[9] = { 0.999899, 0.006075, -0.012839, -0.006119, 0.999976, -0.003321, 0.012819, 0.003399, 0.999912 };
    double T[3] = { -0.006699, 0.000942, -0.033191 };
#endif

    memcpy(self->kcal->depth_to_rgb_rot, R, 9*sizeof(double));
    memcpy(self->kcal->depth_to_rgb_translation, T, 3*sizeof(double));

    self->lcm = lcm;
    self->viewer = viewer;
    self->pw = BOT_GTK_PARAM_WIDGET(bot_gtk_param_widget_new());

    self->uncompress_buffer = NULL;
    self->uncompress_buffer_size = 0;

    renderer->draw = _draw;
    renderer->destroy = _free;
    renderer->name = "Kinect";
    renderer->widget = GTK_WIDGET(self->pw);
    renderer->enabled = 1;
    renderer->user = self;

    g_signal_connect (G_OBJECT (self->pw), "changed",
                      G_CALLBACK (on_param_widget_changed), self);

    kinect_frame_msg_t_subscribe(self->lcm, "KINECT_FRAME", on_kinect_frame, self);

    bot_viewer_add_renderer(viewer, renderer, priority);
}
