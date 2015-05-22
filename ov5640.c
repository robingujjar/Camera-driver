/* linux/drivers/media/video/ov5640.c
 *
 * Copyright (c) 2010 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com/
 * 
 * This file implement the Ominivision OV5640 camera sensor driver. 
 *
 * Driver code  is derived form the s5k4ba sensor from Samsung Electronics
 * 1/4" 2.0Mp CMOS Image Sensor SoC with an Embedded Image Processor
 *  
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/version.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/s5k4ba_platform.h>

#ifdef CONFIG_VIDEO_SAMSUNG_V4L2
#include <linux/videodev2_samsung.h>
#endif

#include "s5k4ba.h"

/* maximum time for one frame at minimum fps (15fps) in normal mode */
#define NORMAL_MODE_MAX_ONE_FRAME_DELAY_MS     67



#define S5K4BA_DRIVER_NAME	"OV5640"
#define SAMPLE_CODE 0
/* Default resolution & pixelformat. plz ref s5k4ba_platform.h */
#define DEFAULT_RES		WVGA	/* Index of resoultion */
#define DEFAUT_FPS_INDEX	S5K4BA_15FPS
#define DEFAULT_FMT		V4L2_PIX_FMT_UYVY	/* YUV422 */

/* result values returned to HAL */
enum {
        AUTO_FOCUS_FAILED,
        AUTO_FOCUS_DONE,
        AUTO_FOCUS_CANCELLED,
};

enum af_operation_status {
        AF_NONE = 0,
        AF_START,
        AF_CANCEL,
        AF_INITIAL,
};

enum s5k4ba_oprmode {
        S5K4BA_OPRMODE_VIDEO = 0,
        S5K4BA_OPRMODE_IMAGE = 1,
};


/*
 * Specification
 * Parallel : ITU-R. 656/601 YUV422, RGB565, RGB888 (Up to VGA), RAW10
 * Serial : MIPI CSI2 (single lane) YUV422, RGB565, RGB888 (Up to VGA), RAW10
 * Resolution : 1280 (H) x 1024 (V)
 * Image control : Brightness, Contrast, Saturation, Sharpness, Glamour
 * Effect : Mono, Negative, Sepia, Aqua, Sketch
 * FPS : 15fps @full resolution, 30fps @VGA, 24fps @720p
 * Max. pixel clock frequency : 48MHz(upto)
 * Internal PLL (6MHz to 27MHz input frequency)
 */

/* Camera functional setting values configured by user concept */
struct s5k4ba_userset {
	signed int exposure_bias;	/* V4L2_CID_EXPOSURE */
	unsigned int ae_lock;
	unsigned int awb_lock;
	unsigned int auto_wb;	/* V4L2_CID_AUTO_WHITE_BALANCE */
	unsigned int manual_wb;	/* V4L2_CID_WHITE_BALANCE_PRESET */
	unsigned int wb_temp;	/* V4L2_CID_WHITE_BALANCE_TEMPERATURE */
	unsigned int effect;	/* Color FX (AKA Color tone) */
	unsigned int contrast;	/* V4L2_CID_CONTRAST */
	unsigned int saturation;	/* V4L2_CID_SATURATION */
	unsigned int sharpness;		/* V4L2_CID_SHARPNESS */
	unsigned int glamour;
};

struct s5k4ba_version {
        u32 major;
        u32 minor;
};

struct s5k4ba_date_info {
        u32 year;
        u32 month;
        u32 date;
};

enum s5k4ba_runmode {
        S5K4BA_RUNMODE_NOTREADY,
        S5K4BA_RUNMODE_IDLE,
        S5K4BA_RUNMODE_RUNNING,
        S5K4BA_RUNMODE_CAPTURE,
};


struct s5k4ba_firmware {
        u32 addr;
        u32 size;
};

struct s5k4ba_jpeg_param {
        u32 enable;
        u32 quality;
        u32 main_size;          /* Main JPEG file size */
        u32 thumb_size;         /* Thumbnail file size */
        u32 main_offset;
        u32 thumb_offset;
        u32 postview_offset;
};

struct s5k4ba_position {
        int x;
        int y;
};

struct gps_info_common {
        u32 direction;
        u32 dgree;
        u32 minute;
        u32 second;
};

struct s5k4ba_gps_info {
        unsigned char gps_buf[8];
        unsigned char altitude_buf[4];
        int gps_timeStamp;
};
struct s5k4ba_state {
	struct s5k4ba_platform_data *pdata;
	struct v4l2_subdev sd;
	struct v4l2_pix_format pix;
	struct v4l2_fract timeperframe; 

	struct s5k4ba_date_info dateinfo;
        struct s5k4ba_position position;
        struct v4l2_streamparm strm;
        struct s5k4ba_gps_info gps_info;
	
	struct s5k4ba_version fw;
	
	struct s5k4ba_userset userset; 
	enum af_operation_status af_status; 
	enum s5k4ba_oprmode oprmode; 
	struct mutex ctrl_lock;
	int freq;	/* MCLK in KHz */
	int is_mipi;
	int isize;
	int ver;
	int fps; 
	
	int preview_framesize_index;
        int capture_framesize_index;
        int sensor_version;
        int check_dataline;
        int check_previewdata;
        bool flash_on;
        bool torch_on;
        bool sensor_af_in_low_light_mode;
        int flash_state_on_previous_capture;
        bool initialized;
        bool restore_preview_size_needed;
        int one_frame_delay_ms;

} ;



int mycapture  =0;
static inline struct s5k4ba_state *to_state(struct v4l2_subdev *sd)
{
	return container_of(sd, struct s5k4ba_state, sd);
}
/**
 * struct ov5640_reg - ov5640 register format
 * @reg: 16-bit offset to register
 * @val: 8/16/32-bit register value
 * @length: length of the register
 *
 * Define a structure for OV5640 register initialization values
 */
struct ov5640_reg {
        u16     reg;
        u8      val;
};

static const struct ov5640_reg OV5640_CAMERA_Module_AF_POST[] ={
                             // Auto focus settings     
                         {0x3022, 0x00},
                         {0x3023, 0x00},
                         {0x3024, 0x00},
                         {0x3025, 0x00},
                         {0x3026, 0x00},
                         {0x3027, 0x00},
                         {0x3028, 0x00},
                         {0x3029, 0xFF},
                         {0x3000, 0x00},

};


static const struct ov5640_reg OV5640_EV_M2[]=
{
        {0x3a0f, 0x10},
        {0x3a10, 0x08},
        {0x3a1b, 0x10},
        {0x3a1e, 0x08},
        {0x3a11, 0x20},
        {0x3a1f, 0x10}
};
static const struct ov5640_reg  OV5640_EV_M1[] =
{
        {0x3a0f, 0x20},
        {0x3a10, 0x18},
        {0x3a11, 0x41},
        {0x3a1b, 0x20},
        {0x3a1e, 0x18},
        {0x3a1f, 0x10}
};

static const struct ov5640_reg OV5640_EV_0[] =
{
        {0x3a0f, 0x38},
        {0x3a10, 0x30},
        {0x3a11, 0x61},
        {0x3a1b, 0x38},
        {0x3a1e, 0x30},
        {0x3a1f, 0x10}
};

static const struct ov5640_reg OV5640_EV_P1[] =
{
        {0x3a0f, 0x50},
        {0x3a10, 0x48},
        {0x3a11, 0x90},
        {0x3a1b, 0x50},
        {0x3a1e, 0x48},
        {0x3a1f, 0x20}
};
static const struct ov5640_reg  OV5640_EV_P2[] =
{
        {0x3a0f, 0x60},
        {0x3a10, 0x58},
        {0x3a11, 0xa0},
        {0x3a1b, 0x60},
        {0x3a1e, 0x58},
        {0x3a1f, 0x20}
};



static const struct ov5640_reg regset_capture_resoxxxx[] = {
	{0x3503, 0x03},		// Disable AGC, AEC, 
	{0x3406, 0x01},		// Disable AWB
	//OV5640 5M KEY_110607_ByAllen.txt
	{0x3035, 0x21},
	{0x3036, 0x54},
	{0x3C07, 0x07},
	{0x3820, 0x40},
	{0x3821, 0x06},
	{0x3814, 0x11},
	{0x3815, 0x11},
	{0x3803, 0x00},
	{0x3807, 0x9f},
	//{0x3808, 0x02},		// Resolution Size, Adjust by Program 640x480
	//{0x3809, 0x80},
	//{0x380A, 0x01},
	//{0x380B, 0xE0},
//===========================
/*	{0x3808, 0x08},         // Resolution Size, Adjust by Program 2048x1536 
        {0x3809, 0x00},
        {0x380A, 0x06},
        {0x380B, 0x00},*/
//==========================QVGA ======
/*	{0x3808, 0x01},              
       	{0x3809, 0x40},
        {0x380A, 0x00},
        {0x380B, 0xf0}, */
//================ 5M  - 2592x1936 ==  
  	{0x3808, 0x0a},       
        {0x3809, 0x20},
        {0x380A, 0x07},
        {0x380B, 0x98},
//=========== 5M -xx =====================
	{0x380C, 0x0b},
	{0x380D, 0x1C},
	{0x380E, 0x07},
	{0x380F, 0xb0},
	{0x3813, 0x04},
	{0x3618, 0x04},
	{0x3612, 0x2B},
	{0x3708, 0x21},
	{0x3709, 0x12},
	{0x370C, 0x00},
	{0x3A02, 0x07},
	{0x3A03, 0xB0},
	{0x3A0E, 0x06},
	{0x3A0D, 0x08},
	{0x3A14, 0x07},
	{0x3A15, 0xD0},
	{0x4004, 0x06},
	{0x4713, 0x02},
	{0x4407, 0x0C},
	{0x460B, 0x37},
	{0x460C, 0x20},
	{0x3824, 0x01},


//===========  Changed required for 5 M = 0x83 ================
	{0x5001, 0x83}, 
	//{0x5001, 0xA3},  // 5M: 0x83, other: 0xA3
//============================================================
	{0x3008, 0x02}, 
	{0x3035, 0x21},
	{0x3821, 0x06},
	{0x3002, 0x1c},
	{0x3006, 0xc3},
	{0x4713, 0x02},
	{0x4407, 0x0c},
	{0x460b, 0x35},
	{0x460c, 0x20},
	{0x3824, 0x01},
	{0x4003, 0x82},
	{0x4003, 0x08},
       
};

static const struct ov5640_reg configscript_common1[] = {
        		{ 0x3103, 0x11},
                         { 0x3008, 0x82},
                         { 0x3008, 0x42},
                         { 0x3103, 0x03},
                         { 0x3017, 0xff},
                         { 0x3018, 0xff},
                         { 0x3034, 0x1a},
                         { 0x3035, 0x11},
                         { 0x3036, 0x46},
                         { 0x3037, 0x13},
                         { 0x3108, 0x01},
                         { 0x3630, 0x36},
                         { 0x3631, 0x0e},
                         { 0x3632, 0xe2},
                         { 0x3633, 0x12},
                         { 0x3621, 0xe0},
                         { 0x3704, 0xa0},
                         { 0x3703, 0x5a},
                         { 0x3715, 0x78},
                         { 0x3717, 0x01},
                         { 0x370b, 0x60},
                         { 0x3705, 0x1a},
                         { 0x3905, 0x02},
                         { 0x3906, 0x10},
                         { 0x3901, 0x0a},
                         { 0x3731, 0x12},
                         { 0x3600, 0x08},
                         { 0x3601, 0x33},
                         { 0x302d, 0x60},
                         { 0x3620, 0x52},
                         { 0x371b, 0x20},
                         { 0x471c, 0x50},
                         { 0x3a13, 0x43},
                         { 0x3a18, 0x00},
                         { 0x3a19, 0xf8},
                         { 0x3635, 0x13},
                         { 0x3636, 0x03},
                         { 0x3634, 0x40},
                         { 0x3622, 0x01},
                         { 0x3c01, 0x34},
                         { 0x3c04, 0x28},
                         { 0x3c05, 0x98},
                         { 0x3c06, 0x00},
                         { 0x3c07, 0x08},
                         { 0x3c08, 0x00},
                         { 0x3c09, 0x1c},
                         { 0x3c0a, 0x9c},
                         { 0x3c0b, 0x40},
                         { 0x3820, 0x41},
                         { 0x3821, 0x07},
                         { 0x3814, 0x31},
                         { 0x3815, 0x31},
                         { 0x3800, 0x00},
                         { 0x3801, 0x00},
                         { 0x3802, 0x00},
                         { 0x3803, 0x04},
                         { 0x3804, 0x0a},
                         { 0x3805, 0x3f},
                         { 0x3806, 0x07},
                         { 0x3807, 0x9b},
                         { 0x3808, 0x02},
                         { 0x3809, 0x80},
                         { 0x380a, 0x01},
                         { 0x380b, 0xe0},
                         { 0x380c, 0x07},
                         { 0x380d, 0x68},
                         { 0x380e, 0x03},
                         { 0x380f, 0xd8},
                         { 0x3810, 0x00},
                         { 0x3811, 0x10},
                         { 0x3812, 0x00},
                         { 0x3813, 0x06},
                         { 0x3618, 0x00},
                         { 0x3612, 0x29},
                         { 0x3708, 0x62},
                         { 0x3709, 0x52},
                         { 0x370c, 0x03},
                         { 0x3a02, 0x03},
                         { 0x3a03, 0xd8},
                         { 0x3a08, 0x01},
                         { 0x3a09, 0x27},
                         { 0x3a0a, 0x00},
                         { 0x3a0b, 0xf6},
                         { 0x3a0e, 0x03},
                         { 0x3a0d, 0x04},
                         { 0x3a14, 0x03},
                         { 0x3a15, 0xd8},
                         { 0x4001, 0x02},
                         { 0x4004, 0x02},
                         { 0x3000, 0x00},
                         { 0x3002, 0x1c},
                         { 0x3004, 0xff},
                         { 0x3006, 0xc3},
                         { 0x300e, 0x58},
                         { 0x302e, 0x00},
                         { 0x4300, 0x30},		//YUV422  YUYV 30
                         { 0x501f, 0x00},
                         { 0x4713, 0x03},
                         { 0x4407, 0x04},
                         { 0x440e, 0x00},
                         { 0x460b, 0x35},
                         { 0x460c, 0x22},
                         { 0x3824, 0x02},
                         { 0x5000, 0xa7},
                         { 0x5001, 0xa3},
                         { 0x5180, 0xff},
                         { 0x5181, 0xf2},
                         { 0x5182, 0x00},
                         { 0x5183, 0x14},
                         { 0x5184, 0x25},
                         { 0x5185, 0x24},
                         { 0x5186, 0x09},
                         { 0x5187, 0x09},
                         { 0x5188, 0x09},
                         { 0x5189, 0x75},
                         { 0x518a, 0x54},
                         { 0x518b, 0xe0},
                         { 0x518c, 0xb2},
                         { 0x518d, 0x42},
                         { 0x518e, 0x3d},
                         { 0x518f, 0x56},
                         { 0x5190, 0x46},
                         { 0x5191, 0xf8},
                         { 0x5192, 0x04},
                         { 0x5193, 0x70},
                         { 0x5194, 0xf0},

                         { 0x5195, 0xf0},

                         { 0x5196, 0x03},

                         { 0x5197, 0x01},

                         { 0x5198, 0x04},

                         { 0x5199, 0x12},

                         { 0x519a, 0x04},

                         { 0x519b, 0x00},

                         { 0x519c, 0x06},

                         { 0x519d, 0x82},

                         { 0x519e, 0x38},

                         { 0x5381, 0x1e},

                         { 0x5382, 0x5b},

                         { 0x5383, 0x08},

                         { 0x5384, 0x0a},

                         { 0x5385, 0x7e},

                         { 0x5386, 0x88},

                         { 0x5387, 0x7c},

                         { 0x5388, 0x6c},

                         { 0x5389, 0x10},

                         { 0x538a, 0x01},

                         { 0x538b, 0x98},

                         { 0x5300, 0x08},

                         { 0x5301, 0x30},

                         { 0x5302, 0x10},

                         { 0x5303, 0x00},

                         { 0x5304, 0x08},

                         { 0x5305, 0x30},

                         { 0x5306, 0x08},

                         { 0x5307, 0x16},

                         { 0x5309, 0x08},

                         { 0x530a, 0x30},

                         { 0x530b, 0x04},

                         { 0x530c, 0x06},

                         { 0x5480, 0x01},

                         { 0x5481, 0x08},

                         { 0x5482, 0x14},

                         { 0x5483, 0x28},

                         { 0x5484, 0x51},

                         { 0x5485, 0x65},

                         { 0x5486, 0x71},

                         { 0x5487, 0x7d},

                         { 0x5488, 0x87},

                         { 0x5489, 0x91},

                         { 0x548a, 0x9a},

                         { 0x548b, 0xaa},

                         { 0x548c, 0xb8},

                         { 0x548d, 0xcd},

                         { 0x548e, 0xdd},

                         { 0x548f, 0xea},

                         { 0x5490, 0x1d},

                         { 0x5580, 0x02},

                         { 0x5583, 0x40},

                         { 0x5584, 0x10},

                         { 0x5589, 0x10},

                         { 0x558a, 0x00},

                         { 0x558b, 0xf8},

                         { 0x5800, 0x23},

                         { 0x5801, 0x14},

                         { 0x5802, 0x0f},

                         { 0x5803, 0x0f},

                         { 0x5804, 0x12},

                         { 0x5805, 0x26},

                         { 0x5806, 0x0c},

                         { 0x5807, 0x08},

                         { 0x5808, 0x05},

                         { 0x5809, 0x05},

                         { 0x580a, 0x08},

                         { 0x580b, 0x0d},

                         { 0x580c, 0x08},

                         { 0x580d, 0x03},

                         { 0x580e, 0x00},

                         { 0x580f, 0x00},

                         { 0x5810, 0x03},

                         { 0x5811, 0x09},

                         { 0x5812, 0x07},

                         { 0x5813, 0x03},

                         { 0x5814, 0x00},

                         { 0x5815, 0x01},

                         { 0x5816, 0x03},

                         { 0x5817, 0x08},

                         { 0x5818, 0x0d},

                         { 0x5819, 0x08},

                         { 0x581a, 0x05},

                         { 0x581b, 0x06},

                         { 0x581c, 0x08},

                         { 0x581d, 0x0e},

                         { 0x581e, 0x29},

                         { 0x581f, 0x17},

                         { 0x5820, 0x11},

                         { 0x5821, 0x11},

                         { 0x5822, 0x15},

                         { 0x5823, 0x28},

                         { 0x5824, 0x46},

                         { 0x5825, 0x26},

                         { 0x5826, 0x08},

                         { 0x5827, 0x26},

                         { 0x5828, 0x64},

                         { 0x5829, 0x26},

                         { 0x582a, 0x24},

                         { 0x582b, 0x22},

                         { 0x582c, 0x24},

                         { 0x582d, 0x24},

                         { 0x582e, 0x06},

                         { 0x582f, 0x22},

                         { 0x5830, 0x40},

                         { 0x5831, 0x42},

                         { 0x5832, 0x24},

                         { 0x5833, 0x26},

                         { 0x5834, 0x24},

                         { 0x5835, 0x22},

                         { 0x5836, 0x22},

                         { 0x5837, 0x26},

                         { 0x5838, 0x44},

                         { 0x5839, 0x24},

                         { 0x583a, 0x26},

                         { 0x583b, 0x28},

                         { 0x583c, 0x42},

                         { 0x583d, 0xce},

                         { 0x5025, 0x00},

                         { 0x3a0f, 0x30},

                         { 0x3a10, 0x28},

                         { 0x3a1b, 0x30},

                         { 0x3a1e, 0x26},

                         { 0x3a11, 0x60},

                         { 0x3a1f, 0x14},

                         { 0x3008, 0x02},

	                     //{{ 0x30, 0x31, 0x08}},		// Disable Internal LDO

                         { 0x3503, 0x00},	// Enable AGC}, AEC == Allen's 3A

                         { 0x3406, 0x00},	// Enable AWB





                         { 0x3035, 0x11},

                         { 0x3036, 0x46},

                         { 0x3C07, 0x08},

                         { 0x3820, 0x41},

                         { 0x3821, 0x07},

                         { 0x3814, 0x31},

                         { 0x3815, 0x31},

                         { 0x3803, 0x04},

                         { 0x3807, 0x9B},

                         { 0x3808, 0x02},

                         { 0x3809, 0x80},

                         { 0x380A, 0x01},

                         { 0x380B, 0xE0},

                         { 0x380C, 0x07},

                         { 0x380D, 0x68},

                         { 0x380E, 0x03},

                         { 0x380F, 0xD8},

                         { 0x3813, 0x06},

                         { 0x3618, 0x00},

                         { 0x3612, 0x29},

                         { 0x3708, 0x62},

                         { 0x3709, 0x52},

                         { 0x370C, 0x03},

                         { 0x3A02, 0x03},

                         { 0x3A03, 0xD8},

                         { 0x3A0E, 0x03},

                         { 0x3A0D, 0x04},

                         { 0x3A14, 0x03},

                         { 0x3A15, 0xD8},

                         { 0x4004, 0x02},

                         { 0x4713, 0x03},

                         { 0x4407, 0x04},

                         { 0x460B, 0x35},

                         { 0x460C, 0x22},

                         { 0x3824, 0x02},

                         { 0x5001, 0xA3},

                         { 0x3008, 0x02},

                         

                         { 0x3035, 0x11},	//Modify for Zoom issue

                         { 0x3821, 0x06},

                         { 0x3002, 0x1c},

                         { 0x3006, 0xc3},

                         { 0x4713, 0x02},

                         { 0x4407, 0x0c},

                         { 0x460b, 0x35},

                         { 0x460c, 0x22},	//Modify for Zoom issue

                         { 0x3824, 0x02},	//Modify for Zoom issue

                         

                         { 0x3622, 0x01}, 

                         { 0x3635, 0x1c}, 

                         { 0x3634, 0x40},

                         { 0x3c01, 0x34},

                         { 0x3c00, 0x00},

                         { 0x3c04, 0x28},

                         { 0x3c05, 0x98},

                         { 0x3c06, 0x00},

                         { 0x3c07, 0x08},

                         { 0x3c08, 0x00},

                         { 0x3c09, 0x1c},

                         { 0x300c, 0x22},

                         { 0x3c0a, 0x9c},

                         { 0x3c0b, 0x40},	


	/*{ 0x3103, 0x03 },
        { 0x3017, 0x00 },
        { 0x3018, 0x00 },
        { 0x3630, 0x2e },
        { 0x3632, 0xe2 },
        { 0x3633, 0x23 },
        { 0x3634, 0x44 },
        { 0x3621, 0xe0 },
        { 0x3704, 0xa0 },
        { 0x3703, 0x5a },
        { 0x3715, 0x78 },
        { 0x3717, 0x01 },
        { 0x370b, 0x60 },
        { 0x3705, 0x1a },
        { 0x3905, 0x02 },
        { 0x3906, 0x10 },
        { 0x3901, 0x0a },
        { 0x3731, 0x12 },
        { 0x3600, 0x04 },
        { 0x3601, 0x22 },
        { 0x471c, 0x50 },
        { 0x3002, 0x1c },
        { 0x3006, 0xc3 },
        { 0x300e, 0x05 },
        { 0x302e, 0x08 },
        { 0x3612, 0x4b },
        { 0x3618, 0x04 },
        { 0x3034, 0x18 },
        { 0x3035, 0x11 },
        { 0x3036, 0x54 },
        { 0x3037, 0x13 },
        { 0x3708, 0x21 },
        { 0x3709, 0x12 },
        { 0x370c, 0x00 }, */
};

static const struct ov5640_reg regset_vga_preview[] = {
			 { 0x3035, 0x11},
                         { 0x3036, 0x46},
                         { 0x3C07, 0x08},
                         { 0x3820, 0x41},
                         { 0x3821, 0x07},
                         { 0x3814, 0x31},
                         { 0x3815, 0x31},
                         { 0x3803, 0x04},
                         { 0x3807, 0x9B},
                         { 0x3808, 0x02},
                         { 0x3809, 0x80},
                         { 0x380A, 0x01},
                         { 0x380B, 0xE0},
                         { 0x380C, 0x07},
                         { 0x380D, 0x68},
                         { 0x380E, 0x03},
                         { 0x380F, 0xD8},
                         { 0x3813, 0x06},
                         { 0x3618, 0x00},
                         { 0x3612, 0x29},
	         	 { 0x3708, 0x62},
                         { 0x3709, 0x52},
                         { 0x370C, 0x03},
                         { 0x3A02, 0x03},
                         { 0x3A03, 0xD8},
                         { 0x3A0E, 0x03},
                         { 0x3A0D, 0x04},
                         { 0x3A14, 0x03},
                         { 0x3A15, 0xD8},
                         { 0x4004, 0x02},
                         { 0x4713, 0x03},
                         { 0x4407, 0x04},
                         { 0x460B, 0x35},
                         { 0x460C, 0x22},
                         { 0x3824, 0x02},
                         { 0x5001, 0xA3},
                         { 0x3008, 0x02},
                         { 0x3035, 0x11},       //Modify for Zoom issue
                         { 0x3821, 0x06},
			 { 0x3002, 0x1c},
                         { 0x3006, 0xc3},
                         { 0x4713, 0x02},
                         { 0x4407, 0x0c},
                         { 0x460b, 0x35},
                         { 0x460c, 0x22},       //Modify for Zoom issue
                         { 0x3824, 0x02},       //Modify for Zoom issue

};

static const struct ov5640_reg regset_auto_focus[] = {
	

};

static int ov5640_reg_read(struct v4l2_subdev *sd, u16 reg, u8 *val)
{
        struct i2c_client *client = v4l2_get_subdevdata(sd);
        int ret;
        u8 data[2] = {0};
        struct i2c_msg msg = {
                .addr   = client->addr,
                .flags  = 0,
                .len    = 2,
                .buf    = data,
        };

        data[0] = (u8)(reg >> 8);
        data[1] = (u8)(reg & 0xff);

        ret = i2c_transfer(client->adapter, &msg, 1);
        if (ret < 0)
                goto err;

        msg.flags = I2C_M_RD;
        msg.len = 1;
        ret = i2c_transfer(client->adapter, &msg, 1);
        if (ret < 0)
                goto err;

        *val = data[0];
        return 0;

err:
        dev_err(&client->dev, "Failed reading register 0x%02x!\n", reg);
        return ret;
}

/**
 * s5k4ecgx_i2c_read_twobyte: Read 2 bytes from sensor
 */
static int s5k4ecgx_i2c_read_twobyte(struct v4l2_subdev *sd,
                                  u16 subaddr, u16 *data)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
        int err;
        unsigned char buf[2];
        struct i2c_msg msg[2];

        cpu_to_be16s(&subaddr);

        msg[0].addr = client->addr;
        msg[0].flags = 0;
        msg[0].len = 2;
        msg[0].buf = (u8 *)&subaddr;

        msg[1].addr = client->addr;
        msg[1].flags = I2C_M_RD;
        msg[1].len = 2;
        msg[1].buf = buf;

        err = i2c_transfer(client->adapter, msg, 2);
        if (unlikely(err != 2)) {
                dev_err(&client->dev,
                        "%s: register read fail\n", __func__);
                return -EIO;
        }

        *data = ((buf[0] << 8) | buf[1]);

        return 0;
} 



/**
 * Write a value to a register in ov5640 sensor device.
 * @client: i2c driver client structure.
 * @reg: Address of the register to read value from.
 * @val: Value to be written to a specific register.
 * Returns zero if successful, or non-zero otherwise.
 */
static int ov5640_reg_write(struct v4l2_subdev *sd, u16 reg, u8 val)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
        int ret;
        unsigned char data[3] = { (u8)(reg >> 8), (u8)(reg & 0xff), val };
        struct i2c_msg msg = {
                .addr   = client->addr,
                .flags  = 0,
                .len    = 3,
                .buf    = data,
        };

        ret = i2c_transfer(client->adapter, &msg, 1);
        if (ret < 0) {
                dev_err(&client->dev, "Failed writing register 0x%02x!\n", reg);
                return ret;
        }

        return 0;
}
/**
 * Initialize a list of ov5640 registers.
 * The list of registers is terminated by the pair of values
 * @client: i2c driver client structure.
 * @reglist[]: List of address of the registers to write data.
 * Returns zero if successful, or non-zero otherwise.
 */
static int ov5640_reg_writes(struct v4l2_subdev *sd,
                             const struct ov5640_reg reglist[],
                             int size)
{
        int err = 0, i;

        for (i = 0; i < size; i++) {
                err = ov5640_reg_write(sd, reglist[i].reg,
                                reglist[i].val);
                if (err)
                        return err; 
		//u8 value = 0;
        	//ov5640_reg_read(sd, reglist[i].reg, &value);
		mdelay(1);
		//printk(" register (%x ,  %x)\n",reglist[i].reg, value );
        }
        return 0;
}
static int ov540_block_writes(struct v4l2_subdev *sd, const struct ov5640_reg reglist[],int size){ 

      struct i2c_client *client = v4l2_get_subdevdata(sd);
      int err = 0, i; 
      u16     startaddr;
      u16       preaddr;
      u16         addr;
      u8      data[256];
      
      unsigned int length;
      length    = 0;
      startaddr = 0;
      preaddr   = 0; 
      
      for (i = 0; i < size; i++) {
	addr = reglist[i].reg; 

          if(length == 0){
                 startaddr      = addr;
             data[length++] = reglist[i].val;// buf[i+2];
             preaddr        = addr+1;
          }
          else{
             if((addr != preaddr) || (length > 192)){
		u8 buf[4+length];		
        	unsigned int j =0;
        	struct i2c_msg msg = {client->addr, 0, 2+length, buf};
        	buf[0] = (u8)((startaddr>>8)& 0x00FF);
        	buf[1] = (u8)((startaddr>>0)& 0x00FF);
        	//printk(" [ Buf[0] = 0x%x and buf[1] = 0x%x",buf[0],buf[1]);
        	for (j = 0; j < length; j++){
                	buf[2+j] = data[j];
        		//printk("\n buf[2+%d] = %x",j,buf[2+j]);
		}
		int ret;
		ret = i2c_transfer(client->adapter, &msg, 1);
                if (ret < 0) {
                    dev_err(&client->dev, "Failed writing register 0x%02x!\n", startaddr);
                    return ret;
        	} 
		mdelay(1);

                startaddr = addr;
                length    = 0;
             }
             data[length++] = reglist[i].val;
             preaddr        = addr+1;
          }
   
      } 
      if(length){
		u8 buf[length];
                unsigned int k =0;
                struct i2c_msg msg = {client->addr, 0, 2+length, buf};
                buf[0] = (u8)((startaddr>>8)& 0x00FF);
                buf[1] = (u8)((startaddr>>0)& 0x00FF);
                //printk(" [ Buf[0] = 0x%x and buf[1] = 0x%x",buf[0],buf[1]);
                for (k = 0; k < length; k++)
                      buf[2+k] = data[k];
                //printk("\n buf[2+%d] = %x",i,buf[2+i]);
                     int ret;
                     ret = i2c_transfer(client->adapter, &msg, 1);
                     if (ret < 0) {
                            dev_err(&client->dev, "Failed writing register 0x%02x!\n", startaddr);
                        return ret;
                     }
		    mdelay(1);
      }
        return 0;
}

static int  ov5640_firmware_download_af(struct v4l2_subdev *sd){

        u8 *firmwarebuf = (unsigned char *)OV5640_CAMERA_Module_AF_Init_DATA;
        unsigned int length = sizeof(OV5640_CAMERA_Module_AF_Init_DATA ); 
	//printk(" Firmware buffer length = %d \n ",length);

        struct i2c_client *client = v4l2_get_subdevdata(sd);

        int err = 0; u8 value = 0;
        err = ov5640_reg_write(sd, 0x3000,0x20);
        if(err){
                printk("\n Error in fimware start download start cmd.{0x3000,0x20 }");
        }
        mdelay(1);
        ov5640_reg_read(sd, 0x3000, &value);
        printk(" Frimware download start(%x ,  %x)\n",0x3000, value );

        u8 buf[2+length];
	unsigned int i =0;
        struct i2c_msg msg = {client->addr, 0, 2+length, buf};
        u16 addr = 0x8000;
        buf[0] = (u8)((addr>>8)& 0x00FF);
        buf[1] = (u8)((addr>>0)& 0x00FF);

        //printk(" [ Buf[0] = 0x%x and buf[1] = 0x%x",buf[0],buf[1]);
        for (i = 0; i < length; i++) 
                buf[2+i] = firmwarebuf[i]; 
	//	printk("\n buf[2+%d] = %x",i,buf[2+i]);
	

	
        int ret;

        ret = i2c_transfer(client->adapter, &msg, 1);
        if (ret < 0) {
                dev_err(&client->dev, "Failed writing register 0x%02x!\n", addr);
                return ret;
        }

        //err = ov5640_reg_writes(sd, OV5640_CAMERA_Module_AF_POST,
          //    ARRAY_SIZE(OV5640_CAMERA_Module_AF_POST));
	err = ov540_block_writes(sd, OV5640_CAMERA_Module_AF_POST,ARRAY_SIZE(OV5640_CAMERA_Module_AF_POST));
	if (err){
                printk(" OV5640 AF setting failed ");
        }

	return 0;
}

/*
 * S5K4BA register structure : 2bytes address, 2bytes value
 * retry on write failure up-to 5 times
 */
static inline int s5k4ba_write(struct v4l2_subdev *sd, u8 addr, u8 val)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct i2c_msg msg[1];
	unsigned char reg[2];
	int err = 0;
	int retry = 0;


	if (!client->adapter)
		return -ENODEV;

again:
	msg->addr = client->addr;
	msg->flags = 0;
	msg->len = 2;
	msg->buf = reg;

	reg[0] = addr & 0xff;
	reg[1] = val & 0xff;

	err = i2c_transfer(client->adapter, msg, 1);
	if (err >= 0)
		return err;	/* Returns here on success */

	/* abnormal case: retry 5 times */
	if (retry < 5) {
		dev_err(&client->dev, "%s: address: 0x%02x%02x, " \
			"value: 0x%02x%02x\n", __func__, \
			reg[0], reg[1], reg[2], reg[3]);
		retry++;
		goto again;
	}

	return err;
}

static int s5k4ba_i2c_write(struct v4l2_subdev *sd, unsigned char i2c_data[],
				unsigned char length)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	unsigned char buf[length], i;
	struct i2c_msg msg = {client->addr, 0, length, buf};

	for (i = 0; i < length; i++)
		buf[i] = i2c_data[i];

	return i2c_transfer(client->adapter, &msg, 1) == 1 ? 0 : -EIO;
}

static int s5k4ba_write_regs(struct v4l2_subdev *sd, unsigned char regs[],
				int size)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int i, err;

	for (i = 0; i < size; i++) {
		err = s5k4ba_i2c_write(sd, &regs[i], sizeof(regs[i]));
		if (err < 0)
			v4l_info(client, "%s: register set failed\n", \
			__func__);
	}

	return 0;	/* FIXME */
}

static const char *s5k4ba_querymenu_wb_preset[] = {
	"WB Tungsten", "WB Fluorescent", "WB sunny", "WB cloudy", NULL
};

static const char *s5k4ba_querymenu_effect_mode[] = {
	"Effect Sepia", "Effect Aqua", "Effect Monochrome",
	"Effect Negative", "Effect Sketch", NULL
};

static const char *s5k4ba_querymenu_ev_bias_mode[] = {
	"-3EV",	"-2,1/2EV", "-2EV", "-1,1/2EV",
	"-1EV", "-1/2EV", "0", "1/2EV",
	"1EV", "1,1/2EV", "2EV", "2,1/2EV",
	"3EV", NULL
};

static struct v4l2_queryctrl s5k4ba_controls[] = {
	{
		/*
		 * For now, we just support in preset type
		 * to be close to generic WB system,
		 * we define color temp range for each preset
		 */
		.id = V4L2_CID_WHITE_BALANCE_TEMPERATURE,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "White balance in kelvin",
		.minimum = 0,
		.maximum = 10000,
		.step = 1,
		.default_value = 0,	/* FIXME */
	},
	{
		.id = V4L2_CID_WHITE_BALANCE_PRESET,
		.type = V4L2_CTRL_TYPE_MENU,
		.name = "White balance preset",
		.minimum = 0,
		.maximum = ARRAY_SIZE(s5k4ba_querymenu_wb_preset) - 2,
		.step = 1,
		.default_value = 0,
	},
	{
		.id = V4L2_CID_AUTO_WHITE_BALANCE,
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.name = "Auto white balance",
		.minimum = 0,
		.maximum = 1,
		.step = 1,
		.default_value = 0,
	},
	{
		.id = V4L2_CID_EXPOSURE,
		.type = V4L2_CTRL_TYPE_MENU,
		.name = "Exposure bias",
		.minimum = 0,
		.maximum = ARRAY_SIZE(s5k4ba_querymenu_ev_bias_mode) - 2,
		.step = 1,
		.default_value = \
			(ARRAY_SIZE(s5k4ba_querymenu_ev_bias_mode) - 2) / 2,
			/* 0 EV */
	},
	{
		.id = V4L2_CID_COLORFX,
		.type = V4L2_CTRL_TYPE_MENU,
		.name = "Image Effect",
		.minimum = 0,
		.maximum = ARRAY_SIZE(s5k4ba_querymenu_effect_mode) - 2,
		.step = 1,
		.default_value = 0,
	},
	{
		.id = V4L2_CID_CONTRAST,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Contrast",
		.minimum = 0,
		.maximum = 4,
		.step = 1,
		.default_value = 2,
	},
	{
		.id = V4L2_CID_SATURATION,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Saturation",
		.minimum = 0,
		.maximum = 4,
		.step = 1,
		.default_value = 2,
	},
	{
		.id = V4L2_CID_SHARPNESS,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Sharpness",
		.minimum = 0,
		.maximum = 4,
		.step = 1,
		.default_value = 2,
	},
};

const char * const *s5k4ba_ctrl_get_menu(u32 id)
{
	switch (id) {
	case V4L2_CID_WHITE_BALANCE_PRESET:
		return s5k4ba_querymenu_wb_preset;

	case V4L2_CID_COLORFX:
		return s5k4ba_querymenu_effect_mode;

	case V4L2_CID_EXPOSURE:
		return s5k4ba_querymenu_ev_bias_mode;

	default:
		return v4l2_ctrl_get_menu(id);
	}
}

static inline struct v4l2_queryctrl const *s5k4ba_find_qctrl(int id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(s5k4ba_controls); i++)
		if (s5k4ba_controls[i].id == id)
			return &s5k4ba_controls[i];

	return NULL;
}

static int s5k4ba_queryctrl(struct v4l2_subdev *sd, struct v4l2_queryctrl *qc)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(s5k4ba_controls); i++) {
		if (s5k4ba_controls[i].id == qc->id) {
			memcpy(qc, &s5k4ba_controls[i], \
				sizeof(struct v4l2_queryctrl));
			return 0;
		}
	}

	return -EINVAL;
}

static int s5k4ba_querymenu(struct v4l2_subdev *sd, struct v4l2_querymenu *qm)
{
	struct v4l2_queryctrl qctrl;

	qctrl.id = qm->id;
	s5k4ba_queryctrl(sd, &qctrl);

	return v4l2_ctrl_query_menu(qm, &qctrl, s5k4ba_ctrl_get_menu(qm->id));
}

/*
 * Clock configuration
 * Configure expected MCLK from host and return EINVAL if not supported clock
 * frequency is expected
 *	freq : in Hz
 *	flag : not supported for now
 */
static int s5k4ba_s_crystal_freq(struct v4l2_subdev *sd, u32 freq, u32 flags)
{
	int err = -EINVAL;

	return err;
}

static int ov5640_enum_framesizes(struct v4l2_subdev *sd, \
					struct v4l2_frmsizeenum *fsize)
{
        struct s5k4ba_state *state = to_state(sd);
        u32 err, old_mode;

        /*
        * The camera interface should read this value, this is the resolution
        * at which the sensor would provide framedata to the camera i/f
        * In case of image capture,
        * this returns the default camera resolution (VGA)
        */ 
	printk("\n  <<<< Enumerating sensor capture width and height !!!");

		
	if(mycapture == 1 ) {
		printk("\n  <<<< Enumerating sensor capture width and height !!! mycapture = 1");
		fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
        	fsize->discrete.width = 2592;////2048;
        	fsize->discrete.height = 1936;//1536;
	}else{
		printk("\n  <<<< Enumerating sensor capture width and height mycapture = 0");
		fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
                fsize->discrete.width = 640;
                fsize->discrete.height = 480;
		
	} 
	
 
	mycapture = 0;

        return 0;

}
/* called by HAL after auto focus was started to get the first search result*/
static int ov5640_get_auto_focus_result_first(struct v4l2_subdev *sd,
                                        struct v4l2_control *ctrl)
{
        struct i2c_client *client = v4l2_get_subdevdata(sd);
        struct s5k4ba_state *state =
                container_of(sd, struct s5k4ba_state, sd);
        u16 read_value;
        int ret = 0;
	
	int ret_val = as3643_flash_off();
                printk("\n ov5640 ==> flash offf ==> ret_value = 0x%0x",ret_val);

        if (state->af_status == AF_INITIAL) {
                dev_dbg(&client->dev, "%s: Check AF Result\n", __func__); 
		printk("\n %s: Check AF Result\n", __func__);
                if (state->af_status == AF_NONE) {
                        dev_dbg(&client->dev,
                                "%s: auto focus never started, returning 0x2\n",
                                __func__);
                        ctrl->value = AUTO_FOCUS_CANCELLED;
                        return 0;
                }

                state->af_status = AF_START;
        } else if (state->af_status == AF_CANCEL) {
                dev_dbg(&client->dev,
                        "%s: AF is cancelled while doing\n", __func__) ;
		printk("\n %s: AF is cancelled while doing \n", __func__);
                ctrl->value = AUTO_FOCUS_CANCELLED;
                return 0;
        }
//      (client, 0x0F12, &read_value);

        u8 read_byte = 0;

        ret = ov5640_reg_read(sd, 0x3023, &read_byte);
        if (ret) {
                printk("Failed to read auto focus result 0x3023.\n");

        }else{
		printk("%s: 1st i2c_read 0x3023 --- read_value == 0x%x\n",
                                __func__, read_byte);
                if(read_byte == 0x01){
                        ctrl->value = read_byte;
                        return 0; //Robin Need to check it .
                }
        }

        ret = ov5640_reg_read(sd, 0x3028, &read_byte);
        if (ret) {
                printk("Failed to read auto focus result 0x3028.\n");

        }else{
		printk("%s: 1st i2c_read ---0x3028  read_value == 0x%x\n",
                                        __func__, read_byte);
                if(read_byte){
                        ctrl->value = read_byte;
                        return 0;
                }else{
                        ctrl->value = 0;
                        return 0;
                }
        }
        printk("%s: 1st i2c_read --- outer loop read_value == 0x%x\n", __func__, read_byte);

        return 0;

}



static int s5k4ba_enum_frameintervals(struct v4l2_subdev *sd,
					struct v4l2_frmivalenum *fival)
{
	int err = 0;

	return err;
}

static int ov5640_g_parm(struct v4l2_subdev *sd, struct v4l2_streamparm *param)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int err = 0;

	dev_dbg(&client->dev, "%s\n", __func__);

	return err;
}

static int ov5640_set_flash_mode(struct v4l2_subdev *sd, int value)
{
        struct s5k4ba_state *state =
                container_of(sd, struct s5k4ba_state, sd);
        struct sec_cam_parm *parms =
                (struct sec_cam_parm *)&state->strm.parm.raw_data;
	/*
        if (parms->flash_mode == value)
                return 0;

        if ((value >= FLASH_MODE_OFF) && (value <= FLASH_MODE_TORCH)) {
                pr_debug("%s: setting flash mode to %d\n",
                        __func__, value);
                parms->flash_mode = value;
                if (parms->flash_mode == FLASH_MODE_TORCH)
                        s5k4ecgx_enable_torch(sd);
                else
                        s5k4ecgx_disable_torch(sd);
                return 0;
        }

        */

        printk("\n AS3643 Torch ON or OFF = %d",value);

        if ((value >= FLASH_MODE_OFF) && (value <= FLASH_MODE_TORCH)) {
                printk("%s: setting flash mode to %d\n",
                        __func__, value);
                if (value == FLASH_MODE_TORCH) {
			printk("\n parms->flash_mode = %d , Test: =FLASH_MODE_OFF =%d",parms->flash_mode,FLASH_MODE_OFF); 
			if (parms->flash_mode !=FLASH_MODE_OFF)
                        	as3643_torch_mode_on();
                } else {
                        as3643_torch_off();
		}
                return 0;
        }


        printk("%s: trying to set invalid flash mode %d\n",
                __func__, value);
        return -EINVAL;
}

static int ov5640_s_parm(struct v4l2_subdev *sd, struct v4l2_streamparm *param)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int err = 0;

	struct s5k4ba_state *state =
                container_of(sd, struct s5k4ba_state, sd);
        struct sec_cam_parm *new_parms =
                (struct sec_cam_parm *)&param->parm.raw_data;
        struct sec_cam_parm *parms =
                (struct sec_cam_parm *)&state->strm.parm.raw_data;

	dev_dbg(&client->dev, "%s: numerator %d, denominator: %d\n", \
		__func__, param->parm.capture.timeperframe.numerator, \
		param->parm.capture.timeperframe.denominator); 
	
	err = ov5640_set_flash_mode(sd, new_parms->flash_mode);
	/* Must delay 150ms before setting macro mode due to a camera
         * sensor requirement */
        if ((new_parms->focus_mode == FOCUS_MODE_MACRO) &&
                        (parms->focus_mode != FOCUS_MODE_MACRO))
                msleep(150);
        //err |= ov5640_set_focus_mode(sd, new_parms->focus_mode);

	

	return err;
}

static int ov5640_g_ctrl(struct v4l2_subdev *sd, struct v4l2_control *ctrl)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct s5k4ba_state *state = to_state(sd);
	struct s5k4ba_userset userset = state->userset;
	int err = -EINVAL; 
	
        struct sec_cam_parm *parms =
                (struct sec_cam_parm *)&state->strm.parm.raw_data;

	mutex_lock(&state->ctrl_lock);
	switch (ctrl->id) { 
	case V4L2_CID_CAMERA_WHITE_BALANCE:
                ctrl->value = parms->white_balance;
                break;
        case V4L2_CID_CAMERA_EFFECT:
                ctrl->value = parms->effects;
                break;
        case V4L2_CID_CAMERA_CONTRAST:
                ctrl->value = parms->contrast;
                break;
        case V4L2_CID_CAMERA_SATURATION:
                ctrl->value = parms->saturation;
                break;
        case V4L2_CID_CAMERA_SHARPNESS:
                ctrl->value = parms->sharpness;
                break;
	case V4L2_CID_EXPOSURE:
		ctrl->value = userset.exposure_bias;
		err = 0;
		break;
	case V4L2_CID_CAMERA_AUTO_FOCUS_RESULT_FIRST:
                err = ov5640_get_auto_focus_result_first(sd, ctrl);
                break;
	 case V4L2_CID_CAM_DATE_INFO_YEAR:
                ctrl->value = 2013;
                break;
        case V4L2_CID_CAM_DATE_INFO_MONTH:
                ctrl->value = 1;
                break;
        case V4L2_CID_CAM_DATE_INFO_DATE:
                ctrl->value = 1;
                break;
	case V4L2_CID_CAMERA_EXIF_ISO:
		/* TODO Need to implement  Robin Singh*/
                //err = s5k4ecgx_get_iso(sd, ctrl);
		ctrl->value = ISO_50;
                break;
        case V4L2_CID_CAMERA_EXIF_EXPTIME:
		/* TODO : Robin */
                //err = s5k4ecgx_get_shutterspeed(sd, ctrl);
                break;
        case V4L2_CID_CAMERA_EXIF_FLASH:
                ctrl->value = state->flash_state_on_previous_capture;
                break;
        case V4L2_CID_CAMERA_OBJ_TRACKING_STATUS:
        case V4L2_CID_CAMERA_SMART_AUTO_STATUS:
                break;
	default:
		dev_err(&client->dev, "%s: no such ctrl\n", __func__);
		break;
	}

	mutex_unlock(&state->ctrl_lock);
	return err;
}

static int ov5640_set_capture_size(struct v4l2_subdev *sd)
{
        struct i2c_client *client = v4l2_get_subdevdata(sd);
        
	struct s5k4ba_state *state = to_state(sd);
        struct s5k4ba_userset userset = state->userset;

	int err = 0;
	/*
        dev_err(&client->dev, "%s: index:%d\n", __func__,
                state->capture_framesize_index);

        err = s5k4ecgx_set_from_table(sd, "capture_size",
                                state->regs->capture_size,
                                ARRAY_SIZE(state->regs->capture_size),
                                state->capture_framesize_index);
        if (err < 0) {
                dev_err(&client->dev,
                        "%s: failed: i2c_write for capture_size index %d\n",
                        __func__, state->capture_framesize_index);
        }
        state->runmode = S5K4ECGX_RUNMODE_CAPTURE;
	*/

	printk("\n ov5640_set_capture_size : sensor setting for cap size");
	err = ov5640_reg_writes(sd, regset_capture_resoxxxx,
                        ARRAY_SIZE(regset_capture_resoxxxx));
        if (err){
                printk(" OV5640 i2cregister write for Capture : resolution =  .... failed ");
        }

	

        return err;
}

static int ov5640_start_capture(struct v4l2_subdev *sd)
{
        int err;
        u16 read_value;
        u16 light_level;
        int poll_time_ms;
        struct i2c_client *client = v4l2_get_subdevdata(sd);
        struct s5k4ba_state *state =
                container_of(sd, struct s5k4ba_state, sd);
        struct ov5640_platform_data *pdata = client->dev.platform_data;

	mycapture = 1;

	err = ov5640_set_capture_size(sd);
        if (err < 0) {
                dev_err(&client->dev,
                        "%s: failed: i2c_write for capture_resolution\n",
                        __func__);
                return -EIO;
        }
        dev_info(&client->dev, "%s: send Capture_Start cmd\n", __func__);
        //s5k4ecgx_set_from_table(sd, "capture start",
          //                      &state->regs->capture_start, 1, 0);

        /* restore Preview  mode */


	
	return 0;

} 
 
static int  OV5640_CAMERA_Module_AF_Init(struct v4l2_subdev *sd){ 
    
        int err = 0, i;
	u16 firmwareaddr = 0x8000;
	u8  value = 0; 
	u8 *firmwarebuf = (unsigned char *)OV5640_CAMERA_Module_AF_Init_DATA;

	err = ov5640_reg_write(sd, 0x3000,0x20);
	if(err){
		printk("\n Error in fimware start download start cmd.{0x3000,0x20 }");
	} 
	mdelay(1);
 	ov5640_reg_read(sd, 0x3000, &value);
        printk(" Frimware download start(%x ,  %x)\n",0x3000, value );

	
        for (i = 0; i < sizeof(OV5640_CAMERA_Module_AF_Init_DATA); i++) {
		//printk(" AF Firmware download (%x ,  %x)\n",firmwareaddr, firmwarebuf[i]);	
                err = ov5640_reg_write(sd, firmwareaddr,firmwarebuf[i]);
                if (err)
                        return err; 
               // ov5640_reg_read(sd, firmwareaddr, &value);
                //printk(" Frimware download (%x ,  %x)\n",firmwareaddr, value );
		mdelay(1);
		firmwareaddr++;
        } 
	
	err = ov5640_reg_writes(sd, OV5640_CAMERA_Module_AF_POST,
              ARRAY_SIZE(OV5640_CAMERA_Module_AF_POST));

        if (err){
                printk(" OV5640 AF setting failed ");
        }
        return 0;

}

static int ov5640_set_brightness(struct v4l2_subdev *sd,
        struct v4l2_control *ctrl)
{
        struct v4l2_queryctrl qc = {0,};
        int val = ctrl->value, err;
        u32 exposure[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
        printk("E, value %d\n", val);

        switch (val)
        {
        case (5):
		err = ov5640_reg_writes(sd, OV5640_EV_P2,
        	      ARRAY_SIZE(OV5640_EV_P2));

	        if (err){
                	printk(" OV5640 brightness OV5640_EV_P2  setting failed ");
        	}
                break;
        case (4):
		err = ov5640_reg_writes(sd, OV5640_EV_P1,
                      ARRAY_SIZE(OV5640_EV_P1));

                if (err){
                        printk(" OV5640 brightness OV5640_EV_P1  setting failed ");
                }

                break;

        case (3):
		err = ov5640_reg_writes(sd, OV5640_EV_0,
                      ARRAY_SIZE(OV5640_EV_0));

                if (err){
                        printk(" OV5640 brightness OV5640_EV_0  setting failed ");
                }

                break;

        case (2):
		err = ov5640_reg_writes(sd, OV5640_EV_M1,
                      ARRAY_SIZE(OV5640_EV_M1));

                if (err){
                        printk(" OV5640 brightness OV5640_EV_M1  setting failed ");
                }

                break;

        case (1):
		err = ov5640_reg_writes(sd, OV5640_EV_M2,
                      ARRAY_SIZE(OV5640_EV_M2));

                if (err){
                        printk(" OV5640 brightness OV5640_EV_M2  setting failed ");
                }

                break;

        default:
		err = ov5640_reg_writes(sd, OV5640_EV_0,
                      ARRAY_SIZE(OV5640_EV_0));

                if (err){
                        printk(" OV5640 brightness OV5640_EV_0  setting failed ");
                }

                break;
        }

        return 0;
}


/* called by HAL after auto focus was finished.
 * it might off the assist flash
 */
static int ov5640_finish_auto_focus(struct v4l2_subdev *sd)
{
        struct i2c_client *client = v4l2_get_subdevdata(sd);
        struct s5k4ba_state *state =
                container_of(sd, struct s5k4ba_state, sd);


        state->af_status = AF_NONE;
        return 0;
}

static int ov5640_return_focus(struct v4l2_subdev *sd)
{

	return 0;
}

static int ov5640_stop_auto_focus(struct v4l2_subdev *sd)
{
	return 0;
}

static int OV5640_CAMERA_Module_AF_STOP( struct v4l2_subdev *sd){
	u8 read_byte = 0;
	int err = 0, i;
        // Release Focus 
        err = ov5640_reg_write(sd, 0x3023,0x01);
                if (err)
                        return err;
        err = ov5640_reg_write(sd, 0x3022,0x08);
                if (err)
                        return err;

	  for(i = 0; i < 200; i++){

		 err = ov5640_reg_read(sd, 0x3023, &read_byte);
                if (err) {
                        printk("Failed to read auto focus result 0x3023.\n");
                        return err;
                }
                printk("%s: read 0x3023 --- read_value == 0x%x\n",
                                __func__, read_byte);
                if(read_byte == 0x00)
                        break;

		mdelay(10000);

	  }
	return 0;
}
static int ov5640_auto_focus_enable( struct v4l2_subdev *sd,int mode){

	  printk("+AF START\r\n");

      if(OV5640_CAMERA_Module_AF_STOP(sd)){

	  	 printk("OV5640_CAMERA_Module_AF_STOP error\r\n");

         return 0;

	  }
	
	u8   uBuf = 0;
        int err = 0, i;
        // Release Focus 
        err = ov5640_reg_write(sd, 0x3023,0x01);
                if (err)
                        return err;
	err = ov5640_reg_write(sd, 0x3022,0x03);
                if (err)
                        return err;

	return 0;
}
static int ov5640_enable_auto_focus(struct v4l2_subdev *sd,int mode)
{
	struct s5k4ba_state *state =
                container_of(sd, struct s5k4ba_state, sd);
        struct sec_cam_parm *parms =
                (struct sec_cam_parm *)&state->strm.parm.raw_data;


        u8   uBuf = 0; 
	int err = 0, i; 
	u8 read_byte = 0;
	u8 value = 0;
        // Release Focus 
	err = ov5640_reg_write(sd, 0x3023,0x01);
                if (err)
                        return err; 
	mdelay(1);
        ov5640_reg_read(sd, 0x3023, &value);
        printk(" Enable Auto focus(%x ,  %x)\n",0x3023, value );
	

	err = ov5640_reg_write(sd, 0x3022,0x08);
                if (err)
                        return err;

	mdelay(1);
        ov5640_reg_read(sd, 0x3022, &value);
        printk(" Enable Auto focus(%x ,  %x)\n",0x3022, value );

        for (i = 0; i < 200; i++)
        {
               msleep(10); 
 	       err = ov5640_reg_read(sd, 0x3023, &read_byte);
        	if (err) {
                	printk("Failed to read auto focus result 0x3023.\n");
			return err;			
        	}
                printk("%s: read 0x3023 --- read_value == 0x%x\n",
      	        	        __func__, read_byte);
                if(read_byte == 0x00)
                        break;	
        }
        if (mode == 1)                // Single Auto Focus
        {
		//int ret_val = as3643_assit_mode_on();

		printk("\n  OLD value param->flash_state_on_previous_capture  = %d",state->flash_state_on_previous_capture);
                if (state->flash_state_on_previous_capture !=FLASH_MODE_OFF) {
			int ret_val = as3643_flash_on();
                	printk("\n ov5640 ==> flash ==> ret_value = 0x%0x",ret_val);
		}
		//int ret_val = as3643_flash_on();
		//printk("\n ov5640 ==> flash ==> ret_value = 0x%0x",ret_val);
                printk("OV5640_EnableAF::Single Auto Focus\r\n"); 
		err = ov5640_reg_write(sd, 0x3023,0x01);
                if (err) {
                	printk("\n ov5640_enable_auto_focus write error 0x3023 : Single mode."); 
		       return err;
		}

		mdelay(1);
        	ov5640_reg_read(sd, 0x3023, &value);
       		 printk(" Enable Single focus(%x ,  %x)\n",0x3023, value );



       	        err = ov5640_reg_write(sd, 0x3022,0x03);
                if (err) {
			printk("\n ov5640_enable_auto_focus write error 0x3022: Single mode.");
                        return err;
		} 
		mdelay(1);
        	ov5640_reg_read(sd, 0x3022, &value);
        	printk(" Enable single Auto focus(%x ,  %x)\n",0x3022, value ); 

		as3643_assit_mode_off();


	//	msleep(2000);
        }
        else if (mode == 2)           // Continue Auto Focus
        {
                printk("OV5640_EnableAF::Continue Auto Focus\r\n");	
		err = ov5640_reg_write(sd, 0x3023,0x01);
                if (err)
                        return err;
        	err = ov5640_reg_write(sd, 0x3022,0x04);
                if (err)
                        return err;

                for (i = 0; i < 200; i++)
                {
                        msleep(10);
			err = ov5640_reg_read(sd, 0x3023, &read_byte);
                	if (err) {
                        	printk("Failed to read auto focus result 0x3023.\n");
                        	return err;
                	}
                	printk("%s: Continue Auto Focus _read 0x3023 --- read_value == 0x%x\n",
                                __func__, read_byte);
                	if(read_byte == 0x00)
                        	break;
                }
        } 
	return 0;
}


static int ov5640_stat_auto_focus_XXXXXXXXXXXXX(struct v4l2_subdev *sd){
	int light_level;
        struct i2c_client *client = v4l2_get_subdevdata(sd);
        struct s5k4ba_state *state =
                container_of(sd, struct s5k4ba_state, sd);
	int err = 0;
	err = ov5640_enable_auto_focus(sd,1);
	if(err)
		printk(" ov5640_stat_auto_focus_XXXXXXXXXXXXX failed");
	
        return 0;

} 
static int ov5640_start_auto_focus(struct v4l2_subdev *sd)
{
	/* Need to implemnet it with all the flash setting anf light*/
	return 0;
}

static int ov5640_s_ctrl(struct v4l2_subdev *sd, struct v4l2_control *ctrl)
{ 
	printk(" \n  s5k4ba_s_ctrl start ");
#ifdef S5K4BA_COMPLETE
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int err = -EINVAL;
	struct s5k4ba_state *state =
                container_of(sd, struct s5k4ba_state, sd);
	
	  struct sec_cam_parm *parms =
                (struct sec_cam_parm *)&state->strm.parm.raw_data;
	
	int value = ctrl->value;

	mutex_lock(&state->ctrl_lock);

	switch (ctrl->id) { 

	case V4L2_CID_CAMERA_CAPTURE:
                err = ov5640_start_capture(sd);
                break;
	case V4L2_CID_EXPOSURE:
		dev_dbg(&client->dev, "%s: V4L2_CID_EXPOSURE\n", __func__);
		err = s5k4ba_write_regs(sd, \
		(unsigned char *) s5k4ba_regs_ev_bias[ctrl->value], \
			sizeof(s5k4ba_regs_ev_bias[ctrl->value]));
		break;

	case V4L2_CID_AUTO_WHITE_BALANCE:
		dev_dbg(&client->dev, "%s: V4L2_CID_AUTO_WHITE_BALANCE\n", \
			__func__);
		err = s5k4ba_write_regs(sd, \
		(unsigned char *) s5k4ba_regs_awb_enable[ctrl->value], \
			sizeof(s5k4ba_regs_awb_enable[ctrl->value]));
		break;

	case V4L2_CID_WHITE_BALANCE_PRESET:
		dev_dbg(&client->dev, "%s: V4L2_CID_WHITE_BALANCE_PRESET\n", \
			__func__);
		err = s5k4ba_write_regs(sd, \
		(unsigned char *) s5k4ba_regs_wb_preset[ctrl->value], \
			sizeof(s5k4ba_regs_wb_preset[ctrl->value]));
		break;

	case V4L2_CID_COLORFX:
		dev_dbg(&client->dev, "%s: V4L2_CID_COLORFX\n", __func__);
		err = s5k4ba_write_regs(sd, \
		(unsigned char *) s5k4ba_regs_color_effect[ctrl->value], \
			sizeof(s5k4ba_regs_color_effect[ctrl->value]));
		break;

	case V4L2_CID_CONTRAST:
		dev_dbg(&client->dev, "%s: V4L2_CID_CONTRAST\n", __func__);
		err = s5k4ba_write_regs(sd, \
		(unsigned char *) s5k4ba_regs_contrast_bias[ctrl->value], \
			sizeof(s5k4ba_regs_contrast_bias[ctrl->value]));
		break;

	case V4L2_CID_SATURATION:
		dev_dbg(&client->dev, "%s: V4L2_CID_SATURATION\n", __func__);
		err = s5k4ba_write_regs(sd, \
		(unsigned char *) s5k4ba_regs_saturation_bias[ctrl->value], \
			sizeof(s5k4ba_regs_saturation_bias[ctrl->value]));
		break;

	case V4L2_CID_SHARPNESS:
		dev_dbg(&client->dev, "%s: V4L2_CID_SHARPNESS\n", __func__);
		err = s5k4ba_write_regs(sd, \
		(unsigned char *) s5k4ba_regs_sharpness_bias[ctrl->value], \
			sizeof(s5k4ba_regs_sharpness_bias[ctrl->value]));
		break; 
	case V4L2_CID_CAMERA_FLASH_MODE: 
        	parms->flash_mode = value; 
	
		printk("\n [ V4L2_CID_CAMERA_FLASH_MODE: value = %d and ",value);	
		state->flash_state_on_previous_capture = value; 
		printk(" %s, <<<<< HAL setting Flash MODE value = %d ]",__func__,state->flash_state_on_previous_capture);
                err = ov5640_set_flash_mode(sd, value);
                break;

	 case V4L2_CID_CAMERA_BRIGHTNESS:
                err = ov5640_set_brightness(sd, ctrl);
		break;
	case V4L2_CID_CAMERA_SET_AUTO_FOCUS:
                if (value == AUTO_FOCUS_ON)
                        err= ov5640_stat_auto_focus_XXXXXXXXXXXXX(sd);//err = ov5640_start_auto_focus(sd);
                else if (value == AUTO_FOCUS_OFF)
                        err = ov5640_stop_auto_focus(sd);
                else {
                        err = -EINVAL;
                        dev_err(&client->dev,
                                "%s: bad focus value requestion %d\n",
                                __func__, value);
                }
                break;
	case V4L2_CID_CAMERA_RETURN_FOCUS:
                //if (parms->focus_mode != FOCUS_MODE_MACRO)
                        err = ov5640_return_focus(sd);
                break;
        case V4L2_CID_CAMERA_FINISH_AUTO_FOCUS:
                err = ov5640_finish_auto_focus(sd);
                break;


	default:
		dev_err(&client->dev, "%s: no such control\n", __func__);
		break;
	}

	if (err < 0){
		goto out;
	
	} else {
		mutex_unlock(&state->ctrl_lock);
		return 0;
	}
out: 
	mutex_unlock(&state->ctrl_lock);
	dev_dbg(&client->dev, "%s: vidioc_s_ctrl failed\n", __func__);
	return err;
#else
	return 0;
#endif
}
static int ov5640_read_chip_id(struct v4l2_subdev *sd ){
	
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret =0;

	u8 chip_id_high_byte = 0;
        ret = ov5640_reg_read(sd, 0x300A, &chip_id_high_byte);
        if (ret) {
                dev_err(&client->dev, "Failure to detect OV5640 chip_id_high_byte\n");

        }
        //chip_id_high_byte &= 0xF;

        dev_info(&client->dev, "Detected a OV5640 chip_id_high_byte_lower 4 bit  %x\n",
                 chip_id_high_byte);
	return 0;
}

static void ov5640_init_parameters(struct v4l2_subdev *sd)
{

	struct s5k4ba_state *state =
                container_of(sd, struct s5k4ba_state, sd);


        struct sec_cam_parm *parms =
                (struct sec_cam_parm *)&state->strm.parm.raw_data;

        state->strm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parms->capture.capturemode = 0;
        parms->capture.timeperframe.numerator = 1;
        parms->capture.timeperframe.denominator = 30;
        parms->contrast = CONTRAST_DEFAULT;
        parms->effects = IMAGE_EFFECT_NONE;
        parms->brightness = EV_DEFAULT;
        parms->flash_mode = FLASH_MODE_AUTO;
        parms->focus_mode = FOCUS_MODE_AUTO;
        parms->iso = ISO_AUTO;
        parms->metering = METERING_CENTER;
        parms->saturation = SATURATION_DEFAULT;
        parms->scene_mode = SCENE_MODE_NONE;
        parms->sharpness = SHARPNESS_DEFAULT;
        parms->white_balance = WHITE_BALANCE_AUTO;


        state->fw.major = 1;

        state->one_frame_delay_ms = NORMAL_MODE_MAX_ONE_FRAME_DELAY_MS;
}



static int ov5640_init(struct v4l2_subdev *sd, u32 val)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct s5k4ba_state *state =
                container_of(sd, struct s5k4ba_state, sd);
	int err = -EINVAL, i;

	int ret = 0; 

	v4l_info(client, "%s: camera initialization start\n", __func__);

/*	ov5640_read_chip_id(sd );
	u8 revision = 0;
	ret = ov5640_reg_read(sd, 0x302A, &revision);
        if (ret) {
                dev_err(&client->dev, "Failure to detect OV5640 chip\n");
                
        }
	//revision &= 0xF;

        dev_info(&client->dev, "Detected a OV5640 chip, revision %x\n",
                 revision); 
*/ 
	if(val == 0 ) { 
		ov5640_init_parameters(sd);
		//ret = ov5640_reg_writes(sd, configscript_common1,
              	//	ARRAY_SIZE(configscript_common1));
		ret = ov540_block_writes(sd, configscript_common1,
                        ARRAY_SIZE(configscript_common1));
        	if (ret){
			printk(" OV5640 i2c register setting failed ....");	
		} 
	
		
        	//err = OV5640_CAMERA_Module_AF_Init(sd); //old byte by byte write function , not in use. 

		err = ov5640_firmware_download_af(sd);
        	if(err){
                	printk("\n OV5640 AF init failed");
                	return err;
        	}	
        	state->af_status = AF_INITIAL;
        	printk("%s: af_status set to start\n", __func__); 

	} else {
		printk("\n regset_vga_preview : restoring preview"); 
		//ret = ov5640_reg_writes(sd, regset_vga_preview,
              	//			ARRAY_SIZE(regset_vga_preview));
		ret = ov540_block_writes(sd, regset_vga_preview,
                                        ARRAY_SIZE(regset_vga_preview));
	        if (ret){
        	        printk(" OV5640 i2c : regset_vga_preview restore fail.....");
        	} 
	} 
	
	return 0;
}

static int ov5640_s_fmt(struct v4l2_subdev *sd, struct v4l2_mbus_framefmt *fmt)
{
	struct s5k4ba_state *state =
                container_of(sd, struct s5k4ba_state, sd);
        struct i2c_client *client = v4l2_get_subdevdata(sd);

        dev_err(&client->dev, "%s: code = 0x%x, field = 0x%x,"
                " colorspace = 0x%x, width = %d, height = %d\n",
                __func__, fmt->code, fmt->field,
                fmt->colorspace,
                fmt->width, fmt->height);

        /*if (fmt->code == V4L2_MBUS_FMT_FIXED &&
                fmt->colorspace != V4L2_COLORSPACE_JPEG) {
                dev_err(&client->dev,
                        "%s: mismatch in pixelformat and colorspace\n",
                        __func__);
                return -EINVAL;
        }*/

	state->pix.width = fmt->width;
        state->pix.height = fmt->height;
        if (fmt->colorspace == V4L2_COLORSPACE_JPEG)
                state->pix.pixelformat = V4L2_PIX_FMT_JPEG;
        else
                state->pix.pixelformat = 0; /* is this used anywhere? */

	#ifdef SAMPLE_CODE 
        if (fmt->colorspace == V4L2_COLORSPACE_JPEG) {
                state->oprmode = S5K4BA_OPRMODE_IMAGE;
                /*
                 * In case of image capture mode,
                 * if the given image resolution is not supported,
                 * use the next higher image resolution. */
                //s5k4ecgx_set_framesize(sd, s5k4ecgx_capture_framesize_list,
                  //              ARRAY_SIZE(s5k4ecgx_capture_framesize_list),
                    //            false);

        } else {
                state->oprmode = S5K4BA_OPRMODE_VIDEO;
                /*
                 * In case of video mode,
                 * if the given video resolution is not matching, use
                 * the default rate (currently S5K4ECGX_PREVIEW_WVGA).
                 */
                //s5k4ecgx_set_framesize(sd, s5k4ecgx_preview_framesize_list,
                  //              ARRAY_SIZE(s5k4ecgx_preview_framesize_list),
                    //            true);
        } 
	#endif 
	//robin: Later need to use it , whem jpg setting reqired for ov5640. 
        //state->jpeg.enable = state->pix.pixelformat == V4L2_PIX_FMT_JPEG;

	return 0;
}

static int ov5640_s_stream(struct v4l2_subdev *sd, int enable)
{
        return 0;
}


static const struct v4l2_subdev_core_ops ov5640_core_ops = {
	.init = ov5640_init,	/* initializing API */
	.queryctrl = s5k4ba_queryctrl,
	.querymenu = s5k4ba_querymenu,
	.g_ctrl = ov5640_g_ctrl,
	.s_ctrl = ov5640_s_ctrl,
};

static const struct v4l2_subdev_video_ops ov5640_video_ops = {
	.s_crystal_freq = s5k4ba_s_crystal_freq,
	.enum_framesizes = ov5640_enum_framesizes,
	.enum_frameintervals = s5k4ba_enum_frameintervals,
	.s_mbus_fmt = ov5640_s_fmt,
	.g_parm = ov5640_g_parm,
	.s_parm = ov5640_s_parm,
	.s_stream = ov5640_s_stream,
};

static const struct v4l2_subdev_ops ov5640_ops = {
	.core = &ov5640_core_ops,
	.video = &ov5640_video_ops,
};

/*
 * ov5640_probe
 * Fetching platform data is being done with s_config subdev call.
 * In probe routine, we just register subdev device
 */
static int ov5640_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct s5k4ba_state *state;
	struct v4l2_subdev *sd;

	state = kzalloc(sizeof(struct s5k4ba_state), GFP_KERNEL);
	if (state == NULL)
		return -ENOMEM;

	mutex_init(&state->ctrl_lock);

	sd = &state->sd;
	strcpy(sd->name, S5K4BA_DRIVER_NAME);
        //capture test flag
	mycapture = 0;


	/* Registering subdev */
	v4l2_i2c_subdev_init(sd, client, &ov5640_ops);
	printk("%s\n", __func__);
	dev_info(&client->dev, "ov5640 has been probed\n");
	return 0;
}


static int ov5640_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);

	v4l2_device_unregister_subdev(sd);
	mutex_destroy(&state->ctrl_lock);
	kfree(to_state(sd));
	return 0;
}

static const struct i2c_device_id ov5640_id[] = {
	{ S5K4BA_DRIVER_NAME, 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, ov5640_id);

static struct i2c_driver ov5640_i2c_driver = {
	.driver = {
		.name	= S5K4BA_DRIVER_NAME,
	},
	.probe		= ov5640_probe,
	.remove		= ov5640_remove,
	.id_table	= ov5640_id,
};

static int __init ov5640_mod_init(void)
{
	return i2c_add_driver(&ov5640_i2c_driver);
}

static void __exit ov5640_mod_exit(void)
{
	i2c_del_driver(&ov5640_i2c_driver);
}
module_init(ov5640_mod_init);
module_exit(ov5640_mod_exit);

MODULE_DESCRIPTION("Ominivision OV5640 5M camera sensor driver");
MODULE_AUTHOR("Robin Singh <robin.gujjar@gmail.com>");
MODULE_LICENSE("GPL");
